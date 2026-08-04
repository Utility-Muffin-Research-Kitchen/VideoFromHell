#!/usr/bin/env python3
"""Build the Video From Hell launcher icon: pak/res/icon.png (256x256).

House style (Disco Boy's Game Boy, Fugazi's CRT panel) is a physical object with
depth, not a flat glyph. Here: a VHS cassette possessed -- devil horns, the reels
burning behind the window, and the handheld's own D-pad and A/B buttons on the
shell, so it reads as "a tape that plays on this device".

UMRK palette (Leaf green #7FB069 on base #0F160E) carries the brand; the fire is
the accent. Drawn with Pillow at SS x scale and downsampled, same approach as
DiscoBoy/scripts/make-record-placeholder.py, so the PNG is reproducible from
source instead of being an opaque binary.

    python3 scripts/make-icon.py [--out PATH] [--size N]
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(ROOT, "pak", "res", "icon.png")
# Catastrophe is the sibling checkout; its bundled display face gives the
# wordmark the same chunky rounded feel as the rest of the UMRK UI.
FONT_CANDIDATES = [
    os.path.join(ROOT, "..", "Catastrophe", "res", "fonts", "Baloo2", "Baloo2-Bold.ttf"),
    os.path.join(ROOT, "..", "Catastrophe", "res", "fonts", "Fredoka", "Fredoka-Bold.ttf"),
    os.path.join(ROOT, "..", "Catastrophe", "res", "fonts", "Nunito", "Nunito-Bold.ttf"),
]

SS = 4  # supersample factor

BASE = (15, 22, 14, 255)         # #0F160E badge
GREEN = (127, 176, 105, 255)     # #7FB069 Leaf green
GREEN_LT = (168, 209, 147, 255)
SHELL_HI = (58, 70, 54, 255)
SHELL_MID = (38, 48, 35, 255)
SHELL_LO = (24, 32, 22, 255)
BEVEL_LT = (104, 124, 96, 190)
BEVEL_DK = (8, 12, 7, 200)
WINDOW = (12, 8, 6, 255)
REEL_DARK = (26, 20, 16, 255)
FLAME_DEEP = (198, 58, 18)
FLAME_MID = (247, 130, 32)
FLAME_HOT = (255, 214, 92)
BTN_RED = (208, 62, 58, 255)
BTN_YELLOW = (226, 178, 52, 255)
INK = (22, 38, 14, 255)


def load_font(px):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, px)
            except OSError:
                continue
    return ImageFont.load_default()


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def vgradient(size, stops, box, radius=0):
    """Vertical multi-stop gradient clipped to a rounded rect."""
    w, h = size
    x0, y0, x1, y1 = box
    height = max(1, y1 - y0)
    strip = Image.new("RGBA", (1, height))
    for y in range(height):
        t = y / max(1, height - 1)
        for i in range(len(stops) - 1):
            a_pos, a_col = stops[i]
            b_pos, b_col = stops[i + 1]
            if a_pos <= t <= b_pos:
                local = (t - a_pos) / max(1e-6, b_pos - a_pos)
                strip.putpixel((0, y), lerp(a_col, b_col, local))
                break
        else:
            strip.putpixel((0, y), stops[-1][1])
    strip = strip.resize((max(1, x1 - x0), height))
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    layer.paste(strip, (x0, y0), mask.crop(box))
    return layer


def flame_polygon(cx, base_y, w, h, lean=0.10):
    """Silhouette of a single flame: wide flickering base tapering to a tip that
    leans slightly, so a row of them does not look like identical cones."""
    return [
        (cx - w * 0.50, base_y),
        (cx - w * 0.56, base_y - h * 0.30),
        (cx - w * 0.24, base_y - h * 0.52),
        (cx - w * 0.30, base_y - h * 0.72),
        (cx + w * lean, base_y - h),
        (cx + w * 0.30, base_y - h * 0.66),
        (cx + w * 0.22, base_y - h * 0.44),
        (cx + w * 0.54, base_y - h * 0.26),
        (cx + w * 0.50, base_y),
    ]


def draw_fire(size, seeds, blur):
    """Layered fire: deep orange body, brighter mid, hot core. Each layer is a
    little smaller so the flame reads as having depth rather than a flat cutout."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for scale, color, alpha in ((1.00, FLAME_DEEP, 235),
                                (0.72, FLAME_MID, 245),
                                (0.42, FLAME_HOT, 250)):
        for (cx, base_y, w, h, lean) in seeds:
            d.polygon(flame_polygon(cx, base_y, w * scale, h * scale, lean),
                      fill=color + (alpha,))
    return layer.filter(ImageFilter.GaussianBlur(radius=blur))


