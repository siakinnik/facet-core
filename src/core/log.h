// Logging to stderr (journald picks it up under systemd).
#pragma once

namespace facet::log {

void info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void warn(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace facet::log
