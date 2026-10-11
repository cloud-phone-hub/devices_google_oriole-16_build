// note13-5G-15 health HAL（android.hardware.health IHealth v3）——自研实现。
//
// 形态与真机（gold：/vendor/bin/hw/android.hardware.health-service.example，rc 服务
// vendor.health-default）一致：注册 android.hardware.health.IHealth/default，
// MIUI BatteryService 经它取电池数据。源码由本模块从 AOSP 接口与 cuttlefish 假电池
// 语义移植而来（原先是从 lineage com.google.cf.health.apex 提取二进制）。
//
// 本机没有电池硬件（/sys/class/power_supply 为空，内核无电池驱动），电池数据按真机
// 实测口径固化（真机 dumpsys battery：level 100/FULL/GOOD，AC+USB 在线，
// voltage 4389mV，温度 29.0℃，Li-poly，charge counter 4967uAh，max current/voltage 0）：
//   * 想改口径直接改下方 kProfile 常量后重建（构建期固化的模拟值）；
//   * 只想临时改框架上报值可用 `dumpsys battery set level|status ...`（不改本 HAL）。
//
// 存储/磁盘统计：本机无 eMMC 健康与块设备统计需求，返回空数组（真机由 eMMC 驱动填充）。

#include <android/binder_ibinder.h>
#include <android/binder_interface_utils.h>
#include <android/binder_status.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/health/BatteryCapacityLevel.h>
#include <aidl/android/hardware/health/BatteryChargingPolicy.h>
#include <aidl/android/hardware/health/BatteryChargingState.h>
#include <aidl/android/hardware/health/BatteryHealth.h>
#include <aidl/android/hardware/health/BatteryHealthData.h>
#include <aidl/android/hardware/health/BatteryPartStatus.h>
#include <aidl/android/hardware/health/BatteryStatus.h>
#include <aidl/android/hardware/health/BnHealth.h>
#include <aidl/android/hardware/health/DiskStats.h>
#include <aidl/android/hardware/health/HealthInfo.h>
#include <aidl/android/hardware/health/IHealthInfoCallback.h>
#include <aidl/android/hardware/health/StorageInfo.h>

#include <mutex>
#include <string>
#include <vector>

using aidl::android::hardware::health::BatteryCapacityLevel;
using aidl::android::hardware::health::BatteryChargingPolicy;
using aidl::android::hardware::health::BatteryChargingState;
using aidl::android::hardware::health::BatteryHealth;
using aidl::android::hardware::health::BatteryHealthData;
using aidl::android::hardware::health::BatteryPartStatus;
using aidl::android::hardware::health::BatteryStatus;
using aidl::android::hardware::health::BnHealth;
using aidl::android::hardware::health::DiskStats;
using aidl::android::hardware::health::HealthInfo;
using aidl::android::hardware::health::IHealthInfoCallback;
using aidl::android::hardware::health::StorageInfo;
using ndk::ScopedAStatus;

static constexpr const char *kHealthServiceName =
    "android.hardware.health.IHealth/default";

