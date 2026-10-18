import sys, os
from PIL import Image, ImageDraw
d = sys.argv[1]; out = sys.argv[2]; step = int(sys.argv[3]) if len(sys.argv) > 3 else 1
fs = sorted(f for f in os.listdir(d) if f.endswith('.png'))[::step][:16]
cols = 4; rows = (len(fs) + cols - 1) // cols
W, H = 240, 136
img = Image.new('RGB', (cols * W, rows * (H + 14)), 'gray')
dr = ImageDraw.Draw(img)
for i, f in enumerate(fs):
    im = Image.open(os.path.join(d, f)).convert('RGB').resize((W, H))
    x, y = (i % cols) * W, (i // cols) * (H + 14)
    img.paste(im, (x, y + 14)); dr.text((x + 2, y), f, fill='white')
img.save(out); print(out, len(fs))
