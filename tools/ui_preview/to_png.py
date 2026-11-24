"""Convert emulator PGM dumps to 2x PNGs plus one contact sheet (contact_sheet.png)."""
import glob, os, sys
from PIL import Image, ImageDraw

out = sys.argv[1]
paths = sorted(glob.glob(os.path.join(out, "*.pgm")))
tiles = []
for p in paths:
    img = Image.open(p).convert("L")
    big = img.resize((img.width * 2, img.height * 2), Image.NEAREST)
    png = p[:-4] + ".png"
    big.save(png)
    os.remove(p)
    tiles.append((os.path.basename(png)[:-4], img))

cols, pad, label_h = 3, 16, 22
w, h = 400, 300
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + pad + label_h) + pad), (205, 200, 190))
draw = ImageDraw.Draw(sheet)
for i, (name, img) in enumerate(tiles):
    x = pad + (i % cols) * (w + pad)
    y = pad + (i // cols) * (h + pad + label_h)
    draw.text((x, y + 4), name, fill=(40, 40, 40))
    sheet.paste(img.convert("RGB"), (x, y + label_h))
sheet.save(os.path.join(out, "contact_sheet.png"))
print(f"{len(tiles)} scenes -> {out}/contact_sheet.png")
