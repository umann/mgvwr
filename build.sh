#!/bin/bash
# Build script for Windows using MinGW cross-compiler
set -euo pipefail

# Parse arguments
STANDALONE=false
for arg in "$@"; do
    case $arg in
        -s|--standalone)
            STANDALONE=true
            shift
            ;;
    esac
done

# Configuration
BUILD_DIR="build_windows"
BUILD_DIR_ABS="$PWD/$BUILD_DIR"
INSTALL_PREFIX="$PWD/install_windows"

# Create build directory
# Always clean the build directory to avoid stale cache when switching modes
if [ -d "$BUILD_DIR" ]; then
    # Remove directory, handling Windows reserved names (like 'nul')
    rm -rf "$BUILD_DIR" 2>/dev/null || true
    if [ -d "$BUILD_DIR" ] && command -v powershell >/dev/null 2>&1; then
        powershell -Command "Remove-Item '$BUILD_DIR' -Recurse -Force -ErrorAction SilentlyContinue" || true
    fi
fi
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Select build tool
GENERATOR=""
BUILD_CMD=()

if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
    BUILD_CMD=(ninja)
elif command -v mingw32-make >/dev/null 2>&1; then
    GENERATOR="MinGW Makefiles"
    BUILD_CMD=(mingw32-make)
elif command -v make >/dev/null 2>&1; then
    GENERATOR="Unix Makefiles"
    BUILD_CMD=(make)
else
    echo "No build tool found. Install Ninja or MinGW Make (mingw32-make)." >&2
    exit 1
fi

# Configure with CMake for MinGW
CMAKE_ARGS=(
    -G "$GENERATOR"
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres
    -DCMAKE_SYSTEM_NAME=Windows
    -DCMAKE_SYSTEM_PROCESSOR=x86_64
    -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
)

if [ "$STANDALONE" = true ]; then
    echo "Configuring standalone build (static linking requested)..."
    CMAKE_ARGS+=(-DBUILD_STANDALONE=ON)
else
    echo "Building with dynamic libraries..."
fi

cmake .. "${CMAKE_ARGS[@]}"

# Build
"${BUILD_CMD[@]}" -j"$(nproc)"

# Copy runtime DLLs next to executable (MinGW/MSYS2) - only if not standalone
if [ "$STANDALONE" = false ] && [[ -d /mingw64/bin ]]; then
    RUNTIME_DLLS=(
        libfreetype-6.dll
        libbrotlidec.dll
        libbrotlicommon.dll
        libbz2-1.dll
        zlib1.dll
        libpng16-16.dll
        libharfbuzz-0.dll
        libgcc_s_seh-1.dll
        libstdc++-6.dll
        libwinpthread-1.dll
        libgraphite2.dll
        libglib-2.0-0.dll
        libintl-8.dll
        libiconv-2.dll
        libpcre2-8-0.dll
    )

    for dll in "${RUNTIME_DLLS[@]}"; do
        if [[ -f "/mingw64/bin/$dll" ]]; then
            cp -u "/mingw64/bin/$dll" "$BUILD_DIR_ABS/"
        fi
    done
else
    # If standalone, remove any existing DLLs from previous builds
    if [ "$STANDALONE" = true ]; then
        rm -f "$BUILD_DIR_ABS"/*.dll
    fi
fi

echo ""
echo "Build complete!"
echo \"Executable: $BUILD_DIR/mgvwr.exe\"
if [ "$STANDALONE" = true ]; then
    echo "Mode: Standalone (fully static)"
else
    echo "Mode: Dynamic linking (requires DLLs)"
fi
