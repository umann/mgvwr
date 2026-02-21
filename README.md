# MgVwr – Image Viewer with Map

A feature-rich image viewer for Windows with EXIF metadata support, GPS mapping, and keyword filtering.

It’s written 98% by Copilot; the rest is me refactoring and normalizing the code.

I don’t speak C++. The last time I programmed in C was in 1992. I used Perl a lot, and now Python. But Copilot 
suggested I would be better off with C++ and a compiled `.exe` for this project.

## Features

- **Metadata-aware**: Reads EXIF data including GPS coordinates, dates, keywords, and place names  
- **Interactive mapping**: View the current photo’s location and all other photos in the same folder inline on OpenStreetMap  
- **Smart filtering**: Filter images by metadata keywords using configurable filters  
- **Folder navigation**: Navigate through watched folders seamlessly with intelligent sorting  
- **Fullscreen/windowed modes**: Toggle between modes with proper state preservation  
- **Configuration-driven**: YAML-based configuration with schema validation  
- **Supported formats**: JPG, PNG, BMP, GIF, TIFF (metadata currently supported for JPG only)

## Keyboard Controls

- `Right/Left`, `MouseWheel` – Next/previous image in folder, continuing into next/previous folder  
- `PageDown/PageUp` – Next/previous folder  
- `Home/End` – First/last image in folder  
- `.` – Toggle map view  
- `Enter` – Toggle fullscreen/windowed mode  
- `F1` – Toggle help overlay  
- `Ctrl + MouseWheel` – Zoom map  
- `Esc` – Close help / exit app  
- Configurable keys to toggle keyword filters

## Building

### Prerequisites (MSYS2/Windows)

1. Install [MSYS2](https://www.msys2.org/)
2. Install dependencies:
   ```bash
   pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-sfml
   ```

### Quick Build (Recommended)

```bash
# From MSYS2 MinGW64 terminal - builds standalone executable
./build.sh --standalone

# Or for development (with DLL dependencies)
./build.sh

# Run with self-check
./build_windows/mgvwr.exe --self-check
```

The `build.sh` script automatically:
- Detects and uses Ninja or Make
- Cleans build directory for fresh builds
- Copies runtime DLLs (non-standalone mode)
- Configures cross-compilation if needed

### Manual Build

For more control or Windows cmd.exe:

```bash
# Configure (first time, or after CMakeLists.txt changes)
cmake -B build_windows -G Ninja -DBUILD_STANDALONE=ON

# Build
cd build_windows
ninja
```

Or use `build.bat` from Windows Command Prompt.

## Configuration

Edit `mgvwr.yaml` to configure:
- Watched folders for image scanning
- Map providers and zoom levels
- Keyword filters
- Font paths and sizes
- Window dimensions
- Cache settings

## Usage

```bash
# Start with first watched folder
mgvwr.exe

# Start with specific image
mgvwr.exe C:\photos\image.jpg

# Start with specific config
mgvwr.exe --config custom.yaml

# Run self-check
mgvwr.exe --self-check
```

## File Structure

```
mgvwr/
├── src/
│   ├── main.cpp                  # Main application
│   ├── config.cpp/h              # YAML config & schema validation
│   ├── map_viewer.cpp/h          # Map integration
│   ├── help.cpp/h                # Help overlay with Inja templates
│   ├── poor_mans_exiftool.cpp/h  # EXIF metadata parsing
│   ├── utils.cpp/h               # Utility functions
│   └── json.hpp                  # nlohmann/json (auto-downloaded)
├── mgvwr.yaml                    # Main configuration file
├── CMakeLists.txt                # Build configuration (C++20)
├── build.sh                      # Unix/MSYS2 build script (recommended)
├── build.bat                     # Windows cmd build script
└── README.md                     # This file
```

## Dependencies

Automatically fetched by CMake:
- **SFML 3.0+** - Graphics/windowing (system package)
- **yaml-cpp 0.8.0** - YAML parsing (FetchContent)
- **inja 3.4.0** - Template rendering (FetchContent)
- **nlohmann/json 3.11.2** - JSON and JSON Schema handling (downloaded header)

### Optional but Strongly Recommended

- **[exiftool](https://exiftool.org/)** - Fast batch EXIF metadata extraction (by Phil Harvey)
  
  While MgVwr includes a fallback metadata parser, exiftool provides much faster and more comprehensive EXIF data extraction. Install it for best performance:
  
  ```bash
  # Windows via Chocolatey
  choco install exiftool
  
  # Or download from https://exiftool.org/install.html
  ```

## Requirements

- C++20 compiler (GCC 10+ / MSVC 2019+ / Clang 10+)
- CMake 3.10+
- Windows 7+ (for runtime)
