#!/usr/bin/env bash
# Build Nam A2 module for Move Anything (ARM64)
#
# Two-phase build:
#   1. Build NeuralAudio as a static library via CMake cross-compilation
#   2. Compile nam_a2_plugin.cpp and link against libNeuralAudio.a
#
# Automatically uses Docker for cross-compilation if needed.
# Set CROSS_PREFIX to skip Docker (e.g., for native ARM builds).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-nam-a2-builder"

# Check if we need Docker
if [ -z "$CROSS_PREFIX" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== Nam A2 Module Build (via Docker) ==="
    echo ""

    # Build Docker image if needed
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image (first time only)..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
        echo ""
    fi

    # Run build inside container
    echo "Running build..."
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh

    echo ""
    echo "=== Done ==="
    exit 0
fi

# === Actual build (runs in Docker or with cross-compiler) ===
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"

cd "$REPO_ROOT"

echo "=== Building Nam A2 Module ==="
echo "Cross prefix: $CROSS_PREFIX"

# Create build directories. Wipe dist/nam-a2 first so stale files from a
# previous build (e.g. removed/renamed models) don't get repackaged.
mkdir -p build/neuralaudio
rm -rf dist/nam-a2
mkdir -p dist/nam-a2

# --- ABI safety: target the glibc Move actually runs, not the build ---
# --- host's. This must run BEFORE Phase 1. ---
#
# It used to sit between the two phases, so only the plugin's own
# translation unit got --sysroot and the whole of NeuralAudio was compiled
# against the host's glibc 2.39 headers. Those headers redirect strtol and
# friends to __isoc23_strto*, which exist only in glibc 2.38+ - and because
# a -shared link does not require its undefined symbols to resolve, the
# build stayed green and emitted a .so carrying five unresolvable
# references. On the device that is a silent RTLD_NOW failure: the module
# never loads and nothing anywhere says why. The link line below now also
# passes -Wl,--no-undefined, and the build ends with an explicit check
# against Move's GLIBC ceiling, so neither half can regress quietly again.
#
# Move's rootfs is close to Ubuntu 22.04 (glibc 2.35) - that's why
# scripts/Dockerfile pins ubuntu:22.04. Compiling with a newer host's own
# cross-toolchain (e.g. Ubuntu 24.04's, glibc 2.39) silently produces a .so
# that requires glibc symbols Move doesn't have (GLIBC_2.38 measured) -
# dlopen fails on-device with nothing but a cryptic version-not-found line,
# while every check on the BUILD machine (compiles clean, correct ELF
# arch/entry point) looks fine. This bit a real build: native (no-Docker)
# CROSS_PREFIX compiles happened to run on a newer host and produced an
# unloadable module that "wouldn't open" with no error visible until the
# GLIBC symbol versions were inspected directly.
#
# Inside the project's own Docker image this is already correct (it IS
# Ubuntu 22.04), so only the native/no-Docker path needs a fetched sysroot.
SYSROOT_FLAGS=""
if [ ! -f "/.dockerenv" ]; then
    JAMMY_SYSROOT="build/jammy-arm64-sysroot"
    if [ ! -f "$JAMMY_SYSROOT/usr/aarch64-linux-gnu/lib/libc.so.6" ]; then
        echo ""
        echo "--- Fetching a jammy (glibc 2.35) aarch64 sysroot for ABI-correct native linking ---"
        JAMMY_PKG_DIR="build/jammy-pkgs"
        mkdir -p "$JAMMY_PKG_DIR" "$JAMMY_SYSROOT"
        JAMMY_SOURCELIST="build/jammy-sources.list"
        echo "deb http://archive.ubuntu.com/ubuntu jammy main universe" > "$JAMMY_SOURCELIST"

        apt-get -o Dir::Etc::sourcelist="$REPO_ROOT/$JAMMY_SOURCELIST" \
            -o Dir::Etc::sourceparts=/dev/null update

        # `apt-cache policy` prints "  VERSION PRIORITY" then, on the next
        # line, "  PRIORITY http://.../jammy/..." - track the version from
        # the first line shape and only read it back on the second, or the
        # http line's own leading priority number gets picked up instead.
        JAMMY_APT_VER() {
            apt-cache -o Dir::Etc::sourcelist="$REPO_ROOT/$JAMMY_SOURCELIST" \
                -o Dir::Etc::sourceparts=/dev/null policy "$1" \
                | awk '/^ +[^ ]+ [0-9]+$/ && !/http/ { ver=$1 } /http:\/\/.*jammy\/main/ { print ver; exit }'
        }
        JAMMY_LIBC_VER=$(JAMMY_APT_VER libc6-arm64-cross)
        JAMMY_KERNEL_VER=$(JAMMY_APT_VER linux-libc-dev-arm64-cross)

        if [ -z "$JAMMY_LIBC_VER" ] || [ -z "$JAMMY_KERNEL_VER" ]; then
            echo "ERROR: could not resolve jammy arm64-cross package versions from archive.ubuntu.com"
            exit 1
        fi

        (cd "$JAMMY_PKG_DIR" && apt-get \
            -o Dir::Etc::sourcelist="$REPO_ROOT/$JAMMY_SOURCELIST" \
            -o Dir::Etc::sourceparts=/dev/null \
            download \
            "libc6-arm64-cross=$JAMMY_LIBC_VER" \
            "libc6-dev-arm64-cross=$JAMMY_LIBC_VER" \
            "linux-libc-dev-arm64-cross=$JAMMY_KERNEL_VER")

        for deb in "$JAMMY_PKG_DIR"/*.deb; do
            dpkg-deb -x "$deb" "$JAMMY_SYSROOT"
        done
    fi
    # --sysroot alone is not enough for the HEADERS: it makes gcc look in
    # <sysroot>/usr/include, while libc6-dev-arm64-cross unpacks its headers
    # to <sysroot>/usr/aarch64-linux-gnu/include. Without the -isystem, gcc
    # silently falls back to the host's own /usr/aarch64-linux-gnu/include -
    # which is glibc 2.39's, and that is what emits the __isoc23_strto*
    # references in the first place. Name it explicitly.
    JAMMY_INC="$REPO_ROOT/$JAMMY_SYSROOT/usr/aarch64-linux-gnu/include"
    SYSROOT_FLAGS="--sysroot=$REPO_ROOT/$JAMMY_SYSROOT -isystem $JAMMY_INC -B$REPO_ROOT/$JAMMY_SYSROOT/usr/aarch64-linux-gnu/lib"
    CMAKE_SYSROOT_LINE="set(CMAKE_SYSROOT $REPO_ROOT/$JAMMY_SYSROOT)"
    # CMAKE_SYSROOT only passes --sysroot, which does not reach these
    # headers (see above), so Phase 1 needs the -isystem spelled out too or
    # NeuralAudio keeps compiling against the host's glibc.
    SYSROOT_INC="-isystem $JAMMY_INC"
    echo "Building against jammy sysroot: $JAMMY_SYSROOT"
fi

echo ""
echo "--- Phase 1: Building NeuralAudio static library ---"

# Create CMake toolchain file for cross-compilation
cat > build/aarch64-toolchain.cmake << TOOLCHAIN_EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
${CMAKE_SYSROOT_LINE}
TOOLCHAIN_EOF

cmake -S deps/NeuralAudio -B build/neuralaudio \
    -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/build/aarch64-toolchain.cmake" \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_CXX_FLAGS="-Ofast -march=armv8-a -mtune=cortex-a72 -DNDEBUG $SYSROOT_INC" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_UTILS=OFF \
    -DBUILD_NAMCORE=OFF \
    -DBUILD_STATIC_RTNEURAL=OFF \
    -DWAVENET_FRAMES=128 \
    -DBUFFER_PADDING=8

cmake --build build/neuralaudio -j"$(nproc)"

echo "NeuralAudio static library built."

# --- Phase 2: Compile Nam A2 plugin and link ---
echo ""
echo "--- Phase 2: Compiling Nam A2 plugin ---"

# Find the static libraries we need to link
NA_LIB="build/neuralaudio/NeuralAudio/libNeuralAudio.a"
RT_LIB=$(find build/neuralaudio -name "libRTNeural.a" | head -1)

# Newer NeuralAudio versions declare the NeuralAudio CMake target as an
# OBJECT library (no linkable .a of its own - just .o files under
# CMakeFiles/NeuralAudio.dir/) rather than a STATIC one. Archive those
# object files ourselves so Phase 2's direct g++ link line keeps working
# whichever form the pinned deps/NeuralAudio checkout uses.
if [ ! -f "$NA_LIB" ]; then
    NA_OBJDIR="build/neuralaudio/NeuralAudio/CMakeFiles/NeuralAudio.dir"
    NA_OBJS=$(find "$NA_OBJDIR" -name "*.o" 2>/dev/null)
    if [ -n "$NA_OBJS" ]; then
        echo "NeuralAudio built as an OBJECT library; archiving its .o files into $NA_LIB"
        ${CROSS_PREFIX}ar rcs "$NA_LIB" $NA_OBJS
    fi
fi

if [ ! -f "$NA_LIB" ]; then
    echo "ERROR: NeuralAudio library not found: $NA_LIB"
    find build/neuralaudio -name "*.a" 2>/dev/null
    exit 1
fi
if [ -z "$RT_LIB" ] || [ ! -f "$RT_LIB" ]; then
    echo "ERROR: RTNeural library not found"
    find build/neuralaudio -name "*.a" 2>/dev/null
    exit 1
fi
echo "Found NeuralAudio: $NA_LIB"
echo "Found RTNeural: $RT_LIB"

# --- Keep the two phases' preprocessor definitions IDENTICAL --------------
#
# The plugin includes NeuralAudio's headers and links its objects, so the
# two must agree on every macro those headers read. They did not: Phase 1
# compiled with 17 definitions and this file restated six of them by hand,
# so the library was built with BUILD_RTNEURAL, NAM_ENABLE_A2_FAST,
# BUILD_STATIC_INTERNAL_NAMA2, NAM_A2_RING_MODE, RTNEURAL_USE_EIGEN and
# RTNEURAL_DEFAULT_ALIGNMENT while the plugin's view of the same classes
# had none of them.
#
# That is an ODR violation, and it does not fail to build or to load - the
# plugin gets a NeuralModel whose members sit at different offsets than the
# code operating on them expects, so Process() reads and writes the wrong
# memory and the output is noise. Which is exactly what the device did:
# white noise on line in, with the module otherwise working.
#
# Restating the list correctly would fix it once and break again the next
# time NeuralAudio adds an option. So the list is not restated: it is read
# out of the compile command CMake actually used for NeuralModel.cpp, the
# translation unit that defines the classes this plugin calls into.
# *_EXPORTS is dropped - that one belongs to a target we do not build into.
NA_DEFS=$(python3 - "$REPO_ROOT/build/neuralaudio/compile_commands.json" << 'PYEOF'
import json, re, sys
entries = json.load(open(sys.argv[1]))
for e in entries:
    if e["file"].endswith("NeuralAudio/NeuralModel.cpp"):
        defs = re.findall(r'-D[^ ]+', e.get("command") or " ".join(e["arguments"]))
        print(" ".join(d for d in sorted(set(defs)) if not d.endswith("_EXPORTS")))
        break
else:
    sys.exit("ERROR: NeuralModel.cpp not found in compile_commands.json")
PYEOF
)
if [ -z "$NA_DEFS" ]; then
    echo "ERROR: could not read NeuralAudio's compile definitions."
    echo "Without them the plugin and the library disagree on class layout,"
    echo "which builds and loads cleanly and outputs noise."
    exit 1
fi
echo "NeuralAudio definitions carried into the plugin:"
echo "  $NA_DEFS" | tr ' ' '\n' | sed 's/^/    /' | grep -v '^ *$'

# The compat TU (see src/dsp/glibc_compat.c) supplies the handful of
# symbols gcc-13's own static libstdc++ reaches for that Move's glibc does
# not export. It is compiled against the same sysroot as everything else.
${CROSS_PREFIX}gcc -O2 -fPIC $SYSROOT_FLAGS \
    -march=armv8-a -mtune=cortex-a72 \
    -c src/dsp/glibc_compat.c -o build/glibc_compat.o

${CROSS_PREFIX}g++ -Ofast -shared -fPIC \
    -std=c++20 \
    $SYSROOT_FLAGS \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -static-libstdc++ \
    $NA_DEFS \
    src/dsp/nam_a2_plugin.cpp \
    build/glibc_compat.o \
    -o build/nam-a2.so \
    -Isrc/dsp \
    -Ideps/NeuralAudio \
    -Ideps/NeuralAudio/NeuralAudio \
    -Ideps/NeuralAudio/deps/RTNeural/modules/Eigen \
    -Ideps/NeuralAudio/deps/RTNeural/modules/json \
    -Ideps/NeuralAudio/deps/RTNeural/modules/json/single_include \
    -Ideps/NeuralAudio/deps/RTNeural \
    -Ideps/NeuralAudio/deps/math_approx/include \
    -Ideps/NeuralAudio/deps/RTNeural-NAM/wavenet \
    -Ideps/NeuralAudio/deps/NeuralAmpModelerCore \
    "$NA_LIB" \
    "$RT_LIB" \
    -Wl,--no-undefined \
    -lm -lpthread

echo "Plugin compiled: build/nam-a2.so"

# --- ABI gate -------------------------------------------------------------
#
# Move's rootfs tops out at GLIBC_2.34. That number is measured, not
# assumed: the shipped schwung-nam module loads on the device and its
# highest versioned reference is @GLIBC_2.34, with no unversioned strong
# references at all.
#
# Both failure modes below are invisible until the device refuses to load
# the module, and the device has no way to say why - dlopen's error goes to
# the host's log, which is off by default, and the module's own code never
# runs to report anything. So they are caught here instead, where the
# message can name the symbol.
MAX_GLIBC_MINOR=34

BAD_UNVERSIONED=$(${CROSS_PREFIX}nm -D --undefined-only build/nam-a2.so \
    | awk '$1 == "U" { print $2 }' | grep -v '@' || true)

BAD_VERSIONED=$(${CROSS_PREFIX}nm -D --undefined-only build/nam-a2.so \
    | grep -oE '[^ ]+@GLIBC_2\.[0-9]+' \
    | awk -F'@GLIBC_2.' -v max="$MAX_GLIBC_MINOR" '$2 + 0 > max { print $0 }' || true)

if [ -n "$BAD_UNVERSIONED" ] || [ -n "$BAD_VERSIONED" ]; then
    echo ""
    echo "ERROR: build/nam-a2.so cannot load on Move."
    [ -n "$BAD_UNVERSIONED" ] && {
        echo "  Unversioned undefined symbols (device libc has no such symbol):"
        echo "$BAD_UNVERSIONED" | sed 's/^/    /'
    }
    [ -n "$BAD_VERSIONED" ] && {
        echo "  Symbols newer than Move's GLIBC_2.$MAX_GLIBC_MINOR:"
        echo "$BAD_VERSIONED" | sed 's/^/    /'
    }
    echo ""
    echo "  Add them to src/dsp/glibc_compat.c, or stop pulling in whatever"
    echo "  reaches for them. Shipping this .so gives a module that is"
    echo "  installed and selectable and silently never opens."
    exit 1
fi

echo "ABI gate: no undefined symbol above GLIBC_2.$MAX_GLIBC_MINOR, none unversioned."

# --- Package ---
echo ""
echo "--- Packaging ---"

cat src/module.json > dist/nam-a2/module.json
[ -f src/help.json ] && cat src/help.json > dist/nam-a2/help.json
cat build/nam-a2.so > dist/nam-a2/nam-a2.so
chmod +x dist/nam-a2/nam-a2.so

# Always create models/ and cabs/ in the tarball so the Module Store install
# lands the expected directory layout. Existing files are preserved on
# extract; tar only overwrites same-named files (these dirs are empty by
# default - no models/cabs are bundled).
mkdir -p dist/nam-a2/models dist/nam-a2/cabs

# find, not a glob: src/models and src/cabs normally hold only a .gitkeep
# placeholder (dotfile) so any bundled real models/cabs stay opt-in, and a
# bare `src/models/*` glob doesn't match dotfiles - it matches nothing, so
# on a .gitkeep-only tree the literal unexpanded pattern was passed to cp
# and it failed with "cannot stat 'src/models/*'".
if [ -d "src/models" ]; then
    find src/models -maxdepth 1 -type f ! -name '.gitkeep' -exec cp {} dist/nam-a2/models/ \;
fi
if [ -d "src/cabs" ]; then
    find src/cabs -maxdepth 1 -type f ! -name '.gitkeep' -exec cp {} dist/nam-a2/cabs/ \;
fi

# Create tarball for release
cd dist
tar -czvf nam-a2-module.tar.gz nam-a2/
cd ..

echo ""
echo "=== Build Complete ==="
echo "Output: dist/nam-a2/"
echo "Tarball: dist/nam-a2-module.tar.gz"
echo ""
echo "To install on Move:"
echo "  ./scripts/install.sh"
