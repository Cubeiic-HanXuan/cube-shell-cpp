"""从 venv 的 pyqtdarktheme-fork 导出真实 QSS + SVG 图标，供 C++ 版编进二进制。

用法（需先激活含 pyqtdarktheme-fork 的 venv）:
    QT_QPA_PLATFORM=offscreen \
        /Users/hanxuan/Python/cube-shell/venv/bin/python export_from_venv.py

机制说明:
    qdarktheme.load_stylesheet() 通过模板引擎渲染 QSS，其中 url 过滤器会把
    按主题上色后的 SVG 写到 ~/.cache/qdarktheme/v<ver>/{id}_{hex}_{rotate}.svg
    并在 QSS 里以绝对路径引用（无法直接移植）。本脚本：
      1. 在 QApplication（offscreen）存在的前提下调用 load_stylesheet，
         与 Python 版 cube-shell.py::setDarkTheme/setLightTheme 的运行时输出
         逐字一致（含 standard icons 附加段）；
      2. 把 QSS 引用到的全部 SVG 拷到本目录 svg/ 下（文件名含颜色 hex，
         dark/light 互不冲突）；
      3. 把 url(<绝对路径>) 改写为 url(":/qdarktheme/svg/<名字>.svg")；
      4. 生成 dark.qss / light.qss 与 qdarktheme.qrc（prefix "/qdarktheme"）。
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication  # noqa: E402

app = QApplication(sys.argv)  # 必须先建 app，qdarktheme 才会附加 standard icons 段

import qdarktheme  # noqa: E402

OUT_DIR = pathlib.Path(__file__).resolve().parent
SVG_DIR = OUT_DIR / "svg"
SVG_DIR.mkdir(parents=True, exist_ok=True)

# 与 cube-shell.py::setDarkTheme / setLightTheme 完全相同的调用参数
dark_qss = qdarktheme.load_stylesheet(custom_colors={"[dark]": {"primary": "#00A1FF"}})
light_qss = qdarktheme.load_stylesheet(
    theme="light", custom_colors={"[light]": {"primary": "#E05B00"}}
)

URL_RE = re.compile(r"url\(([^)\"']+\.svg)\)")
copied: set[str] = set()


def rewrite(qss: str) -> str:
    def repl(match: re.Match[str]) -> str:
        src = pathlib.Path(match.group(1))
        if not src.is_file():
            raise FileNotFoundError(f"svg referenced by qss not found: {src}")
        shutil.copyfile(src, SVG_DIR / src.name)
        copied.add(src.name)
        return f'url(":/qdarktheme/svg/{src.name}")'

    return URL_RE.sub(repl, qss)


(OUT_DIR / "dark.qss").write_text(rewrite(dark_qss), encoding="utf-8")
(OUT_DIR / "light.qss").write_text(rewrite(light_qss), encoding="utf-8")

entries = ["dark.qss", "light.qss"] + sorted(f"svg/{name}" for name in copied)
qrc_body = "".join(f"    <file>{entry}</file>\n" for entry in entries)
(OUT_DIR / "qdarktheme.qrc").write_text(
    '<!DOCTYPE RCC>\n<RCC version="1.0">\n'
    '  <qresource prefix="/qdarktheme">\n'
    f"{qrc_body}"
    "  </qresource>\n</RCC>\n",
    encoding="utf-8",
)

print(f"dark.qss:  {len(dark_qss)} chars")
print(f"light.qss: {len(light_qss)} chars")
print(f"svg icons: {len(copied)}")
leftover = [m for m in URL_RE.findall((OUT_DIR / 'dark.qss').read_text())]
print(f"un-rewritten urls: {leftover or 'none'}")
