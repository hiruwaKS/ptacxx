if (BINARY_DIR_NAME STREQUAL "lotus")
  add_executable(lotus-sparrow-aa lotus-sparrow-aa.cpp)

# add_smoke_test_target(lotus-sparrow-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeSparrowAA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-sparrow-aa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(lotus-sparrow-aa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})

  target_link_libraries(lotus-sparrow-aa PRIVATE drivers-common)
  target_link_libraries(lotus-sparrow-aa PRIVATE
    lotus::Andersen
    lotus::CanaryAliasAnalysisWrapper
    lotus::CanaryAliasPtsSet
    lotus::CanaryCUDD
    lotus::AliasSpecManager
    lotus::CanaryLLVMUtils
    lotus::Annotation)

endif()
