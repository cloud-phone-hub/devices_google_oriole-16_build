#include <android/binder_auto_utils.h>
#include <android/binder_ibinder.h>
#include <android/binder_status.h>

extern "C" {
// Android 15 NDK ships these libbinder_ndk symbols on device; this SDK layout
// does not expose binder_manager.h, so keep the exact C ABI declarations here.
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
AIBinder *AServiceManager_checkService(const char *instance);
}

#include <aidl/android/hardware/media/bufferpool2/IClientManager.h>
#include <aidl/android/hardware/media/c2/Buffer.h>
#include <aidl/android/hardware/media/c2/BnComponentInterface.h>
#include <aidl/android/hardware/media/c2/BnComponentStore.h>
#include <aidl/android/hardware/media/c2/BnConfigurable.h>
#include <aidl/android/hardware/media/c2/FieldSupportedValues.h>
#include <aidl/android/hardware/media/c2/FieldSupportedValuesQuery.h>
#include <aidl/android/hardware/media/c2/FieldSupportedValuesQueryResult.h>
#include <aidl/android/hardware/media/c2/IComponent.h>
#include <aidl/android/hardware/media/c2/IComponentInterface.h>
#include <aidl/android/hardware/media/c2/IComponentListener.h>
#include <aidl/android/hardware/media/c2/IComponentStore.h>
#include <aidl/android/hardware/media/c2/IConfigurable.h>
#include <aidl/android/hardware/media/c2/IInputSurface.h>
#include <aidl/android/hardware/media/c2/ParamDescriptor.h>
#include <aidl/android/hardware/media/c2/ParamField.h>
#include <aidl/android/hardware/media/c2/Params.h>
#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/StructDescriptor.h>

#include <unistd.h>

#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using aidl::android::hardware::media::bufferpool2::IClientManager;
using aidl::android::hardware::media::c2::BnComponentInterface;
using aidl::android::hardware::media::c2::BnComponentStore;
using aidl::android::hardware::media::c2::BnConfigurable;
using aidl::android::hardware::media::c2::Buffer;
using aidl::android::hardware::media::c2::FieldSupportedValues;
using aidl::android::hardware::media::c2::FieldSupportedValuesQuery;
using aidl::android::hardware::media::c2::FieldSupportedValuesQueryResult;
using aidl::android::hardware::media::c2::IComponent;
using aidl::android::hardware::media::c2::IComponentInterface;
using aidl::android::hardware::media::c2::IComponentListener;
using aidl::android::hardware::media::c2::IComponentStore;
using aidl::android::hardware::media::c2::IConfigurable;
using aidl::android::hardware::media::c2::IInputSurface;
using aidl::android::hardware::media::c2::ParamDescriptor;
using aidl::android::hardware::media::c2::ParamField;
using aidl::android::hardware::media::c2::Params;
using aidl::android::hardware::media::c2::Status;
using aidl::android::hardware::media::c2::StructDescriptor;
using ndk::ScopedAStatus;

static constexpr const char *kServicePrefix =
    "android.hardware.media.c2.IComponentStore/";
static constexpr const char *kDefault1ServiceName =
    "android.hardware.media.c2.IComponentStore/default1";
static constexpr const char *kDefault9ServiceName =
    "android.hardware.media.c2.IComponentStore/default9";
static constexpr const char *kSoftwareServiceName =
    "android.hardware.media.c2.IComponentStore/software";

// Real-device shape: each vendor binary registers exactly one store instance,
// so the rc blocks never pass --instance and the binary name alone selects
// the instance (matching vendor.media.c2 on the device).
// MTK store 注册 default1（default 让给 v4l2 硬件 store，见 android.hardware.media.v412）。
static constexpr const char *kBinaryMtk =
    "android.hardware.media.c2-mediatek";
static constexpr const char *kBinaryDolby =
    "vendor.dolby.media.c2-default-service-dax";

struct Codec2AliasList {
  const char *const *values;
  size_t count;
};

// C2 枚举值列表（profile/level），见 config/codec2.json profileLevel.note
struct Codec2ValueList {
  const int32_t *values;
  size_t count;
};

