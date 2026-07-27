#include "PointerSymbolTable.h"

extern "C" {
  void __hook_init(size_t k);
  void __hook_push(PtrRecord record);
  void __hook_dump(const char *dumpPath);
}

void __hook_init(size_t k) {
  PtaHook::init(k);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
void __hook_push(PtrRecord record) {
  buffer[bufferIndex++] = record;
  if (bufferIndex == BUFFER_SIZE) PtaHook::stopAndConsume();
}
#pragma clang diagnostic pop

void __hook_dump(const char *dumpPath) {
  PtaHook::stopAndConsume();
  PtaHook::dump(dumpPath);
}
