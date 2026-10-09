#include <relinker/parsing/SelfExtractor.hpp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

template<typename TValue>
void Write(std::vector<std::uint8_t>& bytes, std::size_t offset, TValue value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<std::uint8_t> MakeSelf(std::uint64_t properties) {
    constexpr std::size_t elfOffset = 96;
    constexpr std::size_t sourceOffset = elfOffset + 64 + 112;
    std::vector<std::uint8_t> bytes(sourceOffset + 4);
    Write<std::uint32_t>(bytes, 0, 0xeef51454);
    Write<std::uint16_t>(bytes, 24, 2);
    Write<std::uint64_t>(bytes, 32, properties);
    Write<std::uint64_t>(bytes, 40, sourceOffset);
    Write<std::uint64_t>(bytes, 48, 4);
    Write<std::uint64_t>(bytes, 56, 4);
    Write<std::uint64_t>(bytes, 64, 0x102804);
    Write<std::uint64_t>(bytes, 72, sourceOffset + 4);
    Write<std::uint64_t>(bytes, 80, 16);
    Write<std::uint64_t>(bytes, 88, 16);
    bytes[elfOffset] = 0x7f;
    bytes[elfOffset + 1] = 'E';
    bytes[elfOffset + 2] = 'L';
    bytes[elfOffset + 3] = 'F';
    bytes[elfOffset + 4] = 2;
    bytes[elfOffset + 5] = 1;
    bytes[elfOffset + 6] = 1;
    Write<std::uint16_t>(bytes, elfOffset + 18, 62);
    Write<std::uint64_t>(bytes, elfOffset + 32, 64);
    Write<std::uint64_t>(bytes, elfOffset + 40, 0x1234);
    Write<std::uint16_t>(bytes, elfOffset + 54, 56);
    Write<std::uint16_t>(bytes, elfOffset + 56, 2);
    Write<std::uint16_t>(bytes, elfOffset + 58, 64);
    Write<std::uint16_t>(bytes, elfOffset + 60, 3);
    Write<std::uint16_t>(bytes, elfOffset + 62, 2);
    Write<std::uint32_t>(bytes, elfOffset + 64, 1);
    Write<std::uint64_t>(bytes, elfOffset + 64 + 8, 0x100);
    Write<std::uint64_t>(bytes, elfOffset + 64 + 32, 4);
    Write<std::uint64_t>(bytes, elfOffset + 64 + 40, 4);
    Write<std::uint32_t>(bytes, elfOffset + 120, 0x6fffff00);
    Write<std::uint64_t>(bytes, elfOffset + 120 + 8, 0x200);
    Write<std::uint64_t>(bytes, elfOffset + 120 + 32, 16);
    bytes[sourceOffset] = 1;
    bytes[sourceOffset + 1] = 2;
    bytes[sourceOffset + 2] = 3;
    bytes[sourceOffset + 3] = 4;
    return bytes;
}

void RequireRejected(std::vector<std::uint8_t> bytes, const char* message) {
    bool rejected = false;
    try {
        Relinker::SelfExtractor().Extract(std::move(bytes));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected, message);
}

}

int main() {
    try {
        auto output = Relinker::SelfExtractor().Extract(MakeSelf(0x2804));
        Require(output.size() == 0x210, "plain SELF output size is incorrect");
        Require(output[0] == 0x7f && output[1] == 'E' && output[2] == 'L' && output[3] == 'F',
                "plain SELF output is not ELF");
        Require(output[0x100] == 1 && output[0x101] == 2 && output[0x102] == 3 && output[0x103] == 4,
                "plain SELF segment was not reconstructed");
        RequireRejected(MakeSelf(0x2806), "encrypted SELF segment was accepted");
        RequireRejected(MakeSelf(0x280c), "compressed SELF segment was accepted");
        auto truncatedLoad = MakeSelf(0x2804);
        Write<std::uint32_t>(truncatedLoad, 96 + 120, 1);
        RequireRejected(std::move(truncatedLoad), "truncated loadable SELF segment was accepted");
        RequireRejected(std::vector<std::uint8_t>{0x54, 0x14, 0xf5, 0xee}, "truncated SELF was accepted");
        std::vector<std::uint8_t> elf{0x7f, 'E', 'L', 'F'};
        Require(Relinker::SelfExtractor().Extract(elf) == elf, "plain ELF was changed");
        std::cout << "SELF extractor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
