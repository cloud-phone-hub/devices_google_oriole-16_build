// Copyright (c) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
#include "channel.h"
#include "radio_config.h"
#include "radio_data.h"
#include "radio_messaging.h"
#include "radio_modem.h"
#include "radio_mtkex_sim.h"
#include "radio_network.h"
#include "radio_sim.h"
#include "radio_service.h"
#include "radio_voice.h"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_status.h>
#include <android/log.h>

#include <memory>
#include <string>

namespace {

constexpr char kLogTag[] = "cdh-radio";

template <typename Interface>
bool RegisterService(const std::shared_ptr<Interface> &service,
                     const char *name) {
  if (service == nullptr)
    return false;
  const binder_exception_t result =
      AServiceManager_addService(service->asBinder().get(), name);
  if (result != EX_NONE) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "register service failed: %s (%d)", name, result);
    return false;
  }
  return true;
}

} // namespace

int main() {
  cdh::radio::StateStore state_store;
  // v3 顺延 virtconsole 布局：radio = hvc4（hvc0/1 留给固件 console，hvc2
  // 空置预留，keymint 占 Cuttlefish 基线编号 hvc3，其后通道整体 +1 顺延）
  cdh::radio::Channel channel("/dev/hvc4");
  std::string error;
  if (!channel.Start(&state_store, &error)) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "start guest channel failed: %s", error.c_str());
    return 1;
  }

  if (!ABinderProcess_setThreadPoolMaxThreadCount(8)) {
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "configure binder thread pool failed");
    return 1;
  }

  // dsds 双槽注册：gold 真机每个 radio 接口都有 slot1/slot2 实例（框架按 modem
  // 数建两条 Phone，各自连自己的 slot 实例），radioex 桩同样两槽。
  const auto config = cdh::radio::CreateRadioConfig(channel, state_store);
  for (std::int32_t slot = 0; slot < cdh::radio::kLogicalModemCount; ++slot) {
    const std::string suffix = "/slot" + std::to_string(slot + 1);
    const auto data = cdh::radio::CreateRadioData(channel, state_store, slot);
    const auto messaging =
        cdh::radio::CreateRadioMessaging(channel, state_store, slot);
    const auto modem = cdh::radio::CreateRadioModem(channel, state_store, slot);
    const auto network =
        cdh::radio::CreateRadioNetwork(channel, state_store, slot);
    const auto sim = cdh::radio::CreateRadioSim(channel, state_store, slot);
    const auto mtkex_sim =
        cdh::radio::CreateMtkRadioExSim(channel, state_store, slot);
    const auto voice = cdh::radio::CreateRadioVoice(channel, state_store, slot);

    if (!RegisterService(
            data, ("android.hardware.radio.data.IRadioData" + suffix).c_str()) ||
        !RegisterService(messaging,
                         ("android.hardware.radio.messaging.IRadioMessaging" +
                          suffix)
                             .c_str()) ||
        !RegisterService(modem,
                         ("android.hardware.radio.modem.IRadioModem" + suffix)
                             .c_str()) ||
        !RegisterService(
            network,
            ("android.hardware.radio.network.IRadioNetwork" + suffix).c_str()) ||
        !RegisterService(sim,
                         ("android.hardware.radio.sim.IRadioSim" + suffix)
                             .c_str()) ||
        !RegisterService(voice,
                         ("android.hardware.radio.voice.IRadioVoice" + suffix)
                             .c_str()) ||
        // radioex sim 用真实现注册（见下方注释）：MtkRIL 取到代理后会回填响应
        // 函数并接收 iccidChanged，这是 MTK 侧订阅门禁的唯一解锁点。
        !RegisterService(mtkex_sim,
                         ("vendor.mediatek.hardware.mtkradioex.sim."
                          "IMtkRadioExSim" +
                          suffix)
                             .c_str())) {
      channel.Stop();
      return 1;
    }

    // MTK radioex 别名注册（sim 除外，sim 是真实现）：gold(MIUI/MTK) 的 MtkRIL 有
    // 两个机制依赖这些 vendor.mediatek.hardware.mtkradioex.* AIDL 名字：
    //   1) isRildReady() 用 ServiceManager.checkService 轮询这些名字，要求都可
    //      解锁才继续初始化 —— 因此必须在 servicemanager 注册（主 servicemanager
    //      只按名返回 binder，不校验接口描述符；别名注册即用标准 AIDL 服务 binder
    //      以 radioex 名再注册一份）。
    //   2) getMtkRadioExServiceProxy() 用 waitForDeclaredService 取代理（要求 VINTF
    //      declared）。别名（标准 radio binder 挂在 radioex 名下）【不可】声明：
    //      一旦 declared，waitForDeclaredService 返回它并当作 radioex 接口调用，
    //      事务码整体错位（radioex 的 setResponseFunctionsMtk=37 撞上 IRadioSim 的
    //      真实方法），com.android.phone 会反复崩溃。故 data/messaging/modem/
    //      network/voice 只做别名、不声明：checkService 命中（isRildReady 解锁）、
    //      waitForDeclaredService 落空（MtkRIL 跳过这些 radioex 分支持续走标准
    //      android.hardware.radio.* 接口）。
    //   例外是 sim：MTK 的 MtkUiccController.mIccid[] 只有 iccidChanged 一条写入
    //      路径，取不到 iccid 会卡死 SMSVC 的订阅门禁 checkAllIccIdReady。所以 sim
    //      这个名字上挂的是真实现（BnMtkRadioExSim，事务码与服务端一一对应）并在
    //      VINTF 声明，让 MtkRIL 正常握手。
    RegisterService(config,
                    ("vendor.mediatek.hardware.mtkradioex.data.IMtkRadioExData" +
                     suffix)
                        .c_str());
    RegisterService(messaging,
                    ("vendor.mediatek.hardware.mtkradioex.messaging."
                     "IMtkRadioExMessaging" +
                     suffix)
                        .c_str());
    RegisterService(modem,
                    ("vendor.mediatek.hardware.mtkradioex.modem."
                     "IMtkRadioExModem" +
                     suffix)
                        .c_str());
    RegisterService(network,
                    ("vendor.mediatek.hardware.mtkradioex.network."
                     "IMtkRadioExNetwork" +
                     suffix)
                        .c_str());
    RegisterService(voice,
                    ("vendor.mediatek.hardware.mtkradioex.voice."
                     "IMtkRadioExVoice" +
                     suffix)
                        .c_str());
  }

  if (!RegisterService(config,
                       "android.hardware.radio.config.IRadioConfig/default")) {
    channel.Stop();
    return 1;
  }

  ABinderProcess_startThreadPool();
  ABinderProcess_joinThreadPool();
  channel.Stop();
  return 1;
}
