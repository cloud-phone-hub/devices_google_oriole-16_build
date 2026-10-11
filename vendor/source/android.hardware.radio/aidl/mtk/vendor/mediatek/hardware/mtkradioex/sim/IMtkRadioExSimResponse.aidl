/*
 * MTK radioex sim response（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的
 * vendor.mediatek.hardware.mtkradioex.sim.IMtkRadioExSimResponse。
 * 方法顺序 = ROM 的 transaction code 顺序。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

import android.hardware.radio.RadioResponseInfo;
import vendor.mediatek.hardware.mtkradioex.rsu.RsuResponseInfo;
import vendor.mediatek.hardware.mtkradioex.sim.PhbEntryExt;
import vendor.mediatek.hardware.mtkradioex.sim.PhbEntryStructure;
import vendor.mediatek.hardware.mtkradioex.sim.PhbMemStorageResponse;
import vendor.mediatek.hardware.mtkradioex.sim.SimTypeInfo;
import vendor.mediatek.hardware.mtkradioex.sim.VsimEvent;

@VintfStability
interface IMtkRadioExSimResponse {
    void getIccidResponse(in RadioResponseInfo info, String iccid);
    void activateUiccCardRsp(in RadioResponseInfo info, int ret);
    void deactivateUiccCardRsp(in RadioResponseInfo info, int ret);
    void deleteUPBEntryResponse(in RadioResponseInfo info);
    void editUPBEntryResponse(in RadioResponseInfo info);
    void getATRResponse(in RadioResponseInfo info, String atr);
    void getCurrentUiccCardProvisioningStatusRsp(in RadioResponseInfo info, int status);
    void getPhoneBookMemStorageResponse(in RadioResponseInfo info, in PhbMemStorageResponse storage);
    void getPhoneBookStringsLengthResponse(in RadioResponseInfo info, in int[] lengths);
    void handleStkCallSetupRequestFromSimWithResCodeResponse(in RadioResponseInfo info);
    void queryNetworkLockResponse(in RadioResponseInfo info, int requestId, int category, int count,
            int lockState, int lockStatus, int attempts, int maxAttempts);
    void setNetworkLockResponse(in RadioResponseInfo info);
    void queryPhbStorageInfoResponse(in RadioResponseInfo info, in int[] storageInfo);
    void queryUPBAvailableResponse(in RadioResponseInfo info, in int[] available);
    void queryUPBCapabilityResponse(in RadioResponseInfo info, in int[] capability);
    void readPhbEntryResponse(in RadioResponseInfo info, in PhbEntryStructure[] entries);
    void readPhoneBookEntryExtResponse(in RadioResponseInfo info, in PhbEntryExt[] entries);
    void readUPBAasListResponse(in RadioResponseInfo info, in String[] list);
    void readUPBAnrEntryResponse(in RadioResponseInfo info, in PhbEntryStructure[] entries);
    void readUPBEmailEntryResponse(in RadioResponseInfo info, String email);
    void readUPBGasListResponse(in RadioResponseInfo info, in String[] list);
    void readUPBGrpEntryResponse(in RadioResponseInfo info, in int[] groups);
    void readUPBSneEntryResponse(in RadioResponseInfo info, String sne);
    void sendRsuRequestResponse(in RadioResponseInfo info, in RsuResponseInfo response);
    void vsimNotificationResponse(in RadioResponseInfo info, in VsimEvent event);
    void vsimOperationResponse(in RadioResponseInfo info);
    void setPhoneBookMemStorageResponse(in RadioResponseInfo info);
    void setPhonebookReadyResponse(in RadioResponseInfo info);
    void setSimPowerResponse(in RadioResponseInfo info);
    void writePhbEntryResponse(in RadioResponseInfo info);
    void writePhoneBookEntryExtResponse(in RadioResponseInfo info);
    void writeUPBGrpEntryResponse(in RadioResponseInfo info);
    void supplyDepersonalizationResponse(in RadioResponseInfo info, int ret);
    void supplyDeviceNetworkDepersonalizationResponse(in RadioResponseInfo info, int ret);
    void getSimTypeInfoResponse(in RadioResponseInfo info, in SimTypeInfo info2);
    void switchSimTypeResponse(in RadioResponseInfo info);
}
