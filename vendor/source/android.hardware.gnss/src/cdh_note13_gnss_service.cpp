#include <android/binder_ibinder.h>
#include <android/binder_status.h>

extern "C" {
void ABinderProcess_setThreadPoolMaxThreadCount(uint32_t numThreads);
void ABinderProcess_joinThreadPool(void);
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
}

#include <aidl/android/hardware/gnss/BnAGnss.h>
#include <aidl/android/hardware/gnss/BnAGnssRil.h>
#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/BnGnssAntennaInfo.h>
#include <aidl/android/hardware/gnss/BnGnssBatching.h>
#include <aidl/android/hardware/gnss/BnGnssConfiguration.h>
#include <aidl/android/hardware/gnss/BnGnssDebug.h>
#include <aidl/android/hardware/gnss/BnGnssGeofence.h>
#include <aidl/android/hardware/gnss/BnGnssMeasurementInterface.h>
#include <aidl/android/hardware/gnss/BnGnssNavigationMessageInterface.h>
#include <aidl/android/hardware/gnss/BnGnssPowerIndication.h>
#include <aidl/android/hardware/gnss/BnGnssPsds.h>
#include <aidl/android/hardware/gnss/ElapsedRealtime.h>
#include <aidl/android/hardware/gnss/GnssConstellationType.h>
#include <aidl/android/hardware/gnss/GnssLocation.h>
#include <aidl/android/hardware/gnss/GnssPowerStats.h>
#include <aidl/android/hardware/gnss/IAGnssCallback.h>
#include <aidl/android/hardware/gnss/IAGnssRilCallback.h>
#include <aidl/android/hardware/gnss/IGnssBatchingCallback.h>
#include <aidl/android/hardware/gnss/IGnssCallback.h>
#include <aidl/android/hardware/gnss/IGnssGeofenceCallback.h>
#include <aidl/android/hardware/gnss/IGnssNavigationMessageCallback.h>
#include <aidl/android/hardware/gnss/IGnssPowerIndicationCallback.h>
#include <aidl/android/hardware/gnss/measurement_corrections/BnMeasurementCorrectionsInterface.h>
#include <aidl/android/hardware/gnss/measurement_corrections/IMeasurementCorrectionsInterface.h>
#include <aidl/android/hardware/gnss/measurement_corrections/MeasurementCorrections.h>
#include <aidl/android/hardware/gnss/visibility_control/BnGnssVisibilityControl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <errno.h>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <vector>

using aidl::android::hardware::gnss::BnAGnss;
using aidl::android::hardware::gnss::BnAGnssRil;
using aidl::android::hardware::gnss::BnGnss;
using aidl::android::hardware::gnss::BnGnssAntennaInfo;
using aidl::android::hardware::gnss::BnGnssBatching;
using aidl::android::hardware::gnss::BnGnssConfiguration;
using aidl::android::hardware::gnss::BnGnssDebug;
using aidl::android::hardware::gnss::BnGnssGeofence;
using aidl::android::hardware::gnss::BnGnssMeasurementInterface;
using aidl::android::hardware::gnss::BnGnssNavigationMessageInterface;
using aidl::android::hardware::gnss::BnGnssPowerIndication;
using aidl::android::hardware::gnss::BnGnssPsds;
using aidl::android::hardware::gnss::ElapsedRealtime;
using aidl::android::hardware::gnss::GnssConstellationType;
using aidl::android::hardware::gnss::GnssLocation;
using aidl::android::hardware::gnss::GnssPowerStats;
using aidl::android::hardware::gnss::IAGnss;
using aidl::android::hardware::gnss::IAGnssCallback;
using aidl::android::hardware::gnss::IAGnssRil;
using aidl::android::hardware::gnss::IAGnssRilCallback;
using aidl::android::hardware::gnss::IGnss;
using aidl::android::hardware::gnss::IGnssAntennaInfo;
using aidl::android::hardware::gnss::IGnssBatching;
using aidl::android::hardware::gnss::IGnssBatchingCallback;
using aidl::android::hardware::gnss::IGnssCallback;
using aidl::android::hardware::gnss::IGnssConfiguration;
using aidl::android::hardware::gnss::IGnssDebug;
using aidl::android::hardware::gnss::IGnssGeofence;
using aidl::android::hardware::gnss::IGnssGeofenceCallback;
using aidl::android::hardware::gnss::IGnssMeasurementCallback;
using aidl::android::hardware::gnss::IGnssMeasurementInterface;
using aidl::android::hardware::gnss::IGnssNavigationMessageCallback;
using aidl::android::hardware::gnss::IGnssNavigationMessageInterface;
using aidl::android::hardware::gnss::IGnssPowerIndication;
using aidl::android::hardware::gnss::IGnssPowerIndicationCallback;
using aidl::android::hardware::gnss::IGnssPsds;
using aidl::android::hardware::gnss::IGnssPsdsCallback;
using aidl::android::hardware::gnss::PsdsType;
using aidl::android::hardware::gnss::measurement_corrections::
    BnMeasurementCorrectionsInterface;
