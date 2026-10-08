#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdio>
#include <cstdlib>

namespace {

[[noreturn]] void Fail(const char* message) noexcept {
    std::fprintf(stderr, "Guest lifecycle replacement fixture: %s\n", message);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
}

}

extern "C" void APS5_VABI Record(int event) {
    const auto path = std::getenv("ANYPS5_LIFECYCLE_EVENTS");
    if (!path) Fail("Missing ANYPS5_LIFECYCLE_EVENTS");
    auto* file = std::fopen(path, "ab");
    if (!file) Fail("Cannot open lifecycle event file");
    const auto result = std::fputc(event, file);
    const auto closed = std::fclose(file);
    if (result == EOF || closed != 0) Fail("Cannot write lifecycle event");
}

namespace {

struct Runtime {
    Runtime() { Record('H'); }
    ~Runtime() { Record('h'); }
} runtime;

}
