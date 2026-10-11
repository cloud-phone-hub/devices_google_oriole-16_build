// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_network.h"

#include "radio_service.h"

#include <aidl/android/hardware/radio/RadioAccessFamily.h>
#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/RadioTechnology.h>
#include <aidl/android/hardware/radio/RadioTechnologyFamily.h>
#include <aidl/android/hardware/radio/network/BnRadioNetwork.h>
#include <aidl/android/hardware/radio/network/CdmaRoamingType.h>
#include <aidl/android/hardware/radio/network/CellConnectionStatus.h>
#include <aidl/android/hardware/radio/network/CellIdentity.h>
#include <aidl/android/hardware/radio/network/CellIdentityLte.h>
#include <aidl/android/hardware/radio/network/CellInfo.h>
#include <aidl/android/hardware/radio/network/CellInfoLte.h>
#include <aidl/android/hardware/radio/network/CellInfoRatSpecificInfo.h>
#include <aidl/android/hardware/radio/network/EmergencyRegResult.h>
#include <aidl/android/hardware/radio/network/IRadioNetworkIndication.h>
#include <aidl/android/hardware/radio/network/IRadioNetworkResponse.h>
#include <aidl/android/hardware/radio/network/LteSignalStrength.h>
#include <aidl/android/hardware/radio/network/OperatorInfo.h>
#include <aidl/android/hardware/radio/network/RadioBandMode.h>
#include <aidl/android/hardware/radio/network/RegState.h>
#include <aidl/android/hardware/radio/network/RegStateResult.h>
#include <aidl/android/hardware/radio/network/SignalStrength.h>
#include <aidl/android/hardware/radio/network/UsageSetting.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cdh::radio {
namespace {

namespace network = ::aidl::android::hardware::radio::network;

constexpr std::int32_t kDefaultNetworkTypes =
    static_cast<std::int32_t>(aidl_radio::RadioAccessFamily::GSM) |
    static_cast<std::int32_t>(aidl_radio::RadioAccessFamily::UMTS) |
    static_cast<std::int32_t>(aidl_radio::RadioAccessFamily::LTE) |
    static_cast<std::int32_t>(aidl_radio::RadioAccessFamily::NR);

aidl_radio::RadioTechnology ToRadioTechnology(RadioAccessTechnology rat) {
  switch (rat) {
  case RadioAccessTechnology::kGprs:
    return aidl_radio::RadioTechnology::GPRS;
  case RadioAccessTechnology::kEdge:
    return aidl_radio::RadioTechnology::EDGE;
  case RadioAccessTechnology::kUmts:
    return aidl_radio::RadioTechnology::UMTS;
  case RadioAccessTechnology::kLte:
    return aidl_radio::RadioTechnology::LTE;
  case RadioAccessTechnology::kNr:
    return aidl_radio::RadioTechnology::NR;
  case RadioAccessTechnology::kUnknown:
    return aidl_radio::RadioTechnology::UNKNOWN;
  }
  return aidl_radio::RadioTechnology::UNKNOWN;
}

network::RegState ToRegState(const Registration &registration) {
  switch (registration.state) {
  case RegistrationState::kNotRegistered:
    return registration.emergency_only
               ? network::RegState::NOT_REG_MT_NOT_SEARCHING_OP_EM
               : network::RegState::NOT_REG_MT_NOT_SEARCHING_OP;
  case RegistrationState::kSearching:
    return registration.emergency_only
               ? network::RegState::NOT_REG_MT_SEARCHING_OP_EM
               : network::RegState::NOT_REG_MT_SEARCHING_OP;
  case RegistrationState::kDenied:
    return registration.emergency_only ? network::RegState::REG_DENIED_EM
                                       : network::RegState::REG_DENIED;
  case RegistrationState::kHome:
    return network::RegState::REG_HOME;
  case RegistrationState::kRoaming:
    return network::RegState::REG_ROAMING;
  case RegistrationState::kUnavailable:
    return registration.emergency_only ? network::RegState::UNKNOWN_EM
                                       : network::RegState::UNKNOWN;
  }
  return network::RegState::UNKNOWN;
}

network::OperatorInfo ToOperatorInfo(const Operator &source) {
  return network::OperatorInfo{
      .alphaLong = source.alpha_long,
      .alphaShort = source.alpha_short,
      .operatorNumeric = source.numeric,
      .status = network::OperatorInfo::STATUS_CURRENT,
  };
}

network::CellIdentityLte ToCellIdentityLte(const Operator &source) {
  std::string mcc;
  std::string mnc;
  if (source.numeric.size() >= 5) {
    mcc = source.numeric.substr(0, 3);
    mnc = source.numeric.substr(3);
  }
  return network::CellIdentityLte{
      .mcc = std::move(mcc),
      .mnc = std::move(mnc),
      .ci = 0x01555600,
      .pci = 100,
      .tac = 1000,
      .earfcn = 1650,
      .operatorNames = ToOperatorInfo(source),
      .bandwidth = 20000,
      .additionalPlmns = {},
      .csgInfo = std::nullopt,
      .bands = {},
  };
}

network::CellIdentity ToCellIdentity(const Operator &source) {
  return network::CellIdentity::make<network::CellIdentity::lte>(
      ToCellIdentityLte(source));
}

network::RegStateResult ToRegistration(const Network &source,
                                       const Registration &registration) {
  return network::RegStateResult{
      .regState = ToRegState(registration),
      .rat = ToRadioTechnology(registration.rat),
      .reasonForDenial = network::RegistrationFailCause::NONE,
      .cellIdentity = ToCellIdentity(source.operator_info),
      .registeredPlmn = source.operator_info.numeric,
      .accessTechnologySpecificInfo = {},
  };
}

network::LteSignalStrength ToLteSignalStrength(const Signal &source) {
  network::LteSignalStrength output;
  if (!source.valid)
    return output;
  output.signalStrength = std::clamp((source.rssi_dbm + 113) / 2, 0, 31);
  output.rsrp = -source.rsrp_dbm;
  output.rsrq = -source.rsrq_db;
  output.rssnr = source.rssnr_db * 10;
  output.cqi = 10;
  output.cqiTableIndex = 1;
  return output;
}

network::SignalStrength ToSignalStrength(const Signal &source) {
  network::SignalStrength output;
  if (!source.valid)
    return output;
  output.gsm.signalStrength = std::clamp((source.rssi_dbm + 113) / 2, 0, 31);
  output.gsm.bitErrorRate = 0;
  output.lte = ToLteSignalStrength(source);
  output.nr.ssRsrp = -source.rsrp_dbm;
  output.nr.ssRsrq = -source.rsrq_db;
  output.nr.ssSinr = source.rssnr_db;
  output.nr.csiRsrp = output.nr.ssRsrp;
  output.nr.csiRsrq = output.nr.ssRsrq;
  output.nr.csiSinr = output.nr.ssSinr;
  output.nr.csiCqiTableIndex = 1;
  return output;
}

bool IsRegistered(const Registration &registration) {
  return registration.state == RegistrationState::kHome ||
         registration.state == RegistrationState::kRoaming;
}

network::CellInfo ToCellInfo(const Snapshot &snapshot) {
  const bool registered =
      IsRegistered(snapshot.network.voice_registration) ||
      IsRegistered(snapshot.network.data_registration);
  network::CellInfoLte lte{
      .cellIdentityLte = ToCellIdentityLte(snapshot.network.operator_info),
      .signalStrengthLte = ToLteSignalStrength(snapshot.network.signal),
  };
  return network::CellInfo{
      .registered = registered,
      .connectionStatus = registered
                              ? network::CellConnectionStatus::PRIMARY_SERVING
                              : network::CellConnectionStatus::NONE,
      .ratSpecificInfo = network::CellInfoRatSpecificInfo::make<
          network::CellInfoRatSpecificInfo::lte>(std::move(lte)),
  };
}

std::vector<network::CellInfo> ToCellInfoList(const Snapshot &snapshot) {
  return {ToCellInfo(snapshot)};
}

aidl_radio::RadioError SnapshotError(const std::optional<Snapshot> &snapshot) {
  return snapshot.has_value() ? aidl_radio::RadioError::NONE
                              : aidl_radio::RadioError::RADIO_NOT_AVAILABLE;
}

class RadioNetwork final : public network::BnRadioNetwork,
                           private RadioServiceState {
public:
  RadioNetwork(Channel &channel, StateStore &state_store, std::int32_t slot)
      : RadioServiceState(channel, state_store, slot),
        observer_(state_store, [this](const Snapshot &snapshot) {
          const auto indication = callbacks_.IndicationCallback();
          if (indication == nullptr)
            return;
          const auto type = aidl_radio::RadioIndicationType::UNSOLICITED;
          indication->currentSignalStrength(
              type, ToSignalStrength(snapshot.network.signal));
          indication->voiceRadioTechChanged(
              type, ToRadioTechnology(snapshot.network.voice_registration.rat));
          indication->cellInfoList(type, ToCellInfoList(snapshot));
          indication->networkStateChanged(type);
        }) {}

  ::ndk::ScopedAStatus getAllowedNetworkTypesBitmap(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getAllowedNetworkTypesBitmapResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        allowed_network_types_.load());
  }

