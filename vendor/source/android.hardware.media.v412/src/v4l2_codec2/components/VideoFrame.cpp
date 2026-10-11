// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#define LOG_TAG "VideoFrame"

#include <v4l2_codec2/components/VideoFrame.h>

#include <C2AllocatorGralloc.h>
#include <log/log.h>

namespace android {

// static
std::unique_ptr<VideoFrame> VideoFrame::Create(std::shared_ptr<C2GraphicBlock> block) {
    if (!block) return nullptr;

    std::vector<int> fds;
    const C2Handle* const handle = block->handle();
    for (int i = 0; i < handle->numFds; i++) {
        fds.emplace_back(handle->data[i]);
    }

    return std::unique_ptr<VideoFrame>(new VideoFrame(std::move(block), std::move(fds)));
}

VideoFrame::VideoFrame(std::shared_ptr<C2GraphicBlock> block, std::vector<int> fds)
      : mGraphicBlock(std::move(block)), mFds(fds) {}

VideoFrame::~VideoFrame() = default;

const std::vector<int>& VideoFrame::getFDs() const {
    return mFds;
}

C2GraphicView* VideoFrame::mapGraphicBlock() {
    if (mGraphicView) {
        return mGraphicView.get();
    }

    auto view = std::make_unique<C2GraphicView>(mGraphicBlock->map().get());
    if (view->error() != C2_OK) {
        ALOGE("Failed to map graphic block: %d", view->error());
        return nullptr;
    }
    mGraphicView = std::move(view);
    return mGraphicView.get();
}

void VideoFrame::setVisibleRect(const Rect& visibleRect) {
    mVisibleRect = visibleRect;
}

const Rect& VideoFrame::getVisibleRect() const {
    return mVisibleRect;
}

void VideoFrame::setBitstreamId(int32_t bitstreamId) {
    mBitstreamId = bitstreamId;
}

int32_t VideoFrame::getBitstreamId() const {
    return mBitstreamId;
}

C2ConstGraphicBlock VideoFrame::getGraphicBlock() {
    mGraphicView.reset();
    return mGraphicBlock->share(C2Rect(mVisibleRect.width(), mVisibleRect.height()), C2Fence());
}

}  // namespace android
