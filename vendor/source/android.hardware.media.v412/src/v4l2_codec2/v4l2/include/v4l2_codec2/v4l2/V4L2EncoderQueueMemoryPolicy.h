// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_ENCODER_QUEUE_MEMORY_POLICY_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_ENCODER_QUEUE_MEMORY_POLICY_H

#include <stddef.h>

#include <array>
#include <optional>

#include <linux/videodev2.h>

namespace android {

struct V4L2EncoderQueueAllocationResult {
    size_t bufferCount;
    bool mappingsReady;
};

template <typename AllocateBuffers, typename DeallocateBuffers>
std::optional<v4l2_memory> allocateV4L2EncoderQueueBuffers(size_t requiredBufferCount,
                                                           AllocateBuffers&& allocateBuffers,
                                                           DeallocateBuffers&& deallocateBuffers) {
    if (requiredBufferCount == 0) {
        return std::nullopt;
    }

    constexpr std::array<v4l2_memory, 2> kMemoryPreference = {
            V4L2_MEMORY_MMAP,
            V4L2_MEMORY_USERPTR,
    };
    for (const v4l2_memory memory : kMemoryPreference) {
        const V4L2EncoderQueueAllocationResult result = allocateBuffers(memory);
        if (result.bufferCount >= requiredBufferCount && result.mappingsReady) {
            return memory;
        }
        if (result.bufferCount != 0 && !deallocateBuffers()) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_ENCODER_QUEUE_MEMORY_POLICY_H
