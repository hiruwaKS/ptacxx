add_executable(irHash irHash.cpp)

target_include_directories(irHash SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})

target_link_libraries(irHash PRIVATE LLVMCore LLVMSupport LLVMIRReader LLVMBitReader LLVMAsmParser)
