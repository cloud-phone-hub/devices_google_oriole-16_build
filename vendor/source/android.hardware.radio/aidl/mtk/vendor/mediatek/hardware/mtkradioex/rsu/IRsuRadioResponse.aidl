/*
 * MTK radioex rsu 依赖接口（无 AOSP 官方源，随库维护）。
 *
 * 本云机 radio HAL 不实现 RSU 能力，此接口仅作为
 * IMtkRadioExSim.setResponseFunctionsRsu 的参数类型存在，保证 AIDL 契约完整。
 */
package vendor.mediatek.hardware.mtkradioex.rsu;

@VintfStability
interface IRsuRadioResponse {
}
