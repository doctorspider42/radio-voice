#!/usr/bin/env python3
"""Render the existing RadioVoice icon and the Finder installer's layout assets.

Requires Pillow; the generated ICNS and Retina TIFF are committed resources.
"""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('radiovoice_icon', ROOT / 'tools/make-icon.py')
icon = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icon)


def background(scale):
    w, h = 720 * scale, 500 * scale
    image = Image.new('RGB', (w, h))
    draw = ImageDraw.Draw(image)
    for y in range(h):
        t = y / h
        draw.line((0, y, w, y), fill=tuple(int(a + (b-a)*t) for a,b in zip((31,37,46),(11,15,21))))
    def box(rect, fill, outline=None, radius=12):
        draw.rounded_rectangle(tuple(int(v*scale) for v in rect), radius=radius*scale, fill=fill, outline=outline, width=scale)
    def text(x, y, label, size, color, bold=False, center=False):
        path = '/System/Library/Fonts/Supplemental/Arial' + (' Bold' if bold else '') + '.ttf'
        font = ImageFont.truetype(path, int(size*scale))
        if center:
            x -= draw.textlength(label, font=font) / scale / 2
        draw.text((int(x*scale),int(y*scale)), label, font=font, fill=color)
    accent = (61,209,196)
    # Header and signal trace echo the rack's faceplate and meter accents.
    logo = icon._draw(48, 3).resize((48*scale,48*scale), Image.Resampling.LANCZOS)
    image.paste(logo, (40*scale,34*scale), logo)
    text(103,42,'RADIOVOICE',24,(234,240,245),True)
    box((584,44,674,72),(28,52,54),(44,109,107),14)
    text(629,51,'macOS',12,accent,True,True)
    text(360,115,'Your mic. Your sound.',32,(239,244,247),True,True)
    text(360,160,'A studio voice, in every app.',15,(151,166,181),center=True)
    # The two wells leave room for Finder's real icons and filenames.
    for x in (360,):
        box((x-90,209,x+90,351),(30,38,47),(51,72,80),24)
        # Finder uses black filename labels for image backgrounds, regardless
        # of system appearance. Give those labels their own light nameplates.
        box((x-79,329,x+79,353),(213,229,231),radius=7)
        draw.line((int((x-59)*scale),int(353*scale),int((x+59)*scale),int(353*scale)), fill=accent,width=2*scale)
    text(360,380,'Double-click to install / Kliknij dwukrotnie',18,(231,241,244),True,True)
    text(360,414,'RadioVoice + virtual microphone / wirtualny mikrofon',13,(146,161,176),center=True)
    draw.line((40*scale,465*scale,680*scale,465*scale), fill=(44,57,68),width=scale)
    text(360,478,'MICROPHONE  /  EFFECTS  /  YOUR APPS',10,(114,139,150),True,True)
    return image


def main():
    with tempfile.TemporaryDirectory(prefix='radiovoice-assets-') as temp:
        temp = Path(temp)
        iconset = temp / 'RadioVoice.iconset'
        iconset.mkdir()
        for points in (16,32,128,256,512):
            for scale in (1,2):
                pixels = points*scale
                icon._draw(pixels,2).save(iconset / (f'icon_{points}x{points}' + ('@2x' if scale==2 else '') + '.png'))
        subprocess.run(['/usr/bin/iconutil','-c','icns',str(iconset),'-o',str(ROOT/'res/RadioVoice.icns')],check=True)
        for scale in (1,2):
            background(scale).save(temp / f'background{scale}.png', dpi=(72*scale,72*scale))
        subprocess.run(['/usr/bin/tiffutil','-cathidpicheck',str(temp/'background1.png'),str(temp/'background2.png'),'-out',str(ROOT/'res/dmg-background.tiff')],check=True)


if __name__ == '__main__':
    main()
