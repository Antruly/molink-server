#!/usr/bin/env python3
"""Generate the moLink Server icon set from one set of geometry.

The client icon is a teal-to-blue squircle with a white ``m``. The server
icon keeps that language and adds a two-unit server rack underneath, so the
two products are recognisable as a family but never confused.

Outputs (all paths relative to the repository root):

    packaging/icons/molink-server.svg
    packaging/icons/molink-server-<size>.png     16 … 1024
    packaging/icons/molink-server.ico            multi-size Windows icon
    ui/public/favicon.svg
    ui/public/favicon.ico
    ui/public/apple-touch-icon.png

Run with:  python3 -I tools/gen-icon.py
"""

from __future__ import annotations

import pathlib

from PIL import Image, ImageDraw

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Design canvas and supersampling factor for anti-aliasing.
SIZE = 1024
SS = 4096
K = SS / SIZE

C0 = (46, 209, 198)   # #2ed1c6 top-left, sampled from the client icon
C1 = (55, 192, 241)   # #37c0f1 bottom-right

STEM_X = (316, 512, 708)
STEM_W = 94
STEM_TOP = 430
STEM_BOTTOM = 660
ARCH_R = 98

BAR_X0, BAR_X1 = 288, 736
BAR_H = 82
BAR1_Y = 758
BAR2_Y = 872


def s(value: float) -> int:
    return int(round(value * K))


def lerp(a: int, b: int, t: float) -> int:
    return int(round(a + (b - a) * t))


def gradient(size: int) -> Image.Image:
    # A tiny diagonal ramp resized up is smooth and cheap.
    small = Image.new("RGB", (256, 256))
    pixels = small.load()
    for y in range(256):
        for x in range(256):
            t = (x + y) / 510.0
            pixels[x, y] = (lerp(C0[0], C1[0], t),
                            lerp(C0[1], C1[1], t),
                            lerp(C0[2], C1[2], t))
    return small.resize((size, size), Image.BILINEAR)


def draw_icon() -> Image.Image:
    base = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))

    # Squircle body, same proportions as the client icon.
    mask = Image.new("L", (SS, SS), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [s(24), s(24), s(1000), s(1000)], radius=s(220), fill=255)
    base.paste(gradient(SS), (0, 0), mask)

    draw = ImageDraw.Draw(base)
    white = (255, 255, 255, 255)

    # The lowercase "m": two arches over three stems.
    stem_half = s(STEM_W) // 2
    for cx in STEM_X:
        draw.rounded_rectangle(
            [s(cx) - stem_half, s(STEM_TOP) - stem_half,
             s(cx) + stem_half, s(STEM_BOTTOM) + stem_half],
            radius=stem_half, fill=white)
    band = s(ARCH_R) + stem_half
    half_gap = (STEM_X[1] - STEM_X[0]) / 2
    for first in (STEM_X[0], STEM_X[1]):
        center_x = first + half_gap
        draw.arc(
            [s(center_x) - band, s(STEM_TOP) - band,
             s(center_x) + band, s(STEM_TOP) + band],
            start=180, end=360, fill=white, width=s(STEM_W))

    # Two server units under the m, each with an LED and a vent slot.
    for index, (y, alpha) in enumerate(((BAR1_Y, 255), (BAR2_Y, 214))):
        bar_layer = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
        ImageDraw.Draw(bar_layer).rounded_rectangle(
            [s(BAR_X0), s(y), s(BAR_X1), s(y + BAR_H)],
            radius=s(BAR_H // 2), fill=(255, 255, 255, alpha))
        base = Image.alpha_composite(base, bar_layer)
        # Composite the vent after the bar so the PNG matches the SVG's
        # paint order (slot over the translucent unit, not behind it).
        slot_alpha = 72 if index == 0 else 60
        slot_layer = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
        ImageDraw.Draw(slot_layer).rounded_rectangle(
            [s(404), s(y + 32), s(668), s(y + 50)],
            radius=s(9), fill=(C1[0], C1[1], C1[2], slot_alpha))
        base = Image.alpha_composite(base, slot_layer)
        draw = ImageDraw.Draw(base)
        ledge = C0 if index == 0 else C1
        draw.ellipse(
            [s(336), s(y + 32), s(364), s(y + 60)],
            fill=(ledge[0], ledge[1], ledge[2], 255))

    return base


def svg_text() -> str:
    return f"""<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#2ed1c6"/>
      <stop offset="1" stop-color="#37c0f1"/>
    </linearGradient>
  </defs>
  <rect x="24" y="24" width="976" height="976" rx="220" fill="url(#bg)"/>
  <g fill="none" stroke="#fff" stroke-width="{STEM_W}" stroke-linecap="round">
    <path d="M{STEM_X[0]} {STEM_BOTTOM} V{STEM_TOP} A{ARCH_R} {ARCH_R} 0 0 1 {STEM_X[1]} {STEM_TOP} V{STEM_BOTTOM}"/>
    <path d="M{STEM_X[1]} {STEM_TOP} A{ARCH_R} {ARCH_R} 0 0 1 {STEM_X[2]} {STEM_TOP} V{STEM_BOTTOM}"/>
  </g>
  <rect x="{BAR_X0}" y="{BAR1_Y}" width="{BAR_X1 - BAR_X0}" height="{BAR_H}" rx="{BAR_H // 2}" fill="#fff"/>
  <rect x="{BAR_X0}" y="{BAR2_Y}" width="{BAR_X1 - BAR_X0}" height="{BAR_H}" rx="{BAR_H // 2}" fill="#fff" opacity=".84"/>
  <circle cx="350" cy="{BAR1_Y + BAR_H // 2}" r="14" fill="#2ed1c6"/>
  <circle cx="350" cy="{BAR2_Y + BAR_H // 2}" r="14" fill="#37c0f1"/>
  <rect x="404" y="{BAR1_Y + 32}" width="264" height="18" rx="9" fill="#37c0f1" opacity=".28"/>
  <rect x="404" y="{BAR2_Y + 32}" width="264" height="18" rx="9" fill="#37c0f1" opacity=".22"/>
</svg>
"""


def main() -> None:
    icon = draw_icon()
    master = icon.resize((SIZE, SIZE), Image.LANCZOS)

    icons_dir = ROOT / "packaging" / "icons"
    icons_dir.mkdir(parents=True, exist_ok=True)
    ui_dir = ROOT / "ui" / "public"
    ui_dir.mkdir(parents=True, exist_ok=True)

    (icons_dir / "molink-server.svg").write_text(svg_text(), encoding="utf-8")
    (ui_dir / "favicon.svg").write_text(svg_text(), encoding="utf-8")

    for size in (16, 32, 48, 64, 128, 256, 512, 1024):
        master.resize((size, size), Image.LANCZOS).save(
            icons_dir / f"molink-server-{size}.png")

    master.save(icons_dir / "molink-server.ico",
                sizes=[(16, 16), (24, 24), (32, 32), (48, 48),
                       (64, 64), (128, 128), (256, 256)])
    master.save(ui_dir / "favicon.ico",
                sizes=[(16, 16), (32, 32), (48, 48)])

    # iOS masks the touch icon itself, so flatten onto a full gradient square.
    flat = gradient(SIZE).convert("RGBA")
    flat.alpha_composite(master)
    flat.resize((180, 180), Image.LANCZOS).save(
        ui_dir / "apple-touch-icon.png")

    print("wrote icons to", icons_dir, "and", ui_dir)


if __name__ == "__main__":
    main()
