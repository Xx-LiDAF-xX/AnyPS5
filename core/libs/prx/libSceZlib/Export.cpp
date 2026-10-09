#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include "SceTypes.hpp"
#include "zlib.h"

namespace {

constexpr std::int32_t InvalidArgument = static_cast<std::int32_t>(0x81120002);
constexpr std::int32_t NoMemory = static_cast<std::int32_t>(0x81120003);
constexpr std::int32_t InvalidData = static_cast<std::int32_t>(0x8112000a);
constexpr std::int32_t InsufficientOutput = static_cast<std::int32_t>(0x8112000b);

struct InflateJob {
    std::uint32_t outputSize;
};

InflateJob* ReadJob(const void* request) {
    if (!request) return nullptr;
    InflateJob* job = nullptr;
    std::memcpy(&job, request, sizeof(job));
    return job;
}

}

extern "C" {

std::int32_t APS5_VABI sceZlibInflate(const void* source, std::uint32_t sourceSize, void* destination, std::uint32_t destinationSize, void* request) {
    if (!source || sourceSize == 0 || !destination || destinationSize == 0 || !request) return InvalidArgument;
    auto* job = new (std::nothrow) InflateJob{};
    if (!job) return NoMemory;
    z_stream stream{};
    stream.next_in = const_cast<Bytef*>(static_cast<const Bytef*>(source));
    stream.avail_in = sourceSize;
    stream.next_out = static_cast<Bytef*>(destination);
    stream.avail_out = destinationSize;
    const auto initialized = inflateInit(&stream);
    if (initialized != Z_OK) {
        delete job;
        return initialized == Z_MEM_ERROR ? NoMemory : InvalidData;
    }
    const auto result = inflate(&stream, Z_FINISH);
    job->outputSize = stream.total_out > std::numeric_limits<std::uint32_t>::max() ? 0 : static_cast<std::uint32_t>(stream.total_out);
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        delete job;
        return result == Z_MEM_ERROR ? NoMemory : result == Z_BUF_ERROR ? InsufficientOutput : InvalidData;
    }
    std::memcpy(request, &job, sizeof(job));
    return 0;
}

std::int32_t APS5_VABI sceZlibWaitForDone(const void* request, std::uint32_t) {
    return ReadJob(request) ? 0 : InvalidArgument;
}

std::int32_t APS5_VABI sceZlibGetResult(void* handle, std::uint32_t* outputSize, std::uint32_t* status) {
    if (!handle || !outputSize || !status) return InvalidArgument;
    auto* job = static_cast<InflateJob*>(handle);
    *outputSize = job->outputSize;
    *status = 0;
    delete job;
    return 0;
}

}
