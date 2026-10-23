"""Generate the WaddleOS penguin images.

Usage: python3 assets/waddle/make_assets.py [out_dir]

Writes (default out_dir is this folder):
  waddle-logo-120.png   1-bit, 120x120, for scripts/convert_icon.py (boot logo)
  waddle-gray-big.png   4-level grayscale preview, 480x480
  sleep.bmp             480x800 grayscale sleep screen (Sleep Screen -> Custom)
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

N = 30  # the penguin is drawn on a 30x30 grid
WHITE, LIGHT, DARK, BLACK = 0, 1, 2, 3
LEVELS = {WHITE: 255, LIGHT: 190, DARK: 105, BLACK: 0}

g = [[WHITE] * N for _ in range(N)]


def inside(x, y, cx, cy, rx, ry):
    return ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2


def ellipse(cx, cy, rx, ry, v):
    for y in range(N):
        for x in range(N):
            if inside(x, y, cx, cy, rx, ry) <= 1:
                g[y][x] = v


def put(x, y, v=BLACK):
    if 0 <= x < N and 0 <= y < N:
        g[y][x] = v


def sprite(rows, ox, oy):
    """'#' is black, '.' is white, space leaves the pixel unchanged."""
    for dy, row in enumerate(rows):
        for dx, c in enumerate(row):
            if c == "#":
                put(ox + dx, oy + dy, BLACK)
            elif c == ".":
                put(ox + dx, oy + dy, WHITE)


# body
ellipse(15, 15, 11.5, 11.5, BLACK)
ellipse(15, 21.5, 9, 5.5, BLACK)
# heart-shaped face that flows into the belly
ellipse(12, 12, 3.8, 3.8, WHITE)
ellipse(18, 12, 3.8, 3.8, WHITE)
ellipse(15, 18.5, 8, 7, WHITE)
sprite(["##", "##", "##"], 14, 8)
# eyes with a sparkle
sprite(["##", "##", "##"], 11, 11)
sprite(["##", "##", "##"], 17, 11)
put(12, 11, WHITE)
put(18, 11, WHITE)
# beak
sprite(["##", "##"], 14, 14)
# flippers
sprite(["##", "###", "###", "###", " ##"], 1, 14)
sprite([" ##", "###", "###", "###", "## "], 26, 14)
# feet
sprite(["#####", "#####"], 9, 26)
sprite(["#####", "#####"], 16, 26)

mono = [[v == BLACK for v in row] for row in g]

# grayscale details on top of the 1-bit penguin
for y in range(20, N):
    for x in range(N):
        if g[y][x] == WHITE and 0.72 < inside(x, y, 15, 18.5, 8, 7) <= 1:
            g[y][x] = LIGHT  # belly shadow
for x, y in [(9, 14), (10, 14), (19, 14), (20, 14)]:
    g[y][x] = LIGHT  # cheeks
for x, y in [(14, 14), (15, 14), (14, 15), (15, 15)]:
    g[y][x] = DARK  # beak
for x in range(N):
    if g[27][x] == BLACK:
        g[27][x] = DARK  # feet


def to_image(grid, levels):
    img = Image.new("L", (N, N))
    for y in range(N):
        for x in range(N):
            img.putpixel((x, y), levels(grid[y][x]))
    return img


out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

logo = to_image(mono, lambda black: 0 if black else 255).resize((120, 120), Image.NEAREST)
logo.convert("1").save(os.path.join(out, "waddle-logo-120.png"))

gray = to_image(g, LEVELS.get)
gray.resize((480, 480), Image.NEAREST).save(os.path.join(out, "waddle-gray-big.png"))

W, H = 480, 800
screen = Image.new("L", (W, H), 255)
screen.paste(gray.resize((240, 240), Image.NEAREST), ((W - 240) // 2, 230))
draw = ImageDraw.Draw(screen)
bold = ImageFont.truetype("DejaVuSans-Bold.ttf", 34)
small = ImageFont.truetype("DejaVuSans.ttf", 20)
for y, text, font in ((500, "WaddleOS", bold), (548, "zzz...", small)):
    draw.text(((W - draw.textlength(text, font=font)) / 2, y), text, font=font, fill=0)
screen.convert("RGB").save(os.path.join(out, "sleep.bmp"))
