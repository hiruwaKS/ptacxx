cmake_minimum_required(VERSION 3.15)
project(Ptacxx)

set(CMAKE_CXX_STANDARD 20)
add_compile_options(
  -Weverything
  -Werror
  -Wno-system-headers
  -Wno-c++98-compat
  -Wno-c++98-compat-extra-semi
  -Wno-padded
  -Wno-reserved-identifier
)

get_filename_component(BINARY_DIR_NAME ${CMAKE_BINARY_DIR} NAME)
message(STATUS "BINARY_DIR_NAME: ${BINARY_DIR_NAME}")

if (NOT EXISTS "${BACKEND_BINARY_DIR}" OR NOT EXISTS "${LLVM_DIR}")
  message(FATAL_ERROR "BACKEND_BINARY_DIR or LLVM_DIR not found")
endif ()

if (BINARY_DIR_NAME STREQUAL "lotus")
  find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
  if (NOT LLVM_VERSION_MAJOR EQUAL 14)
    message(FATAL_ERROR "LLVM version 14 != ${LLVM_VERSION_MAJOR}")
  endif ()
  message(STATUS "LLVM version check - ok")

  set(LOTUS_LIB_DIR ${BACKEND_BINARY_DIR}/lib)
  get_filename_component(LOTUS_INCLUDE_DIR ${BACKEND_BINARY_DIR}/../include ABSOLUTE)
  set(_LOTUS_LIBS
    CanaryAliasAnalysisWrapper SVFG CanaryICFG AserPTA CanaryDynamicAA
    CanaryAliasCLIUtils AliasSpecManager CanaryAliasPtsSet Andersen CanaryCUDD CanaryLLVMUtils Annotation
    CanaryDyckAA SeaDsaAnalysis CanaryCFG CanaryParallel CanaryPlatform
    CanaryGSA CanaryFPA FSCSPointerAnalysis TPATransforms LotusAA
  )
  foreach(lib ${_LOTUS_LIBS})
    if (NOT EXISTS "${LOTUS_LIB_DIR}/lib${lib}.a")
      message(FATAL_ERROR "Lotus library (lib${lib}.a) not found in ${LOTUS_LIB_DIR}")
    endif ()
    add_library(lotus::${lib} UNKNOWN IMPORTED)
    set_target_properties(lotus::${lib} PROPERTIES
      IMPORTED_LOCATION "${LOTUS_LIB_DIR}/lib${lib}.a"
      INTERFACE_INCLUDE_DIRECTORIES "${LOTUS_INCLUDE_DIR}"
    )
  endforeach()
  unset(_LOTUS_LIBS)
  message(STATUS "Lotus libs check in ${LOTUS_LIB_DIR} - ok")

elseif (BINARY_DIR_NAME STREQUAL "cclyzerpp")
  find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
  if (NOT LLVM_VERSION_MAJOR EQUAL 14)
    message(FATAL_ERROR "LLVM version 14 != ${LLVM_VERSION_MAJOR}")
  endif ()
  message(STATUS "LLVM version check - ok")
  
  set(CCLYZERPP_LIB_DIR ${BACKEND_BINARY_DIR}) # not put under `/lib`
  get_filename_component(CCLYZERPP_INCLUDE_DIR ${CCLYZERPP_BINARY_DIR}/../.. ABSOLUTE)
  set(_CCLYZERPP_LIBS PAPass SoufflePA)
  foreach(lib ${_CCLYZERPP_LIBS})
    if (NOT EXISTS "${CCLYZERPP_LIB_DIR}/lib${lib}.so")
      message(FATAL_ERROR "Lotus library (lib${lib}.so) not found in ${CCLYZERPP_LIB_DIR}")
    endif ()
    add_library(cclyzerpp::${lib} UNKNOWN IMPORTED)
    set_target_properties(cclyzerpp::${lib} PROPERTIES
      IMPORTED_LOCATION "${CCLYZERPP_LIB_DIR}/lib${lib}.so"
      INTERFACE_INCLUDE_DIRECTORIES "${CCLYZERPP_INCLUDE_DIR}"
    )
  endforeach()
  unset(_CCLYZERPP_LIBS)
  message(STATUS "Cclyzer++ libs check in ${CCLYZERPP_LIB_DIR} - ok")
    
