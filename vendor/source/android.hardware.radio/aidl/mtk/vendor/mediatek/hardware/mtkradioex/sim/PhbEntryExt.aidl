/*
 * MTK radioex sim 补充类型（无 AOSP 官方源，随库维护）。
 * 来源：gold(MIUI/MTK) ROM 反编译。
 */
package vendor.mediatek.hardware.mtkradioex.sim;

@VintfStability
parcelable PhbEntryExt {
    String adnumber;
    String adnumber_a;
    String adnumber_b;
    int adtype;
    String email;
    int email_dcs;
    String group;
    int hidden;
    int index;
    String number;
    String secondtext;
    int secondtext_dcs;
    String text;
    int text_dcs;
    int type;
}
