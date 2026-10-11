/*
 * MTK radioex sim indication（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的
 * vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSimIndication
 * （反编译自 /system_ext/framework/mediatek-telephony-common.jar）。
 *
 * 方法顺序 = ROM 的 transaction code 顺序（必须逐一对齐，否则客户端
 * Proxy 的 transact code 与服务端 Stub 分支错位）。
 * 关键方法 iccidChanged 是 MTK 框架 MtkUiccController.mIccid 的**唯一**
 * 数据来源：MtkRIL.cacheIccid() 只由 MtkRadioExSimIndication.iccidChanged
 * 调用；MtkUiccController.mIccid 为空串时 MTK_SMSVC.checkAllIccIdReady()
 * 永远返回 false，导致 SIM 订阅永不创建（状态栏未插卡/无号码）。
 *
 * 全部方法都是 oneway：ROM 的 Stub$Proxy.iccidChanged 用
 * transact(code, data, null, 1)（第 4 参 = FLAG_ONEWAY、无 reply），Stub
 * 的 onTransact 各 case 也都不写 writeNoException。若按双向调用，服务端
 * 不回写 reply，客户端 AParcel_readStatusHeader 读空 parcel 失败，统一
 * 落到 EX_TRANSACTION_FAILED(-129)，服务端虽有执行但调用方永远看到失败。
 * 声明 oneway 后与真机语义一致（发完即返回，无 reply）。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

import vendor.mediatek.hardware.mtkradioex.sim.SimTypeInfo;
import vendor.mediatek.hardware.mtkradioex.sim.VsimOperationEvent;

@VintfStability
interface IMtkRadioExSimIndication {
    oneway void bipProactiveCommand(int type, String cmd);
    oneway void iccidChanged(int slotId, String iccid);
    oneway void onCardDetectedInd(int type);
    oneway void onImeiLock(int type);
    oneway void onImsiRefreshDone(int type);
    oneway void onRsuEvent(int type, int eventId, String data);
    oneway void onRsuSimLockEvent(int type, int eventId);
    oneway void onSimHotSwapInd(int type, int eventId, String data);
    oneway void onSimPowerChangedInd(int type, in int[] data);
    oneway void onStkMenuReset(int type);
    oneway void onVirtualSimStatusChanged(int type, int slotId);
    oneway void onVsimEventIndication(int type, in VsimOperationEvent event);
    oneway void phbReadyNotification(int type, int isReady);
    oneway void smlSlotLockInfoChangedInd(int type, in int[] lockInfo);
    oneway void simTypeInfoChangedInd(int type, in SimTypeInfo info);
}
