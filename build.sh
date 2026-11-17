#!/usr/bin/env bash
set -euo pipefail

SUPPORTED_SHA256="499c2277d4e00cc45402043fb6ae95aae409bdaaef291808187d0abf4cb2e97d"
FORCED_RETURN="0x001427F1=0"
PIPELINE_ID="1"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLKIT="$ROOT/toolkit/xboxrecomp-0.17.0"
WORK="$ROOT/build/work"
GEN_DIR="$ROOT/port/src/recomp/gen"

TARGET="linux"
JOBS=""
FORCE_RECOMPILE=0
CLEAN=0
GAME=""
STEAM_DIR=""

die() {
    echo "error: $*" >&2
    exit 1
}

step() {
    echo
    echo "==> $*"
}

usage() {
    cat <<'EOF'
Aliens versus Predator: Extinction PC port - build from your own game copy

usage: ./build.sh GAME [options]

  GAME             your Xbox disc image (.iso) or a folder that contains default.xbe
                   (the USA release, title ID 56550022)

options:
  --target T       linux (default) or windows (cross-compile with MinGW-w64)
  -j N, --jobs N   parallel compile jobs (default: all cores)
  --recompile      run the recompiler again even if its output is up to date
  --clean          delete the CMake build folder for the target first
  --steam-dir D    optional: folder with the Steamworks redistributable to embed
                   (steam_api64.dll for windows, libsteam_api.so for linux)
  -h, --help       show this text

The finished program is written to dist/<target>/.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --target)
            [ $# -ge 2 ] || die "--target needs a value (linux or windows)"
            TARGET="$2"; shift 2 ;;
        --target=*) TARGET="${1#--target=}"; shift ;;
        -j|--jobs)
            [ $# -ge 2 ] || die "$1 needs a number"
            JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --jobs=*) JOBS="${1#--jobs=}"; shift ;;
        --recompile) FORCE_RECOMPILE=1; shift ;;
        --clean) CLEAN=1; shift ;;
        --steam-dir)
            [ $# -ge 2 ] || die "--steam-dir needs a folder"
            STEAM_DIR="$2"; shift 2 ;;
        --steam-dir=*) STEAM_DIR="${1#--steam-dir=}"; shift ;;
        -*) die "unknown option: $1 (see --help)" ;;
        *)
            [ -z "$GAME" ] || die "more than one game path given: '$GAME' and '$1'"
            GAME="$1"; shift ;;
    esac
done

[ -n "$GAME" ] || { usage >&2; exit 2; }
case "$TARGET" in
    linux|windows) ;;
    *) die "unknown target '$TARGET' (use linux or windows)" ;;
esac
if [ -z "$JOBS" ]; then
    JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi
case "$JOBS" in
    ''|*[!0-9]*) die "the job count must be a number, got '$JOBS'" ;;
esac

step "Checking the build tools"
missing=()
need() {
    command -v "$1" >/dev/null 2>&1 || missing+=("$1 ($2)")
}
need python3 "Python 3.10 or newer"
need cmake "CMake 3.20 or newer"
need ninja "Ninja"
if [ "$TARGET" = "windows" ]; then
    need x86_64-w64-mingw32-gcc "MinGW-w64 cross compiler"
    need x86_64-w64-mingw32-windres "MinGW-w64 resource compiler"
else
    need cc "a C compiler such as GCC or Clang"
    need pkg-config "pkg-config"
