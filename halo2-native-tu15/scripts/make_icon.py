"""Draws the game's icon (olive armour plate with a gold visor) into res/halo2.ico (16-256 px) and res/halo2.png (512 px).

    python scripts/make_icon.py

The same icon is on every exe (res/*.rc), Setup, its shortcuts and the wizard (scripts/build_installer.py)."""
from pathlib import Path

from PIL import Image, ImageDraw

RES = Path(__file__).resolve().parent.parent / 'res'
N = 1024
K = N / 112.0


def pts(points):
    return [(x * K, y * K) for x, y in points]


def quad(p0, c, p1, n=24):
    return [((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * c[0] + t * t * p1[0],
             (1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * c[1] + t * t * p1[1]) for t in (i / n for i in range(n + 1))]


def draw():
    img = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, N - 1, N - 1], radius=int(22 * K), fill='#1d2418')
    d.polygon(pts([(20, 30), (40, 14), (72, 14), (92, 30), (92, 70), (74, 96), (38, 96), (20, 70)]),
              fill='#4f5d34', outline='#2c3520', width=int(2 * K))
    d.polygon(pts([(26, 34), (42, 22), (70, 22), (86, 34), (86, 66), (70, 86), (42, 86), (26, 66)]), fill='#5f6f3e')
    d.polygon(pts([(28, 44), (84, 44), (78, 64)] + quad((78, 64), (56, 72), (34, 64))[1:]), fill='#c8901e')
    d.polygon(pts([(34, 47), (60, 47), (44, 60)] + quad((44, 60), (38, 60), (35, 58))[1:]), fill='#f4cf6a')
    d.polygon(pts([(66, 50), (80, 48), (77, 58)]), fill='#7a4f0c')
    return img


def main():
    img = draw()
    sizes = [256, 128, 64, 48, 32, 24, 16]
    icons = [img.resize((s, s), Image.LANCZOS) for s in sizes]
    icons[0].save(RES / 'halo2.ico', format='ICO', sizes=[(s, s) for s in sizes], append_images=icons[1:])
    img.resize((512, 512), Image.LANCZOS).save(RES / 'halo2.png')
    print('wrote', RES / 'halo2.ico', RES / 'halo2.png')


if __name__ == '__main__':
    main()
