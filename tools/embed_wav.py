#!/usr/bin/env python3
"""从 data/*.wav 提取 16-bit PCM，生成 src/game_sounds.h"""
from __future__ import annotations

import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"
OUT_CPP = ROOT / "src" / "game_sounds.cpp"
OUT_H = ROOT / "src" / "game_sounds.h"

SOUNDS = (
    ("kSoundMenu", "menu.wav"),
    ("kSoundBegin", "begin.wav"),
    ("kSoundScore", "score.wav"),
    ("kSoundShoot", "shoot.wav"),
    ("kSoundUpgrade", "upgrade.wav"),
    ("kSoundGameOver", "game_over.wav"),
)

REQUIRED_RATE = 44100
REQUIRED_CHANNELS = 2
REQUIRED_WIDTH = 2


def load_pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        width = w.getsampwidth()
        if rate != REQUIRED_RATE or ch != REQUIRED_CHANNELS or width != REQUIRED_WIDTH:
            raise SystemExit(
                f"{path.name}: 需要 {REQUIRED_RATE}Hz / {REQUIRED_CHANNELS}ch / "
                f"{REQUIRED_WIDTH * 8}-bit，实际 {rate}Hz / {ch}ch / {width * 8}-bit"
            )
        return w.readframes(w.getnframes())


def emit_array(name: str, pcm: bytes) -> list[str]:
    if len(pcm) % 2 != 0:
        raise SystemExit(f"{name}: PCM 字节数为奇数")
    lines = [f"static const uint8_t {name}Data[] PROGMEM = {{"]
    row: list[str] = []
    for b in pcm:
        row.append(f"0x{b:02x}")
        if len(row) >= 16:
            lines.append("  " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("  " + ", ".join(row) + ",")
    lines.append("};")
    lines.append(f"static const size_t {name}Bytes = {len(pcm)};")
    return lines


def main() -> None:
    hdr = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "",
        "struct GameSound {",
        "  const uint8_t *data;",
        "  size_t bytes;",
        "};",
        "",
    ]
    defs = ["#include \"game_sounds.h\"", ""]
    externs: list[str] = []
    total = 0
    for sym, fname in SOUNDS:
        path = DATA / fname
        if not path.is_file():
            raise SystemExit(f"缺少文件: {path}")
        pcm = load_pcm(path)
        total += len(pcm)
        defs.extend(emit_array(sym, pcm))
        defs.append("")
        defs.append(f"const GameSound {sym} = {{ {sym}Data, {sym}Bytes }};")
        defs.append("")
        externs.append(f"extern const GameSound {sym};")
    hdr.extend(externs)
    hdr.append("")
    OUT_H.write_text("\n".join(hdr) + "\n", encoding="utf-8")
    OUT_CPP.write_text("\n".join(defs) + "\n", encoding="utf-8")
    print(f"Wrote {OUT_H.name} + {OUT_CPP.name} ({total} PCM bytes)")


if __name__ == "__main__":
    main()
