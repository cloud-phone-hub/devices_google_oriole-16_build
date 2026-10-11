// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include <v4l2_codec2/v4l2/V4L2Memory.h>

#include <array>
#include <cstring>

namespace android {

void setV4L2BufferMemoryType(v4l2_buffer& buffer, v4l2_memory memory) {
    buffer.memory = memory;
}

std::optional<v4l2_memory> selectV4L2MemoryType(uint32_t capabilities,
                                                v4l2_memory probedMemory) {
    if (capabilities & V4L2_BUF_CAP_SUPPORTS_DMABUF) {
        return V4L2_MEMORY_DMABUF;
    }
    if (capabilities & V4L2_BUF_CAP_SUPPORTS_USERPTR) {
        return V4L2_MEMORY_USERPTR;
    }
    if (capabilities == 0 &&
        (probedMemory == V4L2_MEMORY_DMABUF || probedMemory == V4L2_MEMORY_USERPTR)) {
        return probedMemory;
    }
    return std::nullopt;
}

std::optional<v4l2_memory> probeV4L2MemoryType(
        v4l2_buf_type type, const V4L2RequestBuffers& requestBuffers) {
    constexpr std::array<v4l2_memory, 2> kProbeOrder = {
            V4L2_MEMORY_DMABUF,
            V4L2_MEMORY_USERPTR,
    };
    for (const auto memory : kProbeOrder) {
        v4l2_requestbuffers request;
        std::memset(&request, 0, sizeof(request));
        request.type = type;
        request.memory = memory;
        if (requestBuffers(&request) != 0) {
            continue;
        }
        const auto selected = selectV4L2MemoryType(request.capabilities, memory);
        if (selected && *selected == memory) {
            return selected;
        }
    }
    return std::nullopt;
}

}  // namespace android
