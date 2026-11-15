[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Game,
    [int]$Jobs = 0,
    [switch]$Clean,
    [switch]$Recompile,
    [switch]$UseSystemTools,
    [string]$SteamDir = '',
    [ValidateSet('windows')][string]$Target = 'windows',
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$XbeSha256 = '499c2277d4e00cc45402043fb6ae95aae409bdaaef291808187d0abf4cb2e97d'
$ForceReturn = '0x001427F1=0'
$SplitSize = '250'
$MinFreeBytes = 3GB

$Root = $PSScriptRoot

function Join-Segments {
    $path = $args[0]
    foreach ($part in ($args | Select-Object -Skip 1)) { $path = Join-Path $path $part }
    return $path
}

$Toolkit = Join-Segments $Root 'toolkit' 'xboxrecomp-0.17.0'
$Work = Join-Segments $Root 'build' 'work'
$Analysis = Join-Path $Work 'analysis'
$Pristine = Join-Path $Work 'gen_pristine'
$GenDir = Join-Segments $Root 'port' 'src' 'recomp' 'gen'
$BuildDir = Join-Segments $Root 'build' $Target
$DistDir = Join-Segments $Root 'dist' $Target
$ToolsDir = Join-Path $Root '.buildtools'
$MarkerDir = Join-Path $ToolsDir '.markers'
$DownloadDir = Join-Path $ToolsDir '.download'
$RecompStamp = Join-Path $Work 'recomp.stamp'
$InjectStamp = Join-Path $Work 'inject.stamp'

function Fail([string]$Message) {
    throw $Message
}

function Get-Sha([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-Digest([string[]]$Parts) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes(($Parts -join "`n"))
        return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-TreeDigest([string]$Dir, [string]$Filter) {
    $skip = $Dir.Length
    $lines = Get-ChildItem -LiteralPath $Dir -Recurse -File -Filter $Filter |
        Sort-Object -Property FullName |
        ForEach-Object { $_.FullName.Substring($skip) + ' ' + (Get-Sha $_.FullName) }
    return Get-Digest @($lines)
}

function Test-Stamp([string]$Stamp, [string]$Want, [string]$Probe) {
    if (-not (Test-Path -LiteralPath $Probe)) { return $false }
    if (-not (Test-Path -LiteralPath $Stamp)) { return $false }
    return ((Get-Content -LiteralPath $Stamp -Raw).Trim() -eq $Want)
}

function Remove-Path([string]$Path) {
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Recurse -Force }
}

function Find-Command([string[]]$Names) {
    foreach ($name in $Names) {
        if (Get-Command $name -ErrorAction SilentlyContinue) { return $name }
    }
    return $null
}

function Get-Probe([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $out = & $Exe @Arguments 2>$null
    if ($LASTEXITCODE -ne 0 -or -not $out) { return $null }
    return (($out | Select-Object -First 1) -as [string]).Trim()
}

function Invoke-Native([string]$Exe, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    & $Exe @Arguments 2>&1 | ForEach-Object { Write-Host "$_" }
    return $LASTEXITCODE
}

function Invoke-Proc([string]$Exe, [string[]]$Arguments, [string]$Log, [string]$WorkDir) {
    $line = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $p = Start-Process -FilePath $Exe -ArgumentList $line -WorkingDirectory $WorkDir -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $Log -RedirectStandardError "$Log.err"
    $p.WaitForExit()
    return $p.ExitCode
}

function Show-Tail([string]$Log) {
    foreach ($file in @("$Log.err", $Log)) {
        if ((Test-Path -LiteralPath $file) -and (Get-Item -LiteralPath $file).Length -gt 0) {
            Get-Content -LiteralPath $file -Tail 12 | ForEach-Object { Write-Host "  $_" }
            return
        }
    }
}

function Invoke-Py([string]$Name, [string[]]$Arguments, [string]$WorkDir, [string]$LogDir) {
    $log = Join-Path $LogDir "$Name.log"
    $code = Invoke-Proc $script:PyExe (@('-X', 'utf8') + $Arguments) $log $WorkDir
    if ($code -ne 0) {
        Show-Tail $log
        Fail "$Name failed ($code), log $log"
    }
}

function Read-Lock {
    $path = Join-Path $Root 'tools.lock'
    if (-not (Test-Path -LiteralPath $path)) { Fail "missing: $path" }
    $tools = @()
    $n = 0
    foreach ($line in Get-Content -LiteralPath $path) {
        $n++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $f = $line.Trim().Split('|')
        if ($f.Count -ne 8) { Fail "tools.lock:${n}: expected 8 fields" }
        $tools += [pscustomobject]@{
            Name = $f[0]; Version = $f[1]; File = $f[2]; Size = [int64]$f[3]
            Sha256 = $f[4].ToLowerInvariant(); Dest = $f[5]; TopLevel = ($f[6] -eq '1'); Url = $f[7]
        }
    }
    return $tools
}

function Test-Nested($Tool, $Parent) {
    $a = $Tool.Dest.Replace('\', '/')
    $b = $Parent.Dest.Replace('\', '/')
    return $a.StartsWith($b + '/')
}

function Test-Tool($Tool) {
    $marker = Join-Path $MarkerDir "$($Tool.Name).sha256"
    if (-not (Test-Path -LiteralPath $marker)) { return $false }
    if (-not (Test-Path -LiteralPath (Join-Path $ToolsDir $Tool.Dest))) { return $false }
    return ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $Tool.Sha256)
}

function Enter-Lock {
    $path = Join-Path $ToolsDir '.lock'
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $warned = $false
    while ($true) {
        $handle = $null
        try {
            $handle = [IO.File]::Open($path, 'OpenOrCreate', 'ReadWrite', 'None')
        } catch [IO.IOException] {
        }
        if ($handle) { return $handle }
        if ($clock.Elapsed.TotalMinutes -gt 30) { Fail "lock timeout: $path" }
        if (-not $warned) { Write-Host 'wait: .buildtools in use'; $warned = $true }
        Start-Sleep -Seconds 2
    }
}

function Receive-File([string]$Url, [string]$Part, [long]$Size) {
    Add-Type -AssemblyName System.Net.Http
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMinutes(30)
    $reason = ''
    try {
        for ($try = 1; $try -le 4; $try++) {
            $have = 0
            if (Test-Path -LiteralPath $Part) { $have = (Get-Item -LiteralPath $Part).Length }
            if ($have -gt $Size) { Remove-Item -LiteralPath $Part -Force; $have = 0 }
            if ($have -eq $Size) { return }
            if ($have -gt 0) { Write-Host ('resume {0} at {1} MB' -f (Split-Path -Leaf $Part), [int]($have / 1MB)) }
            $resp = $null
            $in = $null
            $out = $null
            try {
                $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, $Url)
                if ($have -gt 0) { $req.Headers.Range = New-Object System.Net.Http.Headers.RangeHeaderValue($have, $null) }
                $resp = $client.SendAsync($req, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
                $status = [int]$resp.StatusCode
                if ($status -eq 416) { Remove-Item -LiteralPath $Part -Force; continue }
                if (-not $resp.IsSuccessStatusCode) { throw "http $status" }
                $mode = 'Create'
                if ($have -gt 0 -and $status -eq 206) { $mode = 'Append' }
                $in = $resp.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
                $out = [IO.File]::Open($Part, $mode, 'Write', 'None')
                $in.CopyTo($out)
            } catch {
                $reason = $_.Exception.Message
                Start-Sleep -Seconds 2
            } finally {
                if ($out) { $out.Dispose() }
                if ($in) { $in.Dispose() }
                if ($resp) { $resp.Dispose() }
            }
        }
    } finally {
        $client.Dispose()
    }
    $size = 0
    if (Test-Path -LiteralPath $Part) { $size = (Get-Item -LiteralPath $Part).Length }
    if ($size -ne $Size) { Fail "download failed: $(Split-Path -Leaf $Part) $reason" }
}

function Get-Archive($Tool) {
    $file = Join-Path $DownloadDir $Tool.File
    $part = "$file.part"
    if (Test-Path -LiteralPath $file) {
        if ((Get-Sha $file) -eq $Tool.Sha256) { return $file }
        Remove-Item -LiteralPath $file -Force
    }
    Write-Host "fetch $($Tool.Name)-$($Tool.Version)"
    Receive-File $Tool.Url $part $Tool.Size
    if ((Get-Sha $part) -ne $Tool.Sha256) {
        Remove-Item -LiteralPath $part -Force
        Fail "sha256 mismatch: $($Tool.File)"
    }
    Move-Item -LiteralPath $part -Destination $file -Force
    return $file
}

function Expand-Tool($Tool, [string]$Archive, $All) {
    $dest = Join-Path $ToolsDir $Tool.Dest
    $temp = Join-Path $DownloadDir "$($Tool.Name).x"
    Remove-Path $temp
    Write-Host "extract $($Tool.Name)-$($Tool.Version)"
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $temp)
    $source = $temp
    if ($Tool.TopLevel) {
        $inner = @(Get-ChildItem -LiteralPath $temp)
        if ($inner.Count -ne 1 -or -not $inner[0].PSIsContainer) { Fail "unexpected layout: $($Tool.File)" }
        $source = $inner[0].FullName
    }
    $nested = @($All | Where-Object { $_.Name -ne $Tool.Name -and (Test-Nested $Tool $_) }).Count -gt 0
    if ($nested) {
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $dest -Recurse -Force
    } else {
        Remove-Path $dest
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Move-Item -LiteralPath $source -Destination $dest
    }
    Remove-Path $temp
    Remove-Item -LiteralPath $Archive -Force
}

function Set-PythonPaths {
    $dir = Join-Path $ToolsDir 'python'
    $pth = Get-ChildItem -LiteralPath $dir -Filter 'python*._pth' -File | Select-Object -First 1
    if (-not $pth) { Fail "no ._pth in $dir" }
    $zip = Get-ChildItem -LiteralPath $dir -Filter 'python*.zip' -File | Select-Object -First 1
    $lines = @()
    if ($zip) { $lines += $zip.Name }
    $lines += '.'
    $lines += (Join-Segments 'Lib' 'site-packages')
    $lines += $Toolkit
    $lines += 'import site'
    $want = ($lines -join "`n") + "`n"
    $have = [IO.File]::ReadAllText($pth.FullName).Replace("`r`n", "`n")
    if ($have -ne $want) { [IO.File]::WriteAllLines($pth.FullName, $lines) }
}

function Install-Tools {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $tools = @(Read-Lock)
    if (@($tools | Where-Object { -not (Test-Tool $_) }).Count -gt 0) {
        New-Item -ItemType Directory -Force -Path $MarkerDir, $DownloadDir | Out-Null
        $lock = Enter-Lock
        try {
            foreach ($tool in $tools) {
                if (Test-Tool $tool) { continue }
                $marker = Join-Path $MarkerDir "$($tool.Name).sha256"
                Remove-Path $marker
                $archive = Get-Archive $tool
                Expand-Tool $tool $archive $tools
                foreach ($child in ($tools | Where-Object { Test-Nested $_ $tool })) {
                    Remove-Path (Join-Path $MarkerDir "$($child.Name).sha256")
                }
                [IO.File]::WriteAllText($marker, $tool.Sha256)
            }
        } finally {
            $lock.Dispose()
        }
    }
    Set-PythonPaths
    $parts = @(
        (Join-Path $ToolsDir 'python'),
        (Join-Segments $ToolsDir 'python' 'Scripts'),
        (Join-Segments $ToolsDir 'cmake' 'bin'),
        (Join-Path $ToolsDir 'ninja'),
        (Join-Segments $ToolsDir 'llvm-mingw' 'bin')
    )
    $env:Path = ($parts -join [IO.Path]::PathSeparator) + [IO.Path]::PathSeparator + $env:Path
}

function Find-Python {
    $probe = 'import sys; sys.exit(1) if sys.version_info < (3, 10) else print(sys.executable)'
    foreach ($candidate in @(@('python'), @('python3'), @('py', '-3'))) {
        $cmd = Get-Command $candidate[0] -ErrorAction SilentlyContinue
        if (-not $cmd) { continue }
        $found = Get-Probe $cmd.Source (@($candidate | Select-Object -Skip 1) + @('-c', $probe))
        if ($found) { return $found }
    }
    return $null
}

function Test-Toolchain {
    $missing = @()
    $script:PyExe = Find-Python
    if (-not $script:PyExe) { $missing += 'python>=3.10' }
    if (-not (Find-Command @('cmake'))) { $missing += 'cmake' }
    if (-not (Find-Command @('ninja'))) { $missing += 'ninja' }
    $script:Cc = Find-Command @('x86_64-w64-mingw32-clang', 'gcc', 'clang')
    if (-not $script:Cc) { $missing += 'mingw-w64 gcc/clang' }
    if ($missing.Count -gt 0) { Fail ("not on PATH: " + ($missing -join ', ')) }
    $machine = Get-Probe (Get-Command $script:Cc).Source @('-dumpmachine')
    if ($machine -notmatch 'mingw|windows-gnu') { Fail "$($script:Cc) targets $machine, need mingw-w64" }
    if (-not (Test-Path -LiteralPath (Join-Path $Toolkit 'CMakeLists.txt'))) { Fail "missing: $Toolkit" }
    if (-not (Test-Path -LiteralPath (Join-Path $Root 'hooks.txt'))) { Fail "missing: $(Join-Path $Root 'hooks.txt')" }
    & $script:PyExe -X utf8 -c 'import capstone' 2>$null
    if ($LASTEXITCODE -ne 0) { Fail 'python module missing: capstone' }
}

function Test-Root {
    if ($Root -cmatch '[^\x21-\x7E]') { Fail "path has spaces or non-ASCII characters: $Root" }
    $free = $null
    try {
        $free = ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root))).AvailableFreeSpace
    } catch {
    }
    if ($null -ne $free -and $free -lt $MinFreeBytes) {
        Fail ('low disk: {0:N1} GB free, need {1:N0}' -f ($free / 1GB), ($MinFreeBytes / 1GB))
    }
}

