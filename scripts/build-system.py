"""Pixel6-15 system 阶段薄壳：转发到 system/build/main.py。

system 的全部逻辑（属性补丁、文件替换 R1–R20、文本替换 T 表、本地/生成文件
注入、删除清单与 debugfs 重打包）已模块化下沉到 system/build/（参考 vendor/build
分层：paths/props/textsubs/replacements/localfiles/generated 规则数据表 +
debugfs 工具层）。本脚本只负责转发参数并透传退出码。

用法（可在任何目录）：
  python3 build/build-system.py                # 默认输出 out/dist/rom/system.img
  python3 build/build-system.py --out <path>   # 指定输出
  python3 build/build-system.py --base <path>  # 指定官方 system.img 基座
"""

import os
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
PIXEL6 = os.path.dirname(os.path.dirname(_HERE))
SYSTEM_BUILD = os.path.join(PIXEL6, "system", "build")


def main():
    entry = os.path.join(SYSTEM_BUILD, "main.py")
    if not os.path.isfile(entry):
        print(f"[err] 缺少 system 构建驱动: {entry}")
        return 1

    cmd = [sys.executable, entry] + sys.argv[1:]
    print(f"==> 调用 {SYSTEM_BUILD}")
    rc = subprocess.run(cmd)
    return rc.returncode


if __name__ == "__main__":
    sys.exit(main())