using aidl::android::hardware::gnss::measurement_corrections::
    IMeasurementCorrectionsCallback;
using aidl::android::hardware::gnss::measurement_corrections::
    IMeasurementCorrectionsInterface;
using aidl::android::hardware::gnss::measurement_corrections::
    MeasurementCorrections;
using aidl::android::hardware::gnss::visibility_control::BnGnssVisibilityControl;
using aidl::android::hardware::gnss::visibility_control::IGnssVisibilityControl;
using aidl::android::hardware::gnss::visibility_control::
    IGnssVisibilityControlCallback;
using ndk::ScopedAStatus;

static constexpr const char *kGnssServiceName =
    "android.hardware.gnss.IGnss/default";
[[maybe_unused]] static constexpr const char *kDefaultControlPath =
    "/data/vendor/gps/fix";

#include "gnss_target.inc"

static void sleepMilliseconds(int delayMs) {
  if (delayMs <= 0) {
    return;
  }
  struct timespec remaining{};
  remaining.tv_sec = delayMs / 1000;
  remaining.tv_nsec = static_cast<long>(delayMs % 1000) * 1000000L;
  while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR) {
  }
}

static int64_t nowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

static int64_t elapsedRealtimeNanos() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

static int64_t statMtimeToken(const struct stat &st) {
  return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
         static_cast<int64_t>(st.st_mtim.tv_nsec);
}

static double parseDouble(const std::map<std::string, std::string> &values,
                          const std::string &key, double fallback) {
  auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  char *end = nullptr;
  const double parsed = std::strtod(it->second.c_str(), &end);
  return end != it->second.c_str() ? parsed : fallback;
}

static int parseInt(const std::map<std::string, std::string> &values,
                    const std::string &key, int fallback, int minValue,
                    int maxValue) {
  auto it = values.find(key);
  if (it == values.end()) {
    return fallback;
  }
  char *end = nullptr;
  const long parsed = std::strtol(it->second.c_str(), &end, 10);
  if (end == it->second.c_str()) {
    return fallback;
  }
  return static_cast<int>(
      std::max<long>(minValue, std::min<long>(maxValue, parsed)));
}

static GnssLocation buildLocation(double longitude, double latitude,
                                  double altitude, double speed,
                                  double horizontalAccuracy, int64_t timeMs) {
  GnssLocation location;
  location.gnssLocationFlags =
      GnssLocation::HAS_LAT_LONG | GnssLocation::HAS_ALTITUDE |
      GnssLocation::HAS_SPEED | GnssLocation::HAS_BEARING |
      GnssLocation::HAS_HORIZONTAL_ACCURACY |
      GnssLocation::HAS_VERTICAL_ACCURACY |
      GnssLocation::HAS_SPEED_ACCURACY | GnssLocation::HAS_BEARING_ACCURACY;
  location.latitudeDegrees = latitude;
  location.longitudeDegrees = longitude;
  location.altitudeMeters = altitude;
  location.speedMetersPerSec = speed;
  location.bearingDegrees = 0.0;
  location.horizontalAccuracyMeters = horizontalAccuracy;
  location.verticalAccuracyMeters = 1.0;
  location.speedAccuracyMetersPerSecond = 0.1;
  location.bearingAccuracyDegrees = 1.0;
  location.timestampMillis = timeMs;
  location.elapsedRealtime.flags =
      ElapsedRealtime::HAS_TIMESTAMP_NS |
      ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
  location.elapsedRealtime.timestampNs = elapsedRealtimeNanos();
  location.elapsedRealtime.timeUncertaintyNs = 1000.0;
  return location;
}

