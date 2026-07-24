cmake_minimum_required(VERSION 3.15)
project(Ptacxx)

# =========================================================
# Stage 1: llvm check and build path check
# =========================================================

if(NOT DEFINED LLVM_DIR)
    message(FATAL_ERROR "LLVM_DIR is not set")
endif()

if(NOT EXISTS ${LLVM_DIR})
    message(FATAL_ERROR "LLVM_DIR does not exist: ${LLVM_DIR}")
endif()

get_filename_component(LLVM_DIR_NAME ${LLVM_DIR} NAME)

if(NOT EXISTS ${LLVM_DIR}/llvm/CMakeLists.txt AND NOT EXISTS ${LLVM_DIR}/bin/llvm-link)
    message(FATAL_ERROR "LLVM_DIR is not a valid LLVM source tree (missing llvm/CMakeLists.txt) or prebuilt (missing bin/llvm-link): ${LLVM_DIR}")
endif()

get_filename_component(CURRENT_DIR ${CMAKE_BINARY_DIR} NAME)

if(CURRENT_DIR MATCHES "^llvm([0-9]+)(\\.[0-9]+\\.[0-9]+)?$")
    set(LLVM_VERSION ${CMAKE_MATCH_1})
    if(CMAKE_MATCH_2)
        string(SUBSTRING ${CURRENT_DIR} 4 -1 LLVM_VERSION_FULL)
        message(STATUS "Building for ${CURRENT_DIR} (full version: ${LLVM_VERSION_FULL})")
    else()
        message(STATUS "Building for ${CURRENT_DIR} (major version: ${LLVM_VERSION})")
    endif()
    
    if(DEFINED LLVM_DIR)
        get_filename_component(LLVM_DIR_NAME ${LLVM_DIR} NAME)
        if(LLVM_DIR_NAME MATCHES "llvm[-_]?([0-9]+)")
            set(LLVM_DIR_VERSION ${CMAKE_MATCH_1})
            if(NOT LLVM_DIR_VERSION EQUAL LLVM_VERSION)
                message(FATAL_ERROR "LLVM_DIR version (${LLVM_DIR_VERSION}) does not match build directory version (${LLVM_VERSION})")
            endif()
        else()
            message(FATAL_ERROR "Could not detect LLVM version from LLVM_DIR: ${LLVM_DIR}")
        endif()
    endif()
    
else()
    message(FATAL_ERROR "Current directory must be named like llvm14, llvm21, got: ${CURRENT_DIR}")
endif()

# =========================================================
# Stage 2: config
# =========================================================