struct Codec2Target {
  const char *instance;
  const char *name;
  const char *kind;
  const char *domain;
  const char *mediaType;
  int32_t rank;
  Codec2AliasList aliases;
  Codec2ValueList profiles;
  Codec2ValueList levels;
};

#include "codec2_targets.inc"

// ---- C2 profile/level 应答（对齐真机 MTK store）----
// framework 的 Codec2InfoBuilder::addSupportedProfileLevels() 会按组件问
// C2StreamProfileLevelInfo 的 profile/level；答不出来时 MediaCodecInfo 退回 mime
// 默认上限（AVC 仅 Level1 级：maxBlocks=99 / 1485 MB/s / 64kbps），表现为能力塌缩，
// 且 c2.mtk.avc.encoder 会因 XML block-count 下限(100)>默认上限(99) 求交出空区间、
// Java 侧 parseFromInfo 抛异常而被整条丢弃（详见 README §9）。
//
// 组件级常量取自真机 dumpsys profile/levels（SDK 值）经 Codec2Mapper 反查：
//   kParamIndexProfileLevel=0x808；C2ProfileLevelStruct{profile,level} 各 4 字节，
//   但 framework 的 C2ParamField 以**整个 param 对象**为基准算字段偏移：
//   sizeof(C2StreamProfileLevelInfo)=16 = 8(C2Param 头) + 4(profile) + 4(level)，
//   故 profile 偏移 8、level 偏移 12（C2 头文件实测值，非推断）。
//   param index = INFO(0xC0000000) | dir(decoder=INPUT=0 / encoder=OUTPUT=0x10000000)
//                 | IS_STREAM_FLAG(0x02000000) | streamId(0) | coreIndex(0x808)
//   （framework 用 C2StreamProfileLevelInfo pl(encoder, 0u) 构造）。
// 匹配只认 core index + 字段偏移，kind/dir/stream 位放开。
static constexpr uint32_t kProfileLevelCoreIndex = 0x808u;
static constexpr int32_t kProfileFieldOffset = 8;
static constexpr int32_t kLevelFieldOffset = 12;

static bool isProfileLevelField(const ParamField &field, int32_t offset) {
  const uint32_t index = static_cast<uint32_t>(field.index);
  if ((index & 0x0000FFFFu) != kProfileLevelCoreIndex) {
    return false;
  }
  return field.fieldId.offset == offset;
}

static FieldSupportedValues makeEmptyValues() {
  return FieldSupportedValues::make<FieldSupportedValues::Tag::empty>(true);
}

static FieldSupportedValues makeValues(const Codec2ValueList &list) {
  std::vector<int64_t> values;
  values.reserve(list.count);
  for (size_t idx = 0; idx < list.count; ++idx) {
    values.push_back(list.values[idx]);
  }
  return FieldSupportedValues::make<FieldSupportedValues::Tag::values>(
      std::move(values));
}

static Status makeStatus(int32_t value) {
  Status status;
  status.status = value;
  return status;
}

static ScopedAStatus serviceError(int32_t value) {
  return ScopedAStatus::fromServiceSpecificError(value);
}

static IComponentStore::ComponentTraits::Kind kindFor(const char *value) {
  if (std::strcmp(value, "decoder") == 0) {
    return IComponentStore::ComponentTraits::Kind::DECODER;
  }
  if (std::strcmp(value, "encoder") == 0) {
    return IComponentStore::ComponentTraits::Kind::ENCODER;
  }
  return IComponentStore::ComponentTraits::Kind::OTHER;
}

static IComponentStore::ComponentTraits::Domain domainFor(const char *value) {
  if (std::strcmp(value, "audio") == 0) {
    return IComponentStore::ComponentTraits::Domain::AUDIO;
  }
  if (std::strcmp(value, "video") == 0) {
    return IComponentStore::ComponentTraits::Domain::VIDEO;
  }
  if (std::strcmp(value, "image") == 0) {
    return IComponentStore::ComponentTraits::Domain::IMAGE;
  }
  return IComponentStore::ComponentTraits::Domain::OTHER;
}

