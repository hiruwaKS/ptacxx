add_executable(irm irm.cpp)

add_smoke_test_target(irm
  ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeIRM.py
  FILE_DEPS
  ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
  ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeIRMInput.cpp
  ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeIRMInput2.cpp
)

target_include_directories(irm PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(irm PRIVATE drivers-common)