namespace {

// 模拟电池口径（真机 dumpsys battery 实测；改这里即改云机上报值）
struct BatteryProfile {
  bool acOnline = true;
  bool usbOnline = true;
  bool wirelessOnline = false;
  bool dockOnline = false;
  int32_t maxChargingCurrentUa = 0;
  int32_t maxChargingVoltageUv = 0;
  BatteryStatus status = BatteryStatus::FULL;
  BatteryHealth health = BatteryHealth::GOOD;
  bool present = true;
  int32_t level = 100;
  int32_t voltageMv = 4389;
  int32_t temperatureDeciC = 290;
  int32_t currentUa = 0;
  int32_t currentAvgUa = 0;
  int32_t cycleCount = 0;
  int32_t fullChargeUah = 5000;
  int32_t chargeCounterUah = 4967;
  const char *technology = "Li-poly";
  BatteryCapacityLevel capacityLevel = BatteryCapacityLevel::FULL;
  BatteryChargingState chargingState = BatteryChargingState::INVALID;
  int64_t manufacturingDateSeconds = 0;
  int64_t firstUsageSeconds = 0;
  int64_t stateOfHealth = 99;
  const char *serialNumber = nullptr;
  BatteryPartStatus partStatus = BatteryPartStatus::UNSUPPORTED;
};

const BatteryProfile kProfile;

void fillBattery(const BatteryProfile &p, BatteryChargingPolicy policy,
                 HealthInfo *out) {
  out->chargerAcOnline = p.acOnline;
  out->chargerUsbOnline = p.usbOnline;
  out->chargerWirelessOnline = p.wirelessOnline;
  out->chargerDockOnline = p.dockOnline;
  out->maxChargingCurrentMicroamps = p.maxChargingCurrentUa;
  out->maxChargingVoltageMicrovolts = p.maxChargingVoltageUv;
  out->batteryStatus = p.status;
  out->batteryHealth = p.health;
  out->batteryPresent = p.present;
  out->batteryLevel = p.level;
  out->batteryVoltageMillivolts = p.voltageMv;
  out->batteryTemperatureTenthsCelsius = p.temperatureDeciC;
  out->batteryCurrentMicroamps = p.currentUa;
  out->batteryCurrentAverageMicroamps = p.currentAvgUa;
  out->batteryCycleCount = p.cycleCount;
  out->batteryFullChargeUah = p.fullChargeUah;
  out->batteryChargeCounterUah = p.chargeCounterUah;
  out->batteryTechnology = p.technology;
  out->batteryCapacityLevel = p.capacityLevel;
  out->batteryChargeTimeToFullNowSeconds = 0;
  out->batteryFullChargeDesignCapacityUah = p.fullChargeUah;
  out->chargingState = p.chargingState;
  out->chargingPolicy = policy;
  out->diskStats.clear();
  out->storageInfos.clear();
}

class HealthService : public BnHealth {
public:
  ScopedAStatus registerCallback(
      const std::shared_ptr<IHealthInfoCallback> &callback) override {
    if (callback == nullptr) {
      return ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_.push_back(callback);
    return ScopedAStatus::ok();
  }

  ScopedAStatus unregisterCallback(
      const std::shared_ptr<IHealthInfoCallback> &callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
      if (it->get() == callback.get()) {
        callbacks_.erase(it);
        break;
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus update() override {
    HealthInfo info;
    fill(&info);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = callbacks_.begin(); it != callbacks_.end();) {
      if (!it->get()->healthInfoChanged(info).isOk()) {
        it = callbacks_.erase(it);
      } else {
        ++it;
      }
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus getChargeCounterUah(int32_t *out) override {
    *out = kProfile.chargeCounterUah;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getCurrentNowMicroamps(int32_t *out) override {
    *out = kProfile.currentUa;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getCurrentAverageMicroamps(int32_t *out) override {
    *out = kProfile.currentAvgUa;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getCapacity(int32_t *out) override {
    *out = kProfile.level;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getEnergyCounterNwh(int64_t *out) override {
    // 本机无能量计（真机亦未上报），按 AOSP 口径返回不支持
    *out = 0;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
  }

  ScopedAStatus getChargeStatus(BatteryStatus *out) override {
    *out = kProfile.status;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getStorageInfo(std::vector<StorageInfo> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus getDiskStats(std::vector<DiskStats> *out) override {
    out->clear();
    return ScopedAStatus::ok();
  }

  ScopedAStatus getHealthInfo(HealthInfo *out) override {
    fill(out);
    return ScopedAStatus::ok();
  }

  ScopedAStatus setChargingPolicy(BatteryChargingPolicy in_value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = in_value;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getChargingPolicy(BatteryChargingPolicy *out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out = policy_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getBatteryHealthData(BatteryHealthData *out) override {
    out->batteryManufacturingDateSeconds = kProfile.manufacturingDateSeconds;
    out->batteryFirstUsageSeconds = kProfile.firstUsageSeconds;
    out->batteryStateOfHealth = kProfile.stateOfHealth;
    if (kProfile.serialNumber != nullptr) {
      out->batterySerialNumber = kProfile.serialNumber;
    }
    out->batteryPartStatus = kProfile.partStatus;
    return ScopedAStatus::ok();
  }

private:
  void fill(HealthInfo *out) {
    BatteryChargingPolicy policy;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      policy = policy_;
    }
    fillBattery(kProfile, policy, out);
  }

  std::mutex mutex_;
  std::vector<std::shared_ptr<IHealthInfoCallback>> callbacks_;
  BatteryChargingPolicy policy_ = BatteryChargingPolicy::INVALID;
};

}  // namespace

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(2);
  auto health = ndk::SharedRefBase::make<HealthService>();
  if (AServiceManager_addService(health->asBinder().get(),
                                 kHealthServiceName) != STATUS_OK) {
    return 2;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}