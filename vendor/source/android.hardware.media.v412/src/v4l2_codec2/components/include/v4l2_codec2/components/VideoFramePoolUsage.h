// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_POOL_USAGE_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_POOL_USAGE_H

#include <cstdint>

namespace android {

constexpr uint64_t buildVideoFramePoolUsage(uint64_t videoDecoderUsage, uint64_t protectedUsage,
                                            uint64_t cpuAccessUsage, bool isSecure,
                                            bool cpuAccessible) {
    if (isSecure) {
        return videoDecoderUsage | protectedUsage;
    }
    if (cpuAccessible) {
        return videoDecoderUsage | cpuAccessUsage;
    }
    return videoDecoderUsage;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_VIDEO_FRAME_POOL_USAGE_H
