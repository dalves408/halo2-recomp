"""Store listing art for a release: icon sizes, a transparent logo, box art and screenshots, in the Setup wizard's
style (scripts/build_installer.py: the Halo 2 manuals' palette, Bahnschrift).

    python scripts/make_store_assets.py --version 1.0.0 --box-bg <capture.png> --shots <capture.png> [...]

Writes installer/Output/store-<version>/ (git-ignored):
    icon/    icon-1024.png ... icon-64.png (scripts/make_icon.py's icon) and halo2.ico
    logo/    logo.png (HALO 2 / RECOMP wordmark, transparent) and logo-icon.png (icon beside the wordmark)
    box/     cover-1200x1800.png, cover-600x900.png (2:3 box art), backdrop-3840x2160.png (16:9, no text: the
             store page shows the logo over it)
    screenshots/  NN.png at the capture's size and NN-1920x1080.png
--box-bg is a capture to crop the cover from (a cutscene close-up; letterbox bars are cut off). Captures come from the
DEL key in a development build (Launch-Halo2-StoreShots.cmd: 2560x1440).
"""
import argparse
import shutil
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_icon                                    # noqa: E402
from build_installer import (BG_BOT, BG_MID, BG_TOP, CYAN, EDGE, ORANGE, WHITE,  # noqa: E402
                             chamfer, font, gradient)

ROOT = Path(__file__).resolve().parent.parent


def wordmark(scale=1.0, shadow=True):
    """HALO 2 over a ruled RECOMP, on transparency."""
    big, small = font(int(260 * scale)), font(int(110 * scale))
    probe = ImageDraw.Draw(Image.new('RGBA', (1, 1)))
    tw = probe.textlength('HALO 2', font=big)
    rw = probe.textlength('R E C O M P', font=small)
    pad = int(40 * scale)
    w = int(max(tw, rw + 2 * 160 * scale)) + 2 * pad
    h = int(440 * scale) + 2 * pad
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text(((w - tw) / 2, pad - 20 * scale), 'HALO 2', font=big, fill=WHITE)
    y = pad + 300 * scale
    d.text(((w - rw) / 2, y), 'R E C O M P', font=small, fill=CYAN)
    mid = y + 62 * scale
    gap = 34 * scale
    d.line([(pad, mid), ((w - rw) / 2 - gap, mid)], fill=EDGE, width=max(2, int(8 * scale)))
    d.line([((w + rw) / 2 + gap, mid), (w - pad, mid)], fill=EDGE, width=max(2, int(8 * scale)))
    if shadow:          # a soft dark edge so it reads on light backgrounds too
        a = img.split()[3].filter(ImageFilter.GaussianBlur(10 * scale))
        a = a.point(lambda v: min(255, int(v * 0.55)))
        back = Image.new('RGBA', img.size, (2, 8, 20, 0))
        back.putalpha(a)
        img = Image.alpha_composite(back, img)
    return img


def chevrons(d, cx, y, size, n=3):
    for i in range(n):
        yy = y + i * size * 0.42
        d.line([(cx - size, yy), (cx, yy + size * 0.35), (cx + size, yy)], fill=ORANGE, width=max(3, int(size * 0.15)))


def crop_bars(img):
    """Cut a capture's black letterbox bars."""
    g = img.convert('L')
    w, h = g.size
    rows = [y for y in range(h) if g.crop((0, y, w, y + 1)).getextrema()[1] > 12]
    return img.crop((0, rows[0], w, rows[-1] + 1)) if rows else img


def grade(img, darken=0.82):
    img = ImageEnhance.Contrast(img).enhance(1.08)
    return ImageEnhance.Brightness(img).enhance(darken)


