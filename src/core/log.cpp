#include "core/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace facet::log {

namespace {
void write(const char* level, const char* fmt, va_list ap) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::fprintf(stderr, "%02d:%02d:%02d %s ", tm.tm_hour, tm.tm_min, tm.tm_sec, level);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}
}  // namespace

#define FACET_LOG_IMPL(name, level) \
    void name(const char* fmt, ...) { \
        va_list ap;                  \
        va_start(ap, fmt);           \
        write(level, fmt, ap);       \
        va_end(ap);                  \
    }

FACET_LOG_IMPL(info, "I")
FACET_LOG_IMPL(warn, "W")
FACET_LOG_IMPL(error, "E")

}  // namespace facet::log
