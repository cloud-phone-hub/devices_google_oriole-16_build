// note13-5G-15 GL 身份覆写：接管 glGetString 的四个「身份」枚举 + 过滤扩展串，
// 其余枚举与全部其它 GL 入口一律原样转发给基座 mesa 驱动（转发表见 table_<tag>.c）。
//
// 覆写项的真机值全部逐字取自 device-checker hardware.gles 在 gold 上的实测返回
// （run-00112/00113），见 config/gpu.json 的 source-of-truth.captured-values。
//
// GL_SHADING_LANGUAGE_VERSION **故意不覆写**（曾经覆写过，是事故）：
// Skia 用它决定 shader 前缀版本。真机返回 "OpenGL ES GLSL ES 3.20"，覆写后 Skia 会
// 发 `#version 320 es`，而基座 mesa 的编译器实际只支持到 3.00（宿主 Apple GL 4.1
// 封顶）→ 全部 shader 编译失败 → App 内容区整片空白（Settings 复现，logcat 有
// "GLSL ES 3.20 is not supported. Supported versions are: 1.00 ES, and 3.00 ES"）。
// 真机能真编译 3.20，我们不能，所以这个字段必须留 mesa 真值。
// 代价：version 声称 3.2 而 GLSL 报 3.00 的内部矛盾保留——两害相权，空白界面
// 远比一个字符串矛盾严重。已记入 config/gpu.json 的 not-aligned。
//
// 为什么不需要覆写 eglGetProcAddress：Android 系统侧 libEGL.so 的 eglGetProcAddress
// 返回的是 cnx->hooks[] 里的指针，而那张表由 Loader.cpp 的 init_api 用
// dlsym(本库, "glGetString") 填充 —— 拿到的已经是本文件的实现。glGetStringi 同理。

#include <stddef.h>
#include <string.h>

#include "cdh_gpu_targets.inc"

// mesa 的真实现，由 table_<tag>.c 的构造函数 dlsym 后写入
void *cdh_ovr_glGetString __attribute__((visibility("hidden")));
void *cdh_ovr_glGetStringi __attribute__((visibility("hidden")));
void *cdh_ovr_glGetIntegerv __attribute__((visibility("hidden")));

#define CDH_GL_VENDOR 0x1F00
#define CDH_GL_RENDERER 0x1F01
#define CDH_GL_VERSION 0x1F02
#define CDH_GL_EXTENSIONS 0x1F03
#define CDH_GL_NUM_EXTENSIONS 0x821D

typedef const unsigned char *(*cdh_glGetString_t)(unsigned int);
typedef const unsigned char *(*cdh_glGetStringi_t)(unsigned int, unsigned int);
typedef void (*cdh_glGetIntegerv_t)(unsigned int, int *);

// ---------------------------------------------------------------------------
// 扩展串过滤：只删不加
//
// 为什么必须删：基座 mesa/virgl 的扩展串里带着直指宿主与驱动的条目——
//   GL_APPLE_sync / GL_APPLE_texture_max_level   → 宿主是 macOS
//   GL_MESA_*                                    → 驱动是 Mesa
//   GL_ANGLE_*                                   → 走了 ANGLE
// 这些在真机 Mali 上都不存在，App 一读就能定位到虚拟化 + 宿主操作系统。
//
// 为什么不能加：真机独有的 57 项里含 GL_EXT_geometry_shader /
// GL_EXT_tessellation_shader / GL_ANDROID_extension_pack_es31a 等，云机 virgl
// 实际不具备（宿主 Apple GL 4.1 封顶，AEP 14 项只命中 2 项）。谎报后 App 会
// 真去调这些入口 → 编译/绘制失败甚至崩溃。少报扩展最多让 App 走保守分支，
// 多报则直接崩，所以只做减法。
//
// 减法本身是安全的：被删的都是 Apple/Mesa/ANGLE 私有或桌面 GL 遗留扩展，
// Android App 不会依赖它们做功能判定。
// ---------------------------------------------------------------------------

#define CDH_EXT_MAX 512
#define CDH_EXT_BUF 16384

static char cdh_ext_joined[CDH_EXT_BUF];
static const char *cdh_ext_list[CDH_EXT_MAX];
static unsigned int cdh_ext_count;
static int cdh_ext_ready;

// 前缀名单来自 config/gpu.json，构建期生成成逗号分隔串
static const char cdh_ext_strip[] = CDH_ID_GL_EXT_STRIP;

