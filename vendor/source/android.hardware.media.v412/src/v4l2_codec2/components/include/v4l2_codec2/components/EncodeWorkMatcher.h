// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_COMPONENTS_ENCODE_WORK_MATCHER_H
#define ANDROID_V4L2_CODEC2_COMPONENTS_ENCODE_WORK_MATCHER_H

namespace android {

inline bool shouldMatchEncodedOutput(bool endOfStream, bool hasInputBuffer) {
    return !endOfStream || hasInputBuffer;
}

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_COMPONENTS_ENCODE_WORK_MATCHER_H
