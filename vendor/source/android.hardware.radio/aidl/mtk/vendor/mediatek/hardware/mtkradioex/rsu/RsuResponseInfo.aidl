/*
 * MTK radioex rsu 补充类型（无 AOSP 官方源，随库维护）。
 * 来源：gold(MIUI/MTK) ROM 反编译。
 */
package vendor.mediatek.hardware.mtkradioex.rsu;

@VintfStability
parcelable RsuResponseInfo {
    String data;
    int errCode;
    int opId;
    int requestId;
    int reserveInt1;
    int reserveInt2;
    String reserveString1;
    int status;
    long time;
    int version;
}