static std::string serviceNameForInstance(const std::string &instance) {
  return std::string(kServicePrefix) + instance;
}

static bool hasArg(int argc, char **argv, const char *value) {
  for (int idx = 1; idx < argc; ++idx) {
    if (std::strcmp(argv[idx], value) == 0) {
      return true;
    }
  }
  return false;
}

static bool serviceIsSoftware(const char *serviceName) {
  return std::strcmp(serviceName, kSoftwareServiceName) == 0;
}

static void writeJSONString(std::ostream &os, const std::string &value) {
  os << '"';
  for (const char ch : value) {
    switch (ch) {
    case '\\':
      os << "\\\\";
      break;
    case '"':
      os << "\\\"";
      break;
    case '\n':
      os << "\\n";
      break;
    case '\r':
      os << "\\r";
      break;
    case '\t':
      os << "\\t";
      break;
    default:
      os << ch;
      break;
    }
  }
  os << '"';
}

static bool traitIsEncoder(const IComponentStore::ComponentTraits &traits) {
  return traits.kind == IComponentStore::ComponentTraits::Kind::ENCODER;
}

static bool traitIsHardwareAccelerated(
    const char *serviceName, const IComponentStore::ComponentTraits &traits) {
  // Android framework treats device Codec2 stores as hardware-backed codec
  // inputs; keep software store entries explicit so DevCheck-style summaries
  // can distinguish HW and SW directions without reading XML.
  return !serviceIsSoftware(serviceName) &&
         traits.domain == IComponentStore::ComponentTraits::Domain::VIDEO;
}

static void appendProbeServiceJSON(std::ostream &os, const char *serviceName,
                                   bool ok, const std::string &error,
                                   size_t componentCount,
                                   const std::string &configurableStatus = "",
                                   const std::string &configurableName = "") {
  os << "{\"name\":";
  writeJSONString(os, serviceName);
  os << ",\"ok\":" << (ok ? "true" : "false");
  if (!error.empty()) {
    os << ",\"error\":";
    writeJSONString(os, error);
  }
  if (ok) {
    os << ",\"componentCount\":" << componentCount;
  }
  if (!configurableStatus.empty()) {
    os << ",\"configurableStatus\":";
    writeJSONString(os, configurableStatus);
  }
  if (!configurableName.empty()) {
    os << ",\"configurableName\":";
    writeJSONString(os, configurableName);
  }
  os << "}";
}

static void appendProbeCodecJSON(
    std::ostream &os, const char *serviceName,
    const IComponentStore::ComponentTraits &traits) {
  const bool software = serviceIsSoftware(serviceName);
  os << "{\"name\":";
  writeJSONString(os, traits.name);
  os << ",\"encoder\":" << (traitIsEncoder(traits) ? "true" : "false");
  os << ",\"hardwareAccelerated\":"
     << (traitIsHardwareAccelerated(serviceName, traits) ? "true" : "false");
  os << ",\"softwareOnly\":" << (software ? "true" : "false");
  os << ",\"vendor\":" << (!software ? "true" : "false");
  os << ",\"supportedTypes\":[";
  writeJSONString(os, traits.mediaType);
  os << "]}";
}