fi
if [ ${#missing[@]} -gt 0 ]; then
    echo "error: these tools were not found on PATH:" >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "Debian/Ubuntu: sudo apt install build-essential cmake ninja-build python3 python3-pip pkg-config libsdl2-dev libepoxy-dev libssl-dev" >&2
    echo "For --target windows also: sudo apt install mingw-w64" >&2
    exit 1
fi

python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' \
    || die "Python 3.10 or newer is required (found $(python3 -c 'import sys; print(sys.version.split()[0])'))"
python3 -c 'import capstone' 2>/dev/null \
    || die "the Python package 'capstone' is missing. Install it with: python3 -m pip install capstone==5.0.9 (or: sudo apt install python3-capstone)"

cmake_version="$(cmake --version | head -n 1 | sed 's/[^0-9.]*//; s/[^0-9.].*//')"
if [ "$(printf '%s\n3.20\n' "$cmake_version" | sort -V | head -n 1)" != "3.20" ]; then
    die "CMake 3.20 or newer is required (found $cmake_version)"
fi

if [ "$TARGET" = "linux" ]; then
    libs_missing=()
    for lib in sdl2 epoxy openssl; do
        pkg-config --exists "$lib" 2>/dev/null || libs_missing+=("$lib")
    done
    if [ ${#libs_missing[@]} -gt 0 ]; then
        echo "error: development packages not found by pkg-config: ${libs_missing[*]}" >&2
        echo "Debian/Ubuntu: sudo apt install libsdl2-dev libepoxy-dev libssl-dev" >&2
        echo "Fedora: sudo dnf install SDL2-devel libepoxy-devel openssl-devel" >&2
        exit 1
    fi
fi

[ -f "$TOOLKIT/CMakeLists.txt" ] || die "the bundled toolkit is missing: $TOOLKIT"
[ -f "$ROOT/hooks.txt" ] || die "hooks.txt is missing from $ROOT"

step "Locating default.xbe"
mkdir -p "$WORK/xbe"
XBE="$WORK/xbe/default.xbe"

find_xbe_in_dir() {
    local dir="$1" hit
    hit="$(find "$dir" -maxdepth 2 -type f -iname default.xbe 2>/dev/null | head -n 1)"
    printf '%s' "$hit"
}

sha256_of() {
    python3 -c 'import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for block in iter(lambda: f.read(1 << 20), b""):
        h.update(block)
print(h.hexdigest())' "$1"
}

[ -e "$GAME" ] || die "'$GAME' does not exist"
GAME="$(cd "$(dirname "$GAME")" && pwd)/$(basename "$GAME")"

if [ -d "$GAME" ]; then
    found="$(find_xbe_in_dir "$GAME")"
    [ -n "$found" ] || die "no default.xbe found in '$GAME' (looked two folder levels deep). Point build.sh at the folder that holds default.xbe, or at your .iso."
    cp -f "$found" "$XBE"
    echo "using $found"
elif [ -f "$GAME" ]; then
    lower="$(printf '%s' "$GAME" | tr '[:upper:]' '[:lower:]')"
    case "$lower" in
        *.iso|*.xiso)
            echo "extracting default.xbe from the disc image"
            (cd "$TOOLKIT" && PYTHONUTF8=1 python3 -m tools.xiso get "$GAME" default.xbe -o "$XBE") \
                || die "could not read default.xbe from '$GAME'. Is it an Xbox disc image (XISO)?"
            ;;
        *default.xbe)
            cp -f "$GAME" "$XBE"
            ;;
        *) die "'$GAME' is not a .iso, a default.xbe or a folder" ;;
    esac
fi

actual="$(sha256_of "$XBE")"
if [ "$actual" != "$SUPPORTED_SHA256" ]; then
    rm -f "$XBE"
    echo "error: this default.xbe is not the supported release." >&2
    echo "  expected SHA-256 $SUPPORTED_SHA256" >&2
    echo "  found    SHA-256 $actual" >&2
    echo "Only the USA Xbox release (title ID 56550022) is supported." >&2
    exit 1
fi
echo "default.xbe verified ($actual)"

STAMP="$WORK/gen_pristine/.stamp"
want_stamp="$actual $PIPELINE_ID $FORCED_RETURN"
if [ "$FORCE_RECOMPILE" -eq 0 ] && [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$want_stamp" ] \
    && [ -f "$WORK/gen_pristine/recomp_dispatch.c" ]; then
    step "Recompiler output is up to date (use --recompile to redo it)"
else
    step "Running the recompiler (about 4 minutes)"
    export PYTHONUTF8=1
    OUT="$WORK/analysis"
    rm -rf "$OUT" "$WORK/gen_pristine"
    mkdir -p "$OUT/disasm" "$OUT/func_id" "$OUT/abi" "$WORK/gen_pristine"
    cd "$TOOLKIT"
    echo "-- parsing the executable"
    python3 -m tools.xbe_parser "$XBE" --json "$WORK/xbe/default_analysis.json" >"$OUT/parser.log" 2>&1 \
        || { tail -n 20 "$OUT/parser.log" >&2; die "the XBE parser failed (log: $OUT/parser.log)"; }
    echo "-- disassembling"
    python3 -m tools.disasm "$XBE" -o "$OUT/disasm" >"$OUT/disasm.log" 2>&1 \
        || { tail -n 20 "$OUT/disasm.log" >&2; die "the disassembler failed (log: $OUT/disasm.log)"; }
    echo "-- identifying functions"
    python3 -m tools.func_id "$XBE" \
        --functions "$OUT/disasm/functions.json" \
        --strings "$OUT/disasm/strings.json" \
        --xrefs "$OUT/disasm/xrefs.json" \
        -o "$OUT/func_id" >"$OUT/func_id.log" 2>&1 \
        || { tail -n 20 "$OUT/func_id.log" >&2; die "function identification failed (log: $OUT/func_id.log)"; }
    echo "-- analysing calling conventions"
    python3 -m tools.abi_analysis "$XBE" \
        --disasm-dir "$OUT/disasm" \
        --func-id-dir "$OUT/func_id" \
        --output-dir "$OUT/abi" >"$OUT/abi.log" 2>&1 \
        || { tail -n 20 "$OUT/abi.log" >&2; die "ABI analysis failed (log: $OUT/abi.log)"; }
    echo "-- translating to C"
    python3 -m tools.recomp "$XBE" --all --split 250 \
        --game-name "AvP: Extinction" \
        --force-return "$FORCED_RETURN" \
        --disasm-dir "$OUT/disasm" \
        --func-id-dir "$OUT/func_id" \
        --abi-dir "$OUT/abi" \
        -o "$OUT/recomp" \
        --gen-dir "$WORK/gen_pristine" >"$OUT/recomp.log" 2>&1 \
        || { tail -n 20 "$OUT/recomp.log" >&2; die "the recompiler failed (log: $OUT/recomp.log)"; }
    [ -f "$WORK/gen_pristine/recomp_dispatch.c" ] || die "the recompiler produced no output (log: $OUT/recomp.log)"
    [ -f "$WORK/gen_pristine/recomp_types.h" ] || die "recomp_types.h was not written (log: $OUT/recomp.log)"
    printf '%s' "$want_stamp" >"$STAMP"
    cd "$ROOT"
fi

step "Injecting the port's hooks"
rm -rf "$GEN_DIR"
mkdir -p "$GEN_DIR"
python3 "$ROOT/tools/inject_hooks.py" "$ROOT/hooks.txt" "$WORK/gen_pristine" "$GEN_DIR" \
    || die "hook injection failed; the generated code does not match hooks.txt"

BUILD_DIR="$ROOT/build/$TARGET"
step "Configuring ($TARGET)"
if [ "$CLEAN" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi
cmake_args=(
    -S "$ROOT/port" -B "$BUILD_DIR" -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DXBOXRECOMP_DIR="$TOOLKIT"
    -DXBOXRECOMP_BUILD_TESTS=OFF
)
if [ "$TARGET" = "windows" ]; then
    cmake_args+=(
        -DCMAKE_TOOLCHAIN_FILE="$TOOLKIT/cmake/mingw-w64-x86_64.cmake"
        -DAVP_ICON_XBE="$XBE"
    )
fi
if [ -n "$STEAM_DIR" ]; then
    cmake_args+=(-DAVP_STEAM_DIR="$STEAM_DIR")
fi
cmake "${cmake_args[@]}" || die "CMake configuration failed"

step "Compiling with $JOBS jobs"
cmake --build "$BUILD_DIR" -j "$JOBS" --target extinction extinction_mp nostromo_editor || die "the build failed"

step "Collecting the result"
DIST="$ROOT/dist/$TARGET"
mkdir -p "$DIST"
if [ "$TARGET" = "windows" ]; then
    strip_tool="x86_64-w64-mingw32-strip"
    ext=".exe"
    built_main="$BUILD_DIR/extinction.exe"
else
    strip_tool="strip"
    ext=""
    built_main="$BUILD_DIR/AvPExtinction"
fi
for pair in "$built_main:AvPExtinction$ext" "$BUILD_DIR/AvPExtinctionMP$ext:AvPExtinctionMP$ext"; do
    built="${pair%%:*}"
    name="${pair##*:}"
    [ -f "$built" ] || die "the build finished but $built was not produced"
    cp -f "$built" "$DIST/$name"
    if command -v "$strip_tool" >/dev/null 2>&1; then
        "$strip_tool" "$DIST/$name"
    fi
done
mkdir -p "$DIST/maps"
cp -f "$ROOT/port/maps/"*.nmap "$ROOT/port/maps/"*.nterr "$DIST/maps/"
final="$DIST/AvPExtinction$ext"
echo
echo "Done: $final"
echo "SHA-256 $(sha256_of "$final")"
if [ "$TARGET" = "linux" ]; then
    echo "Run it from a folder of your choice with: $final --iso /path/to/your.iso   (first run, copies the game files)"
    echo "then: $final --play"
fi