def vertical_fade(size, stops):
    """A black-to-clear mask: stops are (y fraction, alpha 0..255)."""
    w, h = size
    m = Image.new('L', (1, h))
    for y in range(h):
        t = y / (h - 1)
        for (t0, a0), (t1, a1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                m.putpixel((0, y), int(a0 + (a1 - a0) * (t - t0) / max(1e-6, t1 - t0)))
                break
    return m.resize((w, h))


def tint_layer(size, mask, color):
    layer = Image.new('RGBA', size, color + (0,))
    layer.putalpha(mask)
    return layer


def cover(bg, icon, w=1200, h=1800):
    src = crop_bars(bg.convert('RGB'))
    sw, sh = src.size
    cw = int(sh * w / h)
    cx = int(sw * 0.54)                                       # the helmet sits a little right of centre
    left = max(0, min(sw - cw, cx - cw // 2))
    art = src.crop((left, 0, left + cw, sh)).resize((w, h), Image.LANCZOS)
    art = art.filter(ImageFilter.UnsharpMask(radius=3, percent=60, threshold=2))
    art = grade(art).convert('RGBA')
    art = Image.alpha_composite(art, tint_layer((w, h), vertical_fade((w, h), [(0, 235), (0.30, 60), (0.55, 0), (1, 0)]), BG_MID))
    art = Image.alpha_composite(art, tint_layer((w, h), vertical_fade((w, h), [(0, 0), (0.62, 0), (0.82, 215), (1, 250)]), BG_BOT))
    d = ImageDraw.Draw(art)
    chamfer(d, (22, 22, w - 23, h - 23), 70, None, EDGE, 5)
    logo = wordmark(scale=w / 1200 * 1.08)
    art.alpha_composite(logo, ((w - logo.width) // 2, int(h * 0.035)))
    # bottom band: icon, tagline, chevrons
    ic = icon.resize((int(w * 0.16),) * 2, Image.LANCZOS)
    art.alpha_composite(ic, ((w - ic.width) // 2, int(h * 0.735)))
    f = font(int(w * 0.036), bold=False)
    for i, line in enumerate(('ORIGINAL XBOX  ·  TITLE UPDATE 1.5', 'NATIVE ON WINDOWS')):
        lw = d.textlength(line, font=f)
        d.text(((w - lw) / 2, h * 0.855 + i * w * 0.05), line, font=f, fill=(215, 228, 244))
    chevrons(d, w / 2, h * 0.945, w * 0.035)
    return art


def backdrop(bg, w=3840, h=2160):
    """The cover's capture at 16:9, darker on the left where a store puts the logo and text."""
    src = crop_bars(bg.convert('RGB'))
    sw, sh = src.size
    zoom = 1.35                                   # the helmet (54 % across) lands about 73 % across
    cw = int(sw / zoom)
    ch = int(cw * h / w)
    top = max(0, min(sh - ch, int(sh * 0.08)))
    art = src.crop((0, top, cw, top + ch)).resize((w, h), Image.LANCZOS)
    art = grade(art.filter(ImageFilter.UnsharpMask(radius=4, percent=50, threshold=2)), 0.78).convert('RGBA')
    shade = Image.new('L', (w, 1))
    for x in range(w):
        shade.putpixel((x, 0), int(170 * max(0.0, 1 - x / (w * 0.55))))
    art = Image.alpha_composite(art, tint_layer((w, h), shade.resize((w, h)), BG_BOT))
    return Image.alpha_composite(art, tint_layer((w, h), vertical_fade((w, h), [(0, 0), (0.7, 0), (1, 170)]), BG_BOT))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--version', required=True)
    p.add_argument('--box-bg', required=True, help='capture for the cover (a cutscene close-up)')
    p.add_argument('--shots', nargs='+', required=True, help='captures, in store order')
    a = p.parse_args()

    out = ROOT / 'installer' / 'Output' / f'store-{a.version}'
    out.mkdir(parents=True, exist_ok=True)
    for old in out.iterdir():          # empty it (the folder itself may be open in Explorer)
        shutil.rmtree(old) if old.is_dir() else old.unlink()
    for sub in ('icon', 'logo', 'box', 'screenshots'):
        (out / sub).mkdir()

    icon = make_icon.draw()                                   # 1024 px, transparent corners
    for s in (1024, 512, 256, 128, 64):
        (icon if s == 1024 else icon.resize((s, s), Image.LANCZOS)).save(out / 'icon' / f'icon-{s}.png')
    shutil.copyfile(ROOT / 'res' / 'halo2.ico', out / 'icon' / 'halo2.ico')

    wordmark(scale=1.0).save(out / 'logo' / 'logo.png')
    wm = wordmark(scale=1.0)
    ic = icon.resize((wm.height,) * 2, Image.LANCZOS)
    lock = Image.new('RGBA', (ic.width + 40 + wm.width, wm.height), (0, 0, 0, 0))
    lock.alpha_composite(ic, (0, 0))
    lock.alpha_composite(wm, (ic.width + 40, 0))
    lock.save(out / 'logo' / 'logo-icon.png')

    c = cover(Image.open(a.box_bg), icon)
    c.convert('RGB').save(out / 'box' / 'cover-1200x1800.png')
    c.resize((600, 900), Image.LANCZOS).convert('RGB').save(out / 'box' / 'cover-600x900.png')
    backdrop(Image.open(a.box_bg)).convert('RGB').save(out / 'box' / 'backdrop-3840x2160.png')

    for i, s in enumerate(a.shots, 1):
        im = Image.open(s).convert('RGB')
        im.save(out / 'screenshots' / f'{i:02d}.png', optimize=True)
        im.resize((1920, 1080), Image.LANCZOS).save(out / 'screenshots' / f'{i:02d}-1920x1080.png', optimize=True)
    print(f'Store art: {out}')


if __name__ == '__main__':
    main()
