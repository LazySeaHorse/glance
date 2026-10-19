# Generates res/glance.ico: a rounded tile with a sunset-over-hills glyph.
import sys
from PIL import Image, ImageDraw, ImageFilter

def tile(size):
    S = size * 4  # supersample
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # vertical gradient sky
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    top, bot = (255, 176, 92), (233, 74, 112)
    for y in range(S):
        t = y / (S - 1)
        gd.line([(0, y), (S, y)], fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)) + (255,))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([S * 0.06, S * 0.06, S * 0.94, S * 0.94], radius=S * 0.22, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # sun
    r = S * 0.13
    cx, cy = S * 0.64, S * 0.40
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 245, 225, 255))
    # hills
    hills = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hills)
    hd.ellipse([-S * 0.25, S * 0.58, S * 0.75, S * 1.4], fill=(70, 30, 90, 235))
    hd.ellipse([S * 0.35, S * 0.64, S * 1.3, S * 1.5], fill=(110, 40, 105, 245))
    img.alpha_composite(Image.composite(hills, Image.new("RGBA", (S, S), (0, 0, 0, 0)), mask))
    return img.resize((size, size), Image.LANCZOS)

sizes = [16, 24, 32, 48, 64, 128, 256]
imgs = [tile(s) for s in sizes]
imgs[-1].save(sys.argv[1], format="ICO", sizes=[(s, s) for s in sizes], append_images=imgs[:-1])
imgs[-1].save(sys.argv[1].replace(".ico", ".png"))
