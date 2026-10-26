#!/usr/bin/env python3
"""Generate the AkvariumOBS repo banner (monochrome, blurred icon backdrop).

Usage:
    python packaging/branding/make-banner.py

Outputs into packaging/branding/:
    banner.png      1667x458   (EN copy)
    banner-ru.png   1667x458   (RU copy)
    banner@2x.png   3334x916
"""

from __future__ import annotations

import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
ICON = os.path.join(REPO, "frontend", "forms", "images", "obs.png")
FONTS = "C:/Windows/Fonts"

W, H = 1667, 458
S = 2  # supersampling factor for the whole canvas

# monochrome palette
BG = (19, 19, 19)
BG_LIGHT = (35, 35, 35)
INK = (246, 246, 246)
SUB = (176, 176, 176)
PILL_FILL = (52, 52, 52)
PILL_LINE = (98, 98, 98)

# outer corner radius at 1x; scaled automatically for the @2x render
CORNER_RADIUS = 28

F_BOLD = os.path.join(FONTS, "segoeuib.ttf")
F_SEMI = os.path.join(FONTS, "seguisb.ttf")
F_REG = os.path.join(FONTS, "segoeui.ttf")


def load_icon(path: str):
    """Icon as (line art on white paper, alpha silhouette)."""
    im = Image.open(path).convert("RGBA")
    paper = Image.alpha_composite(Image.new("RGBA", im.size, (255, 255, 255, 255)), im).convert("L")
    art = ImageOps.autocontrast(paper, cutoff=1)
    return art, im.getchannel("A")


def radial_gradient(size: tuple[int, int], inner: Image.Image, center, radius: float) -> Image.Image:
    """Soft radial light pool, drawn at 1/8 res then upscaled = perfectly smooth."""
    sw, sh = max(size[0] // 8, 8), max(size[1] // 8, 8)
    small = Image.new("L", (sw, sh), 0)
    d = ImageDraw.Draw(small)
    cx, cy = center[0] * sw, center[1] * sh
    r = radius * sw
    steps = 96
    for i in range(steps, 0, -1):
        t = i / steps
        val = int(255 * (1.0 - t) ** 2.2)
        rr = r * t
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=val)
    return small.resize(size, Image.BICUBIC)


def noise(size: tuple[int, int], amount: int = 7, seed: int = 7) -> Image.Image:
    rnd = random.Random(seed)
    sw, sh = size
    small = Image.new("L", (sw, sh))
    small.putdata([128 + rnd.randint(-amount, amount) for _ in range(sw * sh)])
    return small


