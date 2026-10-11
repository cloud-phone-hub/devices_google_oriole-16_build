/*
 * MTK radioex sim 补充类型（无 AOSP 官方源，随库维护）。
 *
 * 来源：gold(MIUI/MTK) ROM 的 vendor.mediatek.hardware.mtkradioex.sim.SimTypeInfo
 * （反编译自 /system_ext/framework/mediatek-telephony-common.jar）。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

@VintfStability
parcelable SimTypeInfo {
    int simType;
    int source;
    int support;
}
