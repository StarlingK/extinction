[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Game,
    [int]$Jobs = 0,
    [switch]$Recompile,
    [switch]$Clean,
    [switch]$UseSystemTools,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$SupportedSha256 = '499c2277d4e00cc45402043fb6ae95aae409bdaaef291808187d0abf4cb2e97d'
$ForcedReturn = '0x001427F1=0'
$PipelineId = '1'

$Root = $PSScriptRoot
$Toolkit = Join-Path $Root 'toolkit\xboxrecomp-0.17.0'
$Work = Join-Path $Root 'build\work'
$GenDir = Join-Path $Root 'port\src\recomp\gen'
$ToolsDir = Join-Path $Root '.buildtools'
$MarkerDir = Join-Path $ToolsDir '.markers'
$DownloadDir = Join-Path $ToolsDir '.download'

function Stop-Log {
    try { Stop-Transcript | Out-Null } catch { }
}

function Fail([string]$Message) {
    Write-Host "error: $Message" -ForegroundColor Red
    Stop-Log
    exit 1
}

function Step([string]$Message) {
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Show-Usage {
    @'
Aliens versus Predator: Extinction PC port

usage: build.bat [GAME] [-Jobs N] [-Recompile] [-Clean] [-UseSystemTools]
   or: .\build.ps1 [GAME] [options]

  GAME             your Xbox disc image (.iso/.xiso) or a folder that contains
                   default.xbe (the USA release, title ID 56550022).
                   Without GAME, the isofile folder next to this script is used.
                   You can also drag the image or folder onto build.bat.
  -Jobs N          parallel compile jobs (default: all cores)
  -Recompile       run the recompiler again even if its output is up to date
  -Clean           delete the CMake build folder first
  -UseSystemTools  use Python, CMake, Ninja and MinGW-w64 already installed
                   instead of downloading portable copies into .buildtools

The finished program is written to dist\windows\AvPExtinction.exe.
'@
}

if ($Help) { Show-Usage; exit 0 }

Start-Transcript -Path (Join-Path $Root 'build-log.txt') -Force | Out-Null

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Read-ToolLock {
    $lock = Join-Path $Root 'tools.lock'
    if (-not (Test-Path -LiteralPath $lock)) { Fail "tools.lock is missing from $Root" }
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content -LiteralPath $lock) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $f = $line.Trim().Split('|')
        if ($f.Count -ne 8) { Fail "tools.lock has a malformed line: $line" }
        $list.Add([pscustomobject]@{
            Name = $f[0]; Version = $f[1]; File = $f[2]; Size = [int64]$f[3]
            Sha256 = $f[4].ToLowerInvariant(); Dest = $f[5]; TopLevel = ($f[6] -eq '1'); Url = $f[7]
        })
    }
    return $list
}

function Test-ToolReady($Tool) {
    $marker = Join-Path $MarkerDir "$($Tool.Name).sha256"
    $dest = Join-Path $ToolsDir $Tool.Dest
    if (-not (Test-Path -LiteralPath $marker)) { return $false }
    if (-not (Test-Path -LiteralPath $dest)) { return $false }
    return ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $Tool.Sha256)
}

function Save-Tool($Tool) {
    New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
    $file = Join-Path $DownloadDir $Tool.File
    if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
    $mb = [math]::Round($Tool.Size / 1MB, 1)
    Write-Host "downloading $($Tool.Name) $($Tool.Version) ($mb MB)"
    Write-Host "  from $($Tool.Url)"
    $previous = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Tool.Url -OutFile $file -UseBasicParsing
    } catch {
        if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file -Force }
        Fail "could not download $($Tool.Name) from $($Tool.Url): $($_.Exception.Message)"
    } finally {
        $ProgressPreference = $previous
    }
    $actual = Get-Sha256 $file
    if ($actual -ne $Tool.Sha256) {
        Remove-Item -LiteralPath $file -Force
        Write-Host "error: the downloaded $($Tool.File) does not match its pinned SHA-256." -ForegroundColor Red
        Write-Host "  expected $($Tool.Sha256)" -ForegroundColor Red
        Write-Host "  received $actual" -ForegroundColor Red
        Write-Host 'The file was deleted. Nothing from it was used. Try again later or check your network.' -ForegroundColor Red
        Stop-Log
        exit 1
    }
    Write-Host "  SHA-256 verified"
    return $file
}

