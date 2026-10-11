#include <android/binder_ibinder.h>
#include <android/binder_status.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/camera/common/CameraDeviceStatus.h>
#include <aidl/android/hardware/camera/common/CameraMetadataType.h>
#include <aidl/android/hardware/camera/common/CameraResourceCost.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>
#include <aidl/android/hardware/camera/common/VendorTag.h>
#include <aidl/android/hardware/camera/common/VendorTagSection.h>
#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/BufferStatus.h>
#include <aidl/android/hardware/camera/device/CameraMetadata.h>
#include <aidl/android/hardware/camera/device/CameraOfflineSessionInfo.h>
#include <aidl/android/hardware/camera/device/CaptureRequest.h>
#include <aidl/android/hardware/camera/device/CaptureResult.h>
#include <aidl/android/hardware/camera/device/ConfigureStreamsRet.h>
#include <aidl/android/hardware/camera/device/HalStream.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraInjectionSession.h>
#include <aidl/android/hardware/camera/device/ICameraOfflineSession.h>
#include <aidl/android/hardware/camera/device/NotifyMsg.h>
#include <aidl/android/hardware/camera/device/RequestTemplate.h>
#include <aidl/android/hardware/camera/device/ShutterMsg.h>
#include <aidl/android/hardware/camera/device/StreamConfiguration.h>
#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>
#include <aidl/android/hardware/camera/provider/CameraIdAndStreamCombination.h>
#include <aidl/android/hardware/camera/provider/ConcurrentCameraIdCombination.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <aidl/android/hardware/common/fmq/GrantorDescriptor.h>
#include <aidl/android/hardware/common/fmq/MQDescriptor.h>
#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>

#include <android/log.h>
#include <camera_metadata_hidden.h>
#include <fcntl.h>
#include <linux/ashmem.h>
#include <system/camera_metadata.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <errno.h>
#include <memory>
#include <mutex>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

using aidl::android::hardware::camera::common::CameraDeviceStatus;
using aidl::android::hardware::camera::common::CameraMetadataType;
using aidl::android::hardware::camera::common::CameraResourceCost;
using aidl::android::hardware::camera::common::Status;
using aidl::android::hardware::camera::common::TorchModeStatus;
using aidl::android::hardware::camera::common::VendorTag;
using aidl::android::hardware::camera::common::VendorTagSection;
using aidl::android::hardware::camera::device::BnCameraDevice;
using aidl::android::hardware::camera::device::BnCameraDeviceSession;
using aidl::android::hardware::camera::device::BufferStatus;
using aidl::android::hardware::camera::device::BufferCache;
using aidl::android::hardware::camera::device::CameraMetadata;
using aidl::android::hardware::camera::device::CameraOfflineSessionInfo;
using aidl::android::hardware::camera::device::CaptureRequest;
using aidl::android::hardware::camera::device::CaptureResult;
using aidl::android::hardware::camera::device::ConfigureStreamsRet;
using aidl::android::hardware::camera::device::HalStream;
using aidl::android::hardware::camera::device::ICameraDevice;
using aidl::android::hardware::camera::device::ICameraDeviceCallback;
using aidl::android::hardware::camera::device::ICameraDeviceSession;
using aidl::android::hardware::camera::device::ICameraInjectionSession;
using aidl::android::hardware::camera::device::ICameraOfflineSession;
using aidl::android::hardware::camera::device::NotifyMsg;
using aidl::android::hardware::camera::device::RequestTemplate;
using aidl::android::hardware::camera::device::ShutterMsg;
using aidl::android::hardware::camera::device::StreamBufferRet;
using aidl::android::hardware::camera::device::StreamConfiguration;
using aidl::android::hardware::camera::provider::BnCameraProvider;
using aidl::android::hardware::camera::provider::CameraIdAndStreamCombination;
using aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination;
using aidl::android::hardware::camera::provider::ICameraProviderCallback;
using aidl::android::hardware::common::fmq::GrantorDescriptor;
using aidl::android::hardware::common::fmq::MQDescriptor;
using aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::PixelFormat;
using ndk::ScopedAStatus;

static constexpr const char *kProviderServiceName =
    "android.hardware.camera.provider.ICameraProvider/internal/0";
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
static constexpr const char *kLogTag = "cdh_oneplusace6t_camera_provider";
#else
static constexpr const char *kLogTag = "cdh_note13_camera_provider";
#endif
static constexpr size_t kMetadataMsgQueueSize = 1 << 18;
static constexpr int32_t kSynchronizedReadWriteFmqFlag = 0x01;
static constexpr size_t kRingBufferPositionSize = sizeof(uint64_t);
#include "cdh_note13_vendor_tags.inc"
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
static constexpr uint32_t kOplusSupportedCameraIdTypeTag = 0x81170003;

static int oplusVendorTagCount(const vendor_tag_ops_t *) { return 1; }

static void oplusVendorTags(const vendor_tag_ops_t *, uint32_t *tagArray) {
  if (tagArray != nullptr) {
    tagArray[0] = kOplusSupportedCameraIdTypeTag;
  }
}

static const char *oplusVendorTagSection(const vendor_tag_ops_t *,
                                        uint32_t tag) {
  return tag == kOplusSupportedCameraIdTypeTag ? "com.oplus" : nullptr;
}

static const char *oplusVendorTagName(const vendor_tag_ops_t *, uint32_t tag) {
  return tag == kOplusSupportedCameraIdTypeTag ? "supported.cameraid.type"
                                              : nullptr;
}

static int oplusVendorTagType(const vendor_tag_ops_t *, uint32_t tag) {
  if (tag == kOplusSupportedCameraIdTypeTag) {
    return TYPE_BYTE;
  }
  return -1;
}

static const vendor_tag_ops_t kOplusVendorTagOps = {
    oplusVendorTagCount,   oplusVendorTags,   oplusVendorTagSection,
    oplusVendorTagName,    oplusVendorTagType, {nullptr},
};
#else
// note13 真机 vendor tags：真机 gold 全量描述符（331 项 / 24 个 section），
// tagId、section、name、type 必须与真机逐项一致——App 侧 vendor tag 是从
// provider 的 getVendorTags 学的，id/type 不一致会导致
// availableRequestKeys/availableResultKeys 里的 vendor key 解析不出名字，
// 以及用 Key<byte[]>/Key<long[]> 取值时类型不符拿不到值。
// 描述符表由 cdh_note13_vendor_tags.inc（kNote13VendorTagDescs）驱动；
// 静态 metadata 实际写值的 55 项见同文件 kNote13VendorTags。
static const Note13VendorTagDesc *note13VendorTagDesc(uint32_t tag) {
  for (size_t i = 0; i < kNote13VendorTagDescCount; i++) {
    if (kNote13VendorTagDescs[i].tagId == tag) {
      return &kNote13VendorTagDescs[i];
    }
  }
  return nullptr;
}

static int note13VendorTagCount(const vendor_tag_ops_t *) {
  return static_cast<int>(kNote13VendorTagDescCount);
}

static void note13VendorAllTags(const vendor_tag_ops_t *, uint32_t *tagArray) {
  for (size_t i = 0; i < kNote13VendorTagDescCount; i++) {
    tagArray[i] = kNote13VendorTagDescs[i].tagId;
  }
}

static const char *note13VendorTagSection(const vendor_tag_ops_t *,
                                         uint32_t tag) {
  const Note13VendorTagDesc *desc = note13VendorTagDesc(tag);
  return desc != nullptr ? desc->section : nullptr;
}

static const char *note13VendorTagName(const vendor_tag_ops_t *, uint32_t tag) {
  const Note13VendorTagDesc *desc = note13VendorTagDesc(tag);
  return desc != nullptr ? desc->tagName : nullptr;
}

static int note13VendorTagType(const vendor_tag_ops_t *, uint32_t tag) {
  const Note13VendorTagDesc *desc = note13VendorTagDesc(tag);
  return desc != nullptr ? desc->tagType : -1;
}

static const vendor_tag_ops_t kNote13VendorTagOps = {
    note13VendorTagCount,    note13VendorAllTags,   note13VendorTagSection,
    note13VendorTagName,     note13VendorTagType,   {nullptr},
};
#endif

struct Note13CameraTarget {
  const char *id;
  uint8_t lensFacing;
  int32_t sensorOrientation;
  uint8_t flashAvailable;
  int32_t pixelWidth;
  int32_t pixelHeight;
  float physicalWidth;
  float physicalHeight;
  float lensDistortion[5];
  float lensIntrinsicCalibration[5];
  float lensPoseRotation[4];
  float lensPoseTranslation[3];
  float aperture;
  float focalLength;
  uint8_t opticalStabilization[2];
  size_t opticalStabilizationCount;
  float maxDigitalZoom;
  uint8_t colorFilterArrangement;
  int64_t minExposureTime;
  int64_t maxFrameDuration;
  int32_t minSensitivity;
  int32_t maxSensitivity;
  int32_t maxRegions[3];
  uint8_t requestCapabilities[16];
  size_t requestCapabilitiesCount;
  const int32_t *jpegSizes;
  size_t jpegSizeCount;
  const int32_t *streamSizes;
  size_t streamSizeCount;
  const int32_t *highSpeedVideo;
  size_t highSpeedVideoCount;
  const uint8_t *physicalCameraIds;
  size_t physicalCameraIdCount;
  const Note13CameraTarget *physicalTargets;
  size_t physicalTargetCount;
};

#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
static uint8_t oplusCameraIdType(const Note13CameraTarget &target) {
  if (strcmp(target.id, "0") == 0) {
    return 12;
  }
  if (strcmp(target.id, "1") == 0) {
    return 1;
  }
  if (strcmp(target.id, "2") == 0) {
    return 0;
  }
  if (strcmp(target.id, "3") == 0) {
    return 2;
  }
  return 0;
}
#endif

static constexpr int32_t kOutputStream = 0;
static constexpr int32_t kBlob = 0x21;
static constexpr int32_t kImplementationDefined = 0x22;
static constexpr int32_t kYCbCr420888 = 0x23;
static constexpr int32_t kRawSensor = 0x20; // android.graphics.ImageFormat.RAW_SENSOR
// HAL_PIXEL_FORMAT_YV12（0x32315659 = 'YV12'）：真机 SCM 带该输出流，
// Camera1 legacy 的 preview-format-values 会额外报 yuv420p。
static constexpr int32_t kYV12 = 0x32315659;