  ::ndk::ScopedAStatus getAvailableBandModes(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getAvailableBandModesResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        {network::RadioBandMode::BAND_MODE_UNSPECIFIED});
  }

  ::ndk::ScopedAStatus getAvailableNetworks(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    std::vector<network::OperatorInfo> operators;
    if (snapshot.has_value())
      operators.push_back(ToOperatorInfo(snapshot->network.operator_info));
    return response->getAvailableNetworksResponse(
        ResponseInfo(serial, SnapshotError(snapshot)), operators);
  }

  ::ndk::ScopedAStatus getBarringInfo(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getBarringInfoResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        network::CellIdentity{}, {});
  }

  ::ndk::ScopedAStatus getCdmaRoamingPreference(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getCdmaRoamingPreferenceResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        network::CdmaRoamingType::HOME_NETWORK);
  }

  ::ndk::ScopedAStatus getCellInfoList(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getCellInfoListResponse(
        ResponseInfo(serial, SnapshotError(snapshot)),
        snapshot.has_value() ? ToCellInfoList(*snapshot)
                             : std::vector<network::CellInfo>{});
  }

  ::ndk::ScopedAStatus getDataRegistrationState(int32_t serial) override {
    return RespondRegistration(serial, false);
  }

  ::ndk::ScopedAStatus getImsRegistrationState(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getImsRegistrationStateResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        false, aidl_radio::RadioTechnologyFamily::THREE_GPP);
  }

  ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getNetworkSelectionModeResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        manual_selection_.load());
  }

  ::ndk::ScopedAStatus getOperator(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    if (!snapshot.has_value()) {
      return response->getOperatorResponse(
          ResponseInfo(serial, aidl_radio::RadioError::RADIO_NOT_AVAILABLE), "",
          "", "");
    }
    const Operator &operator_info = snapshot->network.operator_info;
    return response->getOperatorResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        operator_info.alpha_long, operator_info.alpha_short,
        operator_info.numeric);
  }

  ::ndk::ScopedAStatus getSignalStrength(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getSignalStrengthResponse(
        ResponseInfo(serial, SnapshotError(snapshot)),
        snapshot.has_value() ? ToSignalStrength(snapshot->network.signal)
                             : network::SignalStrength{});
  }

  ::ndk::ScopedAStatus getSystemSelectionChannels(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    std::vector<network::RadioAccessSpecifier> specifiers;
    {
      std::lock_guard<std::mutex> lock(selection_mutex_);
      specifiers = system_selection_channels_;
    }
    return response->getSystemSelectionChannelsResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE), specifiers);
  }

  ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    return response->getVoiceRadioTechnologyResponse(
        ResponseInfo(serial, SnapshotError(snapshot)),
        snapshot.has_value()
            ? ToRadioTechnology(snapshot->network.voice_registration.rat)
            : aidl_radio::RadioTechnology::UNKNOWN);
  }

  ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t serial) override {
    return RespondRegistration(serial, true);
  }

  ::ndk::ScopedAStatus isNrDualConnectivityEnabled(int32_t serial) override {
    return RespondBoolean(
        serial, nr_dual_connectivity_.load(),
        &network::IRadioNetworkResponse::isNrDualConnectivityEnabledResponse);
  }

  ::ndk::ScopedAStatus responseAcknowledgement() override {
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus
  setAllowedNetworkTypesBitmap(int32_t serial,
                               int32_t network_type_bitmap) override {
    const bool valid = network_type_bitmap != 0;
    if (valid)
      allowed_network_types_.store(network_type_bitmap);
    return RespondLocal(
        serial, valid,
        &network::IRadioNetworkResponse::setAllowedNetworkTypesBitmapResponse);
  }

  ::ndk::ScopedAStatus setBandMode(int32_t serial,
                                   network::RadioBandMode) override {
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::setBandModeResponse);
  }

  ::ndk::ScopedAStatus setBarringPassword(int32_t serial, const std::string &,
                                          const std::string &,
                                          const std::string &) override {
    return RespondUnsupported(
        serial, &network::IRadioNetworkResponse::setBarringPasswordResponse);
  }

  ::ndk::ScopedAStatus
  setCdmaRoamingPreference(int32_t serial, network::CdmaRoamingType) override {
    return RespondUnsupported(
        serial,
        &network::IRadioNetworkResponse::setCdmaRoamingPreferenceResponse);
  }

  ::ndk::ScopedAStatus setCellInfoListRate(int32_t serial, int32_t) override {
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setCellInfoListRateResponse);
  }

  ::ndk::ScopedAStatus setIndicationFilter(int32_t serial, int32_t) override {
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setIndicationFilterResponse);
  }

  ::ndk::ScopedAStatus setLinkCapacityReportingCriteria(
      int32_t serial, int32_t, int32_t, int32_t, const std::vector<int32_t> &,
      const std::vector<int32_t> &, aidl_radio::AccessNetwork) override {
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setLinkCapacityReportingCriteriaResponse);
  }

  ::ndk::ScopedAStatus setLocationUpdates(int32_t serial,
                                          bool enable) override {
    location_updates_.store(enable);
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setLocationUpdatesResponse);
  }

  ::ndk::ScopedAStatus
  setNetworkSelectionModeAutomatic(int32_t serial) override {
    manual_selection_.store(false);
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setNetworkSelectionModeAutomaticResponse);
  }

  ::ndk::ScopedAStatus
  setNetworkSelectionModeManual(int32_t serial,
                                const std::string &operator_numeric,
                                aidl_radio::AccessNetwork) override {
    const bool valid = !operator_numeric.empty();
    if (valid)
      manual_selection_.store(true);
    return RespondLocal(
        serial, valid,
        &network::IRadioNetworkResponse::setNetworkSelectionModeManualResponse);
  }

  ::ndk::ScopedAStatus
  setNrDualConnectivityState(int32_t serial,
                             network::NrDualConnectivityState state) override {
    nr_dual_connectivity_.store(state ==
                                network::NrDualConnectivityState::ENABLE);
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setNrDualConnectivityStateResponse);
  }

  ::ndk::ScopedAStatus setResponseFunctions(
      const std::shared_ptr<network::IRadioNetworkResponse> &response,
      const std::shared_ptr<network::IRadioNetworkIndication> &indication)
      override {
    if (response == nullptr || indication == nullptr) {
      return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    callbacks_.Set(response, indication);
    const auto snapshot = SnapshotState();
    if (!snapshot.has_value())
      return ::ndk::ScopedAStatus::ok();
    const auto type = aidl_radio::RadioIndicationType::UNSOLICITED;
    auto status = indication->currentSignalStrength(
        type, ToSignalStrength(snapshot->network.signal));
    if (!status.isOk())
      return status;
    status = indication->voiceRadioTechChanged(
        type, ToRadioTechnology(snapshot->network.voice_registration.rat));
    if (!status.isOk())
      return status;
    status = indication->cellInfoList(type, ToCellInfoList(*snapshot));
    if (!status.isOk())
      return status;
    return indication->networkStateChanged(type);
  }

  ::ndk::ScopedAStatus setSignalStrengthReportingCriteria(
      int32_t serial,
      const std::vector<network::SignalThresholdInfo> &) override {
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setSignalStrengthReportingCriteriaResponse);
  }

  ::ndk::ScopedAStatus setSuppServiceNotifications(int32_t serial,
                                                   bool) override {
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setSuppServiceNotificationsResponse);
  }

  ::ndk::ScopedAStatus setSystemSelectionChannels(
      int32_t serial, bool specify_channels,
      const std::vector<network::RadioAccessSpecifier> &specifiers) override {
    {
      std::lock_guard<std::mutex> lock(selection_mutex_);
      system_selection_channels_ =
          specify_channels ? specifiers
                           : std::vector<network::RadioAccessSpecifier>{};
    }
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setSystemSelectionChannelsResponse);
  }

  ::ndk::ScopedAStatus
  startNetworkScan(int32_t serial,
                   const network::NetworkScanRequest &) override {
    return RespondUnsupported(
        serial, &network::IRadioNetworkResponse::startNetworkScanResponse);
  }

  ::ndk::ScopedAStatus stopNetworkScan(int32_t serial) override {
    return RespondUnsupported(
        serial, &network::IRadioNetworkResponse::stopNetworkScanResponse);
  }

  ::ndk::ScopedAStatus
  supplyNetworkDepersonalization(int32_t serial, const std::string &) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->supplyNetworkDepersonalizationResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        -1);
  }

  ::ndk::ScopedAStatus
  setUsageSetting(int32_t serial,
                  network::UsageSetting usage_setting) override {
    usage_setting_.store(static_cast<std::int32_t>(usage_setting));
    return RespondLocal(
        serial, true, &network::IRadioNetworkResponse::setUsageSettingResponse);
  }

  ::ndk::ScopedAStatus getUsageSetting(int32_t serial) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->getUsageSettingResponse(
        ResponseInfo(serial, aidl_radio::RadioError::NONE),
        static_cast<network::UsageSetting>(usage_setting_.load()));
  }

  ::ndk::ScopedAStatus setEmergencyMode(int32_t serial,
                                        network::EmergencyMode) override {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return response->setEmergencyModeResponse(
        ResponseInfo(serial, aidl_radio::RadioError::REQUEST_NOT_SUPPORTED),
        network::EmergencyRegResult{});
  }

  ::ndk::ScopedAStatus triggerEmergencyNetworkScan(
      int32_t serial, const network::EmergencyNetworkScanTrigger &) override {
    return RespondUnsupported(
        serial,
        &network::IRadioNetworkResponse::triggerEmergencyNetworkScanResponse);
  }

  ::ndk::ScopedAStatus cancelEmergencyNetworkScan(int32_t serial,
                                                  bool) override {
    return RespondUnsupported(
        serial,
        &network::IRadioNetworkResponse::cancelEmergencyNetworkScanResponse);
  }

  ::ndk::ScopedAStatus exitEmergencyMode(int32_t serial) override {
    return RespondUnsupported(
        serial, &network::IRadioNetworkResponse::exitEmergencyModeResponse);
  }

  ::ndk::ScopedAStatus setNullCipherAndIntegrityEnabled(int32_t serial,
                                                        bool enabled) override {
    null_cipher_integrity_.store(enabled);
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setNullCipherAndIntegrityEnabledResponse);
  }

  ::ndk::ScopedAStatus
  isNullCipherAndIntegrityEnabled(int32_t serial) override {
    return RespondBoolean(serial, null_cipher_integrity_.load(),
                          &network::IRadioNetworkResponse::
                              isNullCipherAndIntegrityEnabledResponse);
  }

  ::ndk::ScopedAStatus isN1ModeEnabled(int32_t serial) override {
    return RespondBoolean(
        serial, n1_mode_.load(),
        &network::IRadioNetworkResponse::isN1ModeEnabledResponse);
  }

  ::ndk::ScopedAStatus setN1ModeEnabled(int32_t serial, bool enable) override {
    n1_mode_.store(enable);
    return RespondLocal(
        serial, true,
        &network::IRadioNetworkResponse::setN1ModeEnabledResponse);
  }

  ::ndk::ScopedAStatus
  isCellularIdentifierTransparencyEnabled(int32_t serial) override {
    return RespondBoolean(serial, cellular_identifier_transparency_.load(),
                          &network::IRadioNetworkResponse::
                              isCellularIdentifierTransparencyEnabledResponse);
  }

  ::ndk::ScopedAStatus
  setCellularIdentifierTransparencyEnabled(int32_t serial,
                                           bool enabled) override {
    cellular_identifier_transparency_.store(enabled);
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setCellularIdentifierTransparencyEnabledResponse);
  }

  ::ndk::ScopedAStatus
  setSecurityAlgorithmsUpdatedEnabled(int32_t serial, bool enable) override {
    security_algorithms_updated_.store(enable);
    return RespondLocal(serial, true,
                        &network::IRadioNetworkResponse::
                            setSecurityAlgorithmsUpdatedEnabledResponse);
  }

  ::ndk::ScopedAStatus
  isSecurityAlgorithmsUpdatedEnabled(int32_t serial) override {
    return RespondBoolean(serial, security_algorithms_updated_.load(),
                          &network::IRadioNetworkResponse::
                              isSecurityAlgorithmsUpdatedEnabledResponse);
  }

