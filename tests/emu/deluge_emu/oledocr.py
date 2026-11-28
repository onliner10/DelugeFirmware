"""Read the text on the emulated OLED by exact glyph matching against the firmware's own fonts.

    from deluge_emu.oledocr import read_text
    print(read_text(dz.read_oled()))      # e.g. "SYNTH\nSECTION 1"

Glyphs come from src/deluge/gui/fonts/fonts.c (column-major, bit 0 = top pixel; lowercase is drawn
as uppercase). Each line of output is one text row; inverted (highlighted) text is shown as [TEXT].
Pixels that are not text (icons, lines, waveforms) are ignored.
"""

from __future__ import annotations

import re
from functools import lru_cache

from . import paths

FONTS = [
    "font_metric_bold_20px",
    "font_metric_bold_13px",
    "font_metric_bold_9px",
    "font_apple",
    "font_5px",
]


def _char_for(i: int) -> str:
    if i <= 0x60 - 0x20:
        return chr(0x20 + i)
    return "{|}~♭"[i - 65] if i - 65 < 5 else "?"


@lru_cache(maxsize=1)
def _glyphs() -> list[tuple[str, int, int, tuple[int, ...]]]:
    """[(char, height, width, column bitmasks)], biggest fonts first."""
    src = (paths.FW_ROOT / "src/deluge/gui/fonts/fonts.c").read_text(
        encoding="utf-8", errors="replace"
    )
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.DOTALL)
    out = []
    for name in FONTS:
        m = re.search(rf"{name}\[\]\s*=\s*\{{(.*?)\}};", src, re.DOTALL)
        d = re.search(rf"{name}_desc\[\]\s*=\s*\{{(.*?)\}};", src, re.DOTALL)
        if not m or not d:
            continue
        data = [
            int(t, 0) for t in re.findall(r"0b[01]+|0x[0-9a-fA-F]+|\b\d+\b", m.group(1))
        ]
        descs = []
        for ent in re.findall(r"\{([^{}]*)\}", d.group(1)):
            w = re.search(r"w_px\s*=\s*(\d+)", ent)
            g = re.search(r"glyph_index\s*=\s*(\d+)", ent)
            if w and g:
                descs.append((int(w.group(1)), int(g.group(1))))
        h = max((v.bit_length() for v in data), default=8)
        # bytes per column from the data: the 13/20 px fonts store several bytes per column
        height = {
            "font_metric_bold_20px": 20,
            "font_metric_bold_13px": 13,
            "font_metric_bold_9px": 9,
            "font_apple": 8,
            "font_5px": 5,
        }.get(name, h)
        bpc = ((height - 1) >> 3) + 1
        for i, (w, gi) in enumerate(descs):
            if i == 0 or w == 0:
                continue  # space
            cols = []
            for c in range(w):
                v = 0
                for b in range(bpc):
                    k = gi + c * bpc + b
                    v |= (data[k] if k < len(data) else 0) << (8 * b)
                cols.append(v & ((1 << height) - 1))
            while cols and cols[-1] == 0:
                cols.pop()
            while cols and cols[0] == 0:
                cols.pop(0)
            if not cols or sum(c.bit_count() for c in cols) < 1:
                continue
            gh = max(c.bit_length() for c in cols)
            out.append((_char_for(i), gh, len(cols), tuple(cols), name))
    # Prefer big, ink-heavy glyphs so small ones ('.', '-', 'I') don't eat pieces of big ones.
    out.sort(key=lambda g: (-g[1], -sum(c.bit_count() for c in g[3])))
    return out


def _colmasks(img: list[list[int]], top: int, h: int) -> list[int]:
    W = len(img[0])
    cols = [0] * W
    for dy in range(h):
        row = img[top + dy]
        for x in range(W):
            if row[x]:
                cols[x] |= 1 << dy
    return cols


def _scan(img: list[list[int]]) -> list[tuple[int, int, int, int, str]]:
    """Exact matches as (y, x, w, h, char); matched pixels are erased."""
    H, W = len(img), len(img[0])
    img = [list(r) for r in img]
    found = []
    glyphs = _glyphs()
    heights = sorted({g[1] for g in glyphs}, reverse=True)
    for gh in heights:
        cand: dict[int, list] = {}
        for g in glyphs:
            if g[1] == gh:
                cand.setdefault(g[3][0], []).append(g)
        for top in range(H - gh + 1):
            cols = _colmasks(img, top, gh)
            above = img[top - 1] if top > 0 else [0] * W
            below = img[top + gh] if top + gh < H else [0] * W
            x = 0
            while x < W:
                if not cols[x]:
                    x += 1
                    continue
                hit = None
                for ch, _, w, gcols, _font in cand.get(cols[x], ()):
                    if x + w > W or tuple(cols[x : x + w]) != gcols:
                        continue
                    # isolated: nothing touches the box, diagonals included (else it's a piece of something bigger)
                    lo, hi = max(0, x - 1), min(W, x + w + 1)
                    if (x > 0 and cols[x - 1]) or (x + w < W and cols[x + w]):
                        continue
                    if any(above[lo:hi]) or any(below[lo:hi]):
                        continue
                    hit = (ch, w)
                    break
                if hit:
                    ch, w = hit
                    found.append((top, x, w, gh, ch))
                    for dy in range(gh):
                        for dx in range(w):
                            img[top + dy][x + dx] = 0
                    cols = _colmasks(img, top, gh)
                    x += w
                else:
                    x += 1
    return found


def _lines(found) -> list[str]:
    found.sort(key=lambda f: (f[0] + f[3], f[1]))
    rows: list[list] = []
    for f in found:
        base = f[0] + f[3]
        for r in rows:
            if abs(r[0] - base) <= 2 and abs(r[1] - f[3]) <= 3:
                r[2].append(f)
                break
        else:
            rows.append([base, f[3], [f]])
    out = []
    for _, h, items in sorted(rows, key=lambda r: r[0]):
        items.sort(key=lambda f: f[1])
        s, prev_end = "", None
        gap = max(2, h // 3)
        for y, x, w, gh, ch in items:
            if prev_end is not None and x - prev_end > gap:
                s += " " * (2 if x - prev_end > 4 * gap else 1)
            s += ch
            prev_end = x + w
        out.append(s)
    return out


def read_text(rows) -> str:
    """rows: the OLED as rows of truthy/falsy pixels (DelugeSession.read_oled())."""
    img = [[1 if p else 0 for p in r] for r in rows]
    if not img or not img[0]:
        return ""
    normal = _scan(img)
    inv = _scan([[0 if p else 1 for p in r] for r in img])
    lines = _lines(normal)
    for ln in _lines(inv):
        if (
            len(ln.replace(" ", "")) >= 2
        ):  # single inverted "glyphs" are bits of icons/frames
            lines.append(f"[{ln}]")
    return "\n".join(x for x in lines if x.strip())