#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
static const int32_t kBackJpegSizes[] = {
    4096, 3072, 3840, 2880, 3840, 2160, 3840, 1644, 3280, 1856,
    3264, 2448, 3264, 1836, 3168, 1440, 2560, 1920, 2560, 1440,
    2520, 1080, 2376, 1080, 2304, 1728, 2304, 1048, 2160, 1080,
    2136, 1200, 1920, 1440, 1920, 1296, 1920, 1080,
};

static const int32_t kBackStreamSizes[] = {
    4096, 3072, 3840, 2880, 3840, 2160, 3840, 1644, 3280, 1856,
    3264, 2448, 3264, 1836, 3168, 1440, 2560, 1920, 2560, 1440,
    2520, 1080, 2376, 1080, 2304, 1728, 2304, 1048, 2160, 1080,
    2136, 1200, 1920, 1440, 1920, 1296, 1920, 1080, 1600, 1200,
    1600, 800,  1440, 1080, 1280, 960,  1280, 720,  1200, 1200,
    1188, 540,  1080, 1080, 1024, 768,  864,  480,  800,  400,
    792,  360,  720,  540,  720,  480,  640,  480,  640,  360,
    640,  290,  480,  480,  352,  288,  320,  240,  176,  144,
};

static const int32_t kBackHighSpeedVideo[] = {
    3840, 2160, 30,  120, 4,  3840, 2160, 60,  120, 2,
    3840, 2160, 120, 120, 1,  1920, 1080, 30,  120, 4,
    1920, 1080, 60,  120, 2,  1920, 1080, 120, 120, 1,
    1920, 1080, 30,  240, 8,  1920, 1080, 60,  240, 4,
    1920, 1080, 240, 240, 1,  1280, 720,  30,  120, 4,
    1280, 720,  60,  120, 2,  1280, 720,  120, 120, 1,
    1280, 720,  30,  240, 8,  1280, 720,  60,  240, 4,
    1280, 720,  240, 240, 1,
};

static const int32_t kFrontJpegSizes[] = {
    2304, 1728, 1920, 1440, 2136, 1200, 1920, 1296,
    2304, 1048, 2160, 1080, 1920, 1080,
};

static const int32_t kFrontStreamSizes[] = {
    2304, 1728, 1920, 1440, 2136, 1200, 1920, 1296, 2304, 1048,
    2160, 1080, 1920, 1080, 1600, 1200, 1440, 1080, 1200, 1200,
    1600, 800,  1280, 960,  1080, 1080, 1280, 720,  1024, 768,
    1188, 540,  864,  480,  720,  540,  720,  480,  800,  400,
    640,  480,  792,  360,  640,  360,  480,  480,  640,  290,
    352,  288,  320,  240,  176,  144,
};

static const int32_t kUltraWideJpegSizes[] = {
    3264, 2448, 3264, 1836, 2560, 1920, 3168, 1440, 2304, 1728,
    2560, 1440, 1920, 1440, 2520, 1080, 2376, 1080, 2136, 1200,
    1920, 1296, 2304, 1048, 2160, 1080, 1920, 1080,
};

static const int32_t kUltraWideStreamSizes[] = {
    3264, 2448, 3264, 1836, 2560, 1920, 3168, 1440, 2304, 1728,
    2560, 1440, 1920, 1440, 2520, 1080, 2376, 1080, 2136, 1200,
    1920, 1296, 2304, 1048, 2160, 1080, 1920, 1080, 1600, 1200,
    1440, 1080, 1200, 1200, 1600, 800,  1280, 960,  1080, 1080,
    1280, 720,  1024, 768,  1188, 540,  864,  480,  720,  540,
    720,  480,  800,  400,  640,  480,  792,  360,  640,  360,
    480,  480,  640,  290,  352,  288,  320,  240,  176,  144,
};

static const uint8_t kBackPhysicalCameraIds[] = {'3', '\0', '2', '\0'};

