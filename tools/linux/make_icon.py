# The AppImage's icon: a candle under a pale moon on a night-violet tile (the menus' palette).
# Drawn from shapes; no game artwork. usage: python make_icon.py out.png
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 4  # supersampling
N = 256 * S
img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# tile with a vertical gradient
tile = Image.new("RGBA", (N, N))
td = ImageDraw.Draw(tile)
top, bottom = (34, 20, 52), (12, 7, 20)
for y in range(N):
    t = y / (N - 1)
    td.line([(0, y), (N, y)], fill=tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
mask = Image.new("L", (N, N), 0)
ImageDraw.Draw(mask).rounded_rectangle([8 * S, 8 * S, N - 8 * S, N - 8 * S], radius=44 * S, fill=255)
img.paste(tile, (0, 0), mask)

# moon: a pale disc with a soft glow, a crescent cut by a second disc of the sky colour
glow = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(glow).ellipse([150 * S, 30 * S, 226 * S, 106 * S], fill=(220, 225, 255, 90))
glow = glow.filter(ImageFilter.GaussianBlur(14 * S))
img.alpha_composite(glow)
moon = Image.new("RGBA", (N, N), (0, 0, 0, 0))
md = ImageDraw.Draw(moon)
md.ellipse([158 * S, 38 * S, 218 * S, 98 * S], fill=(226, 230, 245, 255))
md.ellipse([174 * S, 30 * S, 230 * S, 86 * S], fill=(0, 0, 0, 0))   # the crescent: cut, not painted
img.alpha_composite(moon)
d = ImageDraw.Draw(img)

# candle light
halo = Image.new("RGBA", (N, N), (0, 0, 0, 0))
ImageDraw.Draw(halo).ellipse([66 * S, 50 * S, 166 * S, 150 * S], fill=(255, 190, 90, 110))
halo = halo.filter(ImageFilter.GaussianBlur(22 * S))
img.alpha_composite(halo)
d = ImageDraw.Draw(img)

# candle: a wax column with a drip, on a dish
d.rounded_rectangle([96 * S, 112 * S, 136 * S, 206 * S], radius=6 * S, fill=(236, 222, 196, 255))
d.rounded_rectangle([96 * S, 112 * S, 106 * S, 206 * S], radius=5 * S, fill=(214, 196, 168, 255))
d.ellipse([118 * S, 108 * S, 140 * S, 122 * S], fill=(246, 236, 214, 255))
d.rounded_rectangle([126 * S, 112 * S, 134 * S, 146 * S], radius=4 * S, fill=(246, 236, 214, 255))
d.ellipse([66 * S, 198 * S, 166 * S, 222 * S], fill=(150, 112, 60, 255))
d.ellipse([72 * S, 196 * S, 160 * S, 214 * S], fill=(196, 152, 86, 255))
d.ellipse([110 * S, 202 * S, 122 * S, 208 * S], fill=(150, 20, 40, 255))   # a small ruby

# wick and flame
d.line([(116 * S, 112 * S), (116 * S, 100 * S)], fill=(40, 30, 30, 255), width=3 * S)
d.polygon([(116 * S, 58 * S), (128 * S, 86 * S), (124 * S, 100 * S), (116 * S, 104 * S),
           (108 * S, 100 * S), (104 * S, 86 * S)], fill=(255, 196, 84, 255))
d.polygon([(116 * S, 74 * S), (122 * S, 90 * S), (116 * S, 100 * S), (110 * S, 90 * S)], fill=(255, 244, 200, 255))

img = img.resize((256, 256), Image.LANCZOS)
img.save(sys.argv[1])
print("wrote", sys.argv[1])
