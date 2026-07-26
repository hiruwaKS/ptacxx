#pragma once

#define LLVM_CL_IGNORE_WARNINGS_BEGIN \
    _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wexit-time-destructors\"") \
    _Pragma("clang diagnostic ignored \"-Wglobal-constructors\"")

#define LLVM_CL_IGNORE_WARNINGS_END \
    _Pragma("clang diagnostic pop")

#include <cstdint>

static constexpr int16_t PTR_ACTION_ALLOCA      = 0;
static constexpr int16_t PTR_ACTION_HEAP_ALLOCA = 1;
static constexpr int16_t PTR_ACTION_HEAP_FREE   = 2;
static constexpr int16_t PTR_ACTION_REGION      = 3;
static constexpr int16_t PTR_ACTION_PROBE       = 8;
static constexpr int16_t PTR_ACTION_BEGINSCOPE  = 16;
static constexpr int16_t PTR_ACTION_ENDSCOPE    = 17;
