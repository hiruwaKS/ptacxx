#pragma once

#include "Error.h"

#include <ctime>
#include <cstdint>

namespace ptacxx {

inline timespec timespecGet() {
  timespec ts;
  if (timespec_get(&ts, TIME_UTC) == 0)
    throw TimespecInvalid("timespecinvalid-timespec-get-failed", "timespec_get failed");
  return ts;
}

class Stopwatch {
private:
  timespec start;
  timespec last;

  static intmax_t diffUs(const timespec &from, const timespec &to) {
    return (to.tv_sec - from.tv_sec) * 1000000 +
           (to.tv_nsec - from.tv_nsec) / 1000;
  }

public:
  struct Record {
    intmax_t lap;
    intmax_t split;
  };

  Stopwatch() : start(timespecGet()), last(start) {}

  /// @return lap: time since the previous record, split: time since construction
  /// @throw TimespecInvalid if the current time cannot be read
  Record record() {
    timespec now = timespecGet();
    Record r{diffUs(last, now), diffUs(start, now)};
    last = now;
    return r;
  }
};

} // namespace ptacxx
