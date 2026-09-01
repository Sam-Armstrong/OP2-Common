cmake_minimum_required(VERSION 3.20)
project(op2-flang-scan CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# We deliberately do NOT call find_package(Flang) here: the packaged
# FlangConfig.cmake unconditionally imports clangBasic / clangDriver /
# clangOptions, so it fails when LLVM was built without "clang" in
# LLVM_ENABLE_PROJECTS (a full clang build is ~20 minutes of work we do not
# need for Stage 1 parsing). Instead, we pick up the LLVM config package for
# header paths and LLVMSupport, and link the Flang static parser libraries
# manually. MLIR is not required: the scanner only uses the Flang parser and
# LLVMSupport (no lowering).
#
# Point CMake at the LLVM install with one of:
#   -DCMAKE_PREFIX_PATH=$HOME/.local/llvm
#   -DLLVM_DIR=$HOME/.local/llvm/lib/cmake/llvm

# LLVMExports.cmake may require zstd::libzstd_shared even though we only link
# the Flang parser. Distro LLVM packages often ship libzstd1 without the
# zstd CMake package (libzstd-dev), which then breaks generate.
if(NOT TARGET zstd::libzstd_shared)
    find_library(OP2_ZSTD_LIBRARY NAMES zstd)
    if(NOT OP2_ZSTD_LIBRARY)
        foreach(_zstd_cand
                /usr/lib/x86_64-linux-gnu/libzstd.so
                /usr/lib/x86_64-linux-gnu/libzstd.so.1
                /usr/lib64/libzstd.so
                /usr/lib64/libzstd.so.1
                /usr/lib/libzstd.so
                /usr/lib/libzstd.so.1)
            if(EXISTS "${_zstd_cand}")
                set(OP2_ZSTD_LIBRARY "${_zstd_cand}")
                break()
            endif()
        endforeach()
    endif()
    if(OP2_ZSTD_LIBRARY)
        add_library(zstd::libzstd_shared SHARED IMPORTED)
        set_target_properties(zstd::libzstd_shared PROPERTIES
            IMPORTED_LOCATION "${OP2_ZSTD_LIBRARY}")
        message(STATUS "Providing zstd::libzstd_shared at ${OP2_ZSTD_LIBRARY}")
    endif()
endif()

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

# LLVM / Flang land in the same install prefix under standard LLVM
# conventions. Resolve FLANG_INCLUDE_DIRS and FLANG_LIBRARY_DIR from the
# LLVM install prefix so we do not have to duplicate the path.
get_filename_component(LLVM_INSTALL_PREFIX "${LLVM_INCLUDE_DIRS}" DIRECTORY)
set(FLANG_INCLUDE_DIRS "${LLVM_INSTALL_PREFIX}/include")
set(FLANG_LIBRARY_DIR  "${LLVM_INSTALL_PREFIX}/lib")

# Sanity-check that Flang headers are actually installed beside LLVM.
if(NOT EXISTS "${FLANG_INCLUDE_DIRS}/flang/Parser/parsing.h")
    message(FATAL_ERROR
        "Flang parser headers not found at ${FLANG_INCLUDE_DIRS}/flang/Parser. "
        "Did you build LLVM with LLVM_ENABLE_PROJECTS including \"flang;mlir\"?")
endif()

include_directories(SYSTEM
    ${LLVM_INCLUDE_DIRS}
    ${FLANG_INCLUDE_DIRS}
)

link_directories(${FLANG_LIBRARY_DIR})

add_definitions(${LLVM_DEFINITIONS})

# Flang parser headers (flang/Common/uint128.h) require an endianness macro.
# Flang's own build injects this; installed headers do not.
if(CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    add_compile_definitions(FLANG_BIG_ENDIAN=1)
else()
    add_compile_definitions(FLANG_LITTLE_ENDIAN=1)
endif()

add_executable(op2-flang-scan op2-flang-scan.cpp)

# Stage 1 only needs the Flang parser (no semantics, no lowering, no MLIR
# backend). The set of static libs the parser pulls in has been reshuffled
# several times across LLVM releases (FortranCommon, FortranSupport,
# FortranEvaluate, FortranDecimal have all come and gone / merged). Rather
# than hard-coding a specific release's set, discover the Flang libs that
# actually exist in this install and link them all.
#
# FortranParser is the only mandatory name; everything else is opt-in.
set(OP2_FLANG_LIBS FortranParser)
foreach(lib IN ITEMS FortranCommon FortranSupport FortranEvaluate FortranDecimal)
    if(EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.a"
       OR EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.so")
        list(APPEND OP2_FLANG_LIBS ${lib})
    endif()
endforeach()
message(STATUS "Linking Flang libraries: ${OP2_FLANG_LIBS}")

target_link_libraries(op2-flang-scan PRIVATE ${OP2_FLANG_LIBS})

# LLVMSupport is required by Flang's parser for llvm::raw_ostream, error
# handling, etc. Using the alias target here lets CMake drag in the right
# transitive flags (threading, terminfo, zlib, etc.) from LLVMConfig.cmake.
target_link_libraries(op2-flang-scan PRIVATE LLVMSupport)

# FortranParser contains the OpenMP / OpenACC directive parsers, which call
# out to llvm::omp::* and llvm::acc::* helpers that live in LLVM's frontend
# directive libraries. These dependencies are not tracked by the Flang
# libraries' own link interface, so we add them manually. Discover the ones
# actually present in this LLVM install so the list survives future renames.
set(OP2_LLVM_FRONTEND_LIBS)
foreach(lib IN ITEMS LLVMFrontendOpenMP LLVMFrontendOpenACC LLVMFrontendDirective LLVMFrontendHLSL LLVMTargetParser)
    if(EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.a"
       OR EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.so")
        list(APPEND OP2_LLVM_FRONTEND_LIBS ${lib})
    endif()
endforeach()
message(STATUS "Linking LLVM frontend helper libraries: ${OP2_LLVM_FRONTEND_LIBS}")
target_link_libraries(op2-flang-scan PRIVATE ${OP2_LLVM_FRONTEND_LIBS})

install(TARGETS op2-flang-scan RUNTIME DESTINATION bin)
