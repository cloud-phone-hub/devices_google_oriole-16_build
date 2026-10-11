// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODER_INPUT_BUFFER_POLICY_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODER_INPUT_BUFFER_POLICY_H

#include <stddef.h>

#include <linux/videodev2.h>

namespace android {

// Codec2's pipeline delay of three needs four queue slots. USERPTR QBUF pins
// the complete negotiated sizeimage, so deeper queues share a 32 MiB target.
constexpr size_t kV4L2DecoderMaxInputBufferCount = 16;
constexpr size_t kV4L2DecoderMinInputBufferCount = 4;
constexpr size_t kV4L2DecoderUserPtrStagingTargetBytes = 32U * 1024U * 1024U;

inline size_t selectV4L2DecoderInputBufferCount(v4l2_memory memory, size_t planeLength) {
    if (memory != V4L2_MEMORY_USERPTR) {
        return kV4L2DecoderMaxInputBufferCount;
    }
    if (planeLength == 0) {
        return kV4L2DecoderMinInputBufferCount;
    }

    const size_t budgetCount = kV4L2DecoderUserPtrStagingTargetBytes / planeLength;
    if (budgetCount < kV4L2DecoderMinInputBufferCount) {
        return kV4L2DecoderMinInputBufferCount;
    }
    if (budgetCount > kV4L2DecoderMaxInputBufferCount) {
        return kV4L2DecoderMaxInputBufferCount;
    }
    return budgetCount;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODER_INPUT_BUFFER_POLICY_H
