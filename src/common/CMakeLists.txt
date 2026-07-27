add_library(common STATIC
  IRManager.cpp
  MemoryBuiltins.cpp
  LLVMUtils.cpp
)

target_include_directories(common PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_include_directories(common SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})

target_link_libraries(common PUBLIC LLVMCore LLVMSupport LLVMIRReader LLVMAnalysis LLVMBitWriter)