static std::map<std::string, std::string> readKeyValueFile(const char *path) {
  std::ifstream input(path);
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(input, line)) {
    const size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    values[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return values;
}

static bool readControlFix(GnssLocation *out, int *satellitesOut) {
  const auto values = readKeyValueFile(kControlPath);
  if (values.empty()) {
    return false;
  }
  const double longitude =
      parseDouble(values, "longitude", kDefaultLongitude);
  const double latitude = parseDouble(values, "latitude", kDefaultLatitude);
  const double altitude =
      parseDouble(values, "altitude", kDefaultAltitudeMeters);
  const double speed =
      parseDouble(values, "velocity", kDefaultSpeedMetersPerSecond);
  const double accuracy =
      parseDouble(values, "accuracy", kDefaultHorizontalAccuracyMeters);
  const double timeMsDouble = parseDouble(values, "time_ms", nowMillis());
  const int satellites =
      parseInt(values, "satellites", kDefaultSatellites, 1, 64);
  *out = buildLocation(longitude, latitude, altitude, speed, accuracy,
                       static_cast<int64_t>(timeMsDouble));
  *satellitesOut = satellites;
  return true;
}

class NoopGnssConfiguration : public BnGnssConfiguration {
public:
  ScopedAStatus setSuplVersion(int32_t) override { return ScopedAStatus::ok(); }
  ScopedAStatus setSuplMode(int32_t) override { return ScopedAStatus::ok(); }
  ScopedAStatus setLppProfile(int32_t) override { return ScopedAStatus::ok(); }
  ScopedAStatus setGlonassPositioningProtocol(int32_t) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setEmergencySuplPdn(bool) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setEsExtensionSec(int32_t) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setBlocklist(
      const std::vector<aidl::android::hardware::gnss::BlocklistedSource> &)
      override {
    return ScopedAStatus::ok();
  }
};

class NoopGnssMeasurement : public BnGnssMeasurementInterface {
public:
  ScopedAStatus setCallback(
      const std::shared_ptr<IGnssMeasurementCallback> &, bool, bool) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus close() override { return ScopedAStatus::ok(); }
  ScopedAStatus setCallbackWithOptions(
      const std::shared_ptr<IGnssMeasurementCallback> &,
      const IGnssMeasurementInterface::Options &) override {
    return ScopedAStatus::ok();
  }
};

class NoopGnssBatching : public BnGnssBatching {
public:
  ScopedAStatus init(const std::shared_ptr<IGnssBatchingCallback> &callback)
      override {
    callback_ = callback;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getBatchSize(int32_t *out) override {
    // Expose a minimal nonzero FIFO so framework batching setup can complete
    // without claiming a real hardware batch buffer.
    *out = 1;
    return ScopedAStatus::ok();
  }
  ScopedAStatus start(const IGnssBatching::Options &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus flush() override { return ScopedAStatus::ok(); }
  ScopedAStatus stop() override { return ScopedAStatus::ok(); }
  ScopedAStatus cleanup() override {
    callback_.reset();
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IGnssBatchingCallback> callback_;
};

class NoopGnssGeofence : public BnGnssGeofence {
public:
  ScopedAStatus setCallback(
      const std::shared_ptr<IGnssGeofenceCallback> &callback) override {
    callback_ = callback;
    return ScopedAStatus::ok();
  }
  ScopedAStatus addGeofence(int32_t geofenceId, double, double, double, int32_t,
                            int32_t, int32_t, int32_t) override {
    // Android 15 dereferences the geofence extension during GNSS init. Return
    // a real binder and complete unsupported operations with generic status.
    if (callback_ != nullptr) {
      callback_->gnssGeofenceAddCb(geofenceId,
                                   IGnssGeofenceCallback::ERROR_GENERIC);
    }
    return ScopedAStatus::ok();
  }
  ScopedAStatus pauseGeofence(int32_t geofenceId) override {
    if (callback_ != nullptr) {
      callback_->gnssGeofencePauseCb(geofenceId,
                                     IGnssGeofenceCallback::ERROR_GENERIC);
    }
    return ScopedAStatus::ok();
  }
  ScopedAStatus resumeGeofence(int32_t geofenceId, int32_t) override {
    if (callback_ != nullptr) {
      callback_->gnssGeofenceResumeCb(geofenceId,
                                      IGnssGeofenceCallback::ERROR_GENERIC);
    }
    return ScopedAStatus::ok();
  }
  ScopedAStatus removeGeofence(int32_t geofenceId) override {
    if (callback_ != nullptr) {
      callback_->gnssGeofenceRemoveCb(geofenceId,
                                      IGnssGeofenceCallback::ERROR_GENERIC);
    }
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IGnssGeofenceCallback> callback_;
};

class NoopGnssNavigationMessage : public BnGnssNavigationMessageInterface {
public:
  ScopedAStatus setCallback(
      const std::shared_ptr<IGnssNavigationMessageCallback> &callback)
      override {
    callback_ = callback;
    return ScopedAStatus::ok();
  }
  ScopedAStatus close() override {
    callback_.reset();
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IGnssNavigationMessageCallback> callback_;
};

class NoopMeasurementCorrections
    : public BnMeasurementCorrectionsInterface {
public:
  ScopedAStatus setCorrections(const MeasurementCorrections &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setCallback(
      const std::shared_ptr<IMeasurementCorrectionsCallback> &callback)
      override {
    callback_ = callback;
    if (callback_ != nullptr) {
      // No LOS/excess-path/reflecting-plane corrections are synthesized yet.
      callback_->setCapabilitiesCb(kTargetMeasurementCorrectionsCapabilities);
    }
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IMeasurementCorrectionsCallback> callback_;
};

class NoopGnssPowerIndication : public BnGnssPowerIndication {
public:
  ScopedAStatus
  setCallback(const std::shared_ptr<IGnssPowerIndicationCallback> &callback)
      override {
    callback_ = callback;
    if (callback_ != nullptr) {
      // Android 15 always registers the power sub-HAL when it exists. Report
      // only total energy because that is the sole field this virtual GNSS
      // service can keep internally consistent.
      callback_->setCapabilitiesCb(kTargetPowerCapabilities);
    }
    return ScopedAStatus::ok();
  }
  ScopedAStatus requestGnssPowerStats() override {
    if (callback_ != nullptr) {
      GnssPowerStats stats;
      stats.elapsedRealtime.flags = ElapsedRealtime::HAS_TIMESTAMP_NS;
      stats.elapsedRealtime.timestampNs = elapsedRealtimeNanos();
      stats.totalEnergyMilliJoule = 0.0;
      stats.singlebandTrackingModeEnergyMilliJoule = 0.0;
      stats.multibandTrackingModeEnergyMilliJoule = 0.0;
      stats.singlebandAcquisitionModeEnergyMilliJoule = 0.0;
      stats.multibandAcquisitionModeEnergyMilliJoule = 0.0;
      stats.otherModesEnergyMilliJoule.clear();
      callback_->gnssPowerStatsCb(stats);
    }
    return ScopedAStatus::ok();
  }

private:
  std::shared_ptr<IGnssPowerIndicationCallback> callback_;
};

class NoopGnssPsds : public BnGnssPsds {
public:
  ScopedAStatus injectPsdsData(PsdsType, const std::vector<uint8_t> &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setCallback(const std::shared_ptr<IGnssPsdsCallback> &)
      override {
    return ScopedAStatus::ok();
  }
};

class NoopAGnss : public BnAGnss {
public:
  ScopedAStatus setCallback(const std::shared_ptr<IAGnssCallback> &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus dataConnClosed() override { return ScopedAStatus::ok(); }
  ScopedAStatus dataConnFailed() override { return ScopedAStatus::ok(); }
  ScopedAStatus setServer(IAGnssCallback::AGnssType, const std::string &,
                          int32_t) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus dataConnOpen(int64_t, const std::string &,
                             IAGnss::ApnIpType) override {
    return ScopedAStatus::ok();
  }
};

class NoopAGnssRil : public BnAGnssRil {
public:
  ScopedAStatus setCallback(const std::shared_ptr<IAGnssRilCallback> &)
      override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus
  setRefLocation(const IAGnssRil::AGnssRefLocation &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus setSetId(IAGnssRil::SetIdType, const std::string &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus
  updateNetworkState(const IAGnssRil::NetworkAttributes &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus injectNiSuplMessageData(const std::vector<uint8_t> &,
                                        int32_t) override {
    return ScopedAStatus::ok();
  }
};

class NoopGnssDebug : public BnGnssDebug {
public:
  explicit NoopGnssDebug(const GnssLocation *location) : location_(location) {}

  ScopedAStatus getDebugData(IGnssDebug::DebugData *out) override {
    out->position.valid = true;
    out->position.latitudeDegrees = location_->latitudeDegrees;
    out->position.longitudeDegrees = location_->longitudeDegrees;
    out->position.altitudeMeters =
        static_cast<float>(location_->altitudeMeters);
    out->position.speedMetersPerSec =
        static_cast<float>(location_->speedMetersPerSec);
    out->position.horizontalAccuracyMeters =
        location_->horizontalAccuracyMeters;
    out->position.verticalAccuracyMeters = location_->verticalAccuracyMeters;
    out->position.speedAccuracyMetersPerSecond =
        location_->speedAccuracyMetersPerSecond;
    out->position.bearingAccuracyDegrees = location_->bearingAccuracyDegrees;
    out->position.ageSeconds = 0.0f;
    out->time.timeEstimateMs = location_->timestampMillis;
    out->time.timeUncertaintyNs = 1000.0f;
    out->time.frequencyUncertaintyNsPerSec = 0.0f;
    out->satelliteDataArray.clear();
    return ScopedAStatus::ok();
  }

private:
  const GnssLocation *location_;
};

class NoopGnssVisibilityControl : public BnGnssVisibilityControl {
public:
  ScopedAStatus enableNfwLocationAccess(
      const std::vector<std::string> &) override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus
  setCallback(const std::shared_ptr<IGnssVisibilityControlCallback> &)
      override {
    return ScopedAStatus::ok();
  }
};

class NoopGnssAntennaInfo : public BnGnssAntennaInfo {
public:
  ScopedAStatus
  setCallback(const std::shared_ptr<
              aidl::android::hardware::gnss::IGnssAntennaInfoCallback> &)
      override {
    return ScopedAStatus::ok();
  }
  ScopedAStatus close() override { return ScopedAStatus::ok(); }
};

class Note13Gnss : public BnGnss {
public:
  Note13Gnss()
      : location_(buildLocation(kDefaultLongitude, kDefaultLatitude,
                                kDefaultAltitudeMeters,
                                kDefaultSpeedMetersPerSecond,
                                kDefaultHorizontalAccuracyMeters, nowMillis())),
        satellites_(kDefaultSatellites),
        configuration_(ndk::SharedRefBase::make<NoopGnssConfiguration>()),
        measurement_(ndk::SharedRefBase::make<NoopGnssMeasurement>()),
        batching_(ndk::SharedRefBase::make<NoopGnssBatching>()),
        geofence_(ndk::SharedRefBase::make<NoopGnssGeofence>()),
        navigation_(ndk::SharedRefBase::make<NoopGnssNavigationMessage>()),
        measurementCorrections_(
            ndk::SharedRefBase::make<NoopMeasurementCorrections>()),
        power_(ndk::SharedRefBase::make<NoopGnssPowerIndication>()),
        psds_(ndk::SharedRefBase::make<NoopGnssPsds>()),
        agnss_(ndk::SharedRefBase::make<NoopAGnss>()),
        agnssRil_(ndk::SharedRefBase::make<NoopAGnssRil>()),
        debug_(ndk::SharedRefBase::make<NoopGnssDebug>(&location_)),
        visibility_(ndk::SharedRefBase::make<NoopGnssVisibilityControl>()),
        antenna_(ndk::SharedRefBase::make<NoopGnssAntennaInfo>()) {
    poller_ = std::thread([this] { pollControlFile(); });
  }

  ~Note13Gnss() override {
    stopPoller_ = true;
    if (poller_.joinable()) {
      poller_.join();
    }
  }

  ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback> &callback)
      override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback_ = callback;
    }
    if (callback != nullptr) {
      // Android framework calls setCallback() during init/cleanup probing.
      // Keep this phase side-effect-light: advertise only the scheduling
      // capability that is needed for a normal GPS provider, and defer status
      // and fixes until start() enters an actual GNSS session.
      callback->gnssSetCapabilitiesCb(kTargetGnssCapabilities);
      IGnssCallback::GnssSystemInfo info;
      info.yearOfHw = kTargetYearOfHw;
      info.name = kTargetSystemInfoName;
      callback->gnssSetSystemInfoCb(info);
    }
    return ScopedAStatus::ok();
  }

  ScopedAStatus close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_.reset();
    running_ = false;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getExtensionPsds(std::shared_ptr<IGnssPsds> *out) override {
    *out = psds_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssConfiguration(
      std::shared_ptr<IGnssConfiguration> *out) override {
    *out = configuration_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssMeasurement(
      std::shared_ptr<IGnssMeasurementInterface> *out) override {
    *out = measurement_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssPowerIndication(
      std::shared_ptr<IGnssPowerIndication> *out) override {
    *out = power_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssBatching(
      std::shared_ptr<IGnssBatching> *out) override {
    *out = batching_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssGeofence(
      std::shared_ptr<IGnssGeofence> *out) override {
    *out = geofence_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssNavigationMessage(
      std::shared_ptr<IGnssNavigationMessageInterface> *out) override {
    *out = navigation_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionAGnss(std::shared_ptr<IAGnss> *out) override {
    *out = agnss_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionAGnssRil(std::shared_ptr<IAGnssRil> *out)
      override {
    *out = agnssRil_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssDebug(std::shared_ptr<IGnssDebug> *out)
      override {
    *out = debug_;
    return ScopedAStatus::ok();
  }
  ScopedAStatus getExtensionGnssVisibilityControl(
      std::shared_ptr<IGnssVisibilityControl> *out) override {
    *out = visibility_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus start() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = true;
    }
    notifyStatus(IGnssCallback::GnssStatusValue::ENGINE_ON);
    notifyStatus(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
    publishLocation();
    return ScopedAStatus::ok();
  }

  ScopedAStatus stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    notifyStatus(IGnssCallback::GnssStatusValue::SESSION_END);
    notifyStatus(IGnssCallback::GnssStatusValue::ENGINE_OFF);
    return ScopedAStatus::ok();
  }

  ScopedAStatus injectTime(int64_t, int64_t, int32_t) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus injectLocation(const GnssLocation &location) override {
    setLocation(location, true);
    return ScopedAStatus::ok();
  }

  ScopedAStatus injectBestLocation(const GnssLocation &location) override {
    setLocation(location, true);
    return ScopedAStatus::ok();
  }

  ScopedAStatus deleteAidingData(IGnss::GnssAidingData) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus setPositionMode(const IGnss::PositionModeOptions &) override {
    return ScopedAStatus::ok();
  }

  ScopedAStatus getExtensionGnssAntennaInfo(
      std::shared_ptr<IGnssAntennaInfo> *out) override {
    *out = antenna_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus getExtensionMeasurementCorrections(
      std::shared_ptr<IMeasurementCorrectionsInterface> *out) override {
    *out = measurementCorrections_;
    return ScopedAStatus::ok();
  }

  ScopedAStatus startSvStatus() override {
    publishSvStatus();
    return ScopedAStatus::ok();
  }
  ScopedAStatus stopSvStatus() override { return ScopedAStatus::ok(); }
  ScopedAStatus startNmea() override {
    publishNmea();
    return ScopedAStatus::ok();
  }
  ScopedAStatus stopNmea() override { return ScopedAStatus::ok(); }

private:
  void setLocation(const GnssLocation &location, bool forcePublish) {
    int satellites;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      satellites = satellites_;
    }
    setLocation(location, satellites, forcePublish);
  }

  void setLocation(const GnssLocation &location, int satellites,
                   bool forcePublish) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      location_ = location;
      satellites_ = std::max(1, std::min(64, satellites));
    }
    publishLocation(forcePublish);
  }

  void notifyStatus(IGnssCallback::GnssStatusValue status) {
    std::shared_ptr<IGnssCallback> callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
    }
    if (callback != nullptr) {
      callback->gnssStatusCb(status);
    }
  }

  void publishLocation(bool forcePublish = false) {
    std::shared_ptr<IGnssCallback> callback;
    GnssLocation location;
    bool running;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
      location = location_;
      running = running_;
    }
    if (callback != nullptr && (running || forcePublish)) {
      callback->gnssLocationCb(location);
    }
  }

  void publishSvStatus() {
    std::shared_ptr<IGnssCallback> callback;
    int satellites;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
      satellites = satellites_;
    }
    if (callback == nullptr) {
      return;
    }
    std::vector<IGnssCallback::GnssSvInfo> svs;
    for (int i = 0; i < satellites; ++i) {
      IGnssCallback::GnssSvInfo sv;
      sv.svid = i + 1;
      sv.constellation = GnssConstellationType::GPS;
      sv.cN0Dbhz = 35.0f + static_cast<float>(i);
      sv.basebandCN0DbHz = sv.cN0Dbhz;
      sv.elevationDegrees = 45.0f;
      sv.azimuthDegrees = static_cast<float>(i * 30);
      sv.carrierFrequencyHz = 1575420000;
      // AIDL generates GnssSvFlags as a scoped enum; combine flags through the
      // integer backing type so the callback still receives the official mask.
      sv.svFlag =
          static_cast<int32_t>(IGnssCallback::GnssSvFlags::HAS_EPHEMERIS_DATA) |
          static_cast<int32_t>(IGnssCallback::GnssSvFlags::HAS_ALMANAC_DATA) |
          static_cast<int32_t>(IGnssCallback::GnssSvFlags::USED_IN_FIX) |
          static_cast<int32_t>(
              IGnssCallback::GnssSvFlags::HAS_CARRIER_FREQUENCY);
      svs.push_back(sv);
    }
    callback->gnssSvStatusCb(svs);
  }

  void publishNmea() {
    std::shared_ptr<IGnssCallback> callback;
    GnssLocation location;
    int satellites;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = callback_;
      location = location_;
      satellites = satellites_;
    }
    if (callback == nullptr) {
      return;
    }
    std::ostringstream nmea;
    nmea << "$GPGGA,000000," << std::fabs(location.latitudeDegrees)
         << ",N," << std::fabs(location.longitudeDegrees)
         << ",E,1," << satellites << ",1.0,"
         << location.altitudeMeters << ",M,0.0,M,,";
    callback->gnssNmeaCb(location.timestampMillis, nmea.str());
  }

  void pollControlFile() {
    int64_t lastMtime = 0;
    while (!stopPoller_) {
      struct stat st {};
      if (stat(kControlPath, &st) == 0 && statMtimeToken(st) != lastMtime) {
        lastMtime = statMtimeToken(st);
        GnssLocation location;
        int satellites = kDefaultSatellites;
        // fishbridge 写入的控制文件是 console geo fix 到 GNSS HAL 的唯一输入；
        // 这里消费后强制推送一次 HAL fix，让 adb emu geo fix 在无前台 listener
        // 时也能更新 framework 的 GPS provider 状态。
        if (readControlFix(&location, &satellites)) {
          setLocation(location, satellites, true);
        }
      }
      sleepMilliseconds(250);
    }
  }

  std::mutex mutex_;
  std::shared_ptr<IGnssCallback> callback_;
  GnssLocation location_;
  int satellites_;
  bool running_ = false;
  bool stopPoller_ = false;
  std::thread poller_;
  std::shared_ptr<IGnssConfiguration> configuration_;
  std::shared_ptr<IGnssMeasurementInterface> measurement_;
  std::shared_ptr<IGnssBatching> batching_;
  std::shared_ptr<IGnssGeofence> geofence_;
  std::shared_ptr<IGnssNavigationMessageInterface> navigation_;
  std::shared_ptr<IMeasurementCorrectionsInterface> measurementCorrections_;
  std::shared_ptr<IGnssPowerIndication> power_;
  std::shared_ptr<IGnssPsds> psds_;
  std::shared_ptr<IAGnss> agnss_;
  std::shared_ptr<IAGnssRil> agnssRil_;
  std::shared_ptr<IGnssDebug> debug_;
  std::shared_ptr<IGnssVisibilityControl> visibility_;
  std::shared_ptr<IGnssAntennaInfo> antenna_;
};

static binder_status_t
registerAsService(const std::shared_ptr<ndk::ICInterface> &service,
                  const char *name) {
  return AServiceManager_addService(service->asBinder().get(), name);
}

int main() {
  ABinderProcess_setThreadPoolMaxThreadCount(4);
  auto gnss = ndk::SharedRefBase::make<Note13Gnss>();
  if (registerAsService(gnss, kGnssServiceName) != STATUS_OK) {
    return 2;
  }
  ABinderProcess_joinThreadPool();
  return 0;
}