function Expand-Tool($Tool, [string]$Archive) {
    $dest = Join-Path $ToolsDir $Tool.Dest
    $temp = Join-Path $DownloadDir "$($Tool.Name).extract"
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    $isMerge = ($Tool.Name -eq 'capstone')
    if ((Test-Path -LiteralPath $dest) -and -not $isMerge) { Remove-Item -LiteralPath $dest -Recurse -Force }
    Write-Host "extracting $($Tool.Name)"
    [IO.Compression.ZipFile]::ExtractToDirectory($Archive, $temp)
    $source = $temp
    if ($Tool.TopLevel) {
        $inner = @(Get-ChildItem -LiteralPath $temp)
        if ($inner.Count -ne 1 -or -not $inner[0].PSIsContainer) { Fail "unexpected layout in $($Tool.File)" }
        $source = $inner[0].FullName
    }
    if (Test-Path -LiteralPath $dest) {
        Copy-Item -Path (Join-Path $source '*') -Destination $dest -Recurse -Force
    } else {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
        Move-Item -LiteralPath $source -Destination $dest
    }
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
    Remove-Item -LiteralPath $Archive -Force
}

function Set-PythonPaths {
    $pyDir = Join-Path $ToolsDir 'python'
    $pth = Get-ChildItem -LiteralPath $pyDir -Filter 'python*._pth' -File | Select-Object -First 1
    if (-not $pth) { Fail "the embeddable Python has no ._pth file in $pyDir" }
    $zip = Get-ChildItem -LiteralPath $pyDir -Filter 'python*.zip' -File | Select-Object -First 1
    $lines = @()
    if ($zip) { $lines += $zip.Name }
    $lines += '.'
    $lines += 'Lib\site-packages'
    $lines += $Toolkit
    $lines += 'import site'
    [IO.File]::WriteAllLines($pth.FullName, $lines)
}

function Install-Tools {
    $tools = Read-ToolLock
    $pending = @($tools | Where-Object { -not (Test-ToolReady $_) })
    if ($pending.Count -eq 0) {
        Write-Host 'portable build tools are present and verified'
        return
    }
    $total = [math]::Round((($pending | Measure-Object -Property Size -Sum).Sum) / 1MB, 0)
    Write-Host "need to download $($pending.Count) tool(s), about $total MB, into $ToolsDir"
    try {
        Invoke-WebRequest -Uri $pending[0].Url -Method Head -UseBasicParsing | Out-Null
    } catch {
        Fail "no internet connection to $(([Uri]$pending[0].Url).Host), and the build tools must be downloaded first ($($_.Exception.Message)). Connect to the internet and run build.bat again."
    }
    New-Item -ItemType Directory -Force -Path $MarkerDir | Out-Null
    $pythonChanged = $false
    foreach ($tool in $tools) {
        if ((Test-ToolReady $tool) -and -not ($tool.Name -eq 'capstone' -and $pythonChanged)) { continue }
        $marker = Join-Path $MarkerDir "$($tool.Name).sha256"
        if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
        $archive = Save-Tool $tool
        Expand-Tool $tool $archive
        if ($tool.Name -eq 'python') { $pythonChanged = $true }
        [IO.File]::WriteAllText($marker, $tool.Sha256)
    }
}

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Find-Python {
    foreach ($candidate in @(@('python'), @('python3'), @('py', '-3'))) {
        if (-not (Test-Command $candidate[0])) { continue }
        $extra = @()
        if ($candidate.Count -gt 1) { $extra = $candidate[1..($candidate.Count - 1)] }
        & $candidate[0] @extra -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) { return , $candidate }
    }
    return $null
}

function Invoke-Python {
    param([string[]]$PyArgs)
    & $script:PyExe @script:PyPrefix @PyArgs
}

