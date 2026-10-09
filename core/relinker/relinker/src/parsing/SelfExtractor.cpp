#include <relinker/parsing/SelfExtractor.hpp>
#include <domain/Types.hpp>
#include <algorithm>
#include <cstring>
#include <limits>

namespace Relinker {
namespace {

constexpr std::uint32_t Ps4SelfMagic = 0x1d3d154f;
constexpr std::uint32_t Ps5SelfMagic = 0xeef51454;
constexpr std::size_t SelfHeaderSize = 32;
constexpr std::size_t SelfEntrySize = 32;
constexpr std::size_t ElfHeaderSize = 64;
constexpr std::size_t ElfProgramHeaderSize = 56;
constexpr std::uint64_t ContentEntryTag = 0x2804;
constexpr std::uint64_t EntryTagMask = 0xfffff;
constexpr std::uint32_t SceCommentProgramType = 0x6fffff00;

void RequireRange(std::size_t offset, std::size_t size, std::size_t total, const char* message) {
    if (offset > total || size > total - offset) throw Domain::RelinkerException(message);
}

template<typename TValue>
TValue Read(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* message) {
    RequireRange(offset, sizeof(TValue), bytes.size(), message);
    TValue value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void WriteU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void WriteU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

}

std::vector<std::uint8_t> SelfExtractor::Extract(std::vector<std::uint8_t> bytes) const {
    if (bytes.size() < 4) return bytes;
    const auto magic = Read<std::uint32_t>(bytes, 0, "cannot read executable magic");
    if (magic != Ps4SelfMagic && magic != Ps5SelfMagic) return bytes;
    RequireRange(0, SelfHeaderSize, bytes.size(), "SELF header is truncated");
    const auto entryCount = Read<std::uint16_t>(bytes, 24, "SELF entry count is truncated");
    if (entryCount > (std::numeric_limits<std::size_t>::max() - SelfHeaderSize) / SelfEntrySize)
        throw Domain::RelinkerException("SELF entry table size overflows");
    const std::size_t elfOffset = SelfHeaderSize + static_cast<std::size_t>(entryCount) * SelfEntrySize;
    RequireRange(elfOffset, ElfHeaderSize, bytes.size(), "SELF embedded ELF header is truncated");
    if (Read<std::uint32_t>(bytes, elfOffset, "SELF embedded ELF magic is truncated") != 0x464c457f)
        throw Domain::RelinkerException("SELF does not contain an embedded ELF header");
    if (bytes[elfOffset + 4] != 2 || bytes[elfOffset + 5] != 1 || bytes[elfOffset + 6] != 1 ||
        Read<std::uint16_t>(bytes, elfOffset + 18, "SELF embedded ELF machine is truncated") != 62)
        throw Domain::RelinkerException("SELF embedded image is not little-endian ELF64 x86-64");
    const auto programOffset = Read<std::uint64_t>(bytes, elfOffset + 32, "SELF ELF program offset is truncated");
    const auto programEntrySize = Read<std::uint16_t>(bytes, elfOffset + 54, "SELF ELF program entry size is truncated");
    const auto programCount = Read<std::uint16_t>(bytes, elfOffset + 56, "SELF ELF program count is truncated");
    if (programEntrySize != ElfProgramHeaderSize || programCount == 0)
        throw Domain::RelinkerException("SELF embedded ELF program table is invalid");
    if (programOffset > std::numeric_limits<std::size_t>::max() ||
        programCount > (std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(programOffset)) / programEntrySize)
        throw Domain::RelinkerException("SELF embedded ELF program table size overflows");
    const std::size_t programTableSize = static_cast<std::size_t>(programCount) * programEntrySize;
    if (static_cast<std::size_t>(programOffset) > std::numeric_limits<std::size_t>::max() - elfOffset)
        throw Domain::RelinkerException("SELF embedded ELF program table offset overflows");
    const std::size_t sourceProgramOffset = elfOffset + static_cast<std::size_t>(programOffset);
    RequireRange(sourceProgramOffset, programTableSize, bytes.size(), "SELF embedded ELF program table is truncated");
    std::size_t outputSize = static_cast<std::size_t>(programOffset) + programTableSize;
    for (std::size_t index = 0; index < entryCount; ++index) {
        const std::size_t entryOffset = SelfHeaderSize + index * SelfEntrySize;
        const auto properties = Read<std::uint64_t>(bytes, entryOffset, "SELF entry properties are truncated");
        if ((properties & 2) != 0) throw Domain::RelinkerException("encrypted SELF segments are unsupported");
        if ((properties & 8) != 0) throw Domain::RelinkerException("compressed SELF segments are unsupported");
        if ((properties & EntryTagMask) != ContentEntryTag) continue;
        const auto segmentIndex = properties >> 20;
        if (segmentIndex >= programCount) throw Domain::RelinkerException("SELF segment index exceeds the ELF program table");
        const auto sourceOffset = Read<std::uint64_t>(bytes, entryOffset + 8, "SELF segment offset is truncated");
        const auto storedSize = Read<std::uint64_t>(bytes, entryOffset + 16, "SELF stored segment size is truncated");
        const auto plainSize = Read<std::uint64_t>(bytes, entryOffset + 24, "SELF plain segment size is truncated");
        if (storedSize != plainSize) throw Domain::RelinkerException("SELF segment size requires unsupported decoding");
        const std::size_t headerOffset = sourceProgramOffset + static_cast<std::size_t>(segmentIndex) * programEntrySize;
        const auto destinationOffset = Read<std::uint64_t>(bytes, headerOffset + 8, "SELF ELF segment offset is truncated");
        const auto destinationSize = Read<std::uint64_t>(bytes, headerOffset + 32, "SELF ELF segment size is truncated");
        const auto programType = Read<std::uint32_t>(bytes, headerOffset, "SELF ELF segment type is truncated");
        const auto memorySize = Read<std::uint64_t>(bytes, headerOffset + 40, "SELF ELF segment memory size is truncated");
        if (destinationSize != plainSize) throw Domain::RelinkerException("SELF segment size does not match its ELF program header");
        if (sourceOffset > std::numeric_limits<std::size_t>::max() || storedSize > std::numeric_limits<std::size_t>::max() ||
            destinationOffset > std::numeric_limits<std::size_t>::max() || destinationSize > std::numeric_limits<std::size_t>::max())
            throw Domain::RelinkerException("SELF segment range exceeds the host size limit");
        const bool sourceFits = static_cast<std::size_t>(sourceOffset) <= bytes.size() &&
            static_cast<std::size_t>(storedSize) <= bytes.size() - static_cast<std::size_t>(sourceOffset);
        const bool omittedComment = programType == SceCommentProgramType && memorySize == 0 && sourceOffset == bytes.size();
        if (!sourceFits && !omittedComment) throw Domain::RelinkerException("SELF segment data is truncated");
        const std::size_t destination = static_cast<std::size_t>(destinationOffset);
        const std::size_t size = static_cast<std::size_t>(destinationSize);
        if (destination > std::numeric_limits<std::size_t>::max() - size)
            throw Domain::RelinkerException("SELF output segment range overflows");
        outputSize = std::max(outputSize, destination + size);
    }
    const std::size_t expansionLimit = bytes.size() > std::numeric_limits<std::size_t>::max() - 16 * 1024 * 1024
        ? std::numeric_limits<std::size_t>::max()
        : bytes.size() + 16 * 1024 * 1024;
    for (std::size_t index = 0; index < programCount; ++index) {
        const std::size_t headerOffset = sourceProgramOffset + index * programEntrySize;
        const auto offset = Read<std::uint64_t>(bytes, headerOffset + 8, "SELF ELF segment offset is truncated");
        const auto size = Read<std::uint64_t>(bytes, headerOffset + 32, "SELF ELF segment size is truncated");
        if (offset > std::numeric_limits<std::size_t>::max() || size > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::size_t>(offset) > std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(size))
            throw Domain::RelinkerException("SELF ELF segment range exceeds the host size limit");
        const std::size_t end = static_cast<std::size_t>(offset) + static_cast<std::size_t>(size);
        if (end > expansionLimit) throw Domain::RelinkerException("SELF ELF segment range is unreasonably large");
        outputSize = std::max(outputSize, end);
    }
    std::vector<std::uint8_t> output(outputSize);
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(elfOffset), ElfHeaderSize, output.begin());
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sourceProgramOffset), programTableSize,
                output.begin() + static_cast<std::ptrdiff_t>(programOffset));
    WriteU64(output, 40, 0);
    WriteU16(output, 58, 0);
    WriteU16(output, 60, 0);
    WriteU16(output, 62, 0);
    for (std::size_t index = 0; index < entryCount; ++index) {
        const std::size_t entryOffset = SelfHeaderSize + index * SelfEntrySize;
        const auto properties = Read<std::uint64_t>(bytes, entryOffset, "SELF entry properties are truncated");
        if ((properties & EntryTagMask) != ContentEntryTag) continue;
        const auto segmentIndex = properties >> 20;
        const auto sourceOffset = static_cast<std::size_t>(Read<std::uint64_t>(bytes, entryOffset + 8, "SELF segment offset is truncated"));
        const auto storedSize = static_cast<std::size_t>(Read<std::uint64_t>(bytes, entryOffset + 16, "SELF segment size is truncated"));
        const std::size_t headerOffset = sourceProgramOffset + static_cast<std::size_t>(segmentIndex) * programEntrySize;
        const auto destinationOffset = static_cast<std::size_t>(Read<std::uint64_t>(bytes, headerOffset + 8, "SELF ELF segment offset is truncated"));
        if (sourceOffset == bytes.size()) continue;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset), storedSize,
                    output.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    return output;
}

}
