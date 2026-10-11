// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_ENCODER_INPUT_USAGE_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_ENCODER_INPUT_USAGE_H

#include <cstdint>

namespace android {

constexpr uint64_t buildV4L2EncoderInputUsage(uint64_t videoEncoderUsage, uint64_t cpuReadUsage) {
    return videoEncoderUsage | cpuReadUsage;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_ENCODER_INPUT_USAGE_H