Step 'Checking the folder'
if ($Root -match '\s' -or $Root -match '[^\x21-\x7E]') {
    Fail "this folder path contains spaces or non-ASCII characters: $Root`nMove the project to a plain path such as C:\avp and run build.bat again."
}
try {
    if ($Root -notmatch '^\\\\') {
        $freeGb = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($Root)).AvailableFreeSpace / 1GB
        if ($freeGb -lt 3) { Fail ("only {0:N1} GB free on this drive; about 3 GB is needed for the tools and the build." -f $freeGb) }
    }
} catch [System.IO.IOException] {
}

Step 'Locating your game'
if (-not $Game) {
    $dropDir = Join-Path $Root 'isofile'
    New-Item -ItemType Directory -Force -Path $dropDir | Out-Null
    $iso = Get-ChildItem -LiteralPath $dropDir -File | Where-Object { $_.Extension -in '.iso', '.xiso' } | Select-Object -First 1
    if ($iso) {
        $Game = $iso.FullName
    } elseif (Get-ChildItem -LiteralPath $dropDir -Filter 'default.xbe' -File -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1) {
        $Game = $dropDir
    } else {
        Fail "no game found. Put your Xbox disc image (.iso or .xiso), or a folder that contains default.xbe, into:`n  $dropDir`nor drag it onto build.bat."
    }
}
if (-not (Test-Path -LiteralPath $Game)) { Fail "'$Game' does not exist" }
$gameFull = (Resolve-Path -LiteralPath $Game).ProviderPath
Write-Host "game: $gameFull"

Add-Type -AssemblyName System.IO.Compression.FileSystem
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

Step 'Checking the build tools'
if (-not $UseSystemTools) {
    Install-Tools
    $pathParts = @(
        (Join-Path $ToolsDir 'python'),
        (Join-Path $ToolsDir 'python\Scripts'),
        (Join-Path $ToolsDir 'cmake\bin'),
        (Join-Path $ToolsDir 'ninja'),
        (Join-Path $ToolsDir 'llvm-mingw\bin')
    )
    $env:Path = ($pathParts -join ';') + ';' + $env:Path
    Set-PythonPaths
    Write-Host 'using the portable tools in .buildtools (PATH is changed for this run only)'
} else {
    Write-Host 'using the tools installed on this computer :) (-UseSystemTools)'
}

$problems = New-Object System.Collections.Generic.List[string]

$py = Find-Python
if (-not $py) {
    $problems.Add('Python 3.10 or newer (python.exe on PATH; the Microsoft Store stub does not count)')
} else {
    $script:PyExe = $py[0]
    $script:PyPrefix = @('-X', 'utf8')
    if ($py.Count -gt 1) { $script:PyPrefix = $py[1..($py.Count - 1)] + $script:PyPrefix }
}
if (-not (Test-Command 'cmake')) { $problems.Add('CMake 3.20 or newer') }
if (-not (Test-Command 'ninja')) { $problems.Add('Ninja') }

$cc = $null
foreach ($name in @('x86_64-w64-mingw32-clang', 'gcc', 'clang')) {
    if (Test-Command $name) { $cc = $name; break }
}
if (-not $cc) {
    $problems.Add('a MinGW-w64 C compiler (gcc from MinGW-w64/MSYS2 UCRT64, or clang from llvm-mingw)')
} else {
    $machine = (& $cc -dumpmachine 2>$null | Out-String).Trim()
    if ($machine -notmatch 'mingw|windows-gnu') {
        Fail "'$cc' targets '$machine', not MinGW-w64. Put a MinGW-w64 gcc or an llvm-mingw clang first on PATH."
    }
}

if ($problems.Count -gt 0) {
    Write-Host 'error: these tools were not found on PATH:' -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  $p" -ForegroundColor Red }
    Write-Host 'Run without -UseSystemTools to download them, or see the Windows section of README.md.' -ForegroundColor Red
    Stop-Log
    exit 1
}

Invoke-Python @('-c', 'import capstone') 2>$null
if ($LASTEXITCODE -ne 0) {
    Fail "the Python package 'capstone' is missing. Install it with: python -m pip install capstone==5.0.9"
}

