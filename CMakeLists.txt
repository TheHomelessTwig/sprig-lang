cmake_minimum_required(VERSION 3.19...3.26) # 3.19 required for modern Apple Silicon universal features

# Define the project and enable C++ support
project(SPRIG-LANG
    VERSION 1.0.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# 1. GLOBAL SETTINGS & OPTIMIZATIONS
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force out-of-source builds to prevent cluttering the repository
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "In-source builds are forbidden! Please create a build directory and run cmake from there.")
endif()

# ---------------------------------------------------------------------------
# 2. PLATFORM-SPECIFIC & UNIVERSAL CONFIGURATIONS
# ---------------------------------------------------------------------------

# macOS Apple Silicon + Intel Universal Binary support
if(APPLE)
    # Allows a single build pass to output code for both x86_64 and arm64 architectures
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for macOS")
    message(STATUS "macOS Build Target Architectures: ${CMAKE_OSX_ARCHITECTURES}")
endif()

# ---------------------------------------------------------------------------
# 3. SOURCE FILE DISCOVERY (Modern Target-Based Approach)
# ---------------------------------------------------------------------------
# Automatically finds all source and header files inside src/
file(GLOB_RECURSE SOURCE_FILES
    "src/*.cpp"
    "src/*.cc"
    "src/*.c"
    "include/*.h"
    "include/*.hpp"
)

# Generate version.hpp from the project VERSION declared above
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/version.hpp"
    @ONLY
)

# Define the build executable target
add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# Include directories: source headers + generated version.hpp
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# ---------------------------------------------------------------------------
# 4. COMPILER FLAGS & WARNING HANDLING
# ---------------------------------------------------------------------------
if(MSVC)
    # Windows specific aggressive warnings and optimizations
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX /wd4005)
else()
    # Linux and macOS (GCC / Clang) flags
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# 5. EXTERNAL LIBRARIES (Optional Example)
# ---------------------------------------------------------------------------
# find_package(Threads REQUIRED)
# target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# 6. INSTALLATION RULES
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBSUBDIR}
)
