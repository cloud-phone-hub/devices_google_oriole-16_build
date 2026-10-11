// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODE_MEMORY_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODE_MEMORY_H

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <vector>

namespace android {

struct V4L2InputUserPtr {
    void* address;
    size_t length;
    size_t bytesUsed;
    size_t dataOffset;
};

// Stage a fixable USERPTR input buffer for drivers that require plane length to
// equal the negotiated sizeimage while bytesused carries the compressed payload.
std::optional<V4L2InputUserPtr> stageV4L2InputUserPtr(const uint8_t* mappedData,
                                                      size_t mappedCapacity, size_t payloadOffset,
                                                      size_t payloadSize, size_t planeLength,
                                                      std::vector<uint8_t>* stagingStorage);

struct V4L2YuvUserPtrLayout {
    void* y;
    void* u;
    void* v;
    size_t rootPlanes;
    size_t yStride;
    size_t uStride;
    size_t vStride;
    size_t yColInc;
    size_t uColInc;
    size_t vColInc;
};

std::optional<std::vector<void*>> selectV4L2OutputUserPtrs(uint32_t pixelFormat, size_t planeCount,
                                                           const std::vector<size_t>& planeStrides,
                                                           const V4L2YuvUserPtrLayout& layout);

struct V4L2NV12UserPtrPlanes {
    size_t yPlane;
    size_t yOffset;
    size_t yLength;
    size_t yStride;
    size_t uvPlane;
    size_t uvOffset;
    size_t uvLength;
    size_t uvStride;
};

struct V4L2YV12Destination {
    uint8_t* y;
    uint8_t* u;
    uint8_t* v;
    size_t yRowStride;
    size_t uRowStride;
    size_t vRowStride;
    size_t yColStride;
    size_t uColStride;
    size_t vColStride;
};

bool isV4L2NV12UserPtrPixelFormat(uint32_t pixelFormat);

std::optional<V4L2NV12UserPtrPlanes> resolveV4L2NV12UserPtrPlanes(
        uint32_t pixelFormat, size_t height, const std::vector<size_t>& planeStrides,
        const std::vector<size_t>& planeSizes, const std::vector<size_t>& stagingSizes);

bool copyV4L2NV12ToYV12(const uint8_t* sourceY, size_t sourceYSize, size_t sourceYStride,
                        const uint8_t* sourceUV, size_t sourceUVSize, size_t sourceUVStride,
                        size_t width, size_t height, const V4L2YV12Destination& destination);

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_DECODE_MEMORY_H
