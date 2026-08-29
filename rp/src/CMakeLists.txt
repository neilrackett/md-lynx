# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# if PICOTOOL_OVERRIDE_DIR system environment variable is set,
# then use that as the folder for the picotool executable
if (DEFINED ENV{PICOTOOL_OVERRIDE_DIR})
    message("PICOTOOL_OVERRIDE_DIR env var is set to '$ENV{PICOTOOL_OVERRIDE_DIR}'")
    add_executable(picotool IMPORTED GLOBAL)
    set_property(TARGET picotool PROPERTY IMPORTED_LOCATION $ENV{PICOTOOL_OVERRIDE_DIR}/picotool)
    # check the picotool version:
    execute_process(COMMAND $ENV{PICOTOOL_OVERRIDE_DIR}/picotool version
                    OUTPUT_VARIABLE PICOTOOL_VERSION
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX MATCH "^picotool v${picotool_VERSION_REQUIRED}" PICOTOOL_VERSION_MATCH ${PICOTOOL_VERSION})
    if (NOT PICOTOOL_VERSION_MATCH)
        message("picotool version response was: ${PICOTOOL_VERSION}")
        message(FATAL_ERROR "PICOTOOL_OVERRIDE_DIR is set to '$ENV{PICOTOOL_OVERRIDE_DIR}', but the version of picotool found is not ${picotool_VERSION_REQUIRED}")
    else()
        message("picotool version response was: ${PICOTOOL_VERSION}")
    endif()
endif ()

# If not specified, use pico_w as default board type
set(BOARD_TYPE $ENV{BOARD_TYPE})
if(NOT BOARD_TYPE)
    set(BOARD_TYPE "pico_w")
endif()
set(PICO_BOARD ${BOARD_TYPE})

# Determine board type macros
if("${BOARD_TYPE}" STREQUAL "pico_w")
    add_compile_definitions(BOARD_TYPE_PICO_W=1 BOARD_TYPE_PICO=0 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "pico")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
elseif("${BOARD_TYPE}" STREQUAL "sidecartos_16mb")
    add_compile_definitions(BOARD_TYPE_PICO_W=0 BOARD_TYPE_PICO=1 BOARD_TYPE_CUSTOM16MB=0)
else()
    message(FATAL_ERROR "Unknown BOARD_TYPE: ${BOARD_TYPE}")
endif()

# Resolve bundled Pico dependencies relative to the repo root so configure
# works regardless of the active build directory.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." REALPATH)
set(DEFAULT_FATFS_SDK_PATH "${REPO_ROOT}/fatfs-sdk")
set(DEFAULT_PICO_SDK_PATH "${REPO_ROOT}/pico-sdk")
set(DEFAULT_PICO_EXTRAS_PATH "${REPO_ROOT}/pico-extras")

if(NOT DEFINED ENV{FATFS_SDK_PATH} OR "$ENV{FATFS_SDK_PATH}" STREQUAL "")
    set(ENV{FATFS_SDK_PATH} "${DEFAULT_FATFS_SDK_PATH}")
