#!/usr/bin/env python3
# builds icon0/bg0/startup from the game's own apk art
# usage: livearea_from_apk.py <apk> <out dir>
import io, os, shutil, subprocess, sys, zipfile
from PIL import Image

apk, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
z = zipfile.ZipFile(apk)

def load(name):
    return Image.open(io.BytesIO(z.read(name))).convert('RGBA')

def fit(img, w, h):
    # cover-crop to the target aspect, then scale
    src_ratio, dst_ratio = img.width / img.height, w / h
    if src_ratio > dst_ratio:
        nw = int(img.height * dst_ratio)
        x = (img.width - nw) // 2
        img = img.crop((x, 0, x + nw, img.height))
    else:
        nh = int(img.width / dst_ratio)
        y = (img.height - nh) // 2
        img = img.crop((0, y, img.width, y + nh))
    return img.resize((w, h), Image.LANCZOS)

def save(img, name):
    path = os.path.join(out, name)
    bg = Image.new('RGBA', img.size, (0, 0, 0, 255))
    bg.alpha_composite(img)
    bg.convert('RGB').save(path)
    # the shell wants 8-bit indexed pngs
    subprocess.run(['pngquant', '--force', '--ext', '.png', '256', path], check=True)

splash = load('res/drawable/splash_1280x752.png')
save(fit(load('res/drawable/icon.png'), 128, 128), 'icon0.png')
save(fit(splash, 840, 500), 'bg0.png')
save(fit(splash, 280, 158), 'startup.png')
shutil.copy(os.path.join(os.path.dirname(__file__), '..', 'livearea', 'template.xml'), out)