function Find-Xbe([string]$Dir) {
    return (Get-ChildItem -LiteralPath $Dir -Filter 'default.xbe' -File -Recurse -Depth 2 -ErrorAction SilentlyContinue |
        Select-Object -First 1)
}

function Resolve-Game {
    $path = $Game
    if (-not $path) {
        $drop = Join-Path $Root 'isofile'
        New-Item -ItemType Directory -Force -Path $drop | Out-Null
        $iso = Get-ChildItem -LiteralPath $drop -File | Where-Object { $_.Extension -in '.iso', '.xiso' } | Select-Object -First 1
        if ($iso) {
            $path = $iso.FullName
        } elseif (Find-Xbe $drop) {
            $path = $drop
        } else {
            Fail "no game in $drop (.iso, .xiso or a folder with default.xbe)"
        }
    }
    if (-not (Test-Path -LiteralPath $path)) { Fail "not found: $path" }
    return (Resolve-Path -LiteralPath $path).ProviderPath
}

function Get-Xbe([string]$GamePath) {
    $dir = Join-Path $Work 'xbe'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $xbe = Join-Path $dir 'default.xbe'
    if (Test-Path -LiteralPath $GamePath -PathType Container) {
        $found = Find-Xbe $GamePath
        if (-not $found) { Fail "default.xbe not found under $GamePath" }
        Copy-Item -LiteralPath $found.FullName -Destination $xbe -Force
    } elseif ([IO.Path]::GetExtension($GamePath) -in '.iso', '.xiso') {
        Write-Host 'xiso extract default.xbe'
        $code = Invoke-Proc $script:PyExe @('-X', 'utf8', '-m', 'tools.xiso', 'get', $GamePath, 'default.xbe', '-o', $xbe) (Join-Path $dir 'xiso.log') $Toolkit
        if ($code -ne 0) { Show-Tail (Join-Path $dir 'xiso.log'); Fail "xiso extract failed: $GamePath" }
    } elseif ([IO.Path]::GetFileName($GamePath) -ieq 'default.xbe') {
        Copy-Item -LiteralPath $GamePath -Destination $xbe -Force
    } else {
        Fail "not an iso, default.xbe or folder: $GamePath"
    }
    return $xbe
}

