#!/usr/bin/env python3
"""Mech sprite kit generator (original design, pixel art).

Draws every frame procedurally from a pose description, so animations stay consistent.
Outputs:
  assets/mech_sheet.png      preview sheet (4x scale, one row per animation)
  include/mech_sprites.h     8-bit palette-indexed frames + RGB565 palette for the TFT
Run: python3 tools/mech_sprites.py
"""
import os, struct, zlib

W, H = 80, 64
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# index: (name, rgb)
PALETTE = [
    ("clear",   (0, 0, 0)),
    ("outline", (10, 13, 26)),
    ("white",   (236, 239, 246)),
    ("whiteSh", (165, 174, 198)),
    ("navy",    (34, 46, 104)),
    ("navySh",  (20, 26, 62)),
    ("red",     (218, 32, 42)),
    ("redSh",   (145, 16, 26)),
    ("gold",    (246, 192, 44)),
    ("goldSh",  (180, 128, 20)),
    ("cyan",    (110, 245, 255)),
    ("cyanMid", (20, 150, 225)),
    ("hot",     (255, 255, 255)),
    ("metal",   (112, 118, 140)),
]
C = {name: i for i, (name, _) in enumerate(PALETTE)}
GLOW = {C["cyan"], C["cyanMid"], C["hot"]}   # effects drawn after the outline pass


class Canvas:
    def __init__(self):
        self.px = [[0] * W for _ in range(H)]

    def set(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y][x] = c

    def rect(self, x0, y0, x1, y1, c, shade=None, shadeW=1):
        for y in range(min(y0, y1), max(y0, y1) + 1):
            for x in range(min(x0, x1), max(x0, x1) + 1):
                self.set(x, y, shade if shade is not None and x > max(x0, x1) - shadeW else c)

    def poly(self, pts, c):
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            for x in range(int(min(xs)), int(max(xs)) + 1):
                if inside(x + 0.5, y + 0.5, pts):
                    self.set(x, y, c)

    def line(self, x0, y0, x1, y1, c):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(n + 1):
            t = i / n
            self.set(round(x0 + (x1 - x0) * t), round(y0 + (y1 - y0) * t), c)

    def outline(self):
        src = [row[:] for row in self.px]
        for y in range(H):
            for x in range(W):
                if src[y][x]:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < W and 0 <= ny < H and src[ny][nx] and src[ny][nx] not in GLOW:
                        self.px[y][x] = C["outline"]
                        break


def inside(x, y, pts):
    res = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]; xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            res = not res
        j = i
    return res


def mirror(pts):
    return [(64 - x, y) for x, y in pts]


def lerp_pts(a, b, t):
    return [(ax + (bx - ax) * t, ay + (by - ay) * t) for (ax, ay), (bx, by) in zip(a, b)]


