if (BINARY_DIR_NAME STREQUAL "svf")
  add_executable(svf-wpa svf-wpa.cpp)

# add_smoke_test_target(svf-wpa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeSVFWpa.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )
  find_package(Threads REQUIRED)

  target_include_directories(svf-wpa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(svf-wpa SYSTEM PRIVATE ${SVF_INCLUDE_DIR})

  target_link_libraries(svf-wpa PRIVATE drivers-common)
  target_link_libraries(svf-wpa PRIVATE SVF::SvfLLVM Threads::Threads)
endif()
