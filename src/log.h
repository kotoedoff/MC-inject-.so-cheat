#pragma once
// Tiny thread-safe file logger: writes to /tmp/voltage.log
// (inside flatpak the path differs, so for host-java launches it's always /tmp/voltage.log)
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unistd.h>

namespace voltage {
namespace log {

inline std::mutex& mutex() { static std::mutex m; return m; }
inline const char* path() {
  const char* e = std::getenv("VOLTAGE_LOG");
  return e && *e ? e : "/tmp/voltage.log";
}

inline void write(const char* tag, const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(mutex());
  FILE* f = std::fopen(path(), "a");
  if (!f) return;
  std::fprintf(f, "[voltage][%s][pid=%d] ", tag, (int)::getpid());
  va_list ap; va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fprintf(f, "\n");
  std::fclose(f);
}

#define VLOG(tag, ...) ::voltage::log::write(tag, __VA_ARGS__)

} // namespace log
} // namespace voltage
