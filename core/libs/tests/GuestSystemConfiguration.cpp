#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
extern "C" {
std::int64_t APS5_VABI sysconf_nid_postfix(int);
int APS5_VABI getpagesize_nid_postfix();
int APS5_VABI sysctl_nid_postfix(const int*, unsigned int, void*, std::size_t*, const void*, std::size_t);
int* APS5_VABI __error_nid_postfix();
}
static void Require(bool value) { if (!value) std::abort(); }

static void RequireString(const int* name, unsigned int count, const char* expected) {
    std::size_t size = 0;
    Require(sysctl_nid_postfix(name, count, nullptr, &size, nullptr, 0) == 0);
    Require(size == std::strlen(expected) + 1);
    std::array<char, 64> output{};
    std::size_t capacity = output.size();
    Require(sysctl_nid_postfix(name, count, output.data(), &capacity, nullptr, 0) == 0);
    Require(capacity == size && std::strcmp(output.data(), expected) == 0);
}

int main() {
    *__error_nid_postfix() = 13;
    Require(sysconf_nid_postfix(47) == 0x4000);
    Require(getpagesize_nid_postfix() == sysconf_nid_postfix(47));
    Require(sysconf_nid_postfix(57) > 0);
    Require(sysconf_nid_postfix(58) > 0);
    Require(sysconf_nid_postfix(121) > 0);
    Require(*__error_nid_postfix() == 13);
    Require(sysconf_nid_postfix(-1) == -1); // verifies full-width signed return
    Require(*__error_nid_postfix() == 22);
    Require(sysconf_nid_postfix(0x7fffffff) == -1);

    const int osType[]{1, 1};
    const int osRelease[]{1, 2};
    const int osVersion[]{1, 4};
    const int hostname[]{1, 10};
    const int machine[]{6, 1};
    const int processPath[]{1, 14, 12, -1};
    const int processArgs[]{1, 14, 7, 1};
    RequireString(osType, 2, "FreeBSD");
    RequireString(osRelease, 2, "11.0-CURRENT");
    RequireString(osVersion, 2, "FreeBSD 11.0-CURRENT AnyPS5");
    RequireString(hostname, 2, "localhost");
    RequireString(machine, 2, "amd64");
    RequireString(processPath, 4, "/app0/eboot.bin");
    RequireString(processArgs, 4, "/app0/eboot.bin");

    for (int selector : {6, 12}) {
        const int memoryName[]{6, selector};
        std::uint64_t memory = 0;
        std::size_t size = sizeof(memory);
        Require(sysctl_nid_postfix(memoryName, 2, &memory, &size, nullptr, 0) == 0);
        Require(size == sizeof(memory));
        Require(memory == (selector == 6 ? DIRECT_MEMORY_SIZE : 16ULL * 1024 * 1024 * 1024));
    }

    std::array<char, 4> small{};
    std::size_t smallSize = small.size();
    Require(sysctl_nid_postfix(osType, 2, small.data(), &smallSize, nullptr, 0) == -1);
    Require(*__error_nid_postfix() == 12 && smallSize == 8);
    Require(sysctl_nid_postfix(nullptr, 0, nullptr, &smallSize, nullptr, 0) == -1 && *__error_nid_postfix() == 14);
    Require(sysctl_nid_postfix(osType, 2, nullptr, nullptr, nullptr, 0) == -1 && *__error_nid_postfix() == 14);
    Require(sysctl_nid_postfix(osType, 2, nullptr, &smallSize, "x", 1) == -1 && *__error_nid_postfix() == 1);
    const int unknown[]{99, 1};
    Require(sysctl_nid_postfix(unknown, 2, nullptr, &smallSize, nullptr, 0) == -1 && *__error_nid_postfix() == 2);
}
