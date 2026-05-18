#!/usr/bin/env python3
"""从 data/config.html 生成 UTF-8 的 src/config_page.h"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
html_path = ROOT / "data" / "config.html"
out_path = ROOT / "src" / "config_page.h"

html = html_path.read_text(encoding="utf-8")
if "CONFIGPAGE" in html:
    raise SystemExit("HTML 中不能包含 CONFIGPAGE 分隔符")

out_path.write_text(
    '#pragma once\n'
    'static const char CONFIG_PAGE[] PROGMEM = R"CONFIGPAGE(\n'
    + html
    + '\n)CONFIGPAGE";\n',
    encoding="utf-8",
)
print(f"Wrote {out_path} ({len(html)} bytes UTF-8)")
