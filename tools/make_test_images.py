# Test photos: each shows a big label and an "UP" arrow so orientation bugs are obvious.
import os, sys, subprocess
from PIL import Image, ImageDraw, ImageFont
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
try:
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 200)
except Exception:
    font = ImageFont.load_default()

def photo(w, h, label, c1, c2):
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / (h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3)))
    for x in range(0, w, w // 12):
        d.line([(x, 0), (x, h)], fill=(255, 255, 255), width=2)
    s = min(w, h)
    d.polygon([(w / 2, h * 0.08), (w / 2 - s * 0.12, h * 0.08 + s * 0.16), (w / 2 + s * 0.12, h * 0.08 + s * 0.16)], fill=(255, 255, 255))
    d.text((w / 2, h * 0.08 + s * 0.2), "UP", fill=(255, 255, 255), font=font, anchor="mt")
    d.text((w / 2, h / 2 + s * 0.1), label, fill=(20, 20, 30), font=font, anchor="mm")
    return img

def exif(path, *args):
    subprocess.run(["exiftool", "-q", "-overwrite_original", *args, path], check=True)

# 1. big landscape JPEG with an embedded EXIF thumbnail
p = os.path.join(out, "01 landscape.jpg"); photo(6000, 4000, "01 JPEG", (90, 160, 230), (240, 200, 120)).save(p, quality=90)
photo(160, 107, "01", (90, 160, 230), (240, 200, 120)).save("/tmp/th1.jpg", quality=80)
exif(p, "-ThumbnailImage<=/tmp/th1.jpg")
# 2. portrait photo stored sideways with Orientation=6 (needs 90 CW)
p = os.path.join(out, "02 rotated exif6.jpg")
photo(4000, 6000, "02 EXIF 6", (120, 200, 120), (30, 80, 60)).rotate(90, expand=True).save(p, quality=90)
exif(p, "-Orientation#=6")
# 3. PNG
photo(4000, 3000, "03 PNG", (230, 120, 150), (90, 40, 120)).save(os.path.join(out, "03 image.png"))
# 4. small JPEG (should not upscale)
photo(800, 600, "04", (250, 220, 120), (200, 90, 40)).save(os.path.join(out, "04 small.jpg"), quality=90)
# 5. TIFF
photo(3000, 2000, "05 TIFF", (100, 100, 220), (20, 20, 60)).save(os.path.join(out, "05 image.tif"))
# 6. BMP
photo(1600, 1200, "06 BMP", (200, 200, 200), (60, 60, 60)).save(os.path.join(out, "06 image.bmp"))
# 7. GIF
photo(1200, 900, "07 GIF", (255, 150, 0), (120, 0, 0)).convert("P").save(os.path.join(out, "07 image.gif"))
# 8. Orientation=8 (needs 270 CW)
p = os.path.join(out, "08 rotated exif8.jpg")
photo(3000, 4500, "08 EXIF 8", (240, 240, 120), (120, 160, 40)).rotate(-90, expand=True).save(p, quality=90)
exif(p, "-Orientation#=8")
# 9. very wide panorama
photo(12000, 2000, "09 PANORAMA", (60, 190, 210), (10, 60, 90)).save(os.path.join(out, "09 pano.jpg"), quality=88)
# 10. broken file
open(os.path.join(out, "10 broken.jpg"), "wb").write(b"\xff\xd8\xff\xe0 this is not really a jpeg")
# natural sort check: "11" must come after "10", not after "1"
photo(2400, 1600, "11", (180, 120, 220), (60, 30, 90)).save(os.path.join(out, "11 last.jpg"), quality=90)
# 12. animated GIF (12 frames, 120 ms each)
small = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 60) if font != ImageFont.load_default() else font
frames = []
for i in range(12):
    im = Image.new("RGB", (480, 320), (30, 30, 60))
    d = ImageDraw.Draw(im)
    x = 40 + i * 33
    d.ellipse([x, 120, x + 80, 200], fill=(255, 180, 60))
    d.text((240, 60), f"GIF {i+1:02d}/12", fill=(255, 255, 255), font=small, anchor="mm")
    frames.append(im)
frames[0].save(os.path.join(out, "12 animated.gif"), save_all=True, append_images=frames[1:], duration=120, loop=0)
# 13/14. same gradient, untagged vs. tagged with a wide-gamut profile (should look different when colour-managed)
grad = Image.new("RGB", (1600, 1000))
px = grad.load()
for y in range(1000):
    for x in range(1600):
        px[x, y] = (int(255 * x / 1599), 160, int(255 * (1 - y / 999)))
grad.save(os.path.join(out, "13 untagged.jpg"), quality=92)
wide = "/usr/share/color/icc/ghostscript/rommrgb.icc"  # ProPhoto RGB, from the ghostscript package
if os.path.exists(wide):
    grad.save(os.path.join(out, "14 prophoto.jpg"), quality=92, icc_profile=open(wide, "rb").read())
print("ok")
