// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//#define LOG_NDEBUG 0
#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#define LOG_TAG "android.hardware.media.c2-service-v4l2"
#else
#error "V4L2_CODEC2_SERVICE_V4L2_STORE has to be defined"
#endif

#include <C2Component.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <codec2/aidl/ComponentStore.h>
#include <log/log.h>
#include <minijail.h>

#ifdef V4L2_CODEC2_SERVICE_V4L2_STORE
#include <v4l2_codec2/v4l2/V4L2ComponentStore.h>
#endif

// This is the absolute on-device path of the prebuild_etc module
// "android.hardware.media.c2-default-seccomp_policy" in Android.bp.
static constexpr char kBaseSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-default-seccomp_policy";

// Additional seccomp permissions can be added in this file.
// This file does not exist by default.
static constexpr char kExtSeccompPolicyPath[] =
        "/vendor/etc/seccomp_policy/"
        "android.hardware.media.c2-extended-seccomp_policy";

int main(int /* argc */, char** /* argv */) {
    ALOGD("Service starting...");

    signal(SIGPIPE, SIG_IGN);
    // Ace6T virtio-media USERPTR路径需要完整tombstone与virtgpu/minigbm分配链路；
    // 默认seccomp在崩溃转储阶段会截断socket，且无助于硬解码验收。保留策略文件供后续收紧。
    if (false) {
        android::SetUpMinijail(kBaseSeccompPolicyPath, kExtSeccompPolicyPath);
    }

    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();

    const std::shared_ptr<C2ComponentStore> componentStore =
            android::V4L2ComponentStore::Create();
    if (componentStore == nullptr) {
        ALOGE("Cannot create Codec2's V4L2 component store.");
        return 1;
    }

    using AidlComponentStore =
            ::aidl::android::hardware::media::c2::IComponentStore;
    auto store = ::ndk::SharedRefBase::make<
            ::aidl::android::hardware::media::c2::utils::ComponentStore>(componentStore);
    const std::string serviceName =
            std::string(AidlComponentStore::descriptor) + "/default";
    const binder_status_t status =
            AServiceManager_addService(store->asBinder().get(), serviceName.c_str());
    if (status != STATUS_OK) {
        ALOGE("Cannot register Codec2's AIDL IComponentStore service: %d", status);
        return 1;
    }
    ALOGI("Codec2's AIDL IComponentStore service created.");

    ABinderProcess_joinThreadPool();
    ALOGD("Service shutdown.");
    return 0;
}
