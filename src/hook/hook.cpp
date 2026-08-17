#include "PointerSymbolTable.h"

extern "C" {
  void __hook_init(size_t k);
  void __hook_push(VId vid, int16_t action, uint64_t ptr, uint64_t size);
  void __hook_dump(const char *dumpPath);
}
void __hook_init(size_t k) {
  PtaHook::init(k);
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

void __hook_dump(const char *dumpPath) {
  PtaHook::stopAndConsume();
  PtaHook::dump(dumpPath);
}
