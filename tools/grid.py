#!/usr/bin/env python3
# grid.py OUT DIR FRAME... : tile screenshots DIR/frame_NNNNNN.png (2 columns)
import sys
from PIL import Image
out, d, fs = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3:]]
ims = [Image.open('%s/frame_%06d.png' % (d, f)) for f in fs]
w, h = ims[0].size
o = Image.new('RGB', (w * 2, h * ((len(ims) + 1) // 2)))
for i, im in enumerate(ims): o.paste(im, ((i % 2) * w, (i // 2) * h))
o.save(out)