private:
  using SimpleResponse = ::ndk::ScopedAStatus (
      network::IRadioNetworkResponse::*)(const aidl_radio::RadioResponseInfo &);
  using BooleanResponse = ::ndk::ScopedAStatus (
      network::IRadioNetworkResponse::*)(const aidl_radio::RadioResponseInfo &,
                                         bool);

  ::ndk::ScopedAStatus RespondRegistration(int32_t serial, bool voice) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    const auto snapshot = SnapshotState();
    network::RegStateResult registration;
    if (snapshot.has_value()) {
      registration = ToRegistration(
          snapshot->network, voice ? snapshot->network.voice_registration
                                   : snapshot->network.data_registration);
    }
    if (voice) {
      return response->getVoiceRegistrationStateResponse(
          ResponseInfo(serial, SnapshotError(snapshot)), registration);
    }
    return response->getDataRegistrationStateResponse(
        ResponseInfo(serial, SnapshotError(snapshot)), registration);
  }

  ::ndk::ScopedAStatus RespondLocal(int32_t serial, bool valid,
                                    SimpleResponse method) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         valid ? aidl_radio::RadioError::NONE
                               : aidl_radio::RadioError::INVALID_ARGUMENTS,
                         method);
  }

  ::ndk::ScopedAStatus RespondUnsupported(int32_t serial,
                                          SimpleResponse method) {
    return RespondSimple(callbacks_.ResponseCallback(), serial,
                         aidl_radio::RadioError::REQUEST_NOT_SUPPORTED, method);
  }

  ::ndk::ScopedAStatus RespondBoolean(int32_t serial, bool value,
                                      BooleanResponse method) {
    const auto response = callbacks_.ResponseCallback();
    if (response == nullptr)
      return MissingResponseFunctions();
    return ((*response).*
            method)(ResponseInfo(serial, aidl_radio::RadioError::NONE), value);
  }

  ResponseFunctions<network::IRadioNetworkResponse,
                    network::IRadioNetworkIndication>
      callbacks_;
  std::atomic<std::int32_t> allowed_network_types_{kDefaultNetworkTypes};
  std::atomic<std::int32_t> usage_setting_{
      static_cast<std::int32_t>(network::UsageSetting::DATA_CENTRIC)};
  std::atomic_bool manual_selection_{false};
  std::atomic_bool nr_dual_connectivity_{true};
  std::atomic_bool location_updates_{true};
  std::atomic_bool null_cipher_integrity_{false};
  std::atomic_bool n1_mode_{true};
  std::atomic_bool cellular_identifier_transparency_{true};
  std::atomic_bool security_algorithms_updated_{true};
  std::mutex selection_mutex_;
  std::vector<network::RadioAccessSpecifier> system_selection_channels_;
  StateObserver observer_;
};

} // namespace

std::shared_ptr<network::IRadioNetwork>
CreateRadioNetwork(Channel &channel, StateStore &state_store,
                   std::int32_t slot) {
  return ::ndk::SharedRefBase::make<RadioNetwork>(channel, state_store, slot);
}

} // namespace cdh::radio
