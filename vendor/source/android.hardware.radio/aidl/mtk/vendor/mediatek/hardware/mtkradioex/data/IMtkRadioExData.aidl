/*
 * MTK radioex 补充接口（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的 vendor.mediatek.hardware.mtkradioex.data.IMtkRadioExData
 * （反编译自 /system_ext/framework/mediatek-telephony-common.jar 内 IMtkRadioExData.java）。
 * 事务码取证（Stub 静态常量）：dataConnectionAttach=1, dataConnectionDetach=2,
 * enableDsdaIndication=3, getDsdaStatus=4, resetAllConnections=5,
 * resetMdDataRetryCount=6, setFdMode=7, syncDataSettingsToMd=8,
 * responseAcknowledgementMtk=9, setResponseFunctionsMtk=10,
 * setResponseFunctionsAssist=11, setupDataCallSlice=12, getInterfaceHash=16777214,
 * getInterfaceVersion=16777215。
 *
 * 当前不参与构建（radio HAL 走"别名注册 + VINTF 不声明"，MtkRIL 经
 * waitForDeclaredService 落空跳过 radioex 代理——见 src/main.cpp 注释）。
 * 若未来实现方案 1（真正桥接 MTK radioex 服务），把本包加入 build.py 的
 * AIDL_PACKAGES 并按下面已确认清单补全方法。
 */
package vendor.mediatek.hardware.mtkradioex.data;

interface IMtkRadioExData {
    // RIL→HAL 单向请求（事务码 1~8、12，serial 首参）：
    //   void dataConnectionAttach(int serial, int cid, int reason);      // code 1 ✓
    //   void dataConnectionDetach(int serial, int cid, int reason);      // code 2 ✓
    //   void enableDsdaIndication(int serial, boolean enable);           // code 3
    //   void getDsdaStatus(int serial);                                  // code 4
    //   void resetAllConnections(int serial);                            // code 5
    //   void resetMdDataRetryCount(int serial);                          // code 6
    //   void setFdMode(int serial, int mode);                            // code 7
    //   void syncDataSettingsToMd(int serial);                           // code 8
    //   void responseAcknowledgementMtk();                               // code 9 ✓
    //   void setResponseFunctionsMtk(in IRadioExDataResponse r,
    //                                in IRadioExDataIndication ind);     // code 10 ✓
    //   void setResponseFunctionsAssist(in IRadioExDataResponseAssist r,
    //                                   in IRadioExDataIndicationAssist i); // code 11
    //   // code 12 setupDataCallSlice：参数含 APN/协议/优先级，待按 Proxy 补齐
    // 注：Response/Indication 变体接口（IRadioExDataResponse*/IRadioExDataIndication*）
    //     在 gold ROM 中以内部类形式存在，补全本包时一并建立。

    // AIDL 内置（生成器自动补充，无需手工声明）：int getInterfaceVersion();
    // String getInterfaceHash();
}