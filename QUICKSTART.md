# Quick Start

## Prerequisites

1. Install [MSYS2](https://www.msys2.org/) 
2. Open **MSYS2 MinGW64** terminal
3. Install build tools:
   ```bash
   pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-sfml
   ```
4. **(Strongly Recommended)** Install [exiftool](https://exiftool.org/install.html) for faster EXIF metadata extraction:
   ```bash
   pacman -S mingw-w64-x86_64-exiftool
   ```
   Or download from https://exiftool.org/install.html (by Phil Harvey)

## Build

**Option 1: Use build script (Easiest)**

```bash
# Navigate to project
cd /c/work/mgvwr

# Build standalone executable
./build.sh --standalone

# Test
./build_windows/mgvwr.exe --self-check  # that's included in build.sh
```

**Option 2: Manual CMake (More control)**

```bash
# Navigate to project
cd /c/work/mgvwr

# Configure (first time only, or after CMakeLists.txt changes)
cmake -B build_windows -G Ninja -DBUILD_STANDALONE=ON

# Build
cd build_windows
ninja

# Test
./mgvwr.exe --self-check
```

The build script (`build.sh`) automatically detects Ninja/Make and cleans the build directory.

## Run

```bash
# From build directory
./mgvwr.exe

# Or with specific image
./mgvwr.exe /c/photos/image.jpg

# Or with custom config
./mgvwr.exe --config /c/path/to/config.yaml
```

## Quick Configuration

Edit `mgvwr.yaml` before building:

```yaml
watched_folders:
  - "C:\\Users\\YourName\\Pictures"  # Your photo folder

filters:
  - key: "1"                          # Press '1' to filter
    expression: "Keywords % 'vacation'"
```

## Incremental Builds

After code changes:

```bash
cd build_windows
ninja
```

CMake automatically re-runs if needed. Builds are fast (~5-10 seconds for small changes).

## Troubleshooting

**"Cannot find SFML"**
```bash
pacman -S mingw-w64-x86_64-sfml
```

**"ninja: command not found"**
```bash
pacman -S mingw-w64-x86_64-ninja
```

**IntelliSense errors in VS Code**
- Already configured in `.vscode/c_cpp_properties.json`
- Reload window: Ctrl+Shift+P → "Developer: Reload Window"

**Build errors after git pull**
```bash
cd build_windows
rm -rf *
cmake .. -G Ninja -DBUILD_STANDALONE=ON
ninja
```
