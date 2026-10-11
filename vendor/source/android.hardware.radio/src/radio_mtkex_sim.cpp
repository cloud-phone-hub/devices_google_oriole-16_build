// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "radio_mtkex_sim.h"

#include "radio_service.h"

#include <aidl/vendor/mediatek/hardware/mtkradioex/sim/BnMtkRadioExSim.h>

#include <mutex>
#include <string>

namespace cdh::radio {
namespace {

namespace mtkex_sim = ::aidl::vendor::mediatek::hardware::mtkradioex::sim;

// 空卡槽 ICCID 哨兵：MTK 的 checkAllIccIdReady 以长度 3 判定「未插卡」
// （updateAbsentFromIccid 也把 "N/A" 当无卡处理）。MtkUiccController.mIccid[]
// 初值是空串，空串会让订阅门禁永远不通过 —— 所以空槽必须显式上报 "N/A"。
constexpr char kNoCardIccid[] = "N/A";

::ndk::ScopedAStatus Unsupported() {
  return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

// 其余 radioex 方法本云机不实现（AIDL 生成的 Default 类也是同一应答）。
// MTK 侧调用点带 try/catch 降级（如 MtkRIL.getIccid 的
// handleMtkRadioProxyExceptionForRR），未支持不会阻塞 SIM/网络。
#define CDH_MTKEX_STUB(signature)                                              \
  ::ndk::ScopedAStatus signature override { return Unsupported(); }

class RadioMtkExSim final : public mtkex_sim::BnMtkRadioExSim,
                            private RadioServiceState {
public:
  RadioMtkExSim(Channel &channel, StateStore &state_store, std::int32_t slot)
      : RadioServiceState(channel, state_store, slot),
        observer_(state_store, [this](const Snapshot &) { NotifyIccid(); }) {}

  ::ndk::ScopedAStatus setResponseFunctionsMtk(
      const std::shared_ptr<mtkex_sim::IMtkRadioExSimResponse> &response,
      const std::shared_ptr<mtkex_sim::IMtkRadioExSimIndication> &indication)
      override {
    callbacks_.Set(response, indication);
    // MtkRIL 拿到代理后立刻回填响应函数，此刻上报一次最稳：mIccidRegistrants
    // 若尚未注册（MtkUiccController 建得比 RIL 晚），MtkRIL.cacheIccid 已存下
    // 本值，registerForIccidChanged 注册时会用缓存补发一次，两个时序都能落到
    // mIccid[phoneId]。
    last_iccid_.clear();
    NotifyIccid();
    return ::ndk::ScopedAStatus::ok();
  }

  CDH_MTKEX_STUB(getIccid(int32_t in_serial, int32_t in_cardType));
  CDH_MTKEX_STUB(activateUiccCard(int32_t in_serial, int32_t in_cardType));
  CDH_MTKEX_STUB(deactivateUiccCard(int32_t in_serial, int32_t in_cardType));
  CDH_MTKEX_STUB(deleteUPBEntry(int32_t in_serial, int32_t in_entryType,
                                int32_t in_adnIndex, int32_t in_upbIndex,
                                int32_t in_upbField));
  CDH_MTKEX_STUB(doGeneralSimAuthentication(
      int32_t in_serial, const mtkex_sim::SimAuthStructure &in_sessionInfo,
      int32_t in_tag));
  CDH_MTKEX_STUB(editUPBEntry(int32_t in_serial,
                              const std::vector<std::string> &in_upbEntry,
                              int32_t in_type));
  CDH_MTKEX_STUB(getATR(int32_t in_serial, int32_t in_cardType));
  CDH_MTKEX_STUB(getCurrentUiccCardProvisioningStatus(int32_t in_serial,
                                                      int32_t in_cardType));
  CDH_MTKEX_STUB(getPhoneBookMemStorage(int32_t in_serial,
                                        int32_t in_cardType));
  CDH_MTKEX_STUB(getPhoneBookStringsLength(int32_t in_serial,
                                           int32_t in_cardType));
  CDH_MTKEX_STUB(handleStkCallSetupRequestFromSimWithResCode(
      int32_t in_serial, int32_t in_accept, int32_t in_resCode));
  CDH_MTKEX_STUB(queryNetworkLock(int32_t in_serial, int32_t in_category,
                                  int32_t in_requestId));
  CDH_MTKEX_STUB(queryPhbStorageInfo(int32_t in_serial, int32_t in_type,
                                     int32_t in_count));
  CDH_MTKEX_STUB(queryUPBAvailable(int32_t in_serial, int32_t in_type,
                                   int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readPhbEntry(int32_t in_serial, int32_t in_type,
                              int32_t in_index, int32_t in_count,
                              int32_t in_mode));
  CDH_MTKEX_STUB(readPhoneBookEntryExt(int32_t in_serial, int32_t in_type,
                                       int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readUPBAasList(int32_t in_serial, int32_t in_fileId,
                                int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readUPBAnrEntry(int32_t in_serial, int32_t in_fileId,
                                 int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readUPBEmailEntry(int32_t in_serial, int32_t in_fileId,
                                   int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readUPBGasList(int32_t in_serial, int32_t in_fileId,
                                int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(readUPBGrpEntry(int32_t in_serial, int32_t in_adnIndex,
                                 int32_t in_fileId));
  CDH_MTKEX_STUB(readUPBSneEntry(int32_t in_serial, int32_t in_fileId,
                                 int32_t in_index, int32_t in_count));
  CDH_MTKEX_STUB(sendRsuRequest(int32_t in_serial,
                                const ::aidl::vendor::mediatek::hardware::
                                    mtkradioex::rsu::RsuRequestInfo &in_request,
                                int32_t in_type));
  CDH_MTKEX_STUB(sendVsimNotification(int32_t in_serial,
                                      int32_t in_transactionId,
                                      int32_t in_eventId, int32_t in_simType,
                                      int32_t in_dataLen));
  CDH_MTKEX_STUB(sendVsimOperation(int32_t in_serial, int32_t in_transactionId,
                                   int32_t in_eventId, int32_t in_result,
                                   int32_t in_dataLen,
                                   const std::vector<uint8_t> &in_data,
                                   int32_t in_simType));
  CDH_MTKEX_STUB(setNetworkLock(int32_t in_serial, int32_t in_category,
                                int32_t in_lockState,
                                const std::string &in_password,
                                const std::string &in_data,
                                const std::string &in_clientId,
                                const std::string &in_clientSecret,
                                int32_t in_requestId));
  CDH_MTKEX_STUB(setPhoneBookMemStorage(int32_t in_serial,
                                        const std::string &in_storage,
                                        const std::string &in_password,
                                        int32_t in_type));
  CDH_MTKEX_STUB(setPhonebookReady(int32_t in_serial, int32_t in_ready,
                                   int32_t in_type));
  CDH_MTKEX_STUB(setSimPower(int32_t in_serial, int32_t in_power,
                             int32_t in_cardType));
  CDH_MTKEX_STUB(writePhbEntry(int32_t in_serial,
                               const mtkex_sim::PhbEntryStructure &in_entry,
                               int32_t in_type));
  CDH_MTKEX_STUB(writePhoneBookEntryExt(
      int32_t in_serial, const mtkex_sim::PhbEntryExt &in_entry,
      int32_t in_type));
  CDH_MTKEX_STUB(writeUPBGrpEntry(int32_t in_serial, int32_t in_adnIndex,
                                  const std::vector<int32_t> &in_grpIds,
                                  int32_t in_type));
  CDH_MTKEX_STUB(queryUPBCapability(int32_t in_serial, int32_t in_type));
  CDH_MTKEX_STUB(supplyDepersonalization(int32_t in_serial,
                                         const std::string &in_netpin,
                                         int32_t in_type,
                                         int32_t in_validTime));
  CDH_MTKEX_STUB(supplyDeviceNetworkDepersonalization(
      int32_t in_serial, const std::string &in_netpin, int32_t in_reason));
  CDH_MTKEX_STUB(responseAcknowledgementMtk());
  CDH_MTKEX_STUB(setResponseFunctionsRsu(
      const std::shared_ptr<::aidl::vendor::mediatek::hardware::mtkradioex::rsu::
                                IRsuRadioResponse> &in_response,
      const std::shared_ptr<::aidl::vendor::mediatek::hardware::mtkradioex::rsu::
                                IRsuRadioIndication> &in_indication));
  CDH_MTKEX_STUB(setResponseFunctionsSE(
      const std::shared_ptr<::aidl::vendor::mediatek::hardware::mtkradioex::se::
                                ISERadioResponse> &in_response,
      const std::shared_ptr<::aidl::vendor::mediatek::hardware::mtkradioex::se::
                                ISERadioIndication> &in_indication));
  CDH_MTKEX_STUB(getSimTypeInfo(int32_t in_serial, int32_t in_cardType));
  CDH_MTKEX_STUB(switchSimType(int32_t in_serial, int32_t in_simType,
                               int32_t in_cardType));

private:
  void NotifyIccid() {
    const auto indication = callbacks_.IndicationCallback();
    if (indication == nullptr)
      return;
    const std::string iccid = CurrentIccid();
    {
      std::lock_guard<std::mutex> lock(iccid_mutex_);
      if (iccid == last_iccid_)
        return;
      last_iccid_ = iccid;
    }
    // oneway（与真机 AIDL 一致）：发完即返回，异常码只反映投递是否成功。
    indication->iccidChanged(slot(), iccid);
  }

  // 槽位视图（slot != 0 已被 SnapshotState 置为无卡）→ ICCID 或空卡哨兵。
  std::string CurrentIccid() const {
    const auto snapshot = SnapshotState();
    if (snapshot.has_value() && snapshot->sim.present &&
        !snapshot->sim.identity.iccid.empty()) {
      return snapshot->sim.identity.iccid;
    }
    return kNoCardIccid;
  }

  ResponseFunctions<mtkex_sim::IMtkRadioExSimResponse,
                    mtkex_sim::IMtkRadioExSimIndication>
      callbacks_;
  std::mutex iccid_mutex_;
  std::string last_iccid_;
  StateObserver observer_;
};

#undef CDH_MTKEX_STUB

} // namespace

std::shared_ptr<mtkex_sim::IMtkRadioExSim>
CreateMtkRadioExSim(Channel &channel, StateStore &state_store,
                    std::int32_t slot) {
  return ::ndk::SharedRefBase::make<RadioMtkExSim>(channel, state_store, slot);
}

} // namespace cdh::radio