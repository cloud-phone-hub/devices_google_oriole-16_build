// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_MEMORY_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_MEMORY_H

#include <cstdint>
#include <functional>
#include <optional>

#include <linux/videodev2.h>

namespace android {

void setV4L2BufferMemoryType(v4l2_buffer& buffer, v4l2_memory memory);

std::optional<v4l2_memory> selectV4L2MemoryType(uint32_t capabilities,
                                                v4l2_memory probedMemory);

using V4L2RequestBuffers = std::function<int(v4l2_requestbuffers*)>;
std::optional<v4l2_memory> probeV4L2MemoryType(v4l2_buf_type type,
                                               const V4L2RequestBuffers& requestBuffers);

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_MEMORY_H