def draw(p):
    """p: pose dict. Keys: bob, lift(L,R), armL, aim, wing(0..1), hover, flame, visor, muzzle, dx"""
    cv = Canvas()
    by = 2 + p.get("bob", 0) - p.get("hover", 0)
    ground = 60 - p.get("hover", 0)
    ox = p.get("dx", 0)

    def R(x0, y0, x1, y1, c, sh=None, w=1):
        cv.rect(x0 + ox, y0, x1 + ox, y1, c, sh, w)

    def P(pts, c):
        cv.poly([(x + ox, y) for x, y in pts], c)

    # Wing binders (behind the body), interpolated folded -> deployed
    folded = [(21, 15), (16, 13), (9, 31), (13, 35), (20, 25)]
    spread = [(21, 15), (17, 11), (2, 1), (0, 8), (18, 22)]
    wing = lerp_pts(folded, spread, p.get("wing", 0))
    for pts in (wing, mirror(wing)):
        pts = [(x, y + by) for x, y in pts]
        P(pts, C["white"])
        cv.line(pts[1][0] + ox, pts[1][1], pts[2][0] + ox, pts[2][1], C["red"])
        cv.line(pts[2][0] + ox, pts[2][1], pts[3][0] + ox, pts[3][1], C["gold"])
        mx = (pts[0][0] + pts[4][0]) / 2; my = (pts[0][1] + pts[4][1]) / 2
        cv.line(mx + ox, my, (pts[2][0] + pts[3][0]) / 2 + ox, (pts[2][1] + pts[3][1]) / 2, C["whiteSh"])

    # Legs
    for s, lift in ((-1, p.get("lift", (0, 0))[0]), (1, p.get("lift", (0, 0))[1])):
        lx = 32 + s * 6
        foot_bot = ground - lift
        foot_top = foot_bot - 3
        R(lx - 3, by + 35, lx + 3, by + 41, C["navy"], C["navySh"], 2)
        R(lx - 3, by + 41, lx + 3, by + 44, C["white"], C["whiteSh"])
        cv.set(lx + ox, by + 42, C["gold"])
        R(lx - 4, by + 45, lx + 4, foot_top - 1, C["white"], C["whiteSh"], 2)
        R(lx - 1, by + 47, lx + 1, by + 49, C["navy"])
        R(lx - 5, foot_top, lx + 5, foot_bot, C["red"], C["redSh"], 2)

    # Waist / skirt
    R(23, by + 31, 41, by + 36, C["white"], C["whiteSh"], 2)
    R(30, by + 31, 34, by + 35, C["red"], C["redSh"])
    R(26, by + 27, 38, by + 31, C["navy"], C["navySh"], 2)

    # Chest
    P([(21, by + 16), (43, by + 16), (40, by + 27), (24, by + 27)], C["white"])
    R(39, by + 17, 41, by + 25, C["whiteSh"])
    R(31, by + 16, 33, by + 27, C["red"], C["redSh"])
    R(24, by + 19, 28, by + 21, C["gold"], C["goldSh"])
    R(36, by + 19, 40, by + 21, C["gold"], C["goldSh"])
    R(27, by + 14, 37, by + 16, C["navy"])

    # Head: armored helmet with a visor and a single center crest
    R(27, by + 5, 37, by + 13, C["white"], C["whiteSh"], 2)
    R(28, by + 8, 36, by + 10, C["navySh"])
    vis = p.get("visor", 0)
    R(29, by + 9, 35, by + 9, C["cyanMid"])
    cv.set(29 + vis + ox, by + 9, C["cyan"])
    cv.set(30 + vis + ox, by + 9, C["hot"])
    R(31, by + 1, 33, by + 7, C["red"], C["redSh"])
    R(32, by + 0, 32, by + 0, C["red"])
    R(31, by + 12, 33, by + 14, C["red"])
    R(25, by + 8, 26, by + 11, C["red"])
    R(38, by + 8, 39, by + 11, C["redSh"])

    # Left arm (screen left)
    a = p.get("armL", 0)
    P([(13, by + 15), (22, by + 14), (23, by + 22), (14, by + 23)], C["white"])
    R(13, by + 14, 22, by + 16, C["red"], C["redSh"])
    R(15, by + 23, 20, by + 28, C["navy"], C["navySh"])
    R(14, by + 28 + a, 21, by + 36 + a, C["white"], C["whiteSh"], 2)
    R(14, by + 33 + a, 21, by + 33 + a, C["gold"])
    R(15, by + 36 + a, 20, by + 39 + a, C["navy"], C["navySh"])

    # Right arm + beam rifle
    P(mirror([(13, by + 15), (22, by + 14), (23, by + 22), (14, by + 23)]), C["white"])
    R(42, by + 14, 51, by + 16, C["red"], C["redSh"])
    if p.get("aim"):
        R(44, by + 18, 52, by + 22, C["navy"], C["navySh"])
        R(51, by + 17, 58, by + 23, C["white"], C["whiteSh"])
        R(55, by + 17, 55, by + 23, C["gold"])
        R(58, by + 18, 61, by + 23, C["navy"])
        R(55, by + 15, 73, by + 19, C["navy"], C["navySh"])
        R(56, by + 15, 70, by + 15, C["metal"])
        R(73, by + 16, 78, by + 18, C["metal"])
        R(62, by + 13, 65, by + 14, C["gold"])
        R(57, by + 20, 59, by + 25, C["navySh"])
    else:
        b = -a
        R(44, by + 23, 49, by + 28, C["navy"], C["navySh"])
        R(43, by + 28 + b, 50, by + 36 + b, C["white"], C["whiteSh"], 2)
        R(43, by + 33 + b, 50, by + 33 + b, C["gold"])
        R(44, by + 36 + b, 49, by + 39 + b, C["navy"], C["navySh"])
        R(51, by + 27 + b, 53, by + 52 + b, C["navy"], C["navySh"])
        R(51, by + 27 + b, 51, by + 50 + b, C["metal"])
        R(50, by + 36 + b, 54, by + 40 + b, C["navy"])
        R(52, by + 52 + b, 52, by + 55 + b, C["metal"])

    cv.outline()

    # --- glow effects (no outline) ---
    fl = p.get("flame", 0)
    if fl:
        for s in (-1, 1):
            fx = 32 + s * 6 + ox
            top = ground + 1
            for i in range(fl):
                half = max(0, 3 - i * 3 // fl)
                for dx in range(-half, half + 1):
                    c = C["hot"] if abs(dx) < half - 1 or i < 2 else C["cyan"] if abs(dx) < half else C["cyanMid"]
                    cv.set(fx + dx, top + i, c)
    m = p.get("muzzle", 0)
    if m and p.get("aim"):
        mx, my = 79 + ox, by + 17
        r = 3 if m == 1 else 1
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if abs(dx) + abs(dy) <= r:
                    cv.set(mx + dx, my + dy, C["hot"] if abs(dx) + abs(dy) < r else C["cyan"])
        cv.line(mx - r - 2, my, mx + r, my, C["hot"])
    return cv.px


def recolor(px, mode):
    out = []
    for row in px:
        r = []
        for c in row:
            if c in (0, C["outline"]):
                r.append(c)
            elif mode == "flash":
                r.append(C["hot"])
            else:   # red damage tint
                r.append({C["white"]: C["red"], C["whiteSh"]: C["redSh"], C["navy"]: C["redSh"],
                          C["cyanMid"]: C["gold"]}.get(c, c))
        out.append(r)
    return out


ANIMS = [
    ("idle",  [dict(bob=b, visor=v, wing=0.15) for b, v in ((0, 0), (0, 2), (1, 4), (1, 2))]),
    ("walk",  [dict(lift=l, bob=b, armL=a, wing=0.15) for l, b, a in
               (((3, 0), 1, 2), ((1, 0), 0, 1), ((0, 0), 0, 0), ((0, 3), 1, -2), ((0, 1), 0, -1), ((0, 0), 0, 0))]),
    ("boost", [dict(hover=h, wing=1, flame=f, lift=(1, 1), visor=4) for h, f in ((3, 5), (4, 7), (3, 4), (4, 6))]),
    ("fire",  [dict(aim=1, wing=0.4, visor=4), dict(aim=1, wing=0.4, muzzle=1, visor=5),
               dict(aim=1, wing=0.4, muzzle=2, dx=-1, visor=5), dict(aim=1, wing=0.4, visor=4)]),
    ("wings", [dict(wing=t, visor=i * 2) for i, t in enumerate((0.0, 0.35, 0.7, 1.0))]),
    ("hit",   [recolor(draw(dict(wing=0.15)), "flash"), recolor(draw(dict(wing=0.15, dx=-1)), "tint")]),
]


def frames():
    out = []
    for name, poses in ANIMS:
        out.append((name, [p if isinstance(p, list) else draw(p) for p in poses]))
    return out


def write_png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgb_rows)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def sheet(anims, scale=4, pad=6):
    cols = max(len(f) for _, f in anims)
    sw = (cols * (W + pad) + pad) * scale
    sh = (len(anims) * (H + pad) + pad) * scale
    bg = (12, 16, 36)
    grid = [[bg] * (sw // scale) for _ in range(sh // scale)]
    for r, (_, fs) in enumerate(anims):
        for c, px in enumerate(fs):
            x0 = pad + c * (W + pad); y0 = pad + r * (H + pad)
            for y in range(H):
                for x in range(W):
                    grid[y0 + y][x0 + x] = PALETTE[px[y][x]][1] if px[y][x] else (22, 28, 58)
    rows = []
    for row in grid:
        line = []
        for p in row:
            line += list(p) * scale
        for _ in range(scale):
            rows.append(line)
    write_png(os.path.join(ROOT, "assets", "mech_sheet.png"), sw, sh, rows)


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def header(anims):
    lines = ["#pragma once", "#include <Arduino.h>",
             "// Generated by tools/mech_sprites.py - mech (original design). Do not edit.",
             f"#define MECH_W {W}", f"#define MECH_H {H}",
             "static const uint16_t mech_palette[] = {" +
             ", ".join("0x%04X" % rgb565(*rgb) for _, rgb in PALETTE) + "};  // index 0 = transparent"]
    n = 0
    table = []
    for name, fs in anims:
        lines.append(f"#define MECH_{name.upper()}_START {n}")
        lines.append(f"#define MECH_{name.upper()}_COUNT {len(fs)}")
        for i, px in enumerate(fs):
            # Run-length encode as (count, index) pairs; frames are mostly transparent
            flat = [c for row in px for c in row]
            rle = []
            j = 0
            while j < len(flat):
                k = j
                while k < len(flat) and flat[k] == flat[j] and k - j < 255:
                    k += 1
                rle += [k - j, flat[j]]
                j = k
            ident = f"mech_{name}_{i}"
            body = ",".join(str(v) for v in rle)
            lines.append(f"static const uint8_t {ident}[] PROGMEM = {{{body}}};")
            table.append(ident)
            n += 1
    lines.append(f"#define MECH_FRAME_COUNT {n}")
    lines.append("static const uint8_t* const mech_frames[] = {" + ", ".join(table) + "};")
    open(os.path.join(ROOT, "include", "mech_sprites.h"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    a = frames()
    sheet(a)
    header(a)
    print("frames:", sum(len(f) for _, f in a))
