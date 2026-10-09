#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

extern "C" int* APS5_VABI __error_nid_postfix();

namespace {

int SysctlFail(int error) {
    *__error_nid_postfix() = error;
    return -1;
}

int SysctlCopy(const void* value, std::size_t size, void* output, std::size_t* outputSize) {
    if (!outputSize) return SysctlFail(14);
    const std::size_t capacity = *outputSize;
    *outputSize = size;
    if (!output) return 0;
    if (capacity < size) return SysctlFail(12);
    std::memcpy(output, value, size);
    return 0;
}

int SysctlString(std::string_view value, void* output, std::size_t* outputSize) {
    return SysctlCopy(value.data(), value.size() + 1, output, outputSize);
}

}

extern "C" {
int APS5_VABI sysctl_nid_postfix(const int* name, unsigned int nameLength, void* output, std::size_t* outputSize, const void* input, std::size_t inputSize) {
    if (!name || nameLength == 0) return SysctlFail(14);
    if (input || inputSize != 0) return SysctlFail(1);
    const int saved = *__error_nid_postfix();
    int result = -1;
    if (nameLength == 2 && name[0] == 1) {
        switch (name[1]) {
            case 1: result = SysctlString("FreeBSD", output, outputSize); break;
            case 2: result = SysctlString("11.0-CURRENT", output, outputSize); break;
            case 4: result = SysctlString("FreeBSD 11.0-CURRENT AnyPS5", output, outputSize); break;
            case 10: result = SysctlString("localhost", output, outputSize); break;
            default: return SysctlFail(2);
        }
    } else if (nameLength == 2 && name[0] == 6) {
        if (name[1] == 1) {
            result = SysctlString("amd64", output, outputSize);
        } else if (name[1] == 6 || name[1] == 12) {
            const std::uint64_t memory = name[1] == 6 ? DIRECT_MEMORY_SIZE : 16ULL * 1024 * 1024 * 1024;
            result = SysctlCopy(&memory, sizeof(memory), output, outputSize);
        } else {
            return SysctlFail(2);
        }
    } else if (nameLength == 4 && name[0] == 1 && name[1] == 14 && (name[2] == 7 || name[2] == 12)) {
        result = SysctlString("/app0/eboot.bin", output, outputSize);
    } else {
        return SysctlFail(2);
    }
    if (result == 0) *__error_nid_postfix() = saved;
    return result;
}

// Guest long is 64 bits, including when the Windows host long is 32 bits.
std::int64_t APS5_VABI sysconf_nid_postfix(int name) {
    const int saved = *__error_nid_postfix();
    std::int64_t result = -1;
    switch (name) {
        case 47: // _SC_PAGESIZE
            result = PS5_PAGE_SIZE;
            break;
        case 57: // _SC_NPROCESSORS_CONF
        case 58: // _SC_NPROCESSORS_ONLN
#ifdef _WIN32
            result = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
            result = ::sysconf(name == 57 ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN);
#endif
            break;
        case 121: { // _SC_PHYS_PAGES: report host physical capacity in guest pages
#ifdef _WIN32
            MEMORYSTATUSEX memory{};
            memory.dwLength = sizeof(memory);
            if (GlobalMemoryStatusEx(&memory)) result = memory.ullTotalPhys / PS5_PAGE_SIZE;
#else
            const auto pages = ::sysconf(_SC_PHYS_PAGES);
            const auto pageSize = ::sysconf(_SC_PAGESIZE);
            if (pages > 0 && pageSize > 0)
                result = static_cast<std::uint64_t>(pages) * pageSize / PS5_PAGE_SIZE;
#endif
            break;
        }
        default:
            *__error_nid_postfix() = 22;
            return -1;
    }
    if (result <= 0) { *__error_nid_postfix() = 5; return -1; }
    *__error_nid_postfix() = saved;
    return result;
}
}