def draw_horn(img, tip, base_center, base_w, flip):
    """A tapering curved horn, built from shrinking circles along a quadratic
    curve -- cheaper than a bezier fill and the soft edge suits the shading."""
    d = ImageDraw.Draw(img)
    bx, by = base_center
    tx, ty = tip
    ctrl = (bx + (tx - bx) * (1.35 if not flip else 1.35), by - (by - ty) * 0.25)
    steps = 48
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * bx + 2 * (1 - t) * t * ctrl[0] + t ** 2 * tx
        y = (1 - t) ** 2 * by + 2 * (1 - t) * t * ctrl[1] + t ** 2 * ty
        r = base_w * (1 - t) ** 1.2 / 2
        color = lerp(FLAME_DEEP + (255,), FLAME_HOT + (255,), t ** 1.3)
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)


def build(size):
    s = size * SS
    k = s / 256.0

    def u(v):
        return v * k

    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # badge
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=u(46), fill=BASE)

    # --- horns (behind the shell, so they appear to grow out of it) ---
    horns = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw_horn(horns, (u(32), u(18)), (u(80), u(70)), u(30), False)
    draw_horn(horns, (u(224), u(18)), (u(176), u(70)), u(30), True)
    img.alpha_composite(horns.filter(ImageFilter.GaussianBlur(radius=0.5 * SS)))

    shell_box = (int(u(26)), int(u(58)), int(u(230)), int(u(212)))

    # drop shadow
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        (shell_box[0] + u(3), shell_box[1] + u(6),
         shell_box[2] + u(3), shell_box[3] + u(6)),
        radius=u(16), fill=(0, 0, 0, 165))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(radius=4 * SS)))

    # --- cassette shell, with a top-lit bevel ---
    img.alpha_composite(vgradient((s, s),
                                  [(0.0, SHELL_HI), (0.45, SHELL_MID), (1.0, SHELL_LO)],
                                  shell_box, radius=u(16)))
    d.rounded_rectangle(shell_box, radius=u(16), outline=BEVEL_DK, width=max(1, int(u(3))))
    d.line([shell_box[0] + u(18), shell_box[1] + u(3),
            shell_box[2] - u(18), shell_box[1] + u(3)],
           fill=BEVEL_LT, width=max(1, int(u(2.5))))

    # --- label with the wordmark ---
    label_box = (int(u(40)), int(u(70)), int(u(216)), int(u(106)))
    img.alpha_composite(vgradient((s, s), [(0.0, GREEN_LT), (1.0, GREEN)],
                                  label_box, radius=u(7)))
    d.rounded_rectangle(label_box, radius=u(7), outline=(30, 52, 22, 170),
                        width=max(1, int(u(2))))

    title_font = load_font(int(u(20)))
    hell_font = load_font(int(u(26)))
    left = "VIDEO FROM "
    right = "HELL"
    lw = d.textlength(left, font=title_font)
    rw = d.textlength(right, font=hell_font)
    total = lw + rw
    tx = (label_box[0] + label_box[2]) / 2 - total / 2
    baseline = u(88)
    d.text((tx, baseline), left, font=title_font, fill=(250, 252, 245, 255),
           anchor="lm", stroke_width=max(1, int(u(1.5))), stroke_fill=INK)
    # "HELL" burns: hot fill with a dark outline so it holds against the green
    d.text((tx + lw, baseline), right, font=hell_font, fill=FLAME_MID + (255,),
           anchor="lm", stroke_width=max(1, int(u(2))), stroke_fill=(60, 16, 6, 255))

    # --- reel window: fire behind, spools in front ---
    win = (int(u(44)), int(u(114)), int(u(212)), int(u(168)))
    spill = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(spill).rounded_rectangle(
        (win[0] - u(10), win[1] - u(10), win[2] + u(10), win[3] + u(10)),
        radius=u(16), fill=FLAME_MID + (120,))
    spill = spill.filter(ImageFilter.GaussianBlur(radius=7 * SS))
    spill_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(spill_mask).rounded_rectangle(shell_box, radius=u(16), fill=255)
    img.alpha_composite(Image.composite(spill, Image.new("RGBA", (s, s), (0, 0, 0, 0)),
                                        spill_mask))
    d.rounded_rectangle(win, radius=u(8), fill=WINDOW)

    seeds = [(u(58), u(172), u(30), u(40), 0.16),
             (u(80), u(174), u(34), u(52), -0.12),
             (u(104), u(173), u(30), u(44), 0.20),
             (u(128), u(174), u(36), u(58), -0.06),
             (u(152), u(173), u(30), u(46), 0.14),
             (u(176), u(174), u(34), u(50), -0.18),
             (u(198), u(172), u(28), u(38), 0.10)]
    fire = draw_fire((s, s), seeds, blur=2.2 * SS)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(win, radius=u(8), fill=255)
    img.alpha_composite(Image.composite(fire, Image.new("RGBA", (s, s), (0, 0, 0, 0)), mask))

    # spools, silhouetted against the fire
    for cx, tape_r in ((u(88), u(24)), (u(168), u(24))):
        cy = u(141)
        d.ellipse([cx - tape_r, cy - tape_r, cx + tape_r, cy + tape_r], fill=REEL_DARK)
        d.ellipse([cx - tape_r, cy - tape_r, cx + tape_r, cy + tape_r],
                  outline=(0, 0, 0, 140), width=max(1, int(u(2))))
        for ring in (0.82, 0.64):
            rr = tape_r * ring
            d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                      outline=(58, 44, 34, 200), width=max(1, int(u(1.5))))
        d.ellipse([cx - u(11), cy - u(11), cx + u(11), cy + u(11)],
                  fill=(20, 14, 10, 255), outline=FLAME_HOT + (230,),
                  width=max(1, int(u(2.5))))
        d.ellipse([cx - u(4), cy - u(4), cx + u(4), cy + u(4)], fill=FLAME_HOT + (255,))
    d.rounded_rectangle(win, radius=u(8), outline=(0, 0, 0, 190), width=max(1, int(u(3))))

    # --- controls on the lower shell ---
    # D-pad
    px, py, arm, thick = u(76), u(190), u(15), u(11)
    for box in ([px - arm, py - thick / 2, px + arm, py + thick / 2],
                [px - thick / 2, py - arm, px + thick / 2, py + arm]):
        d.rounded_rectangle(box, radius=u(2.5), fill=BTN_RED)
    d.ellipse([px - u(3.5), py - u(3.5), px + u(3.5), py + u(3.5)],
              fill=(150, 40, 38, 255))
    # A / B
    for (bx, by, r, col, label) in ((u(190), u(188), u(13), BTN_RED, "A"),
                                    (u(157), u(196), u(11), BTN_YELLOW, "B")):
        d.ellipse([bx - r, by - r, bx + r, by + r], fill=col)
        d.arc([bx - r, by - r, bx + r, by + r], start=185, end=355,
              fill=(255, 255, 255, 90), width=max(1, int(u(2))))
        f = load_font(int(r * 1.25))
        d.text((bx, by), label, font=f, fill=(28, 20, 16, 235), anchor="mm")

    # screws
    for sx, sy in ((u(40), u(200)), (u(216), u(200))):
        d.ellipse([sx - u(3), sy - u(3), sx + u(3), sy + u(3)], fill=(14, 20, 12, 220))

    # --- gloss across the whole shell ---
    gloss = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(gloss).polygon(
        [(u(26), u(58)), (u(150), u(58)), (u(26), u(170))], fill=(255, 255, 255, 26))
    gloss = gloss.filter(ImageFilter.GaussianBlur(radius=8 * SS))
    shell_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(shell_mask).rounded_rectangle(shell_box, radius=u(16), fill=255)
    img.alpha_composite(Image.composite(gloss, Image.new("RGBA", (s, s), (0, 0, 0, 0)),
                                        shell_mask))

    return img.resize((size, size), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--size", type=int, default=256)
    args = ap.parse_args()
    icon = build(args.size)
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    icon.save(args.out, "PNG", optimize=True)
    print(f"wrote {args.out} ({args.size}x{args.size})")


if __name__ == "__main__":
    main()
