/*
 * MTK radioex 补充接口（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的 vendor.mediatek.hardware.mtkradioex.network.IMtkRadioExNetwork
 * （反编译自 /system_ext/framework/mediatek-telephony-common.jar 内
 * IMtkRadioExNetwork.java）。
 *
 * 当前不参与构建（radio HAL 走"别名注册 + VINTF 不声明"，MtkRIL 经
 * waitForDeclaredService 落空跳过 radioex 代理——见 src/main.cpp 注释）。
 * 若未来实现方案 1（真正桥接 MTK radioex 服务），把本包加入 build.py 的
 * AIDL_PACKAGES 并按反编译的 Proxy/Stub 补全方法。
 */
package vendor.mediatek.hardware.mtkradioex.network;

interface IMtkRadioExNetwork {
    // TODO(methods): 按 gold ROM IMtkRadioExNetwork.java 反编译补齐
    // AIDL 内置（生成器自动补充，无需手工声明）：int getInterfaceVersion();
    // String getInterfaceHash();
}