endif()
set(FATFS_SDK_PATH "$ENV{FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
get_filename_component(FATFS_SDK_PATH "${FATFS_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FATFS_SDK_PATH "${FATFS_SDK_PATH}" CACHE PATH "Path to the FATFS SDK" FORCE)
set(ENV{FATFS_SDK_PATH} "${FATFS_SDK_PATH}")
message("FATFS_SDK_PATH: ${FATFS_SDK_PATH}")

if(NOT DEFINED ENV{PICO_SDK_PATH} OR "$ENV{PICO_SDK_PATH}" STREQUAL "")
    set(ENV{PICO_SDK_PATH} "${DEFAULT_PICO_SDK_PATH}")
endif()
set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
set(ENV{PICO_SDK_PATH} "${PICO_SDK_PATH}")
message("PICO_SDK_PATH: ${PICO_SDK_PATH}")

if(NOT DEFINED ENV{PICO_EXTRAS_PATH} OR "$ENV{PICO_EXTRAS_PATH}" STREQUAL "")
    set(ENV{PICO_EXTRAS_PATH} "${DEFAULT_PICO_EXTRAS_PATH}")
endif()
set(PICO_EXTRAS_PATH "$ENV{PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
get_filename_component(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" REALPATH BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(PICO_EXTRAS_PATH "${PICO_EXTRAS_PATH}" CACHE PATH "Path to the PICO EXTRAS" FORCE)
set(ENV{PICO_EXTRAS_PATH} "${PICO_EXTRAS_PATH}")
message("PICO_EXTRAS_PATH: ${PICO_EXTRAS_PATH}")

# FatFs (exFAT + heap LFN) is written for malloc-returns-NULL semantics:
# f_mkdir's dir_clear() requests up to 32 KB then halves on each NULL
# until it fits. pico-sdk's default PICO_MALLOC_PANIC=1 turns that first
# over-ceiling request into a hard "Out of memory" panic before the
# fallback runs. The Lynx core fills most of RAM (its 64 KB system RAM is
# heap-allocated), so disable the panic (heap is still capped at
# __StackLimit by _sbrk, so the cart region stays protected) and let
# FatFs degrade gracefully. Declared before the subdirectories so every
# target (incl. settings / FatFs) is built consistently.
add_definitions(-DPICO_MALLOC_PANIC=0)

# Newlib's full malloc grows the heap in page-rounded (4 KB) sbrk steps,
# so with the default hard-fail _sbrk a request can die on the rounding
# excursion even when the actual bytes fit (~3 KB of the window is
# unreachable). Optimistic sbrk clamps an over-limit grant to
# __StackLimit instead of failing, making the whole window usable; the
# cap itself is unchanged, so the cart region stays protected.
add_definitions(-DPICO_USE_OPTIMISTIC_SBRK=1)

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake)

# Set name of project (as PROJECT_NAME) and C/C   standards
project(rp C CXX)
set(CMAKE_C_STANDARD 11)
# The Handy emulation core under lynx/ is C++; it neither throws nor uses
# RTTI, and both cost flash we don't have to spare.
set(CMAKE_CXX_STANDARD 17)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
                    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
                    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
# Creates a pico-sdk subdirectory in our project for the libraries
pico_sdk_init()

add_subdirectory(settings)

# Tell CMake where to find other source code
add_subdirectory(${FATFS_SDK_PATH}/src build)
  
# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
        main.c
)

pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/romemul.pio)
pico_generate_pio_header(${PROJECT_NAME} ${CMAKE_CURRENT_LIST_DIR}/commemul.pio)

target_sources(${PROJECT_NAME} PRIVATE
        aconfig.c
        audio.c
        commemul.c
        emul.c
        fb.c
        fb_blit.c
        fb_blit_asm.S
        fb_chunked.c
        fb_chunked_asm.S
        fb_font.c
        gconfig.c
        ikbd.c
        hw_config.c
        palette.c
        lynxemu.cpp
        lynx/cart.cpp
        lynx/eeprom.cpp
        lynx/lynxdec.cpp
        lynx/mikie.cpp
        lynx/susie.cpp
        lynx/system.cpp
        reset.c
        romemul.c
        sdcard.c
        select.c
        xpadin.c
        settings/settings.c)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# 400 MHz sysclk (constants.h) needs the QSPI flash divider raised from
# the board default /2 to /4: the SSI divisor must be even, and /2 would
# put the flash SCK at 200 MHz, far over the ~133 MHz part limit (/4 =
# 100 MHz). The divider is baked into boot_stage2 (the SDK's bs2_default
# target), so set it on that target directly -- this fails loudly at
# configure time if the SDK ever renames it, unlike a directory-property
# injection which would silently no-op. The add_definitions covers the
# app side (runtime prints / SDK uses).
add_definitions(-DPICO_FLASH_SPI_CLKDIV=4)
target_compile_definitions(bs2_default PRIVATE PICO_FLASH_SPI_CLKDIV=4)

# Link libraries required for the project
target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LINK_LIBRARIES}        # External or additional libraries passed as variables
    hardware_flash           # Flash memory access
    hardware_interp          # SIO interpolators (cojorotozoom texel addressing)
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico                # FATFS library
    pico_stdlib              # Core functionality
    pico_multicore          # Multicore support
    settings                 # Custom settings library
)


# Link custom memmap with reserved memory for ROMs
set_target_properties(${PROJECT_NAME} PROPERTIES
        PICO_TARGET_LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/memmap_rp.ld
)

# Make our project-local FatFs config (rp/src/ff/ffconf.h) win over the
# fatfs-sdk submodule's default copy. fatfs sources do `#include "ffconf.h"`
# which is resolved against the include path; placing this BEFORE PRIVATE
# guarantees our override is found first, so the submodule stays pristine
# and no on-the-fly sed patching is needed.
target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/ff)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/..
    # Xpad ABI, straight from the submodule rather than a vendored copy,
    # so the header the RP compiles against and the equates the m68k
    # assembles against are the same pinned revision.
    ${REPO_ROOT}/lib/xpad/src)


# Use reduced space for ROMs
pico_set_binary_type(${PROJECT_NAME} rp)

# Fetch the values from the environment variables
set(RELEASE_TYPE $ENV{RELEASE_TYPE})
set(RELEASE_VERSION $ENV{RELEASE_VERSION})
set(RELEASE_DATE $ENV{RELEASE_DATE})
set(_DEBUG $ENV{DEBUG_MODE})