function Test-Xbe([string]$Xbe) {
    $sha = Get-Sha $Xbe
    if ($sha -ne $XbeSha256) {
        Remove-Item -LiteralPath $Xbe -Force
        Fail "unsupported default.xbe: sha256 $sha, want $XbeSha256"
    }
    Write-Host "xbe $sha"
    return $sha
}

function Invoke-Recompiler([string]$Xbe, [string]$XbeHash) {
    $want = Get-Digest @('recomp', $XbeHash, $ForceReturn, $SplitSize, (Get-TreeDigest (Join-Path $Toolkit 'tools') '*.py'))
    $probe = Join-Path $Pristine 'recomp_dispatch.c'
    if (-not $Recompile -and (Test-Stamp $RecompStamp $want $probe)) {
        Write-Host 'recompile: current'
        return $want
    }
    Write-Host 'recompile'
    Remove-Path $RecompStamp
    Remove-Path $Analysis
    Remove-Path $Pristine
    $a = $Analysis
    foreach ($dir in @('disasm', 'func_id', 'abi')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $a $dir) | Out-Null
    }
    New-Item -ItemType Directory -Force -Path $Pristine | Out-Null
    $disasm = Join-Path $a 'disasm'
    $funcId = Join-Path $a 'func_id'
    $abi = Join-Path $a 'abi'

    Invoke-Py 'xbe_parser' @('-m', 'tools.xbe_parser', $Xbe, '--json', (Join-Segments $Work 'xbe' 'default_analysis.json')) $Toolkit $a
    Invoke-Py 'disasm' @('-m', 'tools.disasm', $Xbe, '-o', $disasm) $Toolkit $a
    Invoke-Py 'func_id' @(
        '-m', 'tools.func_id', $Xbe,
        '--functions', (Join-Path $disasm 'functions.json'),
        '--strings', (Join-Path $disasm 'strings.json'),
        '--xrefs', (Join-Path $disasm 'xrefs.json'),
        '-o', $funcId) $Toolkit $a
    Invoke-Py 'abi_analysis' @(
        '-m', 'tools.abi_analysis', $Xbe,
        '--disasm-dir', $disasm,
        '--func-id-dir', $funcId,
        '--output-dir', $abi) $Toolkit $a
    Invoke-Py 'recomp' @(
        '-m', 'tools.recomp', $Xbe, '--all', '--split', $SplitSize,
        '--game-name', 'AvP: Extinction',
        '--force-return', $ForceReturn,
        '--disasm-dir', $disasm,
        '--func-id-dir', $funcId,
        '--abi-dir', $abi,
        '-o', (Join-Path $a 'recomp'),
        '--gen-dir', $Pristine) $Toolkit $a

    foreach ($name in @('recomp_dispatch.c', 'recomp_types.h')) {
        if (-not (Test-Path -LiteralPath (Join-Path $Pristine $name))) { Fail "recompiler wrote no $name" }
    }
    [IO.File]::WriteAllText($RecompStamp, $want)
    return $want
}