static int runProbe() {
  // Probe the same Store service names that framework MediaCodecList resolves;
  // XML, dumpsys and app-side providers are intentionally outside this path.
  const char *const serviceNames[] = {
      kDefault1ServiceName,
      kDefault9ServiceName,
      kSoftwareServiceName,
  };
  std::vector<std::string> serviceJSON;
  std::vector<std::string> codecJSON;
  bool ok = true;
  std::string firstError;

  for (const char *serviceName : serviceNames) {
    ndk::SpAIBinder binder(AServiceManager_checkService(serviceName));
    if (binder.get() == nullptr) {
      ok = false;
      const std::string error = std::string(serviceName) + " missing";
      if (firstError.empty()) {
        firstError = error;
      }
      std::ostringstream serviceOut;
      appendProbeServiceJSON(serviceOut, serviceName, false, error, 0);
      serviceJSON.push_back(serviceOut.str());
      continue;
    }
    std::shared_ptr<IComponentStore> store = IComponentStore::fromBinder(binder);
    if (store == nullptr) {
      ok = false;
      const std::string error = std::string(serviceName) + " binder cast failed";
      if (firstError.empty()) {
        firstError = error;
      }
      std::ostringstream serviceOut;
      appendProbeServiceJSON(serviceOut, serviceName, false, error, 0);
      serviceJSON.push_back(serviceOut.str());
      continue;
    }

    std::vector<IComponentStore::ComponentTraits> traits;
    ScopedAStatus status = store->listComponents(&traits);
    if (!status.isOk()) {
      ok = false;
      const std::string error =
          std::string(serviceName) + " listComponents failed: " +
          status.getDescription();
      if (firstError.empty()) {
        firstError = error;
      }
      std::ostringstream serviceOut;
      appendProbeServiceJSON(serviceOut, serviceName, false, error, 0);
      serviceJSON.push_back(serviceOut.str());
      continue;
    }

    std::shared_ptr<IConfigurable> configurable;
    ScopedAStatus configurableCall = store->getConfigurable(&configurable);
    if (!configurableCall.isOk() || configurable == nullptr) {
      ok = false;
      const std::string error = std::string(serviceName) +
          " getConfigurable failed: " + configurableCall.getDescription();
      if (firstError.empty()) {
        firstError = error;
      }
      std::ostringstream serviceOut;
      appendProbeServiceJSON(serviceOut, serviceName, false, error,
                             traits.size(), "failed");
      serviceJSON.push_back(serviceOut.str());
      continue;
    }
    std::string configurableName;
    ScopedAStatus configurableNameCall = configurable->getName(&configurableName);
    if (!configurableNameCall.isOk()) {
      ok = false;
      const std::string error = std::string(serviceName) +
          " configurable getName failed: " + configurableNameCall.getDescription();
      if (firstError.empty()) {
        firstError = error;
      }
      std::ostringstream serviceOut;
      appendProbeServiceJSON(serviceOut, serviceName, false, error,
                             traits.size(), "failed");
      serviceJSON.push_back(serviceOut.str());
      continue;
    }

    std::ostringstream serviceOut;
    appendProbeServiceJSON(serviceOut, serviceName, true, "", traits.size(),
                           "ok", configurableName);
    serviceJSON.push_back(serviceOut.str());
    for (const auto &trait : traits) {
      std::ostringstream codecOut;
      appendProbeCodecJSON(codecOut, serviceName, trait);
      codecJSON.push_back(codecOut.str());
    }
  }

  std::cout << "{\"source\":\"codec2-aidl-probe\",\"ok\":"
            << (ok ? "true" : "false");
  if (!firstError.empty()) {
    std::cout << ",\"error\":";
    writeJSONString(std::cout, firstError);
  }
  std::cout << ",\"services\":[";
  for (size_t idx = 0; idx < serviceJSON.size(); ++idx) {
    if (idx != 0) {
      std::cout << ',';
    }
    std::cout << serviceJSON[idx];
  }
  std::cout << "],\"codecs\":[";
  for (size_t idx = 0; idx < codecJSON.size(); ++idx) {
    if (idx != 0) {
      std::cout << ',';
    }
    std::cout << codecJSON[idx];
  }
  std::cout << "]}\n";
  return ok ? 0 : 2;
}

static bool targetMatchesName(const Codec2Target &target,
                              const std::string &name) {
  if (target.name == name) {
    return true;
  }
  for (size_t idx = 0; idx < target.aliases.count; ++idx) {
    if (target.aliases.values[idx] == name) {
      return true;
    }
  }
  return false;
}

class Codec2StoreConfigurable : public BnConfigurable {
public:
  Codec2StoreConfigurable(std::string name, Codec2ValueList profiles,
                          Codec2ValueList levels)
      : name_(std::move(name)), profiles_(profiles), levels_(levels) {}

  ScopedAStatus config(const Params &inParams, bool,
                       IConfigurable::ConfigResult *out) override {
    out->params = inParams;
    out->failures.clear();
    out->status = makeStatus(Status::OK);
    return ScopedAStatus::ok();
  }

