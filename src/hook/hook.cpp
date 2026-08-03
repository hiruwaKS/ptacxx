#include "PointerSymbolTable.h"

extern "C" {
  void __hook_init(size_t k);
  void __hook_push(int16_t globalIdx, int16_t localIdx, int16_t action, uint64_t ptr, uint64_t size);
  void __hook_dump(const char *dumpPath);
}
void __hook_init(size_t k) {
  PtaHook::init(k);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
void __hook_push(int16_t globalIdx, int16_t localIdx, int16_t action, uint64_t ptr, uint64_t size) {
  buffer[bufferIndex++] = PtrRecord{globalIdx, localIdx, 0, action, ptr, size, 0};
  if (bufferIndex == BUFFER_SIZE) PtaHook::stopAndConsume();
}
#pragma clang diagnostic pop

void __hook_dump(const char *dumpPath) {
  PtaHook::stopAndConsume();
  PtaHook::dump(dumpPath);
}