def spaced(draw, xy, text, font, fill, tracking=0):
    """Draw text with extra letter-spacing, returns width."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x - xy[0]


def spaced_width(draw, text, font, tracking=0):
    return sum(draw.textlength(c, font=font) + tracking for c in text) - (tracking if text else 0)


def rounded_tile(art: Image.Image, alpha: Image.Image, side: int, radius: int) -> Image.Image:
    """Light squircle with the girl pasted onto it (icon alpha preserved)."""
    pad = int(side * 0.05)
    inner = side - 2 * pad
    box = (inner, inner)
    art_r = ImageOps.fit(art, box, method=Image.LANCZOS, centering=(0.5, 0.42)).convert("RGB")
    art_a = ImageOps.fit(alpha, box, method=Image.LANCZOS, centering=(0.5, 0.42))

    tile = Image.new("RGB", (side, side), (246, 246, 246))
    # very subtle vertical gradient so the tile isn't flat
    grad = Image.linear_gradient("L").resize((side, side)).point(lambda v: 255 - int(v * 0.07))
    tile = ImageChops.multiply(tile, grad.convert("RGB"))

    canvas = tile.copy()
    canvas.paste(art_r, (pad, pad), art_a)

    mask = Image.new("L", (side, side), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, side - 1, side - 1], radius=radius, fill=255)
    out = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    out.paste(canvas, (0, 0), mask)

    # hairline inner edge
    edge = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle(
        [1, 1, side - 2, side - 2], radius=radius - 1, outline=(255, 255, 255, 70), width=max(1, S)
    )
    return Image.alpha_composite(out, edge)


def build(title: str, subtitle: str, tags: list[str], out: str, scale: int = S, out_scale: float = 1.0):
    w, h = W * scale, H * scale
    art, alpha = load_icon(ICON)

    # ---- backdrop -------------------------------------------------------
    bg = Image.new("RGB", (w, h), BG)
    # wide light pool, off-centre towards the icon
    pool = radial_gradient((w, h), None, (0.30, 0.46), 0.72)
    bg = Image.composite(Image.new("RGB", (w, h), BG_LIGHT), bg, pool)

    def ghost(center, size, blur, strength, tint_mul=2.6):
        """One heavily blurred copy of the icon silhouette, drawn as a soft aura."""
        g = ImageOps.fit(alpha, (size, size), method=Image.LANCZOS).filter(ImageFilter.GaussianBlur(blur))
        g = g.point(lambda v: int(v * strength / 255))
        tint = g.point(lambda v: min(255, int(v * tint_mul)))
        sub = Image.merge("RGBA", (tint, tint, tint, g))
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        layer.alpha_composite(sub, (int(center[0] - size / 2), int(center[1] - size / 2)))
        return layer

    # soft aura under the tile (blurred icon silhouette) + two clean light hazes
    bg_rgba = bg.convert("RGBA")
    bg_rgba = Image.alpha_composite(bg_rgba, ghost((0.115 * w, 0.5 * h), int(1.30 * h), int(0.045 * h), 105))

    def haze(center, radius, strength, blur):
        """Featureless soft light blob - reads as defocused light, never blotchy."""
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        cx, cy = center
        rx, ry = radius
        d.ellipse([cx - rx, cy - ry, cx + rx, cy + ry], fill=(255, 255, 255, strength))
        return layer.filter(ImageFilter.GaussianBlur(blur))

    bg_rgba = Image.alpha_composite(bg_rgba, haze((0.90 * w, 0.18 * h), (0.26 * w, 0.42 * h), 30, int(0.14 * h)))
    bg_rgba = Image.alpha_composite(bg_rgba, haze((0.62 * w, 0.92 * h), (0.34 * w, 0.30 * h), 16, int(0.16 * h)))
    bg_rgba = Image.alpha_composite(bg_rgba, haze((1.02 * w, 0.72 * h), (0.18 * w, 0.45 * h), 22, int(0.12 * h)))
    bg = bg_rgba.convert("RGB")

    # film grain + vignette
    grain = noise((w // 2, h // 2)).resize((w, h), Image.NEAREST).convert("RGB")
    bg = ImageChops.blend(bg, ImageChops.multiply(bg, grain), 0.16)
    vig = radial_gradient((w, h), None, (0.5, 0.5), 0.85).point(lambda v: 255 - int((255 - v) * 0.55))
    bg = ImageChops.multiply(bg, vig.convert("RGB"))
    canvas = bg.convert("RGBA")

    # ---- icon tile -----------------------------------------------------
    side = int(208 * scale)
    radius = int(side * 0.235)
    tile = rounded_tile(art, alpha, side, radius)
    tx, ty = int(72 * scale), (h - side) // 2

    # grounding shadow under the tile
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        [tx + int(4 * scale), ty + int(10 * scale), tx + side + int(4 * scale), ty + side + int(10 * scale)],
        radius=radius,
        fill=(0, 0, 0, 150),
    )
    shadow = shadow.filter(ImageFilter.GaussianBlur(int(16 * scale)))
    canvas = Image.alpha_composite(canvas, shadow)
    canvas.alpha_composite(tile, (tx, ty))

    # ---- text ----------------------------------------------------------
    draw = ImageDraw.Draw(canvas)
    f_title = ImageFont.truetype(F_BOLD, int(97 * scale))
    f_sub = ImageFont.truetype(F_REG, int(29 * scale))
    f_pill = ImageFont.truetype(F_SEMI, int(22 * scale))

    text_x = tx + side + int(46 * scale)

    # measure the text block
    cap_h = f_title.getbbox(title)[3] - f_title.getbbox(title)[1]
    sub_h = f_sub.getbbox("Ag")[3] - f_sub.getbbox("Ag")[1]
    pill_h = int(46 * scale)
    gap1, gap2 = int(26 * scale), int(30 * scale)
    block_h = cap_h + gap1 + sub_h + gap2 + pill_h
    y = (h - block_h) // 2 - int(6 * scale)

    draw.text((text_x, y), title, font=f_title, fill=INK)
    y += cap_h + gap1
    draw.text((text_x, y), subtitle, font=f_sub, fill=SUB)
    y += sub_h + gap2

    # pills
    px = text_x
    tracking = int(0.4 * scale)
    for tag in tags:
        tw = spaced_width(draw, tag, f_pill, tracking)
        pw = tw + int(46 * scale)
        draw.rounded_rectangle(
            [px, y, px + pw, y + pill_h], radius=pill_h // 2, fill=PILL_FILL, outline=PILL_LINE, width=max(1, scale)
        )
        asc = f_pill.getbbox(tag)
        text_y = y + (pill_h - (asc[3] - asc[1])) // 2 - asc[1]
        spaced(draw, (px + int(23 * scale), text_y), tag, f_pill, INK, tracking)
        px += pw + int(12 * scale)

    final = canvas.resize((int(W * out_scale), int(H * out_scale)), Image.LANCZOS)

    # rounded corners -> transparent alpha, so the banner drops onto any background.
    # Mask is drawn 3x and downsampled, otherwise PIL leaves stair-stepped corners.
    fw, fh = final.size
    ss = 3
    mask = Image.new("L", (fw * ss, fh * ss), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, fw * ss - 1, fh * ss - 1], radius=int(CORNER_RADIUS * out_scale * ss), fill=255
    )
    mask = mask.resize((fw, fh), Image.LANCZOS)
    final.putalpha(mask)
    final.save(out, "PNG", optimize=True)
    print("wrote", out, final.size)


EN = dict(
    title="AkvariumOBS",
    subtitle="OBS Studio fork — voice replay, per-scene audio, app-bound scenes",
    tags=["Voice Replay", "Scene Mixer", "App Scenes", "Voice STT"],
)
RU = dict(
    title="AkvariumOBS",
    subtitle="Форк OBS Studio — голосовые повторы, миксер по сценам, сцены под приложение",
    tags=["Голос", "Миксер", "Сцены", "Voice STT"],
)

if __name__ == "__main__":
    build(out=os.path.join(HERE, "banner.png"), **EN)
    build(out=os.path.join(HERE, "banner-ru.png"), **RU)
    build(out=os.path.join(HERE, "banner@2x.png"), scale=4, out_scale=2.0, **EN)
