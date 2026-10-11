/*
 * MTK radioex sim 补充接口（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的 vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSim
 * （反编译自 /system_ext/framework/mediatek-telephony-common.jar）。
 *
 * 方法顺序 = ROM 的 transaction code 顺序（1..41，必须逐一对齐）。
 *
 * 本云机只实现一个最小可用子集：setResponseFunctionsMtk 保存 MTK 框架传入的
 * indication，随后按宿主 SIM 快照主动发 iccidChanged（MtkUiccController.mIccid
 * 的唯一数据源）。其余方法一律回 REQUEST_NOT_SUPPORTED，MTK 侧会优雅降级。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

import vendor.mediatek.hardware.mtkradioex.rsu.IRsuRadioIndication;
import vendor.mediatek.hardware.mtkradioex.rsu.IRsuRadioResponse;
import vendor.mediatek.hardware.mtkradioex.rsu.RsuRequestInfo;
import vendor.mediatek.hardware.mtkradioex.se.ISERadioIndication;
import vendor.mediatek.hardware.mtkradioex.se.ISERadioResponse;
import vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSimIndication;
import vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSimResponse;
import vendor.mediatek.hardware.mtkradioex.sim.PhbEntryExt;
import vendor.mediatek.hardware.mtkradioex.sim.PhbEntryStructure;
import vendor.mediatek.hardware.mtkradioex.sim.SimAuthStructure;

@VintfStability
interface IMtkRadioExSim {
    void getIccid(int serial, int cardType);
    void activateUiccCard(int serial, int cardType);
    void deactivateUiccCard(int serial, int cardType);
    void deleteUPBEntry(int serial, int entryType, int adnIndex, int upbIndex, int upbField);
    void doGeneralSimAuthentication(int serial, in SimAuthStructure sessionInfo, int tag);
    void editUPBEntry(int serial, in String[] upbEntry, int type);
    void getATR(int serial, int cardType);
    void getCurrentUiccCardProvisioningStatus(int serial, int cardType);
    void getPhoneBookMemStorage(int serial, int cardType);
    void getPhoneBookStringsLength(int serial, int cardType);
    void handleStkCallSetupRequestFromSimWithResCode(int serial, int accept, int resCode);
    void queryNetworkLock(int serial, int category, int requestId);
    void queryPhbStorageInfo(int serial, int type, int count);
    void queryUPBAvailable(int serial, int type, int index, int count);
    void readPhbEntry(int serial, int type, int index, int count, int mode);
    void readPhoneBookEntryExt(int serial, int type, int index, int count);
    void readUPBAasList(int serial, int fileId, int index, int count);
    void readUPBAnrEntry(int serial, int fileId, int index, int count);
    void readUPBEmailEntry(int serial, int fileId, int index, int count);
    void readUPBGasList(int serial, int fileId, int index, int count);
    void readUPBGrpEntry(int serial, int adnIndex, int fileId);
    void readUPBSneEntry(int serial, int fileId, int index, int count);
    void sendRsuRequest(int serial, in RsuRequestInfo request, int type);
    void sendVsimNotification(int serial, int transactionId, int eventId, int simType, int dataLen);
    void sendVsimOperation(int serial, int transactionId, int eventId, int result, int dataLen,
            in byte[] data, int simType);
    void setNetworkLock(int serial, int category, int lockState, in String password,
            in String data, in String clientId, in String clientSecret, int requestId);
    void setPhoneBookMemStorage(int serial, in String storage, in String password, int type);
    void setPhonebookReady(int serial, int ready, int type);
    void setSimPower(int serial, int power, int cardType);
    void writePhbEntry(int serial, in PhbEntryStructure entry, int type);
    void writePhoneBookEntryExt(int serial, in PhbEntryExt entry, int type);
    void writeUPBGrpEntry(int serial, int adnIndex, in int[] grpIds, int type);
    void queryUPBCapability(int serial, int type);
    void supplyDepersonalization(int serial, in String netpin, int type, int validTime);
    void supplyDeviceNetworkDepersonalization(int serial, in String netpin, int reason);
    void responseAcknowledgementMtk();
    void setResponseFunctionsMtk(in IMtkRadioExSimResponse response,
            in IMtkRadioExSimIndication indication);
    void setResponseFunctionsRsu(in IRsuRadioResponse response, in IRsuRadioIndication indication);
    void setResponseFunctionsSE(in ISERadioResponse response, in ISERadioIndication indication);
    void getSimTypeInfo(int serial, int cardType);
    void switchSimType(int serial, int simType, int cardType);
}
