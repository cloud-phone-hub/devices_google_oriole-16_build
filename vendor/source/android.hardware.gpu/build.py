#!/usr/bin/env python3
"""note13-5G-15 GPU 身份 interposer 构建（路线 B：自写库顶替 API 面，不改基座二进制）。

真机事实（config/gpu.json 的 source-of-truth，逐字取自 gold 的
`dumpsys SurfaceFlinger` GLES 行）：

    GLES: ARM, Mali-G57 MC2, OpenGL ES 3.2 v1.r49p1-03bet0.569c83bf81f68e2ce8910542c6846b7e

基座是 lineage-22.0 virtio + Mesa 24.3.4（virgl/venus），上报 Mesa / virgl (<host>)。
pixel6-15 用的 `vendor.mesa.force.gl.*` 属性钩子在本基座 **不存在**（strings 零命中），
所以这里走另一条路：产出与真机同后缀的库，把基座库包一层。

产物（相对路径即镜像内 /vendor 路径，落 out/dist/vendor 供 inject.py 注入）
--------------------------------------------------------------------------
  lib64/egl/libEGL_meow.so           EGL 入口全量转发
  lib64/egl/libGLESv1_CM_meow.so     同上 + 覆写 glGetString
  lib64/egl/libGLESv2_meow.so        同上 + 覆写 glGetString
  lib64/hw/vulkan.mali.so            venus 转发 + 覆写 deviceName

顶替是怎么成立的
----------------
* EGL：Loader.cpp（frameworks/native/opengl/libs/EGL）先试 libGLES_<suffix>.so 单库
  全导出，找不到才退回 libEGL_/libGLESv1_CM_/libGLESv2_<suffix>.so 三件套，并用
  dlsym 从各自 dso 逐个取入口填进 cnx->hooks[]。本模块只产出三件套 → 必然走退回
  路径，与基座 mesa 三件一一对应。App 经 eglGetProcAddress 拿到的也是 hooks 里的
  指针，因此**不需要**自己覆写 eglGetProcAddress。
* Vulkan：libvulkan.so dlopen vulkan.<ro.hardware.vulkan>.so → dlsym "HMI" →
  methods->open(module,"vk0") → hwvulkan_device_t 的三个函数指针。只包装
  GetInstanceProcAddr 即可覆盖全部入口。

转发 stub 怎么来的（不手写 550 个函数签名）
------------------------------------------
GL/EGL 入口签名各异，逐个写 C 包装既写不全也易错。做法是从基座库的动态符号表
（llvm-nm -D --defined-only，只取 T/W）读出**它导出的全部函数名**，生成一段汇编：
每个符号一个 4 指令蹦床，从 cdh_real_table[] 取真地址后 `br x16` 尾调用——
不动 x0-x7/v0-v7/栈，因此对任意签名都成立。构造函数里 dlopen 基座库并 dlsym 逐个
填表。转发表 = 基座导出集，所以 loader 原来能 dlsym 到的，现在照样能。

要覆写的符号（glGetString）从转发表里剔除，改由 C 实现，避免重复定义。

用法
----
  python3 build/vendor/source/android.hardware.gpu/build.py
  python3 build/vendor/source/android.hardware.gpu/build.py --dry-run
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile

# 本文件位于 build/vendor/source/android.hardware.gpu/ 下，向上 5 级为设备根
_MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
_DEVICE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(_MODULE_DIR))))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "scripts"))
sys.path.insert(0, os.path.join(_DEVICE_ROOT, "build", "vendor", "build"))

from buildenv import (  # noqa: E402
    LINEAGE_SUPER_DIR, OUT_DIR, ROM_ROOT_DIR, VENDOR_STAGE_DIR, baselines_file,
)
from debugfs import DEBUGFS  # noqa: E402

CONFIG_PATH = os.path.join(_MODULE_DIR, "config", "gpu.json")
SRC_DIR = os.path.join(_MODULE_DIR, "src")
WORK_DIR = os.path.join(OUT_DIR, "tmp", "gpu-interposer")
GEN_DIR = os.path.join(WORK_DIR, "gen")
BASE_DIR = os.path.join(WORK_DIR, "base")

VENDOR_BASE = os.path.join(LINEAGE_SUPER_DIR, "vendor_a.img")
TARGET_TRIPLE = "aarch64-linux-android35"
# 产物模式：.so 不带可执行位（inject.py 的 STAGE_FILE 名单需同步登记）
OUT_MODE = 0o644

_C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _fail(msg):
    print(f"[err] {msg}", file=sys.stderr)
    sys.exit(1)


def env(name, default):
    return os.environ.get(name) or default


def toolchain():
    """NDK clang（与其它 source 子模块同一套 env 覆盖约定）。"""
    ndk = env("CDH_NDK", os.path.expanduser(
        "~/Library/Android/sdk/ndk/29.0.14206865"))
    prebuilt = os.path.join(ndk, "toolchains", "llvm", "prebuilt",
                            "darwin-x86_64")
    clang = env("CDH_CLANG", os.path.join(prebuilt, "bin", "clang"))
    nm = env("CDH_NM", os.path.join(prebuilt, "bin", "llvm-nm"))
    for tool in (clang, nm):
        if not os.path.isfile(tool):
            _fail(f"工具链缺失: {tool}（设 CDH_NDK 指向可用 NDK）")
    return {"clang": clang, "nm": nm,
            "sysroot": os.path.join(prebuilt, "sysroot")}


def extract(image_path, host_path):
    """从基座 vendor_a.img 抽出原始库（debugfs dump，逐字节原样）。"""
    os.makedirs(os.path.dirname(host_path), exist_ok=True)
    if os.path.exists(host_path):
        os.unlink(host_path)
    res = subprocess.run([DEBUGFS, "-R", f"dump {image_path} {host_path}",
                          VENDOR_BASE], capture_output=True, text=True)
    if res.returncode != 0 or not os.path.isfile(host_path):
        _fail(f"抽取失败 {image_path}: {res.stderr.strip()[:200]}")
    return host_path


def exported_functions(nm, lib_path):
    """读基座库导出的函数符号（动态符号表里的 T/W），按名字排序去重。"""
    res = subprocess.run([nm, "-D", "--defined-only", lib_path],
                         capture_output=True, text=True)
    if res.returncode != 0:
        _fail(f"llvm-nm 失败 {lib_path}: {res.stderr.strip()[:200]}")
    names = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "W"):
            names.append(parts[2])
    names = sorted(set(names))
    bad = [n for n in names if not _C_IDENT.match(n)]
    if bad:
        _fail(f"{os.path.basename(lib_path)} 含非法符号名（无法做汇编标签）: {bad[:5]}")
    if not names:
        _fail(f"{os.path.basename(lib_path)} 没有导出任何函数符号")
    return names


# ---------------- 代码生成 ----------------

def gen_identity_inc(targets, vk, strip_prefixes, path):
    """生成真机身份常量头（唯一事实源是 config/gpu.json）。"""
    lines = [
        "// Generated from config/gpu.json. Do not edit by hand.",
        "// 真机 gold 实测值（device-checker hardware.gles），逐字照抄，禁止凭记忆改动。",
        "#ifndef CDH_GPU_TARGETS_INC",
        "#define CDH_GPU_TARGETS_INC",
        "",
    ]
    mapping = [
        ("CDH_ID_GL_VENDOR", targets["gl_vendor"]),
        ("CDH_ID_GL_RENDERER", targets["gl_renderer"]),
        ("CDH_ID_GL_VERSION", targets["gl_version"]),
        # 注意：GL_SHADING_LANGUAGE_VERSION 不在这里。它必须留 mesa 真值——
        # Skia 用它决定 shader 前缀版本，覆写成真机的 3.20 会让 mesa 编译器拒收
        # #version 320 es，全部 shader 编译失败、App 内容区空白。见 cdh_gl_strings.c。
        # 扩展过滤前缀，逗号分隔；运行时按前缀匹配后剔除（只删不加）
        ("CDH_ID_GL_EXT_STRIP", ",".join(strip_prefixes)),
        ("CDH_ID_VK_DEVICE_NAME", targets["vulkan_device_name"]),
        ("CDH_ID_VK_BASE_SONAME", vk["base-soname"]),
        ("CDH_ID_VK_BASE_PATH", "/vendor" + vk["base-image-path"]),
    ]
    for macro, value in mapping:
        esc = str(value).replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'#define {macro} "{esc}"')
    lines += ["", "#endif  // CDH_GPU_TARGETS_INC", ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def gen_stub_asm(tag, names, path):
    """每个符号一个蹦床：从 cdh_real_table[idx] 取真地址后尾调用。

    只用 x16（IP0，不参与参数传递），不动 x0-x7/v0-v7/栈，故对任意函数签名成立。
    """
    lines = [
        "/* Generated from base driver's dynamic symbol table. Do not edit. */",
        "\t.text",
        "",
        "\t.macro\tCDH_STUB name, off",
        "\t.globl\t\\name",
        "\t.type\t\\name, %function",
        "\t.p2align\t2",
        "\\name:",
        "\tadrp\tx16, cdh_real_table",
        "\tadd\tx16, x16, :lo12:cdh_real_table",
        "\tldr\tx16, [x16, #\\off]",
        "\tbr\tx16",
        "\t.size\t\\name, .-\\name",
        "\t.endm",
        "",
    ]
    for i, name in enumerate(names):
        lines.append(f"\tCDH_STUB\t{name}, {i * 8}")
    lines += ["", '\t.section\t.note.GNU-stack,"",@progbits', ""]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def gen_table_c(tag, names, overrides, path):
    """生成转发表 + 构造函数（dlopen 基座库并 dlsym 填表）。"""
    lines = [
        "// Generated from base driver's dynamic symbol table. Do not edit by hand.",
        "#include <dlfcn.h>",
        "",
        "#include <android/log.h>",
        "",
        "#define CDH_LOG_TAG \"cdh-gpu\"",
        f"#define CDH_N {len(names)}",
        f"#define CDH_NOVR {len(overrides)}",
        "",
        "__attribute__((visibility(\"hidden\")))",
        "void *cdh_real_table[CDH_N];",
        "",
        "static const char *const cdh_names[CDH_N] = {",
    ]
    for i in range(0, len(names), 4):
        chunk = ", ".join(f'"{n}"' for n in names[i:i + 4])
        lines.append(f"    {chunk},")
    lines += ["};", ""]
    for name in overrides:
        # 定义在 cdh_gl_strings.c（hidden），这里只负责把基座真实现填进去
        lines.append(f"extern void *cdh_ovr_{name};")
    if overrides:
        lines += [
            "",
            "static struct { const char *name; void **slot; } cdh_ovr[] = {",
        ]
        for name in overrides:
            lines.append(f'    {{ "{name}", &cdh_ovr_{name} }},')
        lines += ["};", ""]
    lines += [
        "__attribute__((constructor)) static void cdh_init(void) {",
        "    void *h = dlopen(CDH_BASE_PATH, RTLD_NOW | RTLD_LOCAL);",
        "    if (!h) {",
        "        h = dlopen(CDH_BASE_SONAME, RTLD_NOW | RTLD_LOCAL);",
        "    }",
        "    if (!h) {",
        "        __android_log_print(ANDROID_LOG_ERROR, CDH_LOG_TAG,",
        f'                            "{tag}: dlopen 基座库失败: %s",',
        "                            dlerror() ? dlerror() : \"?\");",
        "        return;",
        "    }",
        "    int miss = 0;",
        "    for (int i = 0; i < CDH_N; i++) {",
        "        cdh_real_table[i] = dlsym(h, cdh_names[i]);",
        "        if (!cdh_real_table[i]) miss++;",
        "    }",
        "#if CDH_NOVR > 0",
        "    for (int i = 0; i < CDH_NOVR; i++) {",
        "        *cdh_ovr[i].slot = dlsym(h, cdh_ovr[i].name);",
        "        if (!*cdh_ovr[i].slot) miss++;",
        "    }",
        "#endif",
        "    if (miss) {",
        "        __android_log_print(ANDROID_LOG_ERROR, CDH_LOG_TAG,",
        f'                            "{tag}: %d/%d 个符号未解析（基座库变了？）",',
        "                            miss, CDH_N + CDH_NOVR);",
        "    } else {",
        "        __android_log_print(ANDROID_LOG_INFO, CDH_LOG_TAG,",
        f'                            "{tag}: %d 个入口已接管", CDH_N + CDH_NOVR);',
        "    }",
        "}",
        "",
    ]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


# ---------------- 编译 ----------------

def compile_so(tc, sources, out_path, defines, dry_run=False):
    argv = [
        tc["clang"], "--target=" + TARGET_TRIPLE, "--sysroot=" + tc["sysroot"],
        "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
        "-Wno-unused-parameter", "-Wno-unused-function",
        "-fno-stack-protector", "-Wl,-z,relro", "-Wl,-z,now",
        "-I" + GEN_DIR, "-I" + SRC_DIR,
    ]
    argv += [f"-D{d}" for d in defines]
    argv += sources
    argv += ["-llog", "-ldl", "-o", out_path]
    if dry_run:
        print("    (dry)", " ".join(os.path.basename(a) if a.endswith((".c", ".S"))
                                    else a for a in argv))
        return
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    res = subprocess.run(argv, capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout, file=sys.stderr)
        print(res.stderr, file=sys.stderr)
        _fail(f"编译失败: {os.path.basename(out_path)}")


def stage(out_path, stage_rel, dry_run=False):
    dst = os.path.join(VENDOR_STAGE_DIR, stage_rel.replace("/", os.sep))
    if dry_run:
        print(f"    (dry) 落盘 {dst}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(out_path, "rb") as f:
        data = f.read()
    with open(dst, "wb") as f:
        f.write(data)
    os.chmod(dst, OUT_MODE)
    print(f"    写 {os.path.relpath(dst, VENDOR_STAGE_DIR)}（{len(data)} B，0644）")


# ---------------- baselines 登记 ----------------

def _atomic_write_json(path, doc):
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path),
                               prefix=".baselines.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def register_baselines(labels, properties, dry_run=False):
    """登记 SELinux 标签与属性（幂等）。返回是否有改动。"""
    path = baselines_file()
    if not path or not os.path.isfile(path):
        _fail(f"基线清单缺失: {path or 'None'}")
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    changed = False

    lab = doc.setdefault("vendor", {}).setdefault("selinux-labels", {})
    miss = {k: v for k, v in labels.items() if lab.get(k) != v}
    if miss:
        lab.update(miss)
        changed = True
        print(f"[{'..' if dry_run else 'ok'}] baselines 标签 "
              f"{'将登记' if dry_run else '登记'} {len(miss)} 条: {sorted(miss)}")
    else:
        print("[..] baselines 标签已齐，跳过")

    sets = doc.setdefault("vendor", {}).setdefault("build-prop-sets", [])
    index = {p[0]: p for p in sets if isinstance(p, list) and len(p) == 2}
    for key, value in properties.items():
        pair = index.get(key)
        if pair is None:
            sets.append([key, value])
            changed = True
            print(f"[{'..' if dry_run else 'ok'}] baselines 追加 {key}={value}")
        elif pair[1] != value:
            old, pair[1] = pair[1], value
            changed = True
            print(f"[{'..' if dry_run else 'ok'}] baselines 改 {key}: "
                  f"{old!r} -> {value!r}")
        else:
            print(f"[..] baselines 已含 {key}={value}，跳过")

    if changed and not dry_run:
        _atomic_write_json(path, doc)
    return changed


# ---------------- 主流程 ----------------

def stage_scan_libs(spec, label, dry_run=False):
    """从官方 gold vendor 逐字节回填真机 GPU 库（文件扫描面对齐）。

    每件事前按契约校验 length + sha256，不符即失败退出——回填件是官方固件里的
    真驱动，一旦被换版/损坏而静默进镜像，排查成本远高于构建期报错。
    """
    if not spec or not spec.get("files"):
        return {}
    root = os.path.join(ROM_ROOT_DIR, spec.get("source-root", "vendor_a"))
    if not os.path.isdir(root):
        _fail(f"官方 vendor 解包目录缺失: {root}（先跑 unpack 阶段）")
    print("==> 真机 GPU 库回填（文件扫描面，不参与渲染）")
    labels = {}
    total = 0
    for item in spec["files"]:
        rel = item["stage-path"]
        labels[rel] = label
        if dry_run:
            print(f"    (dry) 回填 {rel}")
            continue
        src = os.path.join(root, item["src"].replace("/", os.sep))
        if not os.path.isfile(src):
            _fail(f"官方库缺失: {src}")
        with open(src, "rb") as f:
            data = f.read()
        want_len = item.get("length")
        if want_len is not None and len(data) != want_len:
            _fail(f"{item['src']} 长度 {len(data)} != 契约 {want_len}")
        want_sha = item.get("sha256")
        if want_sha:
            got = hashlib.sha256(data).hexdigest()
            if got != want_sha:
                _fail(f"{item['src']} sha256 不符（官方固件换版了？）\n"
                      f"      期望 {want_sha}\n      实得 {got}")
        dst = os.path.join(VENDOR_STAGE_DIR, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(data)
        os.chmod(dst, OUT_MODE)
        total += len(data)
        print(f"    回填 {rel}（{len(data)} B，sha256 已校验）")
    if not dry_run:
        print(f"    合计 {total / 1048576:.1f} MiB")
    return labels


def build(dry_run=False):
    with open(CONFIG_PATH, encoding="utf-8") as f:
        doc = json.load(f)
    targets = doc["targets"]
    label = doc["label"]
    vk = doc["vulkan"]

    print("==> 真机事实值（config/gpu.json targets）")
    for k in ("gl_vendor", "gl_renderer", "gl_version", "vulkan_device_name"):
        print(f"    {k:28s} = {targets[k]!r}")
    print(f"    {'gl_extensions 过滤前缀':24s} = "
          f"{doc.get('gl_extensions_strip_prefixes')}")
    print("    gl_shading_language_version  = <留 mesa 真值，不覆写：覆写会致 App 空白>")

    if not os.path.isfile(VENDOR_BASE):
        _fail(f"基座 vendor_a.img 缺失: {VENDOR_BASE}（先跑 unpack 阶段）")
    tc = toolchain()
    for d in (WORK_DIR, GEN_DIR, BASE_DIR):
        os.makedirs(d, exist_ok=True)

    print("==> 生成身份常量头")
    strip = doc.get("gl_extensions_strip_prefixes", [])
    gen_identity_inc(targets, vk, strip,
                     os.path.join(GEN_DIR, "cdh_gpu_targets.inc"))
    if not dry_run:
        print(f"    写 gen/cdh_gpu_targets.inc（扩展过滤前缀 {strip}）")

    labels = {}

    print("==> GL/EGL interposer（三件套）")
    for spec in doc["interposers"]:
        tag = spec["tag"]
        base_host = os.path.join(BASE_DIR, os.path.basename(spec["base-image-path"]))
        if dry_run:
            print(f"    (dry) {tag}: 抽 {spec['base-image-path']} → 生成 stub/table → 编 {spec['stage-path']}")
            labels[spec["stage-path"]] = label
            continue
        extract(spec["base-image-path"], base_host)
        names = exported_functions(tc["nm"], base_host)
        overrides = [o for o in spec.get("overrides", []) if o in names]
        lost = [o for o in spec.get("overrides", []) if o not in names]
        if lost:
            _fail(f"{tag}: 覆写目标不在基座导出集里（基座变了？）: {lost}")
        forwarded = [n for n in names if n not in overrides]
        print(f"    {tag}: 基座导出 {len(names)} 个 → 转发 {len(forwarded)} + "
              f"覆写 {len(overrides)}{overrides if overrides else ''}")

        gen_stub_asm(tag, forwarded, os.path.join(GEN_DIR, f"stub_{tag}.S"))
        gen_table_c(tag, forwarded, overrides,
                    os.path.join(GEN_DIR, f"table_{tag}.c"))

        sources = [os.path.join(GEN_DIR, f"table_{tag}.c"),
                   os.path.join(GEN_DIR, f"stub_{tag}.S")]
        if overrides:
            sources.insert(0, os.path.join(SRC_DIR, "cdh_gl_strings.c"))
        out_so = os.path.join(WORK_DIR, os.path.basename(spec["stage-path"]))
        defines = [
            f'CDH_TAG="{tag}"',
            f'CDH_BASE_SONAME="{spec["base-soname"]}"',
            f'CDH_BASE_PATH="/vendor{spec["base-image-path"]}"',
        ]
        # glGetStringi 只存在于 GLESv2/v3；ES1.1 库里定义它会多导出一个基座没有的
        # 符号，破坏与基座的一一对应，故按 overrides 逐库决定。
        if "glGetStringi" in overrides:
            defines.append("CDH_HAS_GLGETSTRINGI=1")
        compile_so(tc, sources, out_so, defines, dry_run=dry_run)
        stage(out_so, spec["stage-path"], dry_run=dry_run)
        labels[spec["stage-path"]] = label

    print("==> Vulkan HAL interposer")
    if dry_run:
        print(f"    (dry) 编 {vk['stage-path']}（转发 {vk['base-soname']}）")
        labels[vk["stage-path"]] = label
    else:
        out_so = os.path.join(WORK_DIR, os.path.basename(vk["stage-path"]))
        compile_so(tc, [os.path.join(SRC_DIR, "cdh_vulkan_mali.c")], out_so,
                   [], dry_run=dry_run)
        stage(out_so, vk["stage-path"], dry_run=dry_run)
        labels[vk["stage-path"]] = label

    labels.update(stage_scan_libs(doc.get("scan-libs"), label, dry_run=dry_run))

    print("==> SELinux 标签 / 属性登记")
    register_baselines(labels, doc.get("properties", {}), dry_run=dry_run)

    print(f"[ok] GPU interposer 构建完成：{len(labels)} 个库")
    return 0


def main():
    ap = argparse.ArgumentParser(description="note13-5G-15 GPU 身份 interposer 构建")
    ap.add_argument("--dry-run", action="store_true", help="只打印不执行")
    args = ap.parse_args()
    sys.exit(build(dry_run=args.dry_run))


if __name__ == "__main__":
    main()
