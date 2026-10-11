// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define ATRACE_TAG ATRACE_TAG_VIDEO
#define LOG_TAG "FormatConverter"

#include <v4l2_codec2/common/FormatConverter.h>

#include <inttypes.h>

#include <memory>
#include <string>

#include <C2AllocatorGralloc.h>
#include <C2PlatformSupport.h>
#include <android/hardware/graphics/common/1.0/types.h>
#include <inttypes.h>
#include <libyuv.h>
#include <ui/GraphicBuffer.h>
#include <utils/Log.h>
#include <utils/Trace.h>

#include <v4l2_codec2/common/VideoTypes.h>

namespace android {

namespace {
// The helper function to copy a plane pixel by pixel. It assumes bytesPerPixel is 1.
void copyPlaneByPixel(const uint8_t* src, int srcStride, int srcColInc, uint8_t* dst, int dstStride,
                      int dstColInc, int width, int height) {
    for (int row = 0; row < height; row++) {
        const uint8_t* srcRow = src;
        uint8_t* dstRow = dst;
        for (int col = 0; col < width; col++) {
            memcpy(dstRow, srcRow, 1);
            srcRow += srcColInc;
            dstRow += dstColInc;
        }
        src += srcStride;
        dst += dstStride;
    }
}

}  // namespace

ImplDefinedToRGBXMap::ImplDefinedToRGBXMap(sp<GraphicBuffer> buf, uint8_t* addr, int rowInc)
      : mBuffer(std::move(buf)), mAddr(addr), mRowInc(rowInc) {}

ImplDefinedToRGBXMap::~ImplDefinedToRGBXMap() {
    mBuffer->unlock();
}

// static
std::unique_ptr<ImplDefinedToRGBXMap> ImplDefinedToRGBXMap::create(
        const C2ConstGraphicBlock& block) {
    uint32_t width, height, format, stride, igbpSlot, generation;
    uint64_t usage, igbpId;
    android::_UnwrapNativeCodec2GrallocMetadata(block.handle(), &width, &height, &format, &usage,
                                                &stride, &generation, &igbpId, &igbpSlot);

    if (format != HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        ALOGE("The original format (=%u) is not IMPLEMENTATION_DEFINED", format);
        return nullptr;
    }

    native_handle_t* grallocHandle = android::UnwrapNativeCodec2GrallocHandle(block.handle());
    sp<GraphicBuffer> buf = new GraphicBuffer(grallocHandle, GraphicBuffer::CLONE_HANDLE, width,
                                              height, format, 1, usage, stride);
    native_handle_delete(grallocHandle);

    void* pointer = nullptr;
    int32_t status = buf->lock(GRALLOC_USAGE_SW_READ_OFTEN, &pointer);
    if (status != OK) {
        ALOGE("Failed to lock buffer as IMPLEMENTATION_DEFINED format");
        return nullptr;
    }

    uint8_t* addr = reinterpret_cast<uint8_t*>(pointer);
    int rowInc = static_cast<int>(stride * 4);  // RGBX 4-byte data per pixel
    ALOGD("Parsed input format IMPLEMENTATION_DEFINED to RGBX_8888");
    return std::unique_ptr<ImplDefinedToRGBXMap>(
            new ImplDefinedToRGBXMap(std::move(buf), addr, rowInc));
}

// static
std::unique_ptr<FormatConverter> FormatConverter::create(VideoPixelFormat outFormat,
                                                         const ui::Size& visibleSize,
                                                         uint32_t inputCount,
                                                         const ui::Size& codedSize) {
    if (outFormat != VideoPixelFormat::NV12) {
        ALOGE("Unsupported output format: %d", static_cast<int32_t>(outFormat));
        return nullptr;
    }

    std::unique_ptr<FormatConverter> converter(new FormatConverter);
    if (converter->initialize(outFormat, visibleSize, inputCount, codedSize) != C2_OK) {
        ALOGE("Failed to initialize FormatConverter");
        return nullptr;
    }
    return converter;
}

c2_status_t FormatConverter::initialize(VideoPixelFormat outFormat, const ui::Size& visibleSize,
                                        uint32_t inputCount, const ui::Size& codedSize) {
    ALOGV("initialize(out_format=%s, visible_size=%dx%d, input_count=%u, coded_size=%dx%d)",
          videoPixelFormatToString(outFormat).c_str(), visibleSize.width, visibleSize.height,
          inputCount, codedSize.width, codedSize.height);

    mOutFormat = outFormat;
    mVisibleSize = visibleSize;
    mCodedSize = codedSize;

    mTempPlaneU =
            std::unique_ptr<uint8_t[]>(new uint8_t[mVisibleSize.width * mVisibleSize.height / 4]);
    mTempPlaneV =
            std::unique_ptr<uint8_t[]>(new uint8_t[mVisibleSize.width * mVisibleSize.height / 4]);

    // Allocate graphic blocks for format conversion.
    uint32_t requested_buffer_count = std::max(1u, inputCount);
    c2_status_t status = allocateBuffers(requested_buffer_count);
    if (status != C2_OK) {
        ALOGE("Failed to allocate buffers (error: %d)", status);
        return status;
    }

    return C2_OK;
}

c2_status_t FormatConverter::allocateBuffers(uint32_t count) {
    ALOGV("Allocating %u buffers (format: %s, visible size: %dx%d, coded size: %dx%d)", count,
          videoPixelFormatToString(mOutFormat).c_str(), mVisibleSize.width, mVisibleSize.height,
          mCodedSize.width, mCodedSize.height);

    // The guest allocator (minigbm with the virtio_gpu backend) cannot allocate graphic buffers for
    // any YUV format, so we allocate linear blocks from the basic linear pool instead. These are
    // backed by the dma-heap and can be mapped for CPU access.
    std::shared_ptr<C2BlockPool> pool;
    c2_status_t status = GetCodec2BlockPool(C2BlockPool::BASIC_LINEAR, nullptr, &pool);
    if (status != C2_OK) {
        ALOGE("Failed to get basic linear block pool (error: %d)", status);
        return C2_NO_MEMORY;
    }

    // NV12 layout: one Y plane of stride*height bytes followed by one interleaved UV plane of
    // stride*height/2 bytes.
    const uint32_t stride = static_cast<uint32_t>(mCodedSize.width);
    const uint32_t height = static_cast<uint32_t>(mCodedSize.height);
    const uint32_t blockSize = (stride * height * 3 + 1) / 2;

    for (uint32_t i = 0; i < count; i++) {
        std::shared_ptr<C2LinearBlock> block;
        status = pool->fetchLinearBlock(
                blockSize, C2MemoryUsage(C2MemoryUsage::CPU_READ | C2MemoryUsage::CPU_WRITE),
                &block);
        if (status != C2_OK) {
            ALOGE("Failed to fetch linear block (error: %d)", status);
            return C2_NO_MEMORY;
        }
        // Clear the block once on allocation so any padding region stays at a known value.
        C2WriteView view = block->map().get();
        if (view.error() != C2_OK) {
            ALOGE("Failed to map linear block for clearing (error: %d)", view.error());
            return C2_NO_MEMORY;
        }
        memset(view.data(), 0, view.size());
        mGraphicBlocks.emplace_back(new BlockEntry(std::move(block)));
        mAvailableQueue.push(mGraphicBlocks.back().get());
    }

    return C2_OK;
}

c2_status_t FormatConverter::convertBlock(uint64_t frameIndex,
                                          const C2ConstGraphicBlock& inputBlock,
                                          std::shared_ptr<C2LinearBlock>* convertedBlock) {
    ATRACE_CALL();
    const C2GraphicView& inputView = inputBlock.map().get();
    C2PlanarLayout inputLayout = inputView.layout();

    // Determine the input buffer pixel format.
    VideoPixelFormat inputFormat = VideoPixelFormat::UNKNOWN;
    std::unique_ptr<ImplDefinedToRGBXMap> idMap;
    if (inputLayout.type == C2PlanarLayout::TYPE_YUV) {
        if (inputLayout.rootPlanes == 3) {
            inputFormat = VideoPixelFormat::YV12;
        } else if (inputLayout.rootPlanes == 2) {
            const uint8_t* const* data = inputView.data();
            inputFormat = (data[C2PlanarLayout::PLANE_V] > data[C2PlanarLayout::PLANE_U])
                                  ? VideoPixelFormat::NV12
                                  : VideoPixelFormat::NV21;
        }
    } else if (inputLayout.type == C2PlanarLayout::TYPE_RGB) {
        inputFormat = VideoPixelFormat::ABGR;
    } else if (static_cast<uint32_t>(inputLayout.type) == 0u) {
        // The above layout() cannot fill layout information and sets it to 0 instead if the input
        // format is IMPLEMENTATION_DEFINED and its backed format is RGB. We fill the layout by
        // using ImplDefinedToRGBXMap in this case.
        idMap = ImplDefinedToRGBXMap::create(inputBlock);
        if (!idMap) {
            ALOGE("Unable to parse RGBX_8888 from IMPLEMENTATION_DEFINED");
            return C2_CORRUPTED;
        }
        // There is only RGBA_8888 specified in C2AllocationGralloc::map(), no BGRA_8888. Maybe
        // BGRA_8888 is not used now?
        inputFormat = VideoPixelFormat::ABGR;
        inputLayout.type = C2PlanarLayout::TYPE_RGB;
    } else {
        ALOGE("Failed to determine input pixel format: %u", inputLayout.type);
        return C2_CORRUPTED;
    }

    // Note: there is no zero-copy path anymore, the converted result always lives in a linear block
    // which the encoder returns through returnBlock() when it is done with it.

    if (!isReady()) {
        ALOGV("There is no available block for conversion");
        return C2_NO_MEMORY;
    }

    BlockEntry* entry = mAvailableQueue.front();
    std::shared_ptr<C2LinearBlock> outputBlock = entry->mBlock;

    C2WriteView outputView = outputBlock->map().get();
    if (outputView.error() != C2_OK) {
        ALOGE("Failed to map linear output block (error: %d)", outputView.error());
        return C2_CORRUPTED;
    }
    uint8_t* dstY = outputView.data();
    const int dstStrideY = mCodedSize.width;
    uint8_t* dstUV = dstY + static_cast<size_t>(dstStrideY) * mCodedSize.height;
    const int dstStrideUV = mCodedSize.width;

    if (inputLayout.type == C2PlanarLayout::TYPE_YUV) {
        const uint8_t* srcY = inputView.data()[C2PlanarLayout::PLANE_Y];
        const uint8_t* srcU = inputView.data()[C2PlanarLayout::PLANE_U];
        const uint8_t* srcV = inputView.data()[C2PlanarLayout::PLANE_V];
        const int srcStrideY = inputLayout.planes[C2PlanarLayout::PLANE_Y].rowInc;
        const int srcStrideU = inputLayout.planes[C2PlanarLayout::PLANE_U].rowInc;
        const int srcStrideV = inputLayout.planes[C2PlanarLayout::PLANE_V].rowInc;

        switch (inputFormat) {
        case VideoPixelFormat::YV12:
            libyuv::I420ToNV12(srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV, dstY,
                               dstStrideY, dstUV, dstStrideUV, mVisibleSize.width,
                               mVisibleSize.height);
            break;
        case VideoPixelFormat::NV12:
            // The input is already NV12: copy the Y plane row by row and then the interleaved UV
            // plane, which holds |visible width| bytes per row for |height / 2| rows.
            libyuv::CopyPlane(srcY, srcStrideY, dstY, dstStrideY, mVisibleSize.width,
                              mVisibleSize.height);
            libyuv::CopyPlane(srcU, srcStrideU, dstUV, dstStrideUV, mVisibleSize.width,
                              mVisibleSize.height / 2);
            break;
        case VideoPixelFormat::NV21:
            ALOGV("%s(): Converting PIXEL_FORMAT_NV21 -> PIXEL_FORMAT_NV12", __func__);
            libyuv::CopyPlane(srcY, srcStrideY, dstY, dstStrideY, mVisibleSize.width,
                              mVisibleSize.height);
            copyPlaneByPixel(srcU, srcStrideU, 2, dstUV, dstStrideUV, 2, mVisibleSize.width / 2,
                             mVisibleSize.height / 2);
            copyPlaneByPixel(srcV, srcStrideV, 2, dstUV + 1, dstStrideUV, 2, mVisibleSize.width / 2,
                             mVisibleSize.height / 2);
            break;
        default:
            ALOGE("Unsupported pixel format conversion from %s to %s",
                  videoPixelFormatToString(inputFormat).c_str(),
                  videoPixelFormatToString(mOutFormat).c_str());
            return C2_CORRUPTED;
        }
    } else if (inputLayout.type == C2PlanarLayout::TYPE_RGB) {
        const uint8_t* srcRGB = (idMap) ? idMap->addr() : inputView.data()[C2PlanarLayout::PLANE_R];
        const int srcStrideRGB =
                (idMap) ? idMap->rowInc() : inputLayout.planes[C2PlanarLayout::PLANE_R].rowInc;

        if (inputFormat != VideoPixelFormat::ABGR) {
            ALOGE("Unsupported pixel format conversion from %s to %s",
                  videoPixelFormatToString(inputFormat).c_str(),
                  videoPixelFormatToString(mOutFormat).c_str());
            return C2_CORRUPTED;
        }
        // There is no libyuv function to convert ABGR to NV12. Therefore, we first convert to
        // I420 on dst-Y plane and temporary U/V plane. Then we copy U and V pixels from
        // temporary planes to dst-UV interleavedly.
        const int tempStride = mVisibleSize.width / 2;
        libyuv::ABGRToI420(srcRGB, srcStrideRGB, dstY, dstStrideY, mTempPlaneU.get(), tempStride,
                           mTempPlaneV.get(), tempStride, mVisibleSize.width, mVisibleSize.height);
        libyuv::MergeUVPlane(mTempPlaneU.get(), tempStride, mTempPlaneV.get(), tempStride, dstUV,
                             dstStrideUV, mVisibleSize.width / 2, mVisibleSize.height / 2);
    } else {
        ALOGE("Unsupported input layout type");
        return C2_CORRUPTED;
    }

    ALOGV("convertBlock(frame_index=%" PRIu64 ", format=%s)", frameIndex,
          videoPixelFormatToString(inputFormat).c_str());
    entry->mAssociatedFrameIndex = frameIndex;
    mAvailableQueue.pop();

    *convertedBlock = std::move(outputBlock);
    return C2_OK;
}

c2_status_t FormatConverter::returnBlock(uint64_t frameIndex) {
    ALOGV("returnBlock(frame_index=%" PRIu64 ")", frameIndex);

    auto iter = std::find_if(mGraphicBlocks.begin(), mGraphicBlocks.end(),
                             [frameIndex](const std::unique_ptr<BlockEntry>& be) {
                                 return be->mAssociatedFrameIndex == frameIndex;
                             });
    if (iter == mGraphicBlocks.end()) {
        ALOGE("Failed to find graphic block by converted/zero-copied frame index: %" PRIu64 "",
              frameIndex);
        return C2_BAD_INDEX;
    }

    if ((*iter)->mBlock) {
        // Returned block is format converted.
        (*iter)->mAssociatedFrameIndex = kNoFrameAssociated;
        mAvailableQueue.push(iter->get());
    } else {
        // Returned block is zero-copied.
        mGraphicBlocks.erase(iter);
    }
    return C2_OK;
}

}  // namespace android
