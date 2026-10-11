// Copyright 2026 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ANDROID_V4L2_CODEC2_V4L2_V4L2_BUILD_FEATURES_H
#define ANDROID_V4L2_CODEC2_V4L2_V4L2_BUILD_FEATURES_H

#ifndef V4L2_CODEC2_ENABLE_ENCODERS
#define V4L2_CODEC2_ENABLE_ENCODERS 1
#endif

#ifndef V4L2_CODEC2_ENABLE_VP8_DECODER
#define V4L2_CODEC2_ENABLE_VP8_DECODER 1
#endif

#ifndef V4L2_CODEC2_ENABLE_VP9_DECODER
#define V4L2_CODEC2_ENABLE_VP9_DECODER 1
#endif

#ifndef V4L2_CODEC2_ENABLE_SECURE_DECODERS
#define V4L2_CODEC2_ENABLE_SECURE_DECODERS 1
#endif

namespace android {

struct V4L2BuildFeatures {
    static constexpr bool kEncoders = V4L2_CODEC2_ENABLE_ENCODERS != 0;
    static constexpr bool kH264Decoder = true;
    static constexpr bool kHEVCDecoder = true;
    static constexpr bool kVP8Decoder = V4L2_CODEC2_ENABLE_VP8_DECODER != 0;
    static constexpr bool kVP9Decoder = V4L2_CODEC2_ENABLE_VP9_DECODER != 0;
    static constexpr bool kSecureDecoders = V4L2_CODEC2_ENABLE_SECURE_DECODERS != 0;
};

}  // namespace android

#endif  // ANDROID_V4L2_CODEC2_V4L2_V4L2_BUILD_FEATURES_H
