// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)

#include <v4l2_codec2/v4l2/V4L2DecodeMemory.h>

#include <linux/videodev2.h>

#include <cstring>
#include <limits>

namespace android {
namespace {

std::optional<size_t> requiredPlaneBytes(size_t rows, size_t columns, size_t rowStride,
                                         size_t columnStride) {
    if (rows == 0 || columns == 0 || rowStride == 0 || columnStride == 0 ||
        columns - 1 > (std::numeric_limits<size_t>::max() - 1) / columnStride) {
        return std::nullopt;
    }
    const size_t lastRowBytes = (columns - 1) * columnStride + 1;
    if (rowStride < lastRowBytes ||
        rows - 1 > (std::numeric_limits<size_t>::max() - lastRowBytes) / rowStride) {
        return std::nullopt;
    }
    return (rows - 1) * rowStride + lastRowBytes;
}

}  // namespace

std::optional<V4L2InputUserPtr> stageV4L2InputUserPtr(const uint8_t* mappedData,
                                                      size_t mappedCapacity, size_t payloadOffset,
                                                      size_t payloadSize, size_t planeLength,
                                                      std::vector<uint8_t>* stagingStorage) {
    if (mappedData == nullptr || stagingStorage == nullptr || payloadSize == 0 ||
        planeLength == 0 || payloadSize > planeLength || payloadOffset > mappedCapacity ||
        payloadSize > mappedCapacity - payloadOffset) {
        return std::nullopt;
    }

    stagingStorage->assign(planeLength, 0);
    std::memcpy(stagingStorage->data(), mappedData + payloadOffset, payloadSize);
    return V4L2InputUserPtr{
            .address = stagingStorage->data(),
            .length = planeLength,
            .bytesUsed = payloadSize,
            .dataOffset = 0,
    };
}

std::optional<std::vector<void*>> selectV4L2OutputUserPtrs(uint32_t pixelFormat, size_t planeCount,
                                                           const std::vector<size_t>& planeStrides,
                                                           const V4L2YuvUserPtrLayout& layout) {
    if (pixelFormat != V4L2_PIX_FMT_NV12M || planeCount != 2 || planeStrides.size() != 2 ||
        layout.rootPlanes != 2 || layout.y == nullptr || layout.u == nullptr ||
        layout.v == nullptr) {
        return std::nullopt;
    }
    if (layout.yStride != planeStrides[0] || layout.uStride != planeStrides[1] ||
        layout.vStride != planeStrides[1] || layout.yColInc != 1 || layout.uColInc != 2 ||
        layout.vColInc != 2) {
        return std::nullopt;
    }

    auto* u = static_cast<uint8_t*>(layout.u);
    if (static_cast<uint8_t*>(layout.v) != u + 1) {
        return std::nullopt;
    }

    return std::vector<void*>{layout.y, layout.u};
}

bool isV4L2NV12UserPtrPixelFormat(uint32_t pixelFormat) {
    return pixelFormat == V4L2_PIX_FMT_NV12 || pixelFormat == V4L2_PIX_FMT_NV12M;
}

std::optional<V4L2NV12UserPtrPlanes> resolveV4L2NV12UserPtrPlanes(
        uint32_t pixelFormat, size_t height, const std::vector<size_t>& planeStrides,
        const std::vector<size_t>& planeSizes, const std::vector<size_t>& stagingSizes) {
    if (height == 0 || height % 2 != 0 || planeStrides.size() != planeSizes.size() ||
        planeSizes.size() != stagingSizes.size()) {
        return std::nullopt;
    }

    if (pixelFormat == V4L2_PIX_FMT_NV12M) {
        if (planeSizes.size() != 2 || planeStrides[0] == 0 || planeStrides[1] == 0 ||
            planeSizes[0] == 0 || planeSizes[1] == 0 || stagingSizes[0] < planeSizes[0] ||
            stagingSizes[1] < planeSizes[1]) {
            return std::nullopt;
        }
        return V4L2NV12UserPtrPlanes{
                .yPlane = 0,
                .yOffset = 0,
                .yLength = planeSizes[0],
                .yStride = planeStrides[0],
                .uvPlane = 1,
                .uvOffset = 0,
                .uvLength = planeSizes[1],
                .uvStride = planeStrides[1],
        };
    }

    if (pixelFormat != V4L2_PIX_FMT_NV12 || planeSizes.size() != 1 || planeStrides[0] == 0 ||
        planeSizes[0] == 0) {
        return std::nullopt;
    }
    const size_t stride = planeStrides[0];
    if (height > std::numeric_limits<size_t>::max() / stride) {
        return std::nullopt;
    }
    const size_t yLength = stride * height;
    const size_t uvRows = height / 2;
    if (uvRows > std::numeric_limits<size_t>::max() / stride) {
        return std::nullopt;
    }
    const size_t uvLength = stride * uvRows;
    if (yLength > std::numeric_limits<size_t>::max() - uvLength) {
        return std::nullopt;
    }
    const size_t totalLength = yLength + uvLength;
    if (planeSizes[0] < totalLength || stagingSizes[0] < totalLength) {
        return std::nullopt;
    }
    return V4L2NV12UserPtrPlanes{
            .yPlane = 0,
            .yOffset = 0,
            .yLength = yLength,
            .yStride = stride,
            .uvPlane = 0,
            .uvOffset = yLength,
            .uvLength = uvLength,
            .uvStride = stride,
    };
}

bool copyV4L2NV12ToYV12(const uint8_t* sourceY, size_t sourceYSize, size_t sourceYStride,
                        const uint8_t* sourceUV, size_t sourceUVSize, size_t sourceUVStride,
                        size_t width, size_t height, const V4L2YV12Destination& destination) {
    if (sourceY == nullptr || sourceUV == nullptr || destination.y == nullptr ||
        destination.u == nullptr || destination.v == nullptr || width == 0 || height == 0 ||
        width % 2 != 0 || height % 2 != 0) {
        return false;
    }

    const size_t chromaWidth = width / 2;
    const size_t chromaHeight = height / 2;
    const auto requiredSourceY = requiredPlaneBytes(height, width, sourceYStride, 1);
    const auto requiredSourceUV = requiredPlaneBytes(chromaHeight, width, sourceUVStride, 1);
    if (!requiredSourceY || !requiredSourceUV || *requiredSourceY > sourceYSize ||
        *requiredSourceUV > sourceUVSize ||
        !requiredPlaneBytes(height, width, destination.yRowStride, destination.yColStride) ||
        !requiredPlaneBytes(chromaHeight, chromaWidth, destination.uRowStride,
                            destination.uColStride) ||
        !requiredPlaneBytes(chromaHeight, chromaWidth, destination.vRowStride,
                            destination.vColStride)) {
        return false;
    }

    for (size_t row = 0; row < height; ++row) {
        const uint8_t* source = sourceY + row * sourceYStride;
        uint8_t* target = destination.y + row * destination.yRowStride;
        if (destination.yColStride == 1) {
            std::memcpy(target, source, width);
            continue;
        }
        for (size_t column = 0; column < width; ++column) {
            target[column * destination.yColStride] = source[column];
        }
    }

    for (size_t row = 0; row < chromaHeight; ++row) {
        const uint8_t* source = sourceUV + row * sourceUVStride;
        uint8_t* targetU = destination.u + row * destination.uRowStride;
        uint8_t* targetV = destination.v + row * destination.vRowStride;
        for (size_t column = 0; column < chromaWidth; ++column) {
            targetU[column * destination.uColStride] = source[column * 2];
            targetV[column * destination.vColStride] = source[column * 2 + 1];
        }
    }
    return true;
}

}  // namespace android
