#pragma once
#include <time.h>

inline uint64_t getns() {
  timespec ts;
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec * 1000000000 + ts.tv_nsec;
}

inline uint64_t rdtsc() {
    return __builtin_ia32_rdtsc();
}

inline uint64_t rdtscp() {
    unsigned int dummy;
    return __builtin_ia32_rdtscp(&dummy);
}

