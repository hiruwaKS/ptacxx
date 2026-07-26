#pragma once

#define LLVM_CL_IGNORE_WARNINGS_BEGIN \
    _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wexit-time-destructors\"") \
    _Pragma("clang diagnostic ignored \"-Wglobal-constructors\"")

#define LLVM_CL_IGNORE_WARNINGS_END \
    _Pragma("clang diagnostic pop")
