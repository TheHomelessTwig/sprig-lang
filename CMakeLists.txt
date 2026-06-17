cmake_minimum_required(VERSION 3.19...3.26) # 3.19 required for modern Apple Silicon universal features

# Define the project and enable C++ support
project(SPRIG-LANG
    VERSION 0.4.0
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

# Exclude lsp.cpp from the main binary (it has its own entry point)
list(FILTER SOURCE_FILES EXCLUDE REGEX ".*/lsp\\.cpp$")

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
# 5. EXTERNAL LIBRARIES
# ---------------------------------------------------------------------------
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

# SYSTEM suppresses warnings from LLVM headers (their own headers trigger -Wunused-parameter)
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS
    core support irreader
    x86codegen x86asmparser
)

target_link_libraries(${PROJECT_NAME} PRIVATE ${LLVM_LIBS})

# ---------------------------------------------------------------------------
# 6. INSTALLATION RULES
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBSUBDIR}
)

# ---------------------------------------------------------------------------
# 7. SPRIG-LSP (Language Server Protocol server)
# ---------------------------------------------------------------------------
# Uses only the pipeline through borrow checker — no LLVM dependency.

set(LSP_SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/typechecker.cpp
    src/borrowchecker.cpp
    src/lsp.cpp
)

find_package(nlohmann_json QUIET)

add_executable(sprig-lsp ${LSP_SOURCES})
target_include_directories(sprig-lsp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)
target_compile_options(sprig-lsp PRIVATE -Wall -Wextra -Wpedantic -Werror -std=c++17)

if(nlohmann_json_FOUND)
    target_link_libraries(sprig-lsp PRIVATE nlohmann_json::nlohmann_json)
else()
    message(STATUS "nlohmann_json CMake target not found — using system header")
endif()

install(TARGETS sprig-lsp DESTINATION ${CMAKE_INSTALL_BINDIR})
