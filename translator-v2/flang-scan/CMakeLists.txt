cmake_minimum_required(VERSION 3.20)
project(op2-flang-scan CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(LLVM REQUIRED CONFIG)

find_package(MLIR REQUIRED CONFIG)

message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
message(STATUS "Using MLIR from ${MLIR_DIR}")

get_filename_component(LLVM_INSTALL_PREFIX "${LLVM_INCLUDE_DIRS}" DIRECTORY)
set(FLANG_INCLUDE_DIRS "${LLVM_INSTALL_PREFIX}/include")
set(FLANG_LIBRARY_DIR  "${LLVM_INSTALL_PREFIX}/lib")

if(NOT EXISTS "${FLANG_INCLUDE_DIRS}/flang/Parser/parsing.h")
    message(FATAL_ERROR
        "Flang parser headers not found at ${FLANG_INCLUDE_DIRS}/flang/Parser. "
        "Did you build LLVM with LLVM_ENABLE_PROJECTS including \"flang;mlir\"?")
endif()

include_directories(SYSTEM
    ${LLVM_INCLUDE_DIRS}
    ${MLIR_INCLUDE_DIRS}
    ${FLANG_INCLUDE_DIRS}
)

link_directories(${FLANG_LIBRARY_DIR})

add_definitions(${LLVM_DEFINITIONS})

add_executable(op2-flang-scan op2-flang-scan.cpp)

set(OP2_FLANG_LIBS FortranParser)
foreach(lib IN ITEMS FortranCommon FortranSupport FortranEvaluate FortranDecimal)
    if(EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.a"
       OR EXISTS "${FLANG_LIBRARY_DIR}/lib${lib}.so")
        list(APPEND OP2_FLANG_LIBS ${lib})
    endif()
endforeach()
message(STATUS "Linking Flang libraries: ${OP2_FLANG_LIBS}")

target_link_libraries(op2-flang-scan PRIVATE ${OP2_FLANG_LIBS})

target_link_libraries(op2-flang-scan PRIVATE LLVMSupport)

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
