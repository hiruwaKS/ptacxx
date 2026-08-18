add_executable(svf-wpa svf-wpa.cpp)
add_library(svf-wpa_a STATIC svf-wpa.cpp)

# add_smoke_test_target(svf-wpa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeSVFWpa.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )
find_package(Threads REQUIRED)

target_include_directories(svf-wpa PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(svf-wpa_a PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_include_directories(svf-wpa SYSTEM PRIVATE ${SVF_INCLUDE_DIR})
target_link_libraries(svf-wpa_a PRIVATE SVF::SvfLLVM)

target_link_libraries(svf-wpa PRIVATE common)
target_link_libraries(svf-wpa PRIVATE SVF::SvfLLVM Threads::Threads)
add_custom_target(svf-wpa_bc ALL
  COMMAND ${CMAKE_COMMAND} -E env LLVM_TOOLS_DIR=${LLVM_TOOLS_BINARY_DIR}
    bash ${CMAKE_SOURCE_DIR}/scripts/a2bc.sh
    $<TARGET_FILE:svf-wpa_a>
    $<TARGET_FILE:common>
    $<TARGET_FILE:SVF::SvfCore>
    $<TARGET_FILE:SVF::SvfLLVM>
  DEPENDS svf-wpa_a common
)