function Invoke-Inject([string]$RecompKey) {
    $hooks = Join-Path $Root 'hooks.txt'
    $script = Join-Segments $Root 'tools' 'inject_hooks.py'
    $want = Get-Digest @('inject', $RecompKey, (Get-Sha $hooks), (Get-Sha $script))
    if (Test-Stamp $InjectStamp $want (Join-Path $GenDir 'recomp_dispatch.c')) {
        Write-Host 'inject: current'
        return
    }
    Write-Host 'inject'
    Remove-Path $InjectStamp
    Remove-Path $GenDir
    New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
    Invoke-Py 'inject_hooks' @($script, $hooks, $Pristine, $GenDir) $Root $Work
    [IO.File]::WriteAllText($InjectStamp, $want)
}

function Invoke-Compile([string]$Xbe) {
    if ($Clean) { Remove-Path $BuildDir }
    $cmakeArgs = @(
        '-S', (Join-Path $Root 'port'), '-B', $BuildDir, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$((Get-Command $script:Cc).Source)",
        "-DXBOXRECOMP_DIR=$Toolkit",
        '-DXBOXRECOMP_BUILD_TESTS=OFF',
        "-DAVP_ICON_XBE=$Xbe",
        "-DPython3_EXECUTABLE=$script:PyExe"
    )
    if (-not $UseSystemTools) {
        $cmakeArgs += "-DCMAKE_RC_COMPILER=$((Get-Command llvm-windres).Source)"
    }
    if ($SteamDir) { $cmakeArgs += "-DAVP_STEAM_DIR=$SteamDir" }
    $cmake = (Get-Command cmake).Source
    Write-Host 'configure'
    if ((Invoke-Native $cmake $cmakeArgs) -ne 0) { Fail 'cmake configure failed' }
    Write-Host 'build'
    if ((Invoke-Native $cmake @('--build', $BuildDir, '-j', "$Jobs", '--target', 'extinction')) -ne 0) { Fail 'build failed' }
}