  ScopedAStatus getId(int32_t *out) override {
    *out = 0;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getName(std::string *out) override {
    *out = name_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus query(const std::vector<int32_t> &, bool,
                      IConfigurable::QueryResult *out) override {
    out->params.params.clear();
    out->status = makeStatus(Status::OK);
    return ScopedAStatus::ok();
  }

  ScopedAStatus querySupportedParams(int32_t, int32_t,
                                     std::vector<ParamDescriptor> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  // profile/level 是 framework 拿到真实能力上限的唯一来源：按组件答
  // C2StreamProfileLevelInfo 的 profile（POSSIBLE）与 level（CURRENT）列表；
  // 其他字段仍回 OMITTED（本 store 不实例化组件、不承载其它参数语义）。
  ScopedAStatus querySupportedValues(
      const std::vector<FieldSupportedValuesQuery> &inFields, bool,
      IConfigurable::QuerySupportedValuesResult *out) override {
    out->values.clear();
    out->values.reserve(inFields.size());
    for (const FieldSupportedValuesQuery &field : inFields) {
      FieldSupportedValuesQueryResult result;
      const Codec2ValueList *answer = nullptr;
      if (isProfileLevelField(field.field, kProfileFieldOffset)) {
        answer = &profiles_;
      } else if (isProfileLevelField(field.field, kLevelFieldOffset)) {
        answer = &levels_;
      }
      // 诊断：收到的每次查询都记一行（组件数量有限，量很小）
      __android_log_print(ANDROID_LOG_INFO, "cdh-codec2",
                          "%s: qv idx=0x%08x off=%d size=%d type=%d -> %s",
                          name_.c_str(), static_cast<uint32_t>(field.field.index),
                          field.field.fieldId.offset, field.field.fieldId.sizeBytes,
                          static_cast<int>(field.type),
                          (answer != nullptr && answer->count > 0) ? "answer" : "omit");
      if (answer != nullptr && answer->count > 0) {
        result.status = makeStatus(Status::OK);
        result.values = makeValues(*answer);
      } else {
        result.status = makeStatus(Status::OMITTED);
        result.values = makeEmptyValues();
      }
      out->values.push_back(result);
    }
    out->status = makeStatus(Status::OK);
    return ScopedAStatus::ok();
  }

private:
  std::string name_;
  Codec2ValueList profiles_;
  Codec2ValueList levels_;
};

class Codec2StoreInterface : public BnComponentInterface {
public:
  Codec2StoreInterface(const Codec2Target &target)
      : configurable_(ndk::SharedRefBase::make<Codec2StoreConfigurable>(
            target.name, target.profiles, target.levels)) {}

  ScopedAStatus getConfigurable(std::shared_ptr<IConfigurable> *out) override {
    *out = configurable_;
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IConfigurable> configurable_;
};

class Codec2Store : public BnComponentStore {
public:
  explicit Codec2Store(std::string instance)
      : instance_(std::move(instance)),
        configurable_(ndk::SharedRefBase::make<Codec2StoreConfigurable>(
            serviceNameForInstance(instance_), Codec2ValueList{nullptr, 0},
            Codec2ValueList{nullptr, 0})) {
    for (const auto &target : kCodec2Targets) {
      if (target.instance == instance_) {
        targets_.push_back(&target);
      }
    }
  }

  size_t targetCount() const { return targets_.size(); }

  ScopedAStatus copyBuffer(const Buffer &, const Buffer &) override {
    return serviceError(Status::CANNOT_DO);
  }

  ScopedAStatus createComponent(
      const std::string &, const std::shared_ptr<IComponentListener> &,
      const std::shared_ptr<IClientManager> &,
      std::shared_ptr<IComponent> *) override {
    return serviceError(Status::CANNOT_DO);
  }

  ScopedAStatus createInterface(
      const std::string &name,
      std::shared_ptr<IComponentInterface> *out) override {
    const Codec2Target *target = findTarget(name);
    if (target == nullptr) {
      return serviceError(Status::NOT_FOUND);
    }
    *out = ndk::SharedRefBase::make<Codec2StoreInterface>(*target);
    return ScopedAStatus::ok();
  }

  ScopedAStatus getConfigurable(std::shared_ptr<IConfigurable> *out) override {
    *out = configurable_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getPoolClientManager(
      std::shared_ptr<IClientManager> *out) override {
    out->reset();
    return serviceError(Status::CANNOT_DO);
  }

  ScopedAStatus getStructDescriptors(const std::vector<int32_t> &,
                                     std::vector<StructDescriptor> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus
  listComponents(std::vector<IComponentStore::ComponentTraits> *out) override {
    out->clear();
    out->reserve(targets_.size());
    for (const Codec2Target *target : targets_) {
      // DevCheck/AIDA64 codec fields depend on ComponentTraits; this service
      // publishes only the registry metadata and refuses actual codec creation.
      IComponentStore::ComponentTraits traits;
      traits.name = target->name;
      traits.domain = domainFor(target->domain);
      traits.kind = kindFor(target->kind);
      traits.rank = target->rank;
      traits.mediaType = target->mediaType;
      for (size_t idx = 0; idx < target->aliases.count; ++idx) {
        traits.aliases.push_back(target->aliases.values[idx]);
      }
      out->push_back(traits);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus createInputSurface(
      std::shared_ptr<IInputSurface> *) override {
    return serviceError(Status::CANNOT_DO);
  }

private:
  const Codec2Target *findTarget(const std::string &name) const {
    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&](const Codec2Target *target) {
                             return targetMatchesName(*target, name);
                           });
    if (it == targets_.end()) {
      return nullptr;
    }
    return *it;
  }

  std::string instance_;
  std::vector<const Codec2Target *> targets_;
  std::shared_ptr<IConfigurable> configurable_;
};

static binder_status_t
registerAsService(const std::shared_ptr<ndk::ICInterface> &service,
                  const char *name) {
  return AServiceManager_addService(service->asBinder().get(), name);
}

static std::vector<std::string> parseInstances(int argc, char **argv) {
  std::vector<std::string> instances;
  for (int idx = 1; idx + 1 < argc; ++idx) {
    if (std::strcmp(argv[idx], "--instance") == 0) {
      instances.emplace_back(argv[idx + 1]);
      ++idx;
    }
  }
  return instances;
}

static std::string executableBaseName() {
  char path[4096];
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0) {
    return std::string();
  }
  path[length] = '\0';
  const char *slash = std::strrchr(path, '/');
  return slash != nullptr ? std::string(slash + 1) : std::string(path);
}

static std::string instanceForBinary(const std::string &baseName) {
  if (baseName == kBinaryMtk) {
    return "default1";
  }
  if (baseName == kBinaryDolby) {
    return "default9";
  }
  return std::string();
}

int main(int argc, char **argv) {
  if (hasArg(argc, argv, "--probe")) {
    return runProbe();
  }
  std::vector<std::string> instances = parseInstances(argc, argv);
  if (instances.empty()) {
    // Real-device shape: rc passes no arguments, the binary name alone picks
    // the store instance (mtk -> default, dolby -> default9).
    const std::string instance = instanceForBinary(executableBaseName());
    if (instance.empty()) {
      return 1;
    }
    instances.push_back(instance);
  }

  ABinderProcess_setThreadPoolMaxThreadCount(
      static_cast<uint32_t>(instances.size() * 2));
  std::vector<std::shared_ptr<Codec2Store>> stores;
  std::vector<std::string> registeredInstances;
  for (const std::string &instance : instances) {
    const std::string serviceName = serviceNameForInstance(instance);
    if (serviceName != kDefault1ServiceName &&
        serviceName != kDefault9ServiceName) {
      return 1;
    }
    if (std::find(registeredInstances.begin(), registeredInstances.end(),
                  instance) != registeredInstances.end()) {
      return 1;
    }
    registeredInstances.push_back(instance);
    auto store = ndk::SharedRefBase::make<Codec2Store>(instance);
    if (store->targetCount() == 0) {
      return 3;
    }
    if (registerAsService(store, serviceName.c_str()) != STATUS_OK) {
      return 2;
    }
    stores.push_back(store);
  }
  ABinderProcess_joinThreadPool();
  return 0;
}