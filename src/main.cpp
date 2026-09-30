#include <cstdio>
#include <cstring>

#include "app/app.h"
#include "core/build_info.h"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            const auto& b = facet::build::info();
            std::printf("%s\n", b.version);
            std::printf("channel: %s\ncommit:  %s%s\nrepo:    %s\nbuilt:   %s\n", b.channel,
                        *b.commit ? b.commit : "unknown", b.dirty ? " (uncommitted changes)" : "",
                        *b.repo ? b.repo : "unknown", b.date);
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf("Facet %s - touch panel shell.\n"
                        "Usage: facet [--version]\n"
                        "Configured through environment variables; see README.md.\n",
                        facet::build::summary().c_str());
            return 0;
        }
        std::fprintf(stderr, "facet: unknown argument '%s' (try --help)\n", argv[i]);
        return 2;
    }
    facet::App app;
    return app.run();
}