if (-not (Test-Path (Join-Path $Toolkit 'CMakeLists.txt'))) { Fail "the bundled toolkit is missing: $Toolkit" }
if (-not (Test-Path (Join-Path $Root 'hooks.txt'))) { Fail "hooks.txt is missing from $Root" }

Step 'Reading default.xbe'
New-Item -ItemType Directory -Force -Path (Join-Path $Work 'xbe') | Out-Null
$Xbe = Join-Path $Work 'xbe\default.xbe'

if (Test-Path -LiteralPath $gameFull -PathType Container) {
    $found = Get-ChildItem -LiteralPath $gameFull -Filter 'default.xbe' -File -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) {
        Fail "no default.xbe found in '$gameFull' (looked two folder levels deep). Point build.bat at the folder that holds default.xbe, or at your .iso."
    }
    Copy-Item -LiteralPath $found.FullName -Destination $Xbe -Force
    Write-Host "using $($found.FullName)"
} else {
    $ext = [IO.Path]::GetExtension($gameFull).ToLowerInvariant()
    $leaf = [IO.Path]::GetFileName($gameFull).ToLowerInvariant()
    if ($ext -eq '.iso' -or $ext -eq '.xiso') {
        Write-Host 'extracting default.xbe from the disc image'
        Push-Location $Toolkit
        try {
            Invoke-Python @('-m', 'tools.xiso', 'get', $gameFull, 'default.xbe', '-o', $Xbe)
            $isoStatus = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        if ($isoStatus -ne 0) { Fail "could not read default.xbe from '$gameFull'. Is it an Xbox disc image (XISO)?" }
    } elseif ($leaf -like '*default.xbe') {
        Copy-Item -LiteralPath $gameFull -Destination $Xbe -Force
    } else {
        Fail "'$gameFull' is not a .iso, a default.xbe or a folder"
    }
}

$actual = Get-Sha256 $Xbe
if ($actual -ne $SupportedSha256) {
    Remove-Item -LiteralPath $Xbe -Force
    Write-Host 'error: this default.xbe is not the supported release.' -ForegroundColor Red
    Write-Host "  expected SHA-256 $SupportedSha256" -ForegroundColor Red
    Write-Host "  found    SHA-256 $actual" -ForegroundColor Red
    Write-Host 'Only the USA Xbox release (title ID 56550022) is supported.' -ForegroundColor Red
    exit 1
}
Write-Host "default.xbe verified ($actual)"

$Pristine = Join-Path $Work 'gen_pristine'
$Stamp = Join-Path $Pristine '.stamp'
$wantStamp = "$actual $PipelineId $ForcedReturn"
$upToDate = $false
if (-not $Recompile -and (Test-Path $Stamp) -and (Test-Path (Join-Path $Pristine 'recomp_dispatch.c'))) {
    $upToDate = ((Get-Content -LiteralPath $Stamp -Raw).Trim() -eq $wantStamp)
}

function Run-Tool([string]$Label, [string]$LogName, [string[]]$PyArgs) {
    Write-Host "-- $Label"
    $log = Join-Path $script:Out $LogName
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    Push-Location $Toolkit
    try {
        & $script:PyExe @script:PyPrefix @PyArgs *> $log
        $status = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $previous
    }
    if ($status -ne 0) {
        Get-Content -LiteralPath $log -Tail 20 | ForEach-Object { Write-Host $_ -ForegroundColor Red }
        Fail "$Label failed (log: $log)"
    }
}

if ($upToDate) {
    Step 'Recompiler output is up to date (use -Recompile to redo it)'
} else {
    Step 'Running the recompiler (about 4 minutes)'
    $script:Out = Join-Path $Work 'analysis'
    foreach ($dir in @($script:Out, $Pristine)) {
        if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
    }
    foreach ($dir in @('disasm', 'func_id', 'abi')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $script:Out $dir) | Out-Null
    }
    New-Item -ItemType Directory -Force -Path $Pristine | Out-Null
    $o = $script:Out

    Run-Tool 'parsing the executable' 'parser.log' @('-m', 'tools.xbe_parser', $Xbe, '--json', (Join-Path $Work 'xbe\default_analysis.json'))
    Run-Tool 'disassembling' 'disasm.log' @('-m', 'tools.disasm', $Xbe, '-o', "$o\disasm")
    Run-Tool 'identifying functions' 'func_id.log' @(
        '-m', 'tools.func_id', $Xbe,
        '--functions', "$o\disasm\functions.json",
        '--strings', "$o\disasm\strings.json",
        '--xrefs', "$o\disasm\xrefs.json",
        '-o', "$o\func_id")
    Run-Tool 'analysing calling conventions' 'abi.log' @(
        '-m', 'tools.abi_analysis', $Xbe,
        '--disasm-dir', "$o\disasm",
        '--func-id-dir', "$o\func_id",
        '--output-dir', "$o\abi")
    Run-Tool 'translating to C' 'recomp.log' @(
        '-m', 'tools.recomp', $Xbe, '--all', '--split', '250',
        '--game-name', 'AvP: Extinction',
        '--force-return', $ForcedReturn,
        '--disasm-dir', "$o\disasm",
        '--func-id-dir', "$o\func_id",
        '--abi-dir', "$o\abi",
        '-o', "$o\recomp",
        '--gen-dir', $Pristine)

    if (-not (Test-Path (Join-Path $Pristine 'recomp_dispatch.c'))) { Fail "the recompiler produced no output (log: $o\recomp.log)" }
    if (-not (Test-Path (Join-Path $Pristine 'recomp_types.h'))) { Fail "recomp_types.h was not written (log: $o\recomp.log)" }
    [IO.File]::WriteAllText($Stamp, $wantStamp)
}

