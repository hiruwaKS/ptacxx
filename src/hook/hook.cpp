#include "PointerSymbolTable.h"

#include <cstdio>
#include <cstdlib>

extern "C" {
  void __hook_init(uint64_t mode);
  void __hook_push(VId vid, int16_t action, uint64_t ptr, uint64_t size);
  void __hook_dump();
}

void __hook_init(uint64_t mode) {
  PtaHook::init(mode);
}

#if __clang_major__ >= 17
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
void __hook_push(VId vid, int16_t action, uint64_t ptr, uint64_t size) {
  buffer[bufferIndex++] = PtrRecord{vid, action, 0, ptr, size, 0};
  if (bufferIndex == BUFFER_SIZE) PtaHook::stopAndConsume();
}
#if __clang_major__ >= 17
#pragma clang diagnostic pop
#endif

void __hook_dump() {
  PtaHook::stopAndConsume();
  PtaHook::dump();
}
