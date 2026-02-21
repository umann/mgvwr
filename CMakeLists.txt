cmake_minimum_required(VERSION 3.10)
project(MgVwr)

# Allow older cmake_minimum_required values in dependencies like yaml-cpp
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add nlohmann/json header-only library
# Download the single header file if not present
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/src/json.hpp")
    message(STATUS "Downloading nlohmann/json...")
    file(DOWNLOAD 
        "https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp"
        "${CMAKE_SOURCE_DIR}/src/json.hpp"
        SHOW_PROGRESS
    )
endif()

# Fetch yaml-cpp for YAML configuration parsing
include(FetchContent)
FetchContent_Declare(
    yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

# Work around missing <cstdint> include in yaml-cpp on some toolchains
if (TARGET yaml-cpp)
    target_compile_options(yaml-cpp PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/FIcstdint>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-include> $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:cstdint>
    )
endif()

# Fetch inja for template rendering
FetchContent_Declare(
    inja
    URL https://github.com/pantor/inja/archive/refs/tags/v3.4.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(inja)

# Option for standalone build
option(BUILD_STANDALONE "Build fully static standalone executable" OFF)

# For Windows MinGW build - static linking
if(MINGW)
    if(BUILD_STANDALONE)
        message(STATUS "Standalone build requested (static linking flags enabled)")
        
        # Force static linking of all libraries
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        set(BUILD_SHARED_LIBS OFF)
        
        # Add compiler and linker flags for full static linking
        add_compile_options(-DSFML_STATIC)
        add_link_options(-static -static-libgcc -static-libstdc++)
        
        # Ensure SFML uses static libraries
        set(SFML_STATIC_LIBRARIES TRUE)
    else()
        message(STATUS "Building with dynamic linking")
        # Don't add any static flags for dynamic build - let DLLs be used
    endif()
endif()

# Find SFML
set(SFML_STATIC_LIBRARIES TRUE)
find_package(SFML 3.0 COMPONENTS Graphics Window System Network REQUIRED)

# Add executable
add_executable(mgvwr src/main.cpp src/map_viewer.cpp src/config.cpp src/poor_mans_exiftool.cpp src/utils.cpp src/help.cpp)

# Link libraries
target_link_libraries(mgvwr PRIVATE SFML::Graphics SFML::Window SFML::System SFML::Network yaml-cpp pantor::inja)

# Add include directory for json.hpp
target_include_directories(mgvwr PRIVATE "${CMAKE_SOURCE_DIR}/src")

# For standalone builds, add all static dependencies AFTER SFML libraries
# This ensures proper linking order for static libraries
if(BUILD_STANDALONE AND MINGW)
    # Freetype is already linked by SFML, but we need to add its dependencies
    # Libraries must be in reverse dependency order: dependents before dependencies
    target_link_libraries(mgvwr PRIVATE
        Freetype::Freetype
        harfbuzz
        png16
        brotlidec
        brotlicommon
        graphite2
        glib-2.0
        pcre2-8
        bz2
        z
        intl
        iconv
        rpcrt4   # For UuidCreate (harfbuzz dependency)
        dwrite   # For DirectWrite (harfbuzz dependency)
    )
endif()

# Copy mgvwr.yaml to build directory
add_custom_command(TARGET mgvwr POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/mgvwr.yaml
        $<TARGET_FILE_DIR:mgvwr>/mgvwr.yaml
    COMMENT "Copying mgvwr.yaml to output directory"
)
# Run self-check after build (check source config, not copied one)
add_custom_command(TARGET mgvwr POST_BUILD
    COMMAND "$<TARGET_FILE:mgvwr>" --self-check --config "${CMAKE_SOURCE_DIR}/mgvwr.yaml"
    COMMENT "Running self-check on built executable..."
)