/*
 * MTK radioex sim 补充类型（无 AOSP 官方源，随库维护）。
 * 来源：gold(MIUI/MTK) ROM 反编译。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

@VintfStability
parcelable SimAuthStructure {
    int mode;
    String param1;
    String param2;
    int sessionId;
    int tag;
}