# If the environment variables are not set, use default values
if(NOT RELEASE_VERSION)

    # Check if RELEASE_TYPE is set and not empty, otherwise use default "FINAL"
    if(NOT RELEASE_TYPE OR RELEASE_TYPE STREQUAL "")
        set(RELEASE_TYPE "final")
    endif()

    # Determine the file name based on RELEASE_TYPE
    if(RELEASE_TYPE STREQUAL "final")
        set(VERSION_FILE_NAME "version.txt")
    else()
        set(VERSION_FILE_NAME "version-${RELEASE_TYPE}.txt")
    endif()
    
    # Remove newline character if present
    string(STRIP "${RELEASE_VERSION}" RELEASE_VERSION)

    # In case version.txt is empty or does not exist, use a default version
    if("${RELEASE_VERSION}" STREQUAL "")
        set(RELEASE_VERSION "v0.0.1dev")
    endif()
endif()

if(NOT RELEASE_DATE)
        string(TIMESTAMP CURRENT_DATE_TIME "%Y-%m-%d %H:%M:%S")
        set(RELEASE_DATE ${CURRENT_DATE_TIME})
endif()

# Default _DEBUG to 0 when the env var isn't set at all. `if (NOT _DEBUG)`
# would also fire for the string "0" (CMake treats "0" as falsy), silently
# turning a CMakePresets release preset into a debug build. Mirrors the
# pattern used in rp2-atarist-rpikb's CMakeLists.txt.
if ("${_DEBUG}" STREQUAL "")
        set(_DEBUG 0)
endif()

# Debug outputs
pico_enable_stdio_usb(${PROJECT_NAME} 0)
# Workaround to disable USB output in release builds
if(${_DEBUG} STREQUAL "0")
    pico_enable_stdio_uart(${PROJECT_NAME} 0)
else()
    pico_enable_stdio_uart(${PROJECT_NAME} 1)
endif()

# Print the variables on screen
message("RELEASE_TYPE: " ${RELEASE_TYPE})
message("RELEASE_VERSION: " ${RELEASE_VERSION})
message("RELEASE_DATE: " ${RELEASE_DATE})
message("DEBUG_MODE: " ${_DEBUG})

# Pass these values to the C compiler
add_definitions(-DRELEASE_TYPE="${RELEASE_TYPE}")
add_definitions(-DRELEASE_VERSION="${RELEASE_VERSION}")
add_definitions(-DRELEASE_DATE="${RELEASE_DATE}")

# Pass the APP_UUID_KEY to the C compiler
if(DEFINED ENV{APP_UUID_KEY} AND NOT "$ENV{APP_UUID_KEY}" STREQUAL "")
    message(STATUS "Using APP_UUID_KEY from environment: $ENV{APP_UUID_KEY}")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"$ENV{APP_UUID_KEY}\")
else()
    message(STATUS "Using default UUID")
    add_definitions(-DCURRENT_APP_UUID_KEY=\"44444444-4444-4444-8444-444444444444\")
endif()

# Pass the _DEBUG flag to the C compiler
add_definitions(-D_DEBUG=${_DEBUG})

# Device/Computer type
add_definitions(-DDISPLAY_ATARIST)

# We don't modify the flash in Core 1
add_definitions(-DPICO_FLASH_ASSUME_CORE0_SAFE=1)


# Remove unused data
target_link_options(${PROJECT_NAME} PRIVATE
   "-Wl,--gc-sections"
   "-Wl,--strip-all"
)

# Enable clang-tidy (you need to have clang-tidy installed on your system)
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(CLANG_TIDY_EXE)
    # C only -- the vendored Handy core under lynx/ is upstream code we
    # deliberately keep close to its original form.
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
    message(STATUS "Found clang-tidy: ${CLANG_TIDY_EXE}")
else()
    message(WARNING "clang-tidy not found, skipping linting")
endif()

# Find clang-format executable
find_program(CLANG_FORMAT_BIN NAMES clang-format)

if(CLANG_FORMAT_BIN)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_BIN}")
    message(STATUS "Linting in the source dir: ${CMAKE_SOURCE_DIR}")

    # Add a custom target to run clang-format on all source files
    file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
        "${CMAKE_SOURCE_DIR}/*.c"
        "${CMAKE_SOURCE_DIR}/include/*.h"
    )

    add_custom_target(clang-format
        COMMAND ${CLANG_FORMAT_BIN}
        -i
        -style=file
        ${ALL_CXX_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../..
        COMMENT "Running clang-format on the source code..."
        VERBATIM
    )

else()
    message(WARNING "clang-format not found!")
endif()
