"""The launcher's icon (src/launcher/assets/launcher.ico): the shell mark of its header, drawn large and
scaled down. Needs Pillow.

    python tools/launcher_icon.py src/launcher/assets/launcher.ico"""
import sys
from PIL import Image, ImageDraw

S = 1024  # drawn at this size; the mark's own box is 28 x 28
K = S / 28.0
OUTER = [(14, 2), (24.4, 8), (24.4, 20), (14, 26), (3.6, 20), (3.6, 8)]
INNER = [(14, 8.5), (18.8, 11.25), (18.8, 16.75), (14, 19.5), (9.2, 16.75), (9.2, 11.25)]


def pts(p):
    return [(x * K, y * K) for x, y in p]


def outline(d, p, width):
    q = pts(p)
    w = int(width * K)
    d.line(q + [q[0], q[1]], fill=(0, 0, 0, 255), width=w, joint="curve")


img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
d.polygon(pts(OUTER), fill=(0x8B, 0xE8, 0x36, 255))
for a, b in zip(INNER, OUTER):
    d.line(pts([a, b]), fill=(0, 0, 0, 255), width=int(1.6 * K))
d.polygon(pts(INNER), fill=(0x5B, 0x3F, 0xD0, 255))
outline(d, INNER, 1.6)
outline(d, OUTER, 2.2)
img.save(sys.argv[1], sizes=[(16, 16), (20, 20), (24, 24), (32, 32), (40, 40), (48, 48), (64, 64), (256, 256)])
print("icon written:", sys.argv[1])