static int cdh_stripped(const char *name, size_t len) {
    const char *p = cdh_ext_strip;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        if (n > 0 && n <= len && memcmp(name, p, n) == 0) {
            return 1;
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return 0;
}

// 惰性构建过滤后的扩展表；只在首次查询时做一次，之后走缓存。
// 需要 cdh_ext_lock 之类的同步吗？—— glGetString 可能在多线程被调，但构建过程
// 是幂等的（同样的输入得同样的结果），最坏情况是重复构建一次，不会读到半成品：
// cdh_ext_ready 在所有字段填完后才置 1。
static void cdh_build_ext(void) {
    if (cdh_ext_ready) {
        return;
    }
    const char *src = NULL;
    if (cdh_ovr_glGetString) {
        src = (const char *)((cdh_glGetString_t)cdh_ovr_glGetString)(CDH_GL_EXTENSIONS);
    }
    if (!src) {
        cdh_ext_ready = 1;
        return;
    }
    size_t used = 0;
    unsigned int count = 0;
    const char *p = src;
    while (*p && count < CDH_EXT_MAX && used + 1 < CDH_EXT_BUF) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (cdh_stripped(start, len)) {
            continue;
        }
        if (used + len + 2 >= CDH_EXT_BUF) {
            break;
        }
        if (used) {
            cdh_ext_joined[used++] = ' ';
        }
        memcpy(cdh_ext_joined + used, start, len);
        used += len;
        cdh_ext_joined[used] = '\0';
        cdh_ext_list[count++] = cdh_ext_joined + used - len;
    }
    cdh_ext_joined[used] = '\0';
    cdh_ext_count = count;
    cdh_ext_ready = 1;
}

__attribute__((visibility("default")))
const unsigned char *glGetString(unsigned int name) {
    switch (name) {
    case CDH_GL_VENDOR:
        return (const unsigned char *)CDH_ID_GL_VENDOR;
    case CDH_GL_RENDERER:
        return (const unsigned char *)CDH_ID_GL_RENDERER;
    case CDH_GL_VERSION:
        return (const unsigned char *)CDH_ID_GL_VERSION;
    case CDH_GL_EXTENSIONS:
        cdh_build_ext();
        if (cdh_ext_count) {
            return (const unsigned char *)cdh_ext_joined;
        }
        break;   // 没拿到 mesa 的串就退回转发，别返回空串
    default:
        break;
    }
    if (cdh_ovr_glGetString) {
        return ((cdh_glGetString_t)cdh_ovr_glGetString)(name);
    }
    return NULL;
}

// 过滤扩展串后，GL_NUM_EXTENSIONS 必须同步返回过滤后的条数，否则 Skia 按 mesa 的
// 原始条数（101）去 glGetStringi 逐条取，索引 92..100 会拿到 NULL。
__attribute__((visibility("default")))
void glGetIntegerv(unsigned int pname, int *params) {
    if (pname == CDH_GL_NUM_EXTENSIONS) {
        cdh_build_ext();
        if (params) {
            *params = (int)cdh_ext_count;
        }
        return;
    }
    if (cdh_ovr_glGetIntegerv) {
        ((cdh_glGetIntegerv_t)cdh_ovr_glGetIntegerv)(pname, params);
    }
}

// ES 3.x 上下文用 glGetStringi(GL_EXTENSIONS, i) 逐条取；必须与 glGetString
// 返回同一套过滤结果，否则两条路径自相矛盾。
// 该入口只存在于 GLESv2/v3 —— GLES1_CM 没有它，mesa 的 libGLESv1_CM 也不导出，
// 所以由 build.py 按各库的 overrides 决定是否定义 CDH_HAS_GLGETSTRINGI，
// 避免从 ES1.1 库里多导出一个不该有的符号（破坏与基座的符号一一对应）。
#ifdef CDH_HAS_GLGETSTRINGI
__attribute__((visibility("default")))
const unsigned char *glGetStringi(unsigned int name, unsigned int index) {
    if (name == CDH_GL_EXTENSIONS) {
        cdh_build_ext();
        if (index < cdh_ext_count) {
            return (const unsigned char *)cdh_ext_list[index];
        }
        return NULL;
    }
    if (cdh_ovr_glGetStringi) {
        return ((cdh_glGetStringi_t)cdh_ovr_glGetStringi)(name, index);
    }
    return NULL;
}
#endif
