if (BINARY_DIR_NAME STREQUAL "cclyzerpp")
  add_executable(cclyzerpp cclyzerpp.cpp)

  target_include_directories(cclyzerpp PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_include_directories(cclyzerpp SYSTEM PRIVATE ${CCLYZERPP_INCLUDE_DIR})

  target_link_libraries(cclyzerpp PRIVATE common)
  target_link_libraries(cclyzerpp PRIVATE
    cclyzerpp::PAPass
    cclyzerpp::SoufflePA
    LLVMCore
    LLVMSupport
    LLVMIRReader
    LLVMAnalysis
    LLVMBitWriter
    LLVMPasses
  )

  set_target_properties(cclyzerpp PROPERTIES
    BUILD_RPATH "${CCLYZERPP_BINARY_DIR}"
    INSTALL_RPATH "${CCLYZERPP_BINARY_DIR}"
  )
endif()