elseif (BINARY_DIR_NAME STREQUAL "svf")
  find_package(LLVM REQUIRED CONFIG PATHS ${LLVM_DIR} NO_DEFAULT_PATH)
  if (LLVM_VERSION_MAJOR LESS 16)
    message(FATAL_ERROR "LLVM version 16 > ${LLVM_VERSION_MAJOR}")
  endif ()
  message(STATUS "LLVM version check - ok")

  find_package(SVF REQUIRED CONFIG HINTS  ${BACKEND_BINARY_DIR})

else()
  message(FATAL_ERROR "BINARY_DIR_NAME must be 'lotus' 'cclyzerpp' or 'svf', got '${BINARY_DIR_NAME}'")
endif ()

message(STATUS "LLVM version: ${LLVM_VERSION}")
message(STATUS "LLVM tools binary dir: ${LLVM_TOOLS_BINARY_DIR}")

set(_LLVM_TOOLS clang-${LLVM_VERSION_MAJOR} llvm-ar llvm-objcopy llvm-link llvm-dis llvm-nm)
foreach(_tool ${_LLVM_TOOLS})
  find_program(${_tool}_EXECUTABLE NAMES ${_tool} PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
  if (NOT ${_tool}_EXECUTABLE)
    message(FATAL_ERROR "${_tool} not found in ${LLVM_TOOLS_BINARY_DIR}")
  endif ()
endforeach()
unset(_LLVM_TOOLS)
unset(_tool)
set(_TC ${CMAKE_BINARY_DIR}/tests/toolchain)
file(REMOVE_RECURSE ${_TC})
file(MAKE_DIRECTORY ${_TC})
set(CL "${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}")
set(AR "${LLVM_TOOLS_BINARY_DIR}/llvm-ar")
set(_LLVM_BC_TOOLS
"${LLVM_TOOLS_BINARY_DIR}/llvm-objcopy" "${LLVM_TOOLS_BINARY_DIR}/llvm-link" "${LLVM_TOOLS_BINARY_DIR}/llvm-dis"
"${LLVM_TOOLS_BINARY_DIR}/llvm-nm" "${LLVM_TOOLS_BINARY_DIR}/clang++" "${LLVM_TOOLS_BINARY_DIR}/llc")
foreach(_tool ${_LLVM_BC_TOOLS})
if (NOT EXISTS "${_tool}")
  message(FATAL_ERROR "${_tool} not found in ${LLVM_TOOLS_BINARY_DIR}")
endif ()
endforeach()
unset(_LLVM_BC_TOOLS)

execute_process(
COMMAND sh -c "${CL} -c ${CMAKE_SOURCE_DIR}/tests/toolchain/m1.cpp -o ${_TC}/m1.o -flto && \
                ${CL} -c ${CMAKE_SOURCE_DIR}/tests/toolchain/m2.c -o ${_TC}/m2.o -flto && \
                ${AR} rcs ${_TC}/toolchain.a ${_TC}/m1.o ${_TC}/m2.o && \
                ${CMAKE_COMMAND} -E env LLVM_TOOLS_DIR=${LLVM_TOOLS_BINARY_DIR} \
                bash ${CMAKE_SOURCE_DIR}/scripts/a2bc.sh ${_TC}/toolchain.a >/dev/null && \
                ${LLVM_TOOLS_BINARY_DIR}/llc -relocation-model=pic -filetype=obj ${_TC}/toolchain.bc -o ${_TC}/toolchain_run.o && \
                ${LLVM_TOOLS_BINARY_DIR}/clang++ ${_TC}/toolchain_run.o -o ${_TC}/run_test && \
                ${_TC}/run_test > ${_TC}/output.txt && \
                printf '%s\n' '[C] sum=55 val=100 str=TEST123ABC big=18446744073709551615 arr=0 2 4 6 8 ' | diff - ${_TC}/output.txt"
COMMAND_ERROR_IS_FATAL ANY
)
unset(_TC)
message(STATUS "Toolchain check - ok")

include(cmake/modules/post_build.cmake)

add_subdirectory(src)