function Publish-Exe {
    $built = Join-Path $BuildDir 'extinction.exe'
    if (-not (Test-Path -LiteralPath $built)) { Fail "not produced: $built" }
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
    $final = Join-Path $DistDir 'AvPExtinction.exe'
    Copy-Item -LiteralPath $built -Destination $final -Force
    $strip = Find-Command @('x86_64-w64-mingw32-strip', 'strip', 'llvm-strip')
    if ($strip) {
        Write-Host 'strip'
        if ((Invoke-Native (Get-Command $strip).Source @($final)) -ne 0) { Write-Host 'warn: strip failed' }
    }
    Write-Host "ok: $final"
    Write-Host "sha256 $(Get-Sha $final)"
}

function Invoke-Build {
    Test-Root
    $gamePath = Resolve-Game
    Write-Host "game: $gamePath"
    if (-not $UseSystemTools) { Install-Tools }
    Test-Toolchain
    $xbe = Get-Xbe $gamePath
    $hash = Test-Xbe $xbe
    $key = Invoke-Recompiler $xbe $hash
    Invoke-Inject $key
    Invoke-Compile $xbe
    Publish-Exe
}

if ($Help) {
    Write-Host 'usage: build.ps1 [game] [-Jobs n] [-Clean] [-Recompile] [-UseSystemTools] [-SteamDir folder] [-Target windows]'
    exit 0
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }
$clock = [Diagnostics.Stopwatch]::StartNew()
$status = 0
try { Start-Transcript -Path (Join-Path $Root 'build-log.txt') -Force | Out-Null } catch { }
try {
    Invoke-Build
} catch {
    Write-Host "error: $($_.Exception.Message)"
    $status = 1
} finally {
    $elapsed = $clock.Elapsed
    Write-Host ('time {0}:{1:00}' -f [int][math]::Floor($elapsed.TotalMinutes), $elapsed.Seconds)
    try { Stop-Transcript | Out-Null } catch { }
}
exit $status
