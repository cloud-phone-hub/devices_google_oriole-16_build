#!/usr/bin/env python3
"""android.hardware.media —— note13-5G-15 Codec2 假登记 store 构建（自包含 Python 编排）。

对齐 note13 真机（gold）vendor media 形态：双二进制各注册一个
BnComponentStore 假登记实例（listComponents 返回静态 traits 表，
createComponent 等返回 CANNOT_DO），二进制名决定实例（无 rc 参数，同真机）：
  - android.hardware.media.c2-mediatek      -> IComponentStore/default
    （MTK store，15 组件 c2.mtk.*，rank=512）
  - vendor.dolby.media.c2-default-service-dax -> IComponentStore/default9
    （Dolby store，2 组件 c2.dolby.*，rank=8）
软解 software 实例由 apex 提供，与本模块无关。

vendor 静态资产全量对齐真机 /vendor/etc/：
  - init: android.hardware.media.c2-mediatek.rc +
    vendor.dolby.media.c2-default-service-dax.rc（服务名/user/group 与真机
    逐字段一致，仅追加 seclabel mediacodec——基座无 hal_codec2_default 域）
  - vintf/manifest: manifest_media_c2_default.xml +
    vendor.dolby.media.c2-default-service-dax.xml（真机原样）
  - media_codecs{,_c2,_dolby_audio,_google_audio,_google_video_le,_performance,
    _system_default}.xml（真机 /vendor/etc 7 件套原样；media_codecs_mediatek_
    audio.xml 真机亦不存在，Include 静默跳过，形态一致）
产物落 VENDOR_STAGE_DIR（out/dist/vendor，相对路径即镜像内 /vendor）。

单段式：
  1. AIDL NDK 绑定源：4 个版本化包（hardware/interfaces android.hardware.
     {common,common.fmq,media.bufferpool2,media.c2}），-I 根 aidl/aosp（快照
     展开），--hash 取自包 .hash；
  2. codec2_targets.inc：由 config/codec2.json stores 生成静态登记表
     （kCodec2Targets）；
  3. 假登记 store：cdh_codec2_store_service.cpp + AIDL 生成源
     -> 双二进制（PIE，libbinder_ndk/libc++/libnativewindow），basename
     判定实例（同真机无参数形态）。

工具链：NDK r29 clang（--target=aarch64-linux-android35）+ SDK build-tools aidl
+ host lld；经 CDH_NDK/CDH_CLANG/CDH_AIDL/CDH_LLD 可覆盖（同 wifi/radio）。
链接期 ABI 库来源：note13 自身 lineage-22.0 super 的 system_a.img（deps.py 研磨）。

用法：
  python3 build/vendor/source/android.hardware.media/build.py
  python3 build/vendor/source/android.hardware.media/build.py --dry-run
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.media/ 下，向上 5 级为设备根
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import OUT_DIR, VENDOR_STAGE_DIR  # noqa: E402  （不注册 baselines 标签）

MODULE_DIR = os.path.dirname(os.path.abspath(__file__))

# 本模块的 deps.py（ABI 研磨）与 build/vendor/build/deps.py（运行库）
# 同名，用绝对路径加载，避免被 sys.path 上层模块遮蔽。
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    "media_deps", os.path.join(MODULE_DIR, "deps.py"))
media_deps = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(media_deps)

COMMON_DIR = os.path.join(os.path.dirname(MODULE_DIR), "common")
SRC_DIR = os.path.join(MODULE_DIR, "src")
AIDL_ROOT = os.path.join(MODULE_DIR, "aidl", "aosp")
CONFIG_PATH = os.path.join(MODULE_DIR, "config", "codec2.json")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "media-ndk-work")

TARGET = "aarch64-linux-android35"
MIN_SDK = 35

# 产物名（rc 引用）——真机同名的两个二进制，basename 决定注册实例
MTK_BINARY = "android.hardware.media.c2-mediatek"
DOLBY_BINARY = "vendor.dolby.media.c2-default-service-dax"
STORE_BINARIES = [MTK_BINARY, DOLBY_BINARY]

# AIDL 包：<展开相对路径> -> 版本
AIDL_PACKAGES = {
    "android/hardware/common": 2,
    "android/hardware/common/fmq": 1,
    "android/hardware/media/bufferpool2": 2,
    "android/hardware/media/c2": 1,
}

# 假登记 store 链接库
STORE_LIBS = ["libc++.so", "libbinder_ndk.so", "libnativewindow.so"]
STORE_EXTRA = ["-llog", "-ldl", "-pthread", "-lm", "-lc"]

LABEL_BIN = "u:object_r:hal_codec2_default_exec:s0"
LABEL_CONFIG = "u:object_r:vendor_configs_file:s0"

# 模块内 vendor/etc 资产 -> 镜像内 /vendor/etc 路径（真机同名同路径）
VENDOR_ASSETS = [
    "init/android.hardware.media.c2-mediatek.rc",
    "init/vendor.dolby.media.c2-default-service-dax.rc",
    "vintf/manifest/manifest_media_c2_default.xml",
    "vintf/manifest/vendor.dolby.media.c2-default-service-dax.xml",
    "media_codecs.xml",
    "media_codecs_c2.xml",
    "media_codecs_dolby_audio.xml",
    "media_codecs_google_audio.xml",
    "media_codecs_google_video_le.xml",
    "media_codecs_performance.xml",
    "media_codecs_system_default.xml",
]

# 新增文件的 SELinux 标签（bin 走 codec2 exec 类型；rc/xml/vintf 走
# vendor_configs_file——真机 /vendor/etc 静态资产同款标签）
LABELS = {f"bin/hw/{name}": LABEL_BIN for name in STORE_BINARIES}
LABELS.update({f"etc/{rel}": LABEL_CONFIG for rel in VENDOR_ASSETS})
BASELINES = sorted(glob.glob(os.path.join(_DEVICE_ROOT, "build", "baselines",
                                          "gold_images_*.json")))


def env(name, default):
    return os.environ.get(name) or default


def ndk_clang_rt_dir(prebuilt):
    base = os.path.join(prebuilt, "lib", "clang")
    cands = sorted(d for d in glob.glob(os.path.join(base, "*"))
                   if os.path.isdir(d))
    if not cands:
        raise RuntimeError(f"NDK clang 资源目录缺失: {base}")
    return cands[-1]


def toolchain(dry_run=False):
    ndk = env("CDH_NDK", os.path.expanduser(
        "~/Library/Android/sdk/ndk/29.0.14206865"))
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt",
                            "darwin-x86_64")
    return {
        "aidl": env("CDH_AIDL",
                    os.path.expanduser(
                        "~/Library/Android/sdk/build-tools/36.1.0/aidl")),
        "clang": env("CDH_CLANG", os.path.join(prebuilt, "bin", "clang")),
        "lld": env("CDH_LLD", os.path.join(prebuilt, "bin", "ld.lld")),
        "sysroot": os.path.join(prebuilt, "sysroot"),
        "clang_rt_dir": ndk_clang_rt_dir(prebuilt),
        "binder": env("CDH_BINDER",
                      os.path.expanduser(
                          "~/Library/Android/sdk/platforms/android-36/"
                          "optional/libbinder_ndk_cpp")),
        "binder_platform": os.path.join(COMMON_DIR, "ndk-binder-platform"),
        "bionic": os.path.join(COMMON_DIR, "bionic15"),
        "libcxx_config": os.path.join(COMMON_DIR, "libcxx-platform"),
    }


def run(argv, dry_run=False):
    if dry_run:
        print("[..] (dry)", " ".join(argv))
        return 0
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        sys.exit(1)
    return 0


def read_config():
    with open(CONFIG_PATH, encoding="utf-8") as f:
        doc = json.load(f)
    if not str(doc.get("schema", "")).endswith(".codec2.v1"):
        print(f"[err] codec2 目标契约错误: {doc.get('schema')}", file=sys.stderr)
        sys.exit(1)
    if doc.get("serviceNames") != [
            "android.hardware.media.c2.IComponentStore/default1",
            "android.hardware.media.c2.IComponentStore/default9"]:
        print("[err] codec2 serviceNames 契约错误", file=sys.stderr)
        sys.exit(1)
    return doc


def write_codec2_targets(path, cfg, dry_run=False):
    """由 codec2.json stores 生成 codec2_targets.inc（kCodec2Targets 静态表）。

    struct 定义（Codec2AliasList/Codec2ValueList/Codec2Target）由唯一 include 方
    cdh_codec2_store_service.cpp 提供，此处只生成数据，避免重复定义。
    profiles/levels 为 C2 枚举值（见 config/codec2.json profileLevel.note），
    供 store 在 IConfigurable::querySupportedValues 上按组件应答。
    """
    lines = [
        "// Generated from config/codec2.json. Do not edit by hand.",
    ]

    def sym_for(instance, name):
        return "".join(ch if ch.isalnum() or ch == "_" else "_"
                       for ch in instance + "_" + name)

    for s in cfg["stores"]:
        for c in s["components"]:
            sym = sym_for(s["instance"], c["name"])
            if c.get("aliases"):
                quoted = ", ".join(json.dumps(a) for a in c["aliases"])
                lines.append(f"static const char *const kAliases_{sym}[] = {{{quoted}}};")
            for key in ("profiles", "levels"):
                if c.get(key):
                    nums = ", ".join(str(int(v)) for v in c[key])
                    lines.append(f"static const int32_t k{key.capitalize()}_{sym}[] = {{{nums}}};")
    lines.append("static const Codec2Target kCodec2Targets[] = {")
    for s in cfg["stores"]:
        for c in s["components"]:
            sym = sym_for(s["instance"], c["name"])
            alias = f"{{kAliases_{sym}, {len(c['aliases'])}}}" if c.get("aliases") else "{nullptr, 0}"
            profiles = (f"{{kProfiles_{sym}, {len(c['profiles'])}}}"
                        if c.get("profiles") else "{nullptr, 0}")
            levels = (f"{{kLevels_{sym}, {len(c['levels'])}}}"
                      if c.get("levels") else "{nullptr, 0}")
            lines.append(
                f"    {{{json.dumps(s['instance'])}, {json.dumps(c['name'])}, "
                f"{json.dumps(c['kind'])}, {json.dumps(c['domain'])}, "
                f"{json.dumps(c['mediaType'])}, {int(c['rank'])}, {alias}, "
                f"{profiles}, {levels}}},")
    lines.append("};")
    lines.append("")
    if dry_run:
        print("[..] (dry) 生成 codec2_targets.inc")
        return
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    total = sum(len(s["components"]) for s in cfg["stores"])
    with_pl = sum(1 for s in cfg["stores"] for c in s["components"] if c.get("profiles"))
    print(f"    生成 codec2_targets.inc（{total} 组件，其中 {with_pl} 条带 profile/level）")


def codegen_aidl(tc, gen_src, gen_hdr, gen_inc, dry_run):
    """对 4 个 AIDL 包跑 aidl（同 wifi codegen：共同 -I 根 aidl/aosp）。"""
    os.makedirs(gen_src, exist_ok=True)
    os.makedirs(gen_hdr, exist_ok=True)
    gen_inc_src = []  # 记录生成源（供编译）
    for rel, version in AIDL_PACKAGES.items():
        base = os.path.join(AIDL_ROOT, rel)
        inputs = []
        for dirpath, _dirs, files in os.walk(base):
            for name in sorted(files):
                if name.endswith(".aidl"):
                    inputs.append(os.path.join(dirpath, name))
        if not inputs:
            print(f"[err] AIDL 包为空: {rel}", file=sys.stderr)
            sys.exit(1)
        hash_path = os.path.join(base, ".hash")
        if not os.path.isfile(hash_path):
            print(f"[err] 缺 AIDL 包 hash: {hash_path}", file=sys.stderr)
            sys.exit(1)
        with open(hash_path, encoding="ascii") as f:
            snap_hash = f.read().split()[-1]
        argv = [tc["aidl"], "--lang=ndk", "--structured", "--stability=vintf",
                f"--min_sdk_version={MIN_SDK}",
                f"--version={version}", f"--hash={snap_hash}",
                "-I", AIDL_ROOT,
                "--out=" + gen_src, "--header_out=" + gen_hdr]
        argv.extend(inputs)
        run(argv, dry_run=dry_run)
        gen_inc_src.extend(inputs)
        print(f"    {rel}: {len(inputs)} aidl (v{version}, hash {snap_hash[:12]})")
    return gen_inc_src


def collect_cpps(root):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for name in sorted(files):
            if name.endswith(".cpp"):
                out.append(os.path.join(dirpath, name))
    return sorted(out)


def libcxx_args(tc):
    headers = os.path.join(tc["sysroot"], "usr", "include", "c++", "v1")
    return ["-nostdinc++", "-I" + tc["libcxx_config"], "-isystem", headers]


def write_asset(src_rel, dst, dry_run=False):
    src = os.path.join(MODULE_DIR, src_rel)
    if dry_run:
        print(f"[..] (dry) 写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(src, encoding="utf-8") as f:
        content = f.read()
    with open(dst, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"    写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}")


def register_labels(dry_run=False):
    """登记新增文件的 SELinux 标签进 baselines vendor.selinux-labels（幂等）。

    vendor 阶段对注入树每个文件都要求 baselines 有标签（缺失即报错退出），
    media 构建成功落盘后必须补登记；daemon/keymint 等模块同款逻辑。
    """
    if not BASELINES:
        print(f"[err] baselines 清单缺失: {_DEVICE_ROOT}/build/baselines/", file=sys.stderr)
        sys.exit(1)
    baselines_path = BASELINES[0]
    with open(baselines_path, encoding="utf-8") as f:
        doc = json.load(f)
    labels = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    missing = {k: v for k, v in LABELS.items() if labels.get(k) != v}
    if not missing:
        print("[..] baselines 标签已齐，跳过")
        return
    labels.update(missing)
    if dry_run:
        print(f"[..] (dry) baselines 将登记标签 {len(missing)} 条")
        return
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(baselines_path),
                               prefix=".baselines.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(tmp, baselines_path)
    except BaseException:
        os.unlink(tmp)
        raise
    print(f"[ok] baselines 登记标签 {len(missing)} 条")


def build(dry_run=False):
    tc = toolchain(dry_run=dry_run)
    cfg = read_config()

    abi_root = media_deps.prepare_abi(dry_run=dry_run)
    abi_sys = os.path.join(abi_root, "system", "lib64")

    gen_aidl = os.path.join(WORK_DIR, "aidl-gen")
    gen_hdr = os.path.join(gen_aidl, "include")
    gen_src = os.path.join(gen_aidl, "src")

    os.makedirs(WORK_DIR, exist_ok=True)

    if not dry_run:
        print(f"==> 服务契约: {cfg['serviceNames'][0]}")

    print("==> AIDL 代码生成")
    if not dry_run:
        for d in (gen_src, gen_hdr):
            if os.path.isdir(d):
                shutil.rmtree(d)
    codegen_aidl(tc, gen_src, gen_hdr, None, dry_run)
    gen_cpps = collect_cpps(gen_src)

    print("==> codec2_targets.inc")
    write_codec2_targets(os.path.join(WORK_DIR, "codec2_targets.inc"), cfg,
                         dry_run=dry_run)

    # startup object（store 链接用）
    crtbegin = os.path.join(tc["bionic"], "cdh_crtbegin_dynamic.cpp")
    crtend = os.path.join(tc["bionic"], "cdh_crtend_android.S")
    startup_objs = [os.path.join(WORK_DIR, "cdh_crtbegin_dynamic.o"),
                    os.path.join(WORK_DIR, "cdh_crtend_android.o")]
    if not dry_run:
        for s, o in ((crtbegin, startup_objs[0]), (crtend, startup_objs[1])):
            run([tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                 "-nostdlib", "-fno-stack-protector", "-mno-outline-atomics",
                 "-mbranch-protection=standard", "-fPIE", "-c", s, "-o", o])

    print("==> 编译假登记 store 源")
    store_dir = os.path.join(WORK_DIR, "store")
    os.makedirs(store_dir, exist_ok=True)
    store_objs = []
    store_srcs = [os.path.join(SRC_DIR, "cdh_codec2_store_service.cpp")]
    store_srcs += gen_cpps
    for i, s in enumerate(store_srcs):
        obj = os.path.join(store_dir, f"store{i}.o")
        argv = [tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                "-std=c++17", "-fPIC", "-fno-rtti", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-missing-braces",
                "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
                "-I" + WORK_DIR, "-I" + MODULE_DIR,
                "-nostdinc++", "-I" + tc["libcxx_config"],
                "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
                "-c", s, "-o", obj]
        run(argv, dry_run=dry_run)
        store_objs.append(obj)

    print("==> 链接假登记 store 服务")
    builtins = os.path.join(tc["clang_rt_dir"], "lib", "linux",
                            "libclang_rt.builtins-aarch64-android.a")
    store_outs = []
    for binary in STORE_BINARIES:
        store_out = os.path.join(VENDOR_STAGE_DIR, "bin", "hw", binary)
        store_outs.append(store_out)
        if dry_run:
            print(f"[..] (dry) 链接 {binary}")
            continue
        argv = [tc["clang"], "--target=" + TARGET, "--sysroot=" + tc["sysroot"],
                "--ld-path=" + tc["lld"], "-nostdlib", "-fno-stack-protector",
                "-mno-outline-atomics", "-mbranch-protection=standard",
                "-Wl,--dynamic-linker,/system/bin/linker64", "-Wl,-z,relro",
                "-Wl,-z,now", "-std=c++17", "-fPIE", "-pie",
                "-I" + gen_hdr, "-I" + tc["binder"], "-I" + tc["binder_platform"],
                "-I" + WORK_DIR, "-I" + MODULE_DIR,
                "-nostdinc++", "-I" + tc["libcxx_config"],
                "-isystem", os.path.join(tc["sysroot"], "usr", "include", "c++", "v1"),
                crtbegin] + store_objs + [crtend,
                "-L" + abi_sys] + [os.path.join(abi_sys, lib) for lib in STORE_LIBS]
        argv += STORE_EXTRA + [builtins, "-o", store_out]
        run(argv)
        os.chmod(store_out, 0o755)
        print(f"    写 {os.path.relpath(store_out, VENDOR_STAGE_DIR)}")

    print("==> vendor 静态资产（init/vintf/media_codecs，对齐真机 /vendor/etc）")
    for rel in VENDOR_ASSETS:
        write_asset(os.path.join("vendor", "etc", rel),
                    os.path.join(VENDOR_STAGE_DIR, "etc", rel), dry_run=dry_run)

    print("[ok] media (codec2 store) 构建完成")
    for store_out in store_outs:
        print(f"    {os.path.relpath(store_out, _DEVICE_ROOT)}")
    register_labels(dry_run=dry_run)
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 Codec2 假登记 store 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
