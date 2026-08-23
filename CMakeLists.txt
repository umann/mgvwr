cmake_minimum_required(VERSION 3.10)
project(MgVwr)

# Allow older cmake_minimum_required values in dependencies like yaml-cpp
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Fetch nlohmann/json for JSON parsing and schema validation
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Fetch yaml-cpp for YAML configuration parsing
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

# Fetch inja for template rendering. Reuse the project nlohmann_json target
# instead of letting Inja create its own duplicate interface target.
# Keep this dependency minimal for the app build; the upstream test suite is not needed here.
set(INJA_USE_EMBEDDED_JSON OFF CACHE BOOL "Use external nlohmann_json" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable external test targets" FORCE)
set(INJA_BUILD_TESTS OFF CACHE BOOL "Disable Inja unit tests" FORCE)
set(BUILD_BENCHMARK OFF CACHE BOOL "Disable Inja benchmarks" FORCE)
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

find_path(SQLITE3_INCLUDE_DIR sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3 libsqlite3)

if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite3 development files not found")
endif()

# Add executable
set(MGVWR_SOURCES
    src/datetime_utils.cpp
    src/main.cpp
    src/cli.cpp
    src/map_viewer.cpp
    src/config.cpp
    src/schema_engine.cpp
    src/serialize.cpp
    src/thumbnail_cache.cpp
    src/precache.cpp
    src/metadata.cpp
    src/metadata_cache.cpp
    src/poor_mans_exiftool.cpp
    src/utils.cpp
    src/help.cpp
    src/html_utils.cpp
    src/encoding_utils.cpp
)

# Generate embedded schema headers from external YAML files.
set(MGVWR_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MGVWR_GENERATED_DIR}")

set(MGVWR_CONFIG_SCHEMA_FILE "${CMAKE_SOURCE_DIR}/data/json_schema/config.json_schema.yaml")
set(MGVWR_EXIF_SCHEMA_FILE "${CMAKE_SOURCE_DIR}/data/json_schema/exiftool_repsonse.json_schema.yaml")

set(MGVWR_CONFIG_SCHEMA_HEADER "${MGVWR_GENERATED_DIR}/config_schema_embedded.h")
set(MGVWR_EXIF_SCHEMA_HEADER "${MGVWR_GENERATED_DIR}/exiftool_response_schema_embedded.h")

add_custom_command(
    OUTPUT "${MGVWR_CONFIG_SCHEMA_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${MGVWR_CONFIG_SCHEMA_FILE}
        -DOUTPUT_FILE=${MGVWR_CONFIG_SCHEMA_HEADER}
        -DVAR_NAME=SCHEMA_YAML
        -P "${CMAKE_SOURCE_DIR}/cmake/embed_text_as_header.cmake"
    DEPENDS
        "${MGVWR_CONFIG_SCHEMA_FILE}"
        "${CMAKE_SOURCE_DIR}/cmake/embed_text_as_header.cmake"
    VERBATIM
)

add_custom_command(
    OUTPUT "${MGVWR_EXIF_SCHEMA_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${MGVWR_EXIF_SCHEMA_FILE}
        -DOUTPUT_FILE=${MGVWR_EXIF_SCHEMA_HEADER}
        -DVAR_NAME=EXIFTOOL_RESPONSE_SCHEMA_YAML
        -P "${CMAKE_SOURCE_DIR}/cmake/embed_text_as_header.cmake"
    DEPENDS
        "${MGVWR_EXIF_SCHEMA_FILE}"
        "${CMAKE_SOURCE_DIR}/cmake/embed_text_as_header.cmake"
    VERBATIM
)

add_custom_target(embed_schemas
    DEPENDS
        "${MGVWR_CONFIG_SCHEMA_HEADER}"
        "${MGVWR_EXIF_SCHEMA_HEADER}"
)

if(WIN32)
    if(MINGW)
        # Some MinGW installs ship only windres.exe (without the triplet-prefixed alias).
        find_program(MGVWR_WINDRES
            NAMES windres x86_64-w64-mingw32-windres
            HINTS "C:/msys64/mingw64/bin"
        )

        if(MGVWR_WINDRES)
            set(CMAKE_RC_COMPILER "${MGVWR_WINDRES}" CACHE FILEPATH "Resource compiler" FORCE)
            list(APPEND MGVWR_SOURCES src/mgvwr.rc)
        else()
            message(WARNING "No resource compiler found; mgvwr.exe icon resource will not be embedded.")
        endif()
    else()
        list(APPEND MGVWR_SOURCES src/mgvwr.rc)
    endif()
endif()

add_executable(mgvwr ${MGVWR_SOURCES})
add_dependencies(mgvwr embed_schemas)

# Link libraries
target_link_libraries(mgvwr PRIVATE SFML::Graphics SFML::Window SFML::System SFML::Network yaml-cpp pantor::inja nlohmann_json::nlohmann_json ${SQLITE3_LIBRARY})

# Add include directories for local project headers and generated schema output
target_include_directories(mgvwr PRIVATE "${CMAKE_SOURCE_DIR}/src" "${SQLITE3_INCLUDE_DIR}" "${MGVWR_GENERATED_DIR}")

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