static const Note13CameraTarget kBackPhysicalTargets[] = {
    {"2",
     ANDROID_LENS_FACING_BACK,
     90,
     ANDROID_FLASH_INFO_AVAILABLE_TRUE,
     4096,
     3072,
     8.19200039f,
     6.14400005f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {2867.2f, 2867.2f, 2048.0f, 1536.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f},
     1.88f,
     5.59f,
     {0, 0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR,
     78125LL,
     74354131243LL,
     100,
     6400,
     {1, 0, 1},
     {0, 9, 3, 7, 4, 5, 1, 6, 2, 19, 18, 20},
     12,
     kBackJpegSizes,
     std::size(kBackJpegSizes) / 2,
     kBackStreamSizes,
     std::size(kBackStreamSizes) / 2,
     kBackHighSpeedVideo,
     std::size(kBackHighSpeedVideo) / 5,
     nullptr,
     0,
     nullptr,
     0},
    {"3",
     ANDROID_LENS_FACING_BACK,
     90,
     ANDROID_FLASH_INFO_AVAILABLE_TRUE,
     3264,
     2448,
     3.65567994f,
     2.74176002f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {1958.4f, 1958.4f, 1632.0f, 1224.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f},
     2.2f,
     1.68f,
     {0, 0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR,
     79681LL,
     110043906598LL,
     100,
     1550,
     {1, 0, 0},
     {0, 3, 7, 4, 5, 1, 6, 2, 19, 18, 20},
     11,
     kUltraWideJpegSizes,
     std::size(kUltraWideJpegSizes) / 2,
     kUltraWideStreamSizes,
     std::size(kUltraWideStreamSizes) / 2,
     nullptr,
     0,
     nullptr,
     0,
     nullptr,
     0},
};

static const Note13CameraTarget kTargets[] = {
    {"0",
     ANDROID_LENS_FACING_BACK,
     90,
     ANDROID_FLASH_INFO_AVAILABLE_TRUE,
     4096,
     3072,
     8.19200039f,
     6.14400005f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {2867.2f, 2867.2f, 2048.0f, 1536.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f},
     1.88f,
     5.59f,
     {0, 0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR,
     79681LL,
     74354131243LL,
     100,
     6400,
     {1, 0, 1},
     {0, 9, 3, 7, 4, 5, 1, 6, 2, 19, 18, 20, 11},
     13,
     kBackJpegSizes,
     std::size(kBackJpegSizes) / 2,
     kBackStreamSizes,
     std::size(kBackStreamSizes) / 2,
     kBackHighSpeedVideo,
     std::size(kBackHighSpeedVideo) / 5,
     kBackPhysicalCameraIds,
     std::size(kBackPhysicalCameraIds),
     kBackPhysicalTargets,
     std::size(kBackPhysicalTargets)},
    {"1",
     ANDROID_LENS_FACING_FRONT,
     270,
     ANDROID_FLASH_INFO_AVAILABLE_FALSE,
     4608,
     3456,
     4.60799980f,
     3.45600009f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {2764.8f, 2764.8f, 2304.0f, 1728.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f},
     {0.0f, 0.0f, 0.0f},
     2.45f,
     3.06f,
     {0, 0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR,
     35430LL,
     23227006911LL,
     100,
     1600,
     {1, 0, 0},
     {0, 3, 7, 4, 5, 1, 6, 2, 19, 18, 20},
     11,
     kFrontJpegSizes,
     std::size(kFrontJpegSizes) / 2,
     kFrontStreamSizes,
     std::size(kFrontStreamSizes) / 2,
     nullptr,
     0,
     nullptr,
     0,
     nullptr,
     0},
};
#else
static const int32_t kBackJpegSizes[] = {
    4000, 3000, 4000, 2256, 4000, 2252, 4000, 1800, 3264, 2448, 3264, 1840, 3264, 1632, 3264, 1504,
    3264, 1472, 3264, 1344, 3000, 3000, 2560, 1920, 2560, 1080, 2280, 1080, 1920, 1440, 1920, 1088,
    1920, 1080, 1600, 720, 1560, 720, 1520, 720, 1440, 1088, 1440, 720, 1280, 960, 1280, 720, 1080,
    720, 960, 720, 960, 544, 720, 720, 640, 480, 640, 360, 480, 320, 352, 288, 320, 240, 176, 144,
};

// 真机 gold 后摄 PRIVATE/YUV_420_888/YV12 输出尺寸（50 项，顺序同真机
// streamConfigurationMap；末项 4000x3000 = sensor pixel array，同时是 YUV 输入尺寸）。
// 原先截断到 2560x1440 会让 mandatoryStreamCombinations 的 availableSizes 少 14 项，
// 且高分辨率预览/录像档位（4000x2256、3264x2448、3000x3000…）在 App 侧不可选。
static const int32_t kBackStreamSizes[] = {
    4000, 2256, 4000, 2252, 4000, 1800, 3264, 2448, 3264, 1840, 3264, 1632, 3264, 1504, 3264, 1472,
    3264, 1344, 3000, 3000, 2912, 1472, 2912, 1344, 2560, 1920, 2560, 1440, 2560, 1080, 2400, 1080,
    2304, 1728, 2280, 1080, 2160, 1080, 1920, 1440, 1920, 1088, 1920, 1080, 1600, 1200, 1600, 720,
    1560, 720, 1560, 704, 1520, 720, 1440, 1088, 1440, 1080, 1440, 720, 1280, 960, 1280, 720,
    1088, 1088, 1080, 1080, 1080, 720, 960, 720, 960, 544, 800, 400, 720, 720, 720, 480, 640, 480,
    640, 360, 480, 320, 352, 288, 320, 240, 192, 144, 192, 108, 176, 144, 160, 96, 4000, 3000,
};

// 真机 gold 后摄(0) capabilities 含 CONSTRAINED_HIGH_SPEED_VIDEO(9)，需要配套
// 高速视频配置表（StreamConfigurationMap 构造时校验；缺表抛 "sizes was empty"）。
// 表来自真机 StreamConfigurationMap.HighSpeedVideoConfigurations：
//   1280x720 @30-120fps（batch 4）、@120fps（batch 1）
//   640x480  @30-120fps（batch 4）、@120fps（batch 1）
// 前摄(1) capabilities 不含 9（真机高速表为空），故不提供。
static const int32_t kBackHighSpeedVideo[] = {
    1280, 720, 30, 120, 4,
    1280, 720, 120, 120, 1,
    640, 480, 30, 120, 4,
    640, 480, 120, 120, 1,
};

static const int32_t kFrontJpegSizes[] = {
    2304, 1728, 2304, 1296, 2304, 1040, 1920, 1440, 1920, 1088, 1920, 1080, 1728, 1728, 1600, 720,
    1560, 720, 1520, 720, 1440, 1088, 1440, 1080, 1440, 720, 1280, 960, 1280, 720, 1080, 1080,
    1080, 720, 960, 720, 720, 720, 640, 480, 640, 360, 480, 320, 320, 240, 176, 144,
};

// 真机 gold 前摄 PRIVATE/YUV_420_888/YV12 输出尺寸（29 项，顺序同真机）。
// 原先缺最大档 2304x1728（= sensor pixel array），mandatoryStreamCombinations
// 的 availableSizes 少 1 项。
static const int32_t kFrontStreamSizes[] = {
    2304, 1728, 2304, 1296, 2304, 1040, 1920, 1440, 1920, 1088, 1920, 1080, 1728, 1728, 1600, 720,
    1560, 720, 1520, 720, 1440, 1088, 1440, 1080, 1440, 720, 1280, 960, 1280, 720, 1080, 1080,
    1080, 720, 960, 720, 720, 720, 720, 480, 640, 480, 640, 360, 480, 320, 352, 288, 320, 240,
    192, 144, 192, 108, 176, 144, 160, 96,
};

// 真机 gold 无逻辑多摄（原生 2 个独立摄像头，physicalCameraIds=[]），
// 因此不声明物理摄像组，也不导出超广角物理摄。

static const Note13CameraTarget kTargets[] = {
    {"0",
     ANDROID_LENS_FACING_BACK,
     90,
     ANDROID_FLASH_INFO_AVAILABLE_TRUE,
     4000,
     3000,
     7.68f,
     5.76f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {2764.8f, 2764.8f, 2000.0f, 1500.0f, 0.0f},
     {1.0f, 0.0f, 0.0f, 0.0f},
     {0.0f, 0.0f, 0.0f},
     1.7f,
     5.31f,
     {0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG,
     100000LL,
     30000000000LL,
     50,
     12800,
     // ANDROID_CONTROL_MAX_REGIONS 顺序为 {AE, AWB, AF}：真机后摄 ae=1 awb=0 af=1
     {1, 0, 1},
     {0, 1, 2, 5, 3, 6, 7, 9},
     8,
     kBackJpegSizes,
     std::size(kBackJpegSizes) / 2,
     kBackStreamSizes,
     std::size(kBackStreamSizes) / 2,
     kBackHighSpeedVideo,
     std::size(kBackHighSpeedVideo) / 5,
     nullptr,
     0,
     nullptr,
     0},
    {"1",
     ANDROID_LENS_FACING_FRONT,
     270,
     ANDROID_FLASH_INFO_AVAILABLE_FALSE,
     2304,
     1728,
     4.64f,
     3.48f,
     {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
     {1618.56f, 1618.56f, 1152.0f, 864.0f, 0.0f},
     {0.0f, 0.0f, 0.0f, 1.0f},
     {0.0f, 0.0f, 0.0f},
     2.4f,
     3.26f,
     {0},
     1,
     10.0f,
     ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB,
     100000LL,
     278443000LL,
     50,
     3200,
     {1, 0, 0},
     {0, 1, 2, 5, 3, 6, 7},
     7,
     kFrontJpegSizes,
     std::size(kFrontJpegSizes) / 2,
     kFrontStreamSizes,
     std::size(kFrontStreamSizes) / 2,
     nullptr,
     0,
     nullptr,
     0,
     nullptr,
     0},
};
#endif

static ScopedAStatus cameraError(Status status) {
  return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(status));
}

template <typename T>
static bool addEntry(camera_metadata_t *metadata, uint32_t tag, const T *values,
                     size_t count) {
  return add_camera_metadata_entry(metadata, tag, values, count) == 0;
}

template <typename T>
static bool addEntry(camera_metadata_t *metadata, uint32_t tag,
                     const std::vector<T> &values) {
  return !values.empty() && addEntry(metadata, tag, values.data(), values.size());
}

template <typename T>
static bool addSingle(camera_metadata_t *metadata, uint32_t tag, T value) {
  return addEntry(metadata, tag, &value, 1);
}

static std::vector<uint8_t> metadataBytes(camera_metadata_t *metadata) {
  sort_camera_metadata(metadata);
  const size_t size = get_camera_metadata_size(metadata);
  std::vector<uint8_t> out(size);
  std::memcpy(out.data(), metadata, size);
  return out;
}

static void appendStreamConfigs(std::vector<int32_t> *out, int32_t format,
                                const int32_t *sizes, size_t count) {
  for (size_t index = 0; index < count; index++) {
    out->push_back(format);
    out->push_back(sizes[index * 2]);
    out->push_back(sizes[index * 2 + 1]);
    out->push_back(kOutputStream);
  }
}

static void appendStreamDurations(std::vector<int64_t> *out, int32_t format,
                                  const int32_t *sizes, size_t count,
                                  int64_t duration) {
  for (size_t index = 0; index < count; index++) {
    out->push_back(format);
    out->push_back(sizes[index * 2]);
    out->push_back(sizes[index * 2 + 1]);
    out->push_back(duration);
  }
}

// 真机对个别大尺寸单独抬高 minFrameDuration（见 kBackSlow* 表），
// 其余尺寸走 defaultDuration。
struct DurationOverride {
  int32_t width;
  int32_t height;
  int64_t duration;
};

static void appendStreamDurationsWithOverrides(
    std::vector<int64_t> *out, int32_t format, const int32_t *sizes,
    size_t count, int64_t defaultDuration,
    const DurationOverride *overrides, size_t overrideCount) {
  for (size_t index = 0; index < count; index++) {
    const int32_t width = sizes[index * 2];
    const int32_t height = sizes[index * 2 + 1];
    int64_t duration = defaultDuration;
    for (size_t i = 0; i < overrideCount; i++) {
      if (overrides[i].width == width && overrides[i].height == height) {
        duration = overrides[i].duration;
        break;
      }
    }
    out->push_back(format);
    out->push_back(width);
    out->push_back(height);
    out->push_back(duration);
  }
}

static bool addCommonStaticEntries(camera_metadata_t *metadata,
                                   const Note13CameraTarget &target);
// 真机 gold 缺失 standard tags 补齐（定义在 addCommonStaticEntries 之后）。
static bool addMissingStandardEntries(camera_metadata_t *metadata,
                                      const Note13CameraTarget &target);

static bool addLogicalMultiCameraEntries(camera_metadata_t *metadata,
                                         const Note13CameraTarget &target) {
  if (target.physicalCameraIds == nullptr || target.physicalCameraIdCount == 0) {
    return true;
  }
  const uint8_t sensorSyncType =
      ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE_CALIBRATED;
  const int32_t physicalRequestKeys[] = {
      ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_SENSITIVITY,
      ANDROID_SENSOR_FRAME_DURATION, ANDROID_LENS_APERTURE,
      ANDROID_LENS_FOCAL_LENGTH,
  };
  // CameraService parses this byte blob to discover logical camera 0's
  // physical IDs without exposing them as extra public cameras.
  return addEntry(metadata, ANDROID_LOGICAL_MULTI_CAMERA_PHYSICAL_IDS,
                  target.physicalCameraIds, target.physicalCameraIdCount) &&
         addSingle(metadata, ANDROID_LOGICAL_MULTI_CAMERA_SENSOR_SYNC_TYPE,
                   sensorSyncType) &&
         addEntry(metadata,
                  ANDROID_REQUEST_AVAILABLE_PHYSICAL_CAMERA_REQUEST_KEYS,
                  physicalRequestKeys, std::size(physicalRequestKeys));
}

static bool addCommonStaticEntries(camera_metadata_t *metadata,
                                   const Note13CameraTarget &target) {
  const uint8_t aeAntibanding[] = {0, 1, 2, 3};
  // 真机 gold 前后摄 AE 可用 FPS 范围一致（10/15/20/24/30，无 60）
  const int32_t aeFpsRangesBack[] = {10, 10, 15, 15, 15, 20, 20, 20,
                                     24, 24, 10, 30, 30, 30};
  const int32_t aeFpsRangesFront[] = {10, 10, 15, 15, 15, 20, 20, 20,
                                      24, 24, 10, 30, 30, 30};
  const uint8_t aeModesBack[] = {0, 1, 2, 3};
  const uint8_t aeModesFront[] = {0, 1, 5};
  const uint8_t aeLockAvailable = ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE;
  const uint8_t afModesBack[] = {0, 1, 2, 3, 4};
  const uint8_t afModesFront[] = {0};
  const uint8_t effects[] = {0};
  const uint8_t controlModes[] = {0, 1, 2, 3};
  // 真机 gold：后摄 availableSceneModes=[1,2,...,16,18]，前摄一致
  const uint8_t sceneModes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                                12, 13, 14, 15, 16, 18};
  const uint8_t videoStabilization[] = {0, 1};
  const uint8_t awbModes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  const uint8_t awbLockAvailable = ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE;
  const uint8_t edgeModes[] = {0, 1, 2, 3};
  const uint8_t hotPixelModes[] = {0, 1, 2};
  const uint8_t noiseReductionModes[] = {0, 1, 2, 3, 4};
  const uint8_t faceDetectModes[] = {0, 1};
  const int32_t aeCompensationRange[] = {-24, 24};
  const camera_metadata_rational_t aeCompensationStep = {1, 6};
  const int64_t exposureTimeRange[] = {target.minExposureTime,
                                       target.maxFrameDuration};
  const int32_t sensitivityRange[] = {target.minSensitivity,
                                      target.maxSensitivity};
  const int32_t activeArray[] = {0, 0, target.pixelWidth, target.pixelHeight};
  const int32_t pixelArraySize[] = {target.pixelWidth, target.pixelHeight};
  const float physicalSize[] = {target.physicalWidth, target.physicalHeight};
  const float zoomRatioRange[] = {1.0f, target.maxDigitalZoom};
  const int32_t maxOutputStreams[] = {target.maxRegions[0] ? 1 : 0, 3, 1};
  const int32_t jpegThumbnailSizes[] = {0, 0, 192, 80, 192, 108, 192, 144,
                                        192, 192};
  std::vector<int32_t> streamConfigurations;
  streamConfigurations.reserve((target.jpegSizeCount +
                                target.streamSizeCount * 2) *
                               4);
  appendStreamConfigs(&streamConfigurations, kBlob, target.jpegSizes,
                      target.jpegSizeCount);
  appendStreamConfigs(&streamConfigurations, kYCbCr420888, target.streamSizes,
                      target.streamSizeCount);
  appendStreamConfigs(&streamConfigurations, kImplementationDefined,
                      target.streamSizes, target.streamSizeCount);
  // YV12 输出流：真机 SCM 含该格式（Camera1 preview-format-values 缺它则少报 yuv420p）
  appendStreamConfigs(&streamConfigurations, kYV12, target.streamSizes,
                      target.streamSizeCount);
  // RAW_SENSOR 输出流：requestCapabilities 含 RAW(3) 时 StreamConfigurationMap
  // 会解析 RAW surfaceType 的 sizes，缺失抛 "sizes was empty"（真机含 RAW 能力）。
  // 尺寸取 sensor pixel array（后摄 4000x3000 / 前摄 2304x1728），仅输出（不输入）。
  const int32_t rawSizes[] = {target.pixelWidth, target.pixelHeight};
  appendStreamConfigs(&streamConfigurations, kRawSensor, rawSizes, 1);
  // YUV input（reprocess）流：requestCapabilities 含 YUV_REPROCESSING(7) 时，
  // 框架生成 SCALER_MANDATORY_STREAM_COMBINATIONS 要求 YUV 输入流存在，否则
  // getKeys() 抛 "sizes was empty"（真机 input=最大 YUV 尺寸）。仅输入（isInput=1）。
  const int32_t kInputStream = 1;
  streamConfigurations.push_back(kYCbCr420888);
  streamConfigurations.push_back(target.pixelWidth);
  streamConfigurations.push_back(target.pixelHeight);
  streamConfigurations.push_back(kInputStream);
  // 真机 gold 的时长表（minFrameDuration / stallDuration）：
  //   JPEG          stall=33333333；minFrameDuration=33333333，后摄
  //                 4000x3000=66666666（>33.4ms 的 kSlowJpegModeThreshold，
  //                 Camera1 legacy 据此判定 video-snapshot-supported=false）、
  //                 3264x2448=50000000
  //   PRIV/YUV/YV12 stall=0；minFrameDuration=33333333，后摄 4000x3000=66666666，
  //                 4000x2256/4000x1800/3264x2448/3264x1632/3000x3000=50000000
  //   RAW_SENSOR    stall=33333333，minFrameDuration=33333333
  // 时长表只覆盖输出流（真机 availableMinFrameDurations/availableStallDurations
  // 各 740/448 项 = 输出配置数×4，不含 YUV 输入流条目）。多加输入条目会让
  // StreamConfigurationMap 的 (format,w,h)→duration 表被输入值盖掉，
  // 例如 YUV 4000x3000 会变成 33333333 而非真机的 66666666，进而让框架把
  // 4000x3000 也算进 mandatoryStreamCombinations 的 availableSizes。
  static const DurationOverride kBackSlowJpegDurations[] = {
      {4000, 3000, 66666666LL},
      {3264, 2448, 50000000LL},
  };
  static const DurationOverride kBackSlowStreamDurations[] = {
      {4000, 3000, 66666666LL}, {4000, 2256, 50000000LL},
      {4000, 1800, 50000000LL}, {3264, 2448, 50000000LL},
      {3264, 1632, 50000000LL}, {3000, 3000, 50000000LL},
  };
  const bool backCamera = target.lensFacing == ANDROID_LENS_FACING_BACK;
  const DurationOverride *jpegOverrides =
      backCamera ? static_cast<const DurationOverride *>(kBackSlowJpegDurations)
                 : nullptr;
  const size_t jpegOverrideCount =
      backCamera ? std::size(kBackSlowJpegDurations) : 0;
  const DurationOverride *streamOverrides =
      backCamera
          ? static_cast<const DurationOverride *>(kBackSlowStreamDurations)
          : nullptr;
  const size_t streamOverrideCount =
      backCamera ? std::size(kBackSlowStreamDurations) : 0;
  std::vector<int64_t> minFrameDurations;
  minFrameDurations.reserve(streamConfigurations.size());
  appendStreamDurationsWithOverrides(&minFrameDurations, kBlob,
                                     target.jpegSizes, target.jpegSizeCount,
                                     33333333LL, jpegOverrides,
                                     jpegOverrideCount);
  appendStreamDurationsWithOverrides(&minFrameDurations, kYCbCr420888,
                                     target.streamSizes,
                                     target.streamSizeCount, 33333333LL,
                                     streamOverrides, streamOverrideCount);
  appendStreamDurationsWithOverrides(&minFrameDurations,
                                     kImplementationDefined,
                                     target.streamSizes,
                                     target.streamSizeCount, 33333333LL,
                                     streamOverrides, streamOverrideCount);
  appendStreamDurationsWithOverrides(&minFrameDurations, kYV12,
                                     target.streamSizes,
                                     target.streamSizeCount, 33333333LL,
                                     streamOverrides, streamOverrideCount);
  appendStreamDurations(&minFrameDurations, kRawSensor, rawSizes, 1,
                        33333333LL);
  std::vector<int64_t> stallDurations;
  stallDurations.reserve(streamConfigurations.size());
  // 真机 JPEG/RAW_SENSOR 的 stall 都是 33333333ns：原实现 JPEG 写死 200000000、
  // RAW 写死 0，会让 App 侧 getOutputStallDuration 认为云机拍照慢 6 倍。
  appendStreamDurations(&stallDurations, kBlob, target.jpegSizes,
                        target.jpegSizeCount, 33333333LL);
  appendStreamDurations(&stallDurations, kYCbCr420888, target.streamSizes,
                        target.streamSizeCount, 0);
  appendStreamDurations(&stallDurations, kImplementationDefined,
                        target.streamSizes, target.streamSizeCount, 0);
  appendStreamDurations(&stallDurations, kYV12, target.streamSizes,
                        target.streamSizeCount, 0);
  appendStreamDurations(&stallDurations, kRawSensor, rawSizes, 1, 33333333LL);
  const uint8_t hardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_3;
  const uint8_t timestampSource =
      ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME;
  const uint8_t lensFacing = target.lensFacing;
  const uint8_t flashAvailable = target.flashAvailable;
  const uint8_t colorArrangement = target.colorFilterArrangement;
  const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
  const uint8_t lensFocusDistanceCalibration =
      ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
  const uint8_t pipelineDepth = 8;
  const int32_t partialResultCount = 10;
  const int32_t jpegMaxSize = target.pixelWidth * target.pixelHeight * 2;
  // 真机 gold 两个摄像头都有 android.scaler.availableInputOutputFormatsMap
  // （int32[4]：输入格式, 输出格式个数, 输出格式…）= YUV_420_888 → {YUV_420_888, BLOB}。
  // 缺它时 StreamConfigurationMap 的 ValidOutputFormatsForInput 为空，
  // 框架据此认为 YUV 输入流不能重处理成任何输出格式（YUV_REPROCESSING 能力形同虚设）。
  const int32_t inputOutputFormatsMap[] = {kYCbCr420888, 2, kYCbCr420888, kBlob};

  return addEntry(metadata, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
                  aeAntibanding, std::size(aeAntibanding)) &&
         addEntry(metadata, ANDROID_CONTROL_AE_AVAILABLE_MODES,
                  target.lensFacing == ANDROID_LENS_FACING_BACK ? aeModesBack
                                                                : aeModesFront,
                  target.lensFacing == ANDROID_LENS_FACING_BACK
                      ? std::size(aeModesBack)
                      : std::size(aeModesFront)) &&
         addEntry(metadata, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                  target.lensFacing == ANDROID_LENS_FACING_BACK
                      ? aeFpsRangesBack
                      : aeFpsRangesFront,
                  target.lensFacing == ANDROID_LENS_FACING_BACK
                      ? std::size(aeFpsRangesBack)
                      : std::size(aeFpsRangesFront)) &&
         addEntry(metadata, ANDROID_CONTROL_AE_COMPENSATION_RANGE,
                  aeCompensationRange, std::size(aeCompensationRange)) &&
         addEntry(metadata, ANDROID_CONTROL_AE_COMPENSATION_STEP,
                  &aeCompensationStep, 1) &&
         addSingle(metadata, ANDROID_CONTROL_AE_LOCK_AVAILABLE,
                   aeLockAvailable) &&
         addEntry(metadata, ANDROID_CONTROL_AF_AVAILABLE_MODES,
                  target.lensFacing == ANDROID_LENS_FACING_BACK ? afModesBack
                                                                : afModesFront,
                  target.lensFacing == ANDROID_LENS_FACING_BACK
                      ? std::size(afModesBack)
                      : std::size(afModesFront)) &&
         addEntry(metadata, ANDROID_CONTROL_AVAILABLE_EFFECTS, effects,
                  std::size(effects)) &&
         addEntry(metadata, ANDROID_CONTROL_AVAILABLE_MODES, controlModes,
                  std::size(controlModes)) &&
         addEntry(metadata, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, sceneModes,
                  std::size(sceneModes)) &&
         addEntry(metadata,
                  ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
                  videoStabilization, std::size(videoStabilization)) &&
         addEntry(metadata, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes,
                  std::size(awbModes)) &&
         addSingle(metadata, ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
                   awbLockAvailable) &&
         addEntry(metadata, ANDROID_CONTROL_MAX_REGIONS, target.maxRegions,
                  std::size(target.maxRegions)) &&
         addEntry(metadata, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edgeModes,
                  std::size(edgeModes)) &&
         addSingle(metadata, ANDROID_FLASH_INFO_AVAILABLE, flashAvailable) &&
         addEntry(metadata, ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
                  hotPixelModes, std::size(hotPixelModes)) &&
         addSingle(metadata, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
                   hardwareLevel) &&
         addEntry(metadata, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
                  jpegThumbnailSizes, std::size(jpegThumbnailSizes)) &&
         addSingle(metadata, ANDROID_JPEG_MAX_SIZE, jpegMaxSize) &&
         addSingle(metadata, ANDROID_LENS_FACING, lensFacing) &&
         addSingle(metadata, ANDROID_LENS_INFO_AVAILABLE_APERTURES,
                   target.aperture) &&
         addSingle(metadata, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
                   target.focalLength) &&
         addEntry(metadata, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
                  target.opticalStabilization,
                  target.opticalStabilizationCount) &&
         addSingle(metadata, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
                   lensFocusDistanceCalibration) &&
         addSingle(metadata, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
                   target.lensFacing == ANDROID_LENS_FACING_BACK ? 0.1f
                                                                 : 0.33333334f) &&
         addSingle(metadata, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
                   target.lensFacing == ANDROID_LENS_FACING_BACK ? 10.0f
                                                                 : 0.0f) &&
         addEntry(metadata,
                  ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
                  noiseReductionModes, std::size(noiseReductionModes)) &&
         addEntry(metadata, ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
                  target.requestCapabilities, target.requestCapabilitiesCount) &&
         addSingle(metadata, ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, 1) &&
         addEntry(metadata, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
                  maxOutputStreams, std::size(maxOutputStreams)) &&
         addSingle(metadata, ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
                   partialResultCount) &&
         addSingle(metadata, ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
                   pipelineDepth) &&
         addEntry(metadata,
                  ANDROID_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP,
                  inputOutputFormatsMap, std::size(inputOutputFormatsMap)) &&
         addEntry(metadata, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
                  streamConfigurations) &&
         addEntry(metadata, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
                  minFrameDurations) &&
         addEntry(metadata, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
                  stallDurations) &&
         addSingle(metadata, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
                   target.maxDigitalZoom) &&
         addEntry(metadata, ANDROID_CONTROL_ZOOM_RATIO_RANGE, zoomRatioRange,
                  std::size(zoomRatioRange)) &&
         addSingle(metadata, ANDROID_SCALER_CROPPING_TYPE, croppingType) &&
         (target.highSpeedVideoCount == 0 ||
          addEntry(metadata,
                   ANDROID_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS,
                   target.highSpeedVideo, target.highSpeedVideoCount * 5)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArray,
                  std::size(activeArray)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
                  activeArray, std::size(activeArray)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArraySize,
                  std::size(pixelArraySize)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_PHYSICAL_SIZE, physicalSize,
                  std::size(physicalSize)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
                  exposureTimeRange, std::size(exposureTimeRange)) &&
         addEntry(metadata, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
                  sensitivityRange, std::size(sensitivityRange)) &&
         addSingle(metadata, ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
                   colorArrangement) &&
         addSingle(metadata, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
                   target.maxFrameDuration) &&
         addSingle(metadata, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
                   timestampSource) &&
         addEntry(metadata, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
                  faceDetectModes, std::size(faceDetectModes)) &&
         addSingle(metadata, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, 15) &&
         addMissingStandardEntries(metadata, target) &&
         addLogicalMultiCameraEntries(metadata, target);
}

// 真机 gold 存在但原 HAL 缺失的 22 个 standard characteristics tag
// （cam0/cam1 差异仅 maxAnalogSensitivity，用 lensFacing 区分）。
static bool addMissingStandardEntries(camera_metadata_t *metadata,
                                      const Note13CameraTarget &target) {
  const uint8_t aberrationModes[] = {ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF};
  const int32_t postRawBoost[] = {100, 100};
  const float filterDensities[] = {0.0f};
  // ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES 是 TYPE_INT32：必须用 int32 数组，
  // 用 uint8_t 会让 add_camera_metadata_entry 按 4 字节/元素越界读相邻栈数据
  // （真机值 [0, 5] 会变成 [1280, 6553600] 之类的垃圾）。
  const int32_t testPatternModes[] = {0, 5};
  const int32_t blackLevelPattern[] = {64, 64, 64, 64};
  const camera_metadata_rational_t identity3x3[] = {
      {65536, 65536}, {0, 65536},    {0, 65536},
      {0, 65536},     {65536, 65536}, {0, 65536},
      {0, 65536},     {0, 65536},    {65536, 65536},
  };
  const camera_metadata_rational_t colorTransform1[] = {
      {43699, 65536},  {-10413, 65536}, {-5619, 65536},
      {-37614, 65536}, {91081, 65536},  {9373, 65536},
      {-9036, 65536},  {17377, 65536},  {39559, 65536},
  };
  const camera_metadata_rational_t colorTransform2[] = {
      {100366, 65536}, {-30776, 65536}, {-14094, 65536},
      {-31210, 65536}, {94721, 65536},  {439, 65536},
      {-4702, 65536},  {15645, 65536},  {15267, 65536},
  };
  const camera_metadata_rational_t forwardMatrix1[] = {
      {44115, 65536}, {12782, 65536}, {6293, 65536},
      {18100, 65536}, {53622, 65536}, {-6187, 65536},
      {1419, 65536},  {-15234, 65536}, {67896, 65536},
  };
  const camera_metadata_rational_t forwardMatrix2[] = {
      {37650, 65536}, {12059, 65536},  {13481, 65536},
      {12702, 65536}, {48849, 65536},  {3984, 65536},
      {-950, 65536},  {-34648, 65536}, {89680, 65536},
  };
  const uint8_t shadingModes[] = {0, 1, 2};
  const uint8_t hotPixelMapModes[] = {0};
  const uint8_t lensShadingMapModes[] = {0, 1};
  const uint8_t toneMapModes[] = {0, 1, 2};
  const uint8_t referenceIlluminant1 = 21;
  const uint8_t referenceIlluminant2 = 17;
  const int32_t maxAnalogSensitivity =
      target.lensFacing == ANDROID_LENS_FACING_BACK ? 1280 : 320;
  return addEntry(metadata, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
                  aberrationModes, std::size(aberrationModes)) &&
         addEntry(metadata, ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE,
                  postRawBoost, std::size(postRawBoost)) &&
         addEntry(metadata, ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
                  filterDensities, std::size(filterDensities)) &&
         addSingle(metadata, ANDROID_REPROCESS_MAX_CAPTURE_STALL, 3) &&
         addEntry(metadata, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
                  testPatternModes, std::size(testPatternModes)) &&
         addEntry(metadata, ANDROID_SENSOR_BLACK_LEVEL_PATTERN,
                  blackLevelPattern, std::size(blackLevelPattern)) &&
         addEntry(metadata, ANDROID_SENSOR_CALIBRATION_TRANSFORM1,
                  identity3x3, std::size(identity3x3)) &&
         addEntry(metadata, ANDROID_SENSOR_CALIBRATION_TRANSFORM2,
                  identity3x3, std::size(identity3x3)) &&
         addEntry(metadata, ANDROID_SENSOR_COLOR_TRANSFORM1, colorTransform1,
                  std::size(colorTransform1)) &&
         addEntry(metadata, ANDROID_SENSOR_COLOR_TRANSFORM2, colorTransform2,
                  std::size(colorTransform2)) &&
         addEntry(metadata, ANDROID_SENSOR_FORWARD_MATRIX1, forwardMatrix1,
                  std::size(forwardMatrix1)) &&
         addEntry(metadata, ANDROID_SENSOR_FORWARD_MATRIX2, forwardMatrix2,
                  std::size(forwardMatrix2)) &&
         addSingle(metadata, ANDROID_SENSOR_INFO_WHITE_LEVEL, 1023) &&
         addSingle(metadata, ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
                   maxAnalogSensitivity) &&
         addSingle(metadata, ANDROID_SENSOR_REFERENCE_ILLUMINANT1,
                   referenceIlluminant1) &&
         addSingle(metadata, ANDROID_SENSOR_REFERENCE_ILLUMINANT2,
                   referenceIlluminant2) &&
         addEntry(metadata, ANDROID_SHADING_AVAILABLE_MODES, shadingModes,
                  std::size(shadingModes)) &&
         addEntry(metadata,
                  ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
                  hotPixelMapModes, std::size(hotPixelMapModes)) &&
         addEntry(metadata,
                  ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
                  lensShadingMapModes, std::size(lensShadingMapModes)) &&
         addSingle(metadata, ANDROID_SYNC_MAX_LATENCY, 0) &&
         addEntry(metadata, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
                  toneMapModes, std::size(toneMapModes)) &&
         addSingle(metadata, ANDROID_TONEMAP_MAX_CURVE_POINTS, 101);
}

// 真机 vendor tags：按 camera id（"0"/"1"）写 cam0/cam1 值。
// 值统一按 int32 存放，写入宽度必须与描述符声明的 type 一致：真机 55 项里有
// 20 个是 byte、1 个是 int64，一律按 int32 写会让 App 侧用 Key<byte[]>/
// Key<long[]> 取值时类型不符（allValueClass 从 [B/[J 变成 [I）。
static bool addVendorTagEntries(camera_metadata_t *metadata,
                                const char *cameraId) {
  const bool isFront = strcmp(cameraId, "1") == 0;
  for (size_t i = 0; i < kNote13VendorTagCount; i++) {
    const auto &tag = kNote13VendorTags[i];
    const int32_t *values = isFront ? tag.cam1 : tag.cam0;
    const size_t count = isFront ? tag.cam1Count : tag.cam0Count;
    if (values == nullptr || count == 0) {
      continue;  // 该 tag 在此相机不存在（与真机集合一致）
    }
    const Note13VendorTagDesc *desc = note13VendorTagDesc(tag.tagId);
    if (desc == nullptr) {
      return false;
    }
    bool added = false;
    switch (desc->tagType) {
      case TYPE_BYTE:
        added = addEntry(metadata, tag.tagId,
                         std::vector<uint8_t>(values, values + count));
        break;
      case TYPE_INT64:
        added = addEntry(metadata, tag.tagId,
                         std::vector<int64_t>(values, values + count));
        break;
      default:
        added = addEntry(metadata, tag.tagId, values, count);
        break;
    }
    if (!added) {
      return false;
    }
  }
  return true;
}

// available*Keys：直接采用真机 gold 逐项导出的清单（cdh_note13_vendor_tags.inc：
// cam0 = 205 request / 194 result / 22 session，cam1 = 201 / 188 / 21）。
// 原先硬编码 36 个 AOSP external-camera 默认 request key + 8 个 result key，
// 还把 request key 直接当 session key 用，导致 App 侧
// getAvailableCaptureRequestKeys() 只剩 38 项（真机 189）、
// getAvailableCaptureResultKeys() 只剩 13 项（真机 174），
// 手动曝光/RAW/MTK 与小米 vendor 请求键全部缺失。
static bool addKeyLists(camera_metadata_t *metadata, const char *cameraId) {
  const bool isFront = strcmp(cameraId, "1") == 0;
  const uint32_t *requestKeys =
      isFront ? kGoldCam1RequestKeys : kGoldCam0RequestKeys;
  const size_t requestKeyCount = isFront ? std::size(kGoldCam1RequestKeys)
                                         : std::size(kGoldCam0RequestKeys);
  const uint32_t *resultKeys =
      isFront ? kGoldCam1ResultKeys : kGoldCam0ResultKeys;
  const size_t resultKeyCount = isFront ? std::size(kGoldCam1ResultKeys)
                                        : std::size(kGoldCam0ResultKeys);
  const uint32_t *sessionKeys =
      isFront ? kGoldCam1SessionKeys : kGoldCam0SessionKeys;
  const size_t sessionKeyCount = isFront ? std::size(kGoldCam1SessionKeys)
                                         : std::size(kGoldCam0SessionKeys);
  // 真机 gold 摄像头特性 key 清单（顺序与 tag id 升序一致）：
  // Standard 部分按真机 getKeys 顺序补齐，缺少的由上述 addMissingStandardEntries 提供值。
  const int32_t standardCharacteristicsKeys[] = {
      ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
      ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
      ANDROID_CONTROL_AE_AVAILABLE_MODES,
      ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
      ANDROID_CONTROL_AE_COMPENSATION_RANGE,
      ANDROID_CONTROL_AE_COMPENSATION_STEP,
      ANDROID_CONTROL_AE_LOCK_AVAILABLE,
      ANDROID_CONTROL_AF_AVAILABLE_MODES,
      ANDROID_CONTROL_AVAILABLE_EFFECTS,
      ANDROID_CONTROL_AVAILABLE_MODES,
      ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
      ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
      ANDROID_CONTROL_AWB_AVAILABLE_MODES,
      ANDROID_CONTROL_AWB_LOCK_AVAILABLE,
      ANDROID_CONTROL_MAX_REGIONS,
      ANDROID_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE,
      ANDROID_CONTROL_ZOOM_RATIO_RANGE,
      ANDROID_EDGE_AVAILABLE_EDGE_MODES,
      ANDROID_FLASH_INFO_AVAILABLE,
      ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
      ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
      ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
      ANDROID_LENS_FACING,
      ANDROID_LENS_INFO_AVAILABLE_APERTURES,
      ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
      ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
      ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
      ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
      ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE,
      ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE,
      ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
      ANDROID_REPROCESS_MAX_CAPTURE_STALL,
      ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
      ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS,
      ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
      ANDROID_REQUEST_PARTIAL_RESULT_COUNT,
      ANDROID_REQUEST_PIPELINE_MAX_DEPTH,
      ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM,
      ANDROID_SCALER_CROPPING_TYPE,
      ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
      ANDROID_SENSOR_BLACK_LEVEL_PATTERN,
      ANDROID_SENSOR_CALIBRATION_TRANSFORM1,
      ANDROID_SENSOR_CALIBRATION_TRANSFORM2,
      ANDROID_SENSOR_COLOR_TRANSFORM1,
      ANDROID_SENSOR_COLOR_TRANSFORM2,
      ANDROID_SENSOR_FORWARD_MATRIX1,
      ANDROID_SENSOR_FORWARD_MATRIX2,
      ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
      ANDROID_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT,
      ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE,
      ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
      ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
      ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
      ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
      ANDROID_SENSOR_INFO_SENSITIVITY_RANGE,
      ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE,
      ANDROID_SENSOR_INFO_WHITE_LEVEL,
      ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY,
      ANDROID_SENSOR_ORIENTATION,
      ANDROID_SENSOR_REFERENCE_ILLUMINANT1,
      ANDROID_SENSOR_REFERENCE_ILLUMINANT2,
      ANDROID_SHADING_AVAILABLE_MODES,
      ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
      ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
      ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
      ANDROID_STATISTICS_INFO_MAX_FACE_COUNT,
      ANDROID_SYNC_MAX_LATENCY,
      ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES,
      ANDROID_TONEMAP_MAX_CURVE_POINTS,
  };
  // 追加真机 vendor tags（按 cam0/cam1 各自存在的子集），与 metadata 实际写入一致。
  std::vector<int32_t> characteristicsKeys(
      standardCharacteristicsKeys,
      standardCharacteristicsKeys + std::size(standardCharacteristicsKeys));
  for (size_t i = 0; i < kNote13VendorTagCount; i++) {
    const auto &tag = kNote13VendorTags[i];
    if (isFront && (tag.cam1 == nullptr || tag.cam1Count == 0)) {
      continue;
    }
    characteristicsKeys.push_back(static_cast<int32_t>(tag.tagId));
  }
  return addEntry(metadata, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS,
                  requestKeys, requestKeyCount) &&
         addEntry(metadata, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, resultKeys,
                  resultKeyCount) &&
         addEntry(metadata, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
                  characteristicsKeys) &&
         addEntry(metadata, ANDROID_REQUEST_AVAILABLE_SESSION_KEYS,
                  sessionKeys, sessionKeyCount);
}

static CameraMetadata buildStaticMetadata(const Note13CameraTarget &target,
                                          bool *ok) {
  CameraMetadata out;
  // 128 entries 不够容纳补齐的 standard tags + 55 个 vendor tags（每个 1 entry），
  // 容量提升到 384 entries / 131072 bytes 数据区。
  camera_metadata_t *metadata = allocate_camera_metadata(384, 131072);
  if (metadata == nullptr) {
    *ok = false;
    return out;
  }
  bool added = addCommonStaticEntries(metadata, target) &&
               addVendorTagEntries(metadata, target.id) &&
               addSingle(metadata, ANDROID_SENSOR_ORIENTATION,
                         target.sensorOrientation) &&
               addKeyLists(metadata, target.id);
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
  const uint8_t cameraIdType = oplusCameraIdType(target);
  added = added && addEntry(metadata, kOplusSupportedCameraIdTypeTag,
                            &cameraIdType, 1);
#endif
  if (added && validate_camera_metadata_structure(metadata, nullptr) == 0) {
    out.metadata = metadataBytes(metadata);
    *ok = true;
  } else {
    *ok = false;
  }
  free_camera_metadata(metadata);
  return out;
}

static CameraMetadata buildRequestMetadata(const Note13CameraTarget &target,
                                           RequestTemplate type, bool *ok) {
  CameraMetadata out;
  camera_metadata_t *metadata = allocate_camera_metadata(64, 4096);
  if (metadata == nullptr) {
    *ok = false;
    return out;
  }
  const uint8_t aberrationMode = ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF;
  const uint8_t controlMode = ANDROID_CONTROL_MODE_AUTO;
  const uint8_t aeAntibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
  const int32_t aeExposureCompensation = 0;
  const uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
  const uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
  const uint8_t aePrecaptureTrigger =
      ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
  const int32_t aeTargetFpsRange[] = {15, 30};
  const uint8_t afMode = target.lensFacing == ANDROID_LENS_FACING_BACK
                             ? ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE
                             : ANDROID_CONTROL_AF_MODE_OFF;
  const uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
  const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
  const uint8_t awbMode = ANDROID_CONTROL_AWB_MODE_AUTO;
  const uint8_t effectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
  const uint8_t sceneMode = ANDROID_CONTROL_SCENE_MODE_DISABLED;
  const uint8_t settingsOverride = ANDROID_CONTROL_SETTINGS_OVERRIDE_OFF;
  const uint8_t videoStabilizationMode =
      ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
  const float zoomRatio = 1.0f;
  const uint8_t edgeMode = ANDROID_EDGE_MODE_OFF;
  const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
  const uint8_t hotPixelMode = ANDROID_HOT_PIXEL_MODE_OFF;
  const int32_t jpegOrientation = 0;
  const uint8_t jpegQuality = 95;
  const uint8_t jpegThumbnailQuality = 90;
  const uint8_t lensOpticalStabilizationMode =
      ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
  const uint8_t noiseReductionMode = ANDROID_NOISE_REDUCTION_MODE_OFF;
  const uint8_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
  const uint8_t hotPixelMapMode = ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF;
  const uint8_t tonemapMode = ANDROID_TONEMAP_MODE_FAST;
  const int32_t cropRegion[] = {0, 0, target.pixelWidth, target.pixelHeight};
  const int32_t thumbnailSize[] = {0, 0};
  const int64_t exposureTime = 10000000LL;
  const int64_t frameDuration = 33333333LL;
  const int32_t sensitivity = 100;
  uint8_t captureIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
  if (type == RequestTemplate::STILL_CAPTURE) {
    captureIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
  } else if (type == RequestTemplate::VIDEO_RECORD) {
    captureIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
  } else if (type == RequestTemplate::VIDEO_SNAPSHOT) {
    captureIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
  }
  const bool added =
      addSingle(metadata, ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
                aberrationMode) &&
      addSingle(metadata, ANDROID_CONTROL_MODE, controlMode) &&
      addSingle(metadata, ANDROID_CONTROL_AE_ANTIBANDING_MODE,
                aeAntibandingMode) &&
      addSingle(metadata, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
                aeExposureCompensation) &&
      addSingle(metadata, ANDROID_CONTROL_AE_LOCK, aeLock) &&
      addSingle(metadata, ANDROID_CONTROL_AE_MODE, aeMode) &&
      addSingle(metadata, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
                aePrecaptureTrigger) &&
      addEntry(metadata, ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
               aeTargetFpsRange, std::size(aeTargetFpsRange)) &&
      addSingle(metadata, ANDROID_CONTROL_AF_MODE, afMode) &&
      addSingle(metadata, ANDROID_CONTROL_AF_TRIGGER, afTrigger) &&
      addSingle(metadata, ANDROID_CONTROL_AWB_LOCK, awbLock) &&
      addSingle(metadata, ANDROID_CONTROL_AWB_MODE, awbMode) &&
      addSingle(metadata, ANDROID_CONTROL_CAPTURE_INTENT, captureIntent) &&
      addSingle(metadata, ANDROID_CONTROL_EFFECT_MODE, effectMode) &&
      addSingle(metadata, ANDROID_CONTROL_SCENE_MODE, sceneMode) &&
      addSingle(metadata, ANDROID_CONTROL_SETTINGS_OVERRIDE,
                settingsOverride) &&
      addSingle(metadata, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
                videoStabilizationMode) &&
      addSingle(metadata, ANDROID_CONTROL_ZOOM_RATIO, zoomRatio) &&
      addSingle(metadata, ANDROID_EDGE_MODE, edgeMode) &&
      addSingle(metadata, ANDROID_FLASH_MODE, flashMode) &&
      addSingle(metadata, ANDROID_HOT_PIXEL_MODE, hotPixelMode) &&
      addSingle(metadata, ANDROID_JPEG_ORIENTATION, jpegOrientation) &&
      addSingle(metadata, ANDROID_JPEG_QUALITY, jpegQuality) &&
      addSingle(metadata, ANDROID_JPEG_THUMBNAIL_QUALITY,
                jpegThumbnailQuality) &&
      addEntry(metadata, ANDROID_JPEG_THUMBNAIL_SIZE, thumbnailSize,
               std::size(thumbnailSize)) &&
      addSingle(metadata, ANDROID_LENS_APERTURE, target.aperture) &&
      addSingle(metadata, ANDROID_LENS_FOCAL_LENGTH, target.focalLength) &&
      addSingle(metadata, ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
                lensOpticalStabilizationMode) &&
      addSingle(metadata, ANDROID_NOISE_REDUCTION_MODE, noiseReductionMode) &&
      addEntry(metadata, ANDROID_SCALER_CROP_REGION, cropRegion,
               std::size(cropRegion)) &&
      addSingle(metadata, ANDROID_SENSOR_EXPOSURE_TIME, exposureTime) &&
      addSingle(metadata, ANDROID_SENSOR_FRAME_DURATION, frameDuration) &&
      addSingle(metadata, ANDROID_SENSOR_SENSITIVITY, sensitivity) &&
      addSingle(metadata, ANDROID_STATISTICS_FACE_DETECT_MODE,
                faceDetectMode) &&
      addSingle(metadata, ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE,
                hotPixelMapMode) &&
      addSingle(metadata, ANDROID_TONEMAP_MODE, tonemapMode);
  if (added && validate_camera_metadata_structure(metadata, nullptr) == 0) {
    out.metadata = metadataBytes(metadata);
    *ok = true;
  } else {
    *ok = false;
  }
  free_camera_metadata(metadata);
  return out;
}

static CameraMetadata buildResultMetadata(const Note13CameraTarget &target,
                                          int64_t timestamp, bool *ok) {
  CameraMetadata out;
  camera_metadata_t *metadata = allocate_camera_metadata(32, 1024);
  if (metadata == nullptr) {
    *ok = false;
    return out;
  }
  const uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
  const uint8_t afState = ANDROID_CONTROL_AF_STATE_INACTIVE;
  const uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
  const uint8_t flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
  const uint8_t lensState = ANDROID_LENS_STATE_STATIONARY;
  const uint8_t faceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
  const uint8_t pipelineDepth = 1;
  const bool added =
      addSingle(metadata, ANDROID_CONTROL_AE_STATE, aeState) &&
      addSingle(metadata, ANDROID_CONTROL_AF_STATE, afState) &&
      addSingle(metadata, ANDROID_CONTROL_AWB_STATE, awbState) &&
      addSingle(metadata, ANDROID_FLASH_STATE, flashState) &&
      addSingle(metadata, ANDROID_LENS_STATE, lensState) &&
      addSingle(metadata, ANDROID_REQUEST_PIPELINE_DEPTH, pipelineDepth) &&
      addSingle(metadata, ANDROID_SENSOR_TIMESTAMP, timestamp) &&
      addSingle(metadata, ANDROID_STATISTICS_FACE_DETECT_MODE, faceDetectMode);
  if (added && validate_camera_metadata_structure(metadata, nullptr) == 0) {
    out.metadata = metadataBytes(metadata);
    *ok = true;
  } else {
    *ok = false;
  }
  free_camera_metadata(metadata);
  return out;
}

static int64_t nowNs() {
  timespec ts = {};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

static size_t alignToWordBoundary(size_t length) {
  constexpr size_t kAlignmentSize = 8;
  return (length + kAlignmentSize - 1) & ~(kAlignmentSize - 1U);
}

static size_t alignToPageBoundary(size_t length) {
  const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  return (length + pageSize - 1) & ~(pageSize - 1U);
}

static GrantorDescriptor makeGrantor(int32_t fdIndex, size_t offset,
                                     size_t extent) {
  GrantorDescriptor grantor;
  grantor.fdIndex = fdIndex;
  grantor.offset = static_cast<int32_t>(alignToWordBoundary(offset));
  grantor.extent = static_cast<int64_t>(extent);
  return grantor;
}

static std::vector<GrantorDescriptor> makeMetadataQueueGrantors() {
  return {
      makeGrantor(0, 0, kRingBufferPositionSize),
      makeGrantor(0, kRingBufferPositionSize, kRingBufferPositionSize),
      makeGrantor(0, kRingBufferPositionSize * 2, kMetadataMsgQueueSize),
  };
}

static int createMetadataQueueFd(const char *name) {
  const size_t mappedSize = alignToPageBoundary(
      alignToWordBoundary(kMetadataMsgQueueSize) + kRingBufferPositionSize * 2);
  int fd = open("/dev/ashmem", O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "open(/dev/ashmem) for %s failed: %d", name, errno);
    return -1;
  }
  char ashmemName[ASHMEM_NAME_LEN] = {};
  std::strncpy(ashmemName, name, sizeof(ashmemName) - 1);
  if (ioctl(fd, ASHMEM_SET_NAME, ashmemName) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "ASHMEM_SET_NAME(%s) failed: %d", name, errno);
    ::close(fd);
    return -1;
  }
  if (ioctl(fd, ASHMEM_SET_SIZE, mappedSize) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "ASHMEM_SET_SIZE(%s, %zu) failed: %d", name,
                        mappedSize, errno);
    ::close(fd);
    return -1;
  }
  if (ioctl(fd, ASHMEM_SET_PROT_MASK, PROT_READ | PROT_WRITE) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "ASHMEM_SET_PROT_MASK(%s) failed: %d", name, errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

static bool fillMetadataQueueDescriptor(
    int fd, MQDescriptor<int8_t, SynchronizedReadWrite> *out) {
  if (fd < 0 || out == nullptr) {
    return false;
  }
  const int dupFd = dup(fd);
  if (dupFd < 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "dup metadata FMQ fd failed: %d",
                        errno);
    return false;
  }
  out->grantors = makeMetadataQueueGrantors();
  out->handle.fds.clear();
  out->handle.ints.clear();
  out->handle.fds.emplace_back(dupFd);
  out->quantum = 1;
  out->flags = kSynchronizedReadWriteFmqFlag;
  return true;
}

static void appendExposedCameraTargets(
    std::vector<const Note13CameraTarget *> *out) {
  for (const auto &target : kTargets) {
    out->push_back(&target);
  }
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
  for (const auto &logicalTarget : kTargets) {
    for (size_t index = 0; index < logicalTarget.physicalTargetCount; index++) {
      out->push_back(&logicalTarget.physicalTargets[index]);
    }
  }
#endif
}

static const Note13CameraTarget *findTarget(const std::string &id) {
  std::vector<const Note13CameraTarget *> targets;
  appendExposedCameraTargets(&targets);
  for (const Note13CameraTarget *target : targets) {
    if (id == target->id) {
      return target;
    }
  }
  return nullptr;
}

static const Note13CameraTarget *
findPhysicalTarget(const Note13CameraTarget &logicalTarget,
                   const std::string &physicalCameraId) {
  if (logicalTarget.physicalTargets == nullptr) {
    return nullptr;
  }
  for (size_t index = 0; index < logicalTarget.physicalTargetCount; index++) {
    const Note13CameraTarget &physicalTarget =
        logicalTarget.physicalTargets[index];
    if (physicalCameraId == physicalTarget.id) {
      return &physicalTarget;
    }
  }
  return nullptr;
}

static std::string cameraDeviceName(const Note13CameraTarget &target) {
  return std::string("device@1.0/internal/") + target.id;
}

class Note13CameraSession : public BnCameraDeviceSession {
public:
  Note13CameraSession(const Note13CameraTarget &target,
                      std::shared_ptr<ICameraDeviceCallback> callback)
      : target_(target), callback_(std::move(callback)),
        requestMetadataQueueFd_(createMetadataQueueFd("cdh-camera-request-fmq")),
        resultMetadataQueueFd_(createMetadataQueueFd("cdh-camera-result-fmq")) {}

  ~Note13CameraSession() override {
    if (requestMetadataQueueFd_ >= 0) {
      ::close(requestMetadataQueueFd_);
    }
    if (resultMetadataQueueFd_ >= 0) {
      ::close(resultMetadataQueueFd_);
    }
  }

  bool isReady() const {
    return requestMetadataQueueFd_ >= 0 && resultMetadataQueueFd_ >= 0;
  }

  ScopedAStatus close() override { return ScopedAStatus::ok(); }

  ScopedAStatus configureStreams(const StreamConfiguration &requested,
                                 std::vector<HalStream> *out) override {
    *out = makeHalStreams(requested);
    return ScopedAStatus::ok();
  }

  ScopedAStatus constructDefaultRequestSettings(RequestTemplate type,
                                                CameraMetadata *out) override {
    bool ok = false;
    *out = buildRequestMetadata(target_, type, &ok);
    return ok ? ScopedAStatus::ok() : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus flush() override { return ScopedAStatus::ok(); }

  ScopedAStatus
  getCaptureRequestMetadataQueue(
      MQDescriptor<int8_t, SynchronizedReadWrite> *out) override {
    return fillMetadataQueueDescriptor(requestMetadataQueueFd_, out)
               ? ScopedAStatus::ok()
               : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus
  getCaptureResultMetadataQueue(
      MQDescriptor<int8_t, SynchronizedReadWrite> *out) override {
    return fillMetadataQueueDescriptor(resultMetadataQueueFd_, out)
               ? ScopedAStatus::ok()
               : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus
  isReconfigurationRequired(const CameraMetadata &, const CameraMetadata &,
                            bool *out) override {
    *out = false;
    return ScopedAStatus::ok();
  }

  ScopedAStatus processCaptureRequest(const std::vector<CaptureRequest> &requests,
                                      const std::vector<BufferCache> &,
                                      int32_t *out) override {
    if (callback_ == nullptr) {
      *out = 0;
      return cameraError(Status::INTERNAL_ERROR);
    }
    std::vector<NotifyMsg> notifications;
    std::vector<CaptureResult> results;
    notifications.reserve(requests.size());
    results.reserve(requests.size());
    for (const auto &request : requests) {
      const int64_t timestamp = nowNs();
      ShutterMsg shutter;
      shutter.frameNumber = request.frameNumber;
      shutter.timestamp = timestamp;
      shutter.readoutTimestamp = timestamp;
      notifications.push_back(NotifyMsg::make<NotifyMsg::Tag::shutter>(shutter));

      CaptureResult result;
      result.frameNumber = request.frameNumber;
      result.fmqResultSize = 0;
      bool metadataOk = false;
      result.result = buildResultMetadata(target_, timestamp, &metadataOk);
      if (!metadataOk) {
        *out = 0;
        return cameraError(Status::INTERNAL_ERROR);
      }
      result.outputBuffers.reserve(request.outputBuffers.size());
      for (const auto &buffer : request.outputBuffers) {
        auto &returned = result.outputBuffers.emplace_back();
        returned.streamId = buffer.streamId;
        returned.bufferId = buffer.bufferId;
        returned.status = BufferStatus::OK;
      }
      result.inputBuffer.streamId = -1;
      result.inputBuffer.bufferId = 0;
      result.partialResult = 1;
      results.push_back(std::move(result));
    }
    ScopedAStatus notifyStatus = callback_->notify(notifications);
    if (!notifyStatus.isOk()) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "camera notify callback failed: %s",
                          notifyStatus.getDescription().c_str());
      *out = 0;
      return cameraError(Status::INTERNAL_ERROR);
    }
    ScopedAStatus resultStatus = callback_->processCaptureResult(results);
    if (!resultStatus.isOk()) {
      __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                          "camera result callback failed: %s",
                          resultStatus.getDescription().c_str());
      *out = 0;
      return cameraError(Status::INTERNAL_ERROR);
    }
    *out = static_cast<int32_t>(requests.size());
    return ScopedAStatus::ok();
  }

  ScopedAStatus signalStreamFlush(const std::vector<int32_t> &, int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus switchToOffline(
      const std::vector<int32_t> &, CameraOfflineSessionInfo *,
      std::shared_ptr<ICameraOfflineSession> *out) override {
    out->reset();
    return cameraError(Status::OPERATION_NOT_SUPPORTED);
  }

  ScopedAStatus repeatingRequestEnd(int32_t, const std::vector<int32_t> &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus configureStreamsV2(const StreamConfiguration &requested,
                                   ConfigureStreamsRet *out) override {
    out->halStreams = makeHalStreams(requested);
    return ScopedAStatus::ok();
  }

private:
  static std::vector<HalStream>
  makeHalStreams(const StreamConfiguration &requested) {
    std::vector<HalStream> halStreams;
    halStreams.reserve(requested.streams.size());
    for (const auto &stream : requested.streams) {
      HalStream hal;
      hal.id = stream.id;
      hal.overrideFormat = stream.format;
      hal.producerUsage = BufferUsage(0);
      hal.consumerUsage = BufferUsage(0);
      hal.maxBuffers = 2;
      hal.overrideDataSpace = stream.dataSpace;
      hal.physicalCameraId = stream.physicalCameraId;
      hal.supportOffline = false;
      hal.enableHalBufferManager = false;
      halStreams.push_back(hal);
    }
    return halStreams;
  }

  const Note13CameraTarget &target_;
  std::shared_ptr<ICameraDeviceCallback> callback_;
  int requestMetadataQueueFd_ = -1;
  int resultMetadataQueueFd_ = -1;
};

class Note13CameraDevice : public BnCameraDevice {
public:
  explicit Note13CameraDevice(const Note13CameraTarget &target)
      : target_(target) {}

  ScopedAStatus getCameraCharacteristics(CameraMetadata *out) override {
    bool ok = false;
    *out = buildStaticMetadata(target_, &ok);
    return ok ? ScopedAStatus::ok() : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus
  getPhysicalCameraCharacteristics(const std::string &physicalCameraId,
                                   CameraMetadata *out) override {
    const Note13CameraTarget *physicalTarget =
        findPhysicalTarget(target_, physicalCameraId);
    if (physicalTarget == nullptr || out == nullptr) {
      return cameraError(Status::ILLEGAL_ARGUMENT);
    }
    bool ok = false;
    *out = buildStaticMetadata(*physicalTarget, &ok);
    return ok ? ScopedAStatus::ok() : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus getResourceCost(CameraResourceCost *out) override {
    out->resourceCost = 50;
    out->conflictingDevices = {};
    return ScopedAStatus::ok();
  }

  ScopedAStatus isStreamCombinationSupported(const StreamConfiguration &,
                                             bool *out) override {
    *out = true;
    return ScopedAStatus::ok();
  }

  ScopedAStatus open(const std::shared_ptr<ICameraDeviceCallback> &callback,
                     std::shared_ptr<ICameraDeviceSession> *out) override {
    if (callback == nullptr) {
      out->reset();
      return cameraError(Status::ILLEGAL_ARGUMENT);
    }
    auto session = ndk::SharedRefBase::make<Note13CameraSession>(target_, callback);
    if (!session->isReady()) {
      out->reset();
      return cameraError(Status::INTERNAL_ERROR);
    }
    *out = std::move(session);
    return ScopedAStatus::ok();
  }

  ScopedAStatus openInjectionSession(
      const std::shared_ptr<ICameraDeviceCallback> &,
      std::shared_ptr<ICameraInjectionSession> *out) override {
    out->reset();
    return cameraError(Status::OPERATION_NOT_SUPPORTED);
  }

  ScopedAStatus setTorchMode(bool) override {
    if (target_.flashAvailable != ANDROID_FLASH_INFO_AVAILABLE_TRUE) {
      return cameraError(Status::OPERATION_NOT_SUPPORTED);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus turnOnTorchWithStrengthLevel(int32_t) override {
    if (target_.flashAvailable != ANDROID_FLASH_INFO_AVAILABLE_TRUE) {
      return cameraError(Status::OPERATION_NOT_SUPPORTED);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus getTorchStrengthLevel(int32_t *out) override {
    *out = target_.flashAvailable == ANDROID_FLASH_INFO_AVAILABLE_TRUE ? 1 : 0;
    return ScopedAStatus::ok();
  }

  ScopedAStatus constructDefaultRequestSettings(RequestTemplate type,
                                                CameraMetadata *out) override {
    bool ok = false;
    *out = buildRequestMetadata(target_, type, &ok);
    return ok ? ScopedAStatus::ok() : cameraError(Status::INTERNAL_ERROR);
  }

  ScopedAStatus
  isStreamCombinationWithSettingsSupported(const StreamConfiguration &,
                                           bool *out) override {
    *out = true;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getSessionCharacteristics(const StreamConfiguration &,
                                          CameraMetadata *out) override {
    bool ok = false;
    *out = buildStaticMetadata(target_, &ok);
    return ok ? ScopedAStatus::ok() : cameraError(Status::INTERNAL_ERROR);
  }

private:
  const Note13CameraTarget &target_;
};

class Note13CameraProvider : public BnCameraProvider {
public:
  ScopedAStatus
  setCallback(const std::shared_ptr<ICameraProviderCallback> &callback) override {
    std::lock_guard<std::mutex> guard(lock_);
    callback_ = callback;
    if (callback_ != nullptr) {
      std::vector<const Note13CameraTarget *> targets;
      appendExposedCameraTargets(&targets);
      for (const Note13CameraTarget *target : targets) {
        const std::string name = cameraDeviceName(*target);
        callback_->cameraDeviceStatusChange(name, CameraDeviceStatus::PRESENT);
        callback_->torchModeStatusChange(
            name, target->flashAvailable == ANDROID_FLASH_INFO_AVAILABLE_TRUE
                      ? TorchModeStatus::AVAILABLE_OFF
                      : TorchModeStatus::NOT_AVAILABLE);
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus getVendorTags(std::vector<VendorTagSection> *out) override {
    out->clear();
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
    VendorTagSection section;
    section.sectionName = "com.oplus";
    VendorTag tag;
    tag.tagId = static_cast<int32_t>(kOplusSupportedCameraIdTypeTag);
    tag.tagName = "supported.cameraid.type";
    tag.tagType = CameraMetadataType::BYTE;
    section.tags.push_back(std::move(tag));
    out->push_back(std::move(section));
#else
    // 真机 vendor tags：按真机的 section 分组上报，tagId/tagName/tagType 逐项一致。
    // 全名 = sectionName + "." + tagName（vendor_tag_ops 侧同理）。
    std::vector<std::string> sectionNames;
    for (size_t i = 0; i < kNote13VendorTagDescCount; i++) {
      const std::string name = kNote13VendorTagDescs[i].section;
      if (std::find(sectionNames.begin(), sectionNames.end(), name) ==
          sectionNames.end()) {
        sectionNames.push_back(name);
      }
    }
    for (const auto &sectionName : sectionNames) {
      VendorTagSection section;
      section.sectionName = sectionName;
      for (size_t i = 0; i < kNote13VendorTagDescCount; i++) {
        const Note13VendorTagDesc &desc = kNote13VendorTagDescs[i];
        if (sectionName != desc.section) {
          continue;
        }
        VendorTag tag;
        tag.tagId = static_cast<int32_t>(desc.tagId);
        tag.tagName = desc.tagName;
        tag.tagType = static_cast<CameraMetadataType>(desc.tagType);
        section.tags.push_back(std::move(tag));
      }
      out->push_back(std::move(section));
    }
#endif
    return ScopedAStatus::ok();
  }

  ScopedAStatus getCameraIdList(std::vector<std::string> *out) override {
    out->clear();
    std::vector<const Note13CameraTarget *> targets;
    appendExposedCameraTargets(&targets);
    for (const Note13CameraTarget *target : targets) {
      out->push_back(cameraDeviceName(*target));
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "reported %zu AIDL camera device names", out->size());
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  getCameraDeviceInterface(const std::string &cameraDeviceName,
                           std::shared_ptr<ICameraDevice> *out) override {
    std::string id = cameraDeviceName;
    const size_t slash = id.rfind('/');
    if (slash != std::string::npos) {
      id = id.substr(slash + 1);
    }
    const Note13CameraTarget *target = findTarget(id);
    if (target == nullptr) {
      out->reset();
      return cameraError(Status::ILLEGAL_ARGUMENT);
    }
    *out = ndk::SharedRefBase::make<Note13CameraDevice>(*target);
    return ScopedAStatus::ok();
  }

  ScopedAStatus notifyDeviceStateChange(int64_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  getConcurrentCameraIds(std::vector<ConcurrentCameraIdCombination> *out)
      override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus isConcurrentStreamCombinationSupported(
      const std::vector<CameraIdAndStreamCombination> &, bool *out) override {
    *out = false;
    return ScopedAStatus::ok();
  }

private:
  std::mutex lock_;
  std::shared_ptr<ICameraProviderCallback> callback_;
};

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(4);
#if defined(CDH_CAMERA_TARGET_ONEPLUSACE6T)
  if (set_camera_metadata_vendor_ops(&kOplusVendorTagOps) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "failed to register OPlus camera vendor metadata");
    return 3;
  }
#else
  if (set_camera_metadata_vendor_ops(&kNote13VendorTagOps) != 0) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "failed to register Note13 camera vendor metadata");
    return 3;
  }
#endif
  auto service = ndk::SharedRefBase::make<Note13CameraProvider>();
  binder_status_t status =
      AServiceManager_addService(service->asBinder().get(), kProviderServiceName);
  if (status != STATUS_OK) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "failed to register %s: %d", kProviderServiceName,
                        status);
    return 2;
  }
  __android_log_print(ANDROID_LOG_INFO, kLogTag, "registered %s",
                      kProviderServiceName);
  ABinderProcess_joinThreadPool();
  return 0;
}
