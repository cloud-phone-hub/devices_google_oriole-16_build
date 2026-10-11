// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_CAPTURE_FETCH_POLICY_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_CAPTURE_FETCH_POLICY_H

#include <stddef.h>

#include <limits>

namespace android {

enum class V4L2CaptureFetchOrigin {
    Asynchronous,
    Synchronous,
};

struct V4L2CaptureQueueSizing {
    size_t allocationCount;
    size_t refillLowWatermark;
};

constexpr bool shouldScheduleNextCaptureFetch(V4L2CaptureFetchOrigin origin) {
    return origin == V4L2CaptureFetchOrigin::Asynchronous;
}

constexpr V4L2CaptureQueueSizing getV4L2CaptureQueueSizing(size_t driverRequiredBuffers,
                                                           size_t pipelineSlackBuffers,
                                                           size_t pipelineRequiredBuffers) {
    const size_t refillLowWatermark = driverRequiredBuffers == 0 ? 1 : driverRequiredBuffers;
    const size_t maxSize = std::numeric_limits<size_t>::max();
    const size_t driverPipelineBuffers = pipelineSlackBuffers > maxSize - refillLowWatermark
                                                 ? maxSize
                                                 : refillLowWatermark + pipelineSlackBuffers;
    const size_t minimumAllocation =
            driverPipelineBuffers > refillLowWatermark ? driverPipelineBuffers : refillLowWatermark;
    const size_t allocationCount = pipelineRequiredBuffers > minimumAllocation
                                           ? pipelineRequiredBuffers
                                           : minimumAllocation;
    return {allocationCount, refillLowWatermark};
}

constexpr bool shouldFetchV4L2CaptureBuffer(size_t freeBuffers, size_t queuedBuffers,
                                            size_t refillLowWatermark) {
    const size_t lowWatermark = refillLowWatermark == 0 ? 1 : refillLowWatermark;
    return freeBuffers > 0 && queuedBuffers < lowWatermark;
}

constexpr size_t getV4L2CaptureInitialFillCount(size_t freeBuffers, size_t queuedBuffers,
                                                size_t refillLowWatermark) {
    const size_t lowWatermark = refillLowWatermark == 0 ? 1 : refillLowWatermark;
    if (freeBuffers == 0 || queuedBuffers >= lowWatermark) {
        return 0;
    }
    const size_t missingBuffers = lowWatermark - queuedBuffers;
    return freeBuffers < missingBuffers ? freeBuffers : missingBuffers;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_CAPTURE_FETCH_POLICY_H
