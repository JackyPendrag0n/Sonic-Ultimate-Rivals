# objview.py scene.obj out.png [yaw_deg pitch_deg]: textured software render of a rip, to check it
import sys, os, math
import numpy as np
from PIL import Image

path = sys.argv[1]; out = sys.argv[2]
yaw = math.radians(float(sys.argv[3]) if len(sys.argv) > 3 else 30)
pitch = math.radians(float(sys.argv[4]) if len(sys.argv) > 4 else 25)
d = os.path.dirname(path)
V = []; T = []; faces = []; mat = None
mtl = {}
cur = None
skip = False
for l in open(os.path.join(d, 'scene.mtl')):
    p = l.split()
    if not p: continue
    if p[0] == 'newmtl': cur = p[1]
    elif p[0] == 'map_Kd': mtl[cur] = p[1]
for l in open(path):
    p = l.split()
    if not p: continue
    if p[0] == 'v': V.append([float(x) for x in p[1:4]])
    elif p[0] == 'vt': T.append([float(x) for x in p[1:3]])
    elif p[0] == 'o': skip = p[1].startswith('overlay')
    elif p[0] == 'usemtl': mat = p[1]
    elif p[0] == 'f' and not skip: faces.append((mat, [int(x.split('/')[0]) - 1 for x in p[1:4]], [int(x.split('/')[1]) - 1 for x in p[1:4]]))
V = np.array(V); T = np.array(T)
if os.environ.get('ZUP'): V = V[:, [0, 2, 1]] * np.array([1, 1, -1])
tex = {}
def gettex(m):
    if m not in tex:
        f = mtl.get(m)
        tex[m] = np.asarray(Image.open(os.path.join(d, f)).convert('RGBA')).astype(np.float32) if f and os.path.exists(os.path.join(d, f)) else None
    return tex[m]
c = np.median(V, 0); dist = np.linalg.norm(V - c, axis=1); r = np.percentile(dist, 90)
if os.environ.get('CENTER'):
    c = np.array([float(x) for x in os.environ['CENTER'].split(',')]); r = float(os.environ['RADIUS'])
    if os.environ.get('ZUP'): c = c[[0, 2, 1]] * np.array([1, 1, -1])
    dist = np.linalg.norm(V - c, axis=1)
faces = [f for f in faces if dist[f[1]].max() < 1.5 * r]
cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
R = np.array([[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]]) @ np.eye(3)
R = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]]) @ R
P = (V - c) @ R.T
W, H = 960, 720
s = min(W, H) / (2.1 * r)
X = P[:, 0] * s + W / 2; Y = H / 2 - P[:, 1] * s; Z = P[:, 2]
img = np.zeros((H, W, 3), np.float32) + 40; zb = np.full((H, W), 1e30)
for m, vi, ti in faces:
    x = X[vi]; y = Y[vi]; z = Z[vi]
    x0, x1 = int(max(0, math.floor(x.min()))), int(min(W - 1, math.ceil(x.max())))
    y0, y1 = int(max(0, math.floor(y.min()))), int(min(H - 1, math.ceil(y.max())))
    if x1 < x0 or y1 < y0: continue
    den = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
    if abs(den) < 1e-9: continue
    gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / den
    b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / den
    g = 1 - a - b
    inside = (a >= 0) & (b >= 0) & (g >= 0)
    if not inside.any(): continue
    zz = a * z[0] + b * z[1] + g * z[2]
    sub = zb[y0:y1 + 1, x0:x1 + 1]
    t = gettex(m)
    if t is not None:
        uv = T[ti]
        u = (a * uv[0, 0] + b * uv[1, 0] + g * uv[2, 0]) % 1.0
        v = (a * uv[0, 1] + b * uv[1, 1] + g * uv[2, 1]) % 1.0
        tx = (u * (t.shape[1] - 1)).astype(int); ty = ((1 - v) * (t.shape[0] - 1)).astype(int)
        col = t[ty, tx]
        vis = inside & (zz < sub) & (col[..., 3] > 80)
        img[y0:y1 + 1, x0:x1 + 1][vis] = col[..., :3][vis]
    else:
        vis = inside & (zz < sub)
        img[y0:y1 + 1, x0:x1 + 1][vis] = 200
    sub[vis] = zz[vis]
Image.fromarray(img.clip(0, 255).astype(np.uint8)).save(out)
print(len(faces), 'triangles', len(V), 'vertices')
