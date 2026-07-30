if (BINARY_DIR_NAME STREQUAL "lotus")
  add_executable(lotus-sparrow-aa lotus-sparrow-aa.cpp)

# add_smoke_test_target(lotus-sparrow-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeSparrowAA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-sparrow-aa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(lotus-sparrow-aa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})
  
  target_include_directories(lotus-sparrow-aa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-sparrow-aa PRIVATE drivers-common)
  target_link_libraries(lotus-sparrow-aa PRIVATE
    lotus::Andersen
    lotus::CanaryAliasPtsSet
    lotus::CanaryCUDD
    lotus::AliasSpecManager
    lotus::CanaryLLVMUtils
    lotus::Annotation
  )

  add_executable(lotus-aser-aa lotus-aser-aa.cpp)

# add_smoke_test_target(lotus-aser-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeAserAA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-aser-aa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(lotus-aser-aa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})

  target_include_directories(lotus-aser-aa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)
 
  target_link_libraries(lotus-aser-aa PRIVATE drivers-common)
  target_link_libraries(lotus-aser-aa PRIVATE
    lotus::AserPTA
    lotus::CanaryAliasCLIUtils
    lotus::CanaryAliasPtsSet
    lotus::CanaryCUDD
    lotus::AliasSpecManager
    lotus::CanaryLLVMUtils
    lotus::Annotation
    LLVMTransformUtils
  )

  add_executable(lotus-un-dyckaa lotus-un-dyckaa.cpp)

# add_smoke_test_target(lotus-dyckaa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeDyckAA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-un-dyckaa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(lotus-un-dyckaa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})

  target_include_directories(lotus-un-dyckaa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-un-dyckaa PRIVATE drivers-common)
  target_link_libraries(lotus-un-dyckaa PRIVATE
    lotus::CanaryDyckAA
    lotus::CanaryCFG
    lotus::CanaryParallel
    lotus::AliasSpecManager
    lotus::CanaryLLVMUtils
    lotus::Annotation
  )

  add_executable(lotus-un-seadsa-aa lotus-un-seadsa-aa.cpp)

# add_smoke_test_target(lotus-seadsa-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeSeadsa.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-un-seadsa-aa PRIVATE ${CMAKE_SOURCE_DIR}/src)

  target_include_directories(lotus-un-seadsa-aa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})

  target_include_directories(lotus-un-seadsa-aa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-un-seadsa-aa PRIVATE drivers-common)
  target_link_libraries(lotus-un-seadsa-aa PRIVATE
    lotus::SeaDsaAnalysis
    LLVMTransformUtils
  )

  add_executable(lotus-fpa lotus-fpa.cpp)

# add_smoke_test_target(lotus-fpa-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeFPA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-fpa PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_include_directories(lotus-fpa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})
  target_include_directories(lotus-fpa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-fpa PRIVATE drivers-common)
  target_link_libraries(lotus-fpa PRIVATE
    lotus::CanaryFPA
    lotus::CanaryAliasCLIUtils
  )

  add_executable(lotus-tpa lotus-tpa.cpp)

# add_smoke_test_target(lotus-tpa-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeTPA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-tpa PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_include_directories(lotus-tpa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})
  target_include_directories(lotus-tpa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-tpa PRIVATE drivers-common)
  target_link_libraries(lotus-tpa PRIVATE
    lotus::FSCSPointerAnalysis
    lotus::TPATransforms
    lotus::CanaryAliasCLIUtils
    lotus::CanaryLLVMUtils
    lotus::Annotation
  )

  add_executable(lotus-aa lotus-aa.cpp)

# add_smoke_test_target(lotus-aa
#   ${CMAKE_SOURCE_DIR}/tests/units/drivers/smokeLotusAA.py
#   FILE_DEPS
#   ${LLVM_TOOLS_BINARY_DIR}/clang-${LLVM_VERSION_MAJOR}
# )

  target_include_directories(lotus-aa PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_include_directories(lotus-aa SYSTEM PRIVATE ${LOTUS_INCLUDE_DIR})
  target_include_directories(lotus-aa SYSTEM PRIVATE ${LOTUS_BINARY_DIR}/../third-party)

  target_link_libraries(lotus-aa PRIVATE drivers-common)
  target_link_libraries(lotus-aa PRIVATE
    lotus::LotusAA
    lotus::CanaryParallel
    lotus::CanaryGSA
    lotus::CanaryPlatform
    lotus::AliasSpecManager
    lotus::CanaryLLVMUtils
    lotus::Annotation
  )

endif()