Step "Injecting the port's hooks"
if (Test-Path $GenDir) { Remove-Item -LiteralPath $GenDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $GenDir | Out-Null
Invoke-Python @((Join-Path $Root 'tools\inject_hooks.py'), (Join-Path $Root 'hooks.txt'), $Pristine, $GenDir)
if ($LASTEXITCODE -ne 0) { Fail 'hook injection failed; the generated code does not match hooks.txt' }

$BuildDir = Join-Path $Root 'build\windows'
Step 'Configuring'
if ($Clean -and (Test-Path $BuildDir)) { Remove-Item -LiteralPath $BuildDir -Recurse -Force }
$cmakeArgs = @(
    '-S', (Join-Path $Root 'port'), '-B', $BuildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$((Get-Command $cc).Source)",
    "-DXBOXRECOMP_DIR=$Toolkit",
    '-DXBOXRECOMP_BUILD_TESTS=OFF',
    "-DAVP_ICON_XBE=$Xbe"
)
if (-not $UseSystemTools) {
    $cmakeArgs += "-DCMAKE_RC_COMPILER=$((Get-Command llvm-windres).Source)"
}
$cmakeArgs += "-DPython3_EXECUTABLE=$((Get-Command $script:PyExe).Source)"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail 'CMake configuration failed' }

Step "Compiling with $Jobs jobs"
& cmake --build $BuildDir -j $Jobs --target extinction
if ($LASTEXITCODE -ne 0) { Fail 'the build failed' }

Step 'Collecting the result'
$Dist = Join-Path $Root 'dist\windows'
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$built = Join-Path $BuildDir 'extinction.exe'
$final = Join-Path $Dist 'AvPExtinction.exe'
if (-not (Test-Path $built)) { Fail "the build finished but $built was not produced" }
Copy-Item -LiteralPath $built -Destination $final -Force

$strip = $null
foreach ($name in @('x86_64-w64-mingw32-strip', 'strip', 'llvm-strip')) {
    if (Test-Command $name) { $strip = $name; break }
}
if ($strip) {
    & $strip $final
    if ($LASTEXITCODE -ne 0) { Write-Host 'warning: strip failed, keeping the unstripped program' -ForegroundColor Yellow }
}

Write-Host ''
Write-Host "Done: $final"
Write-Host "SHA-256 $(Get-Sha256 $final)"
Write-Host 'Start it, choose "Set up from my disc image..." once, then Play.'
Stop-Log
