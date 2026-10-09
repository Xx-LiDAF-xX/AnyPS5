#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include "SceTypes.hpp"

extern "C" {
std::int32_t APS5_VABI sceZlibInflate(const void*, std::uint32_t, void*, std::uint32_t, void*);
std::int32_t APS5_VABI sceZlibWaitForDone(const void*, std::uint32_t);
std::int32_t APS5_VABI sceZlibGetResult(void*, std::uint32_t*, std::uint32_t*);
}

int main() {
    constexpr std::array<std::uint8_t, 13> compressed{0x78, 0x9c, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0x06, 0x2c, 0x02, 0x15};
    std::array<char, 16> output{};
    std::array<std::uint8_t, 16> request{};
    if (sceZlibInflate(compressed.data(), compressed.size(), output.data(), output.size(), request.data()) != 0) return 1;
    if (sceZlibWaitForDone(request.data(), 0) != 0) return 2;
    void* handle = nullptr;
    std::memcpy(&handle, request.data(), sizeof(handle));
    std::uint32_t outputSize = 0;
    std::uint32_t status = 1;
    if (sceZlibGetResult(handle, &outputSize, &status) != 0) return 3;
    if (outputSize != 5 || status != 0 || std::memcmp(output.data(), "hello", 5) != 0) return 4;
    std::cout << "Guest zlib tests passed\n";
    return 0;
}
