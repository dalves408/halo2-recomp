"""Builds the Halo 2 Recomp player release: the programs, then the Setup and the RecompLauncher ZIP
(after Marvel vs. Capcom 2's build_installer.py).

    python scripts/build_installer.py --version 1.0.0 [--skip-build] [--iscc <ISCC.exe>]

Output in installer/Output: Halo2-Recomp-Setup-<version>.exe and Halo2-Recomp-v<version>-Windows-x64.zip, each with a
.sha256, and Halo2-Recomp-HiRes-Textures-<n>-Setup.exe when installer/texture_payload.iss exists. The game programs hold code translated from the game, so none of these is committed. The programs' .pdb files are kept in
out/symbols/<version> (scripts/symbolize_log.py) and never shipped.

Needs: Visual Studio 2022 C++ build tools, CMake, Ninja, Inno Setup 6.6 or newer, Python with Pillow, the generated
code (src/recomp/gen, src/downloader/gen: the recomp pipeline, from your own default.xbe and downloader.xbe).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / 'build-release'
INSTALLER = ROOT / 'installer'
OUTPUT = INSTALLER / 'Output'
SMALL_SIZES = (55, 64, 80, 96, 128)
PROGRAMS = ('halo2_recomp', 'halo2_downloader', 'Config', 'Insignia Setup', 'Halo2 Updater', 'halo2_import',
            'halo2_texture_download')

# The Halo 2 manuals' palette (src/halo_ui.c)
BG_TOP, BG_MID, BG_BOT = (14, 52, 104), (8, 33, 72), (3, 13, 32)
STREAK, EDGE, BAND, WHITE, CYAN, ORANGE = (20, 62, 118), (70, 140, 214), (16, 62, 122), (242, 246, 251), (130, 206, 255), (247, 148, 29)


def find_iscc():
    """Inno Setup 6's compiler: --iscc or ISCC_EXE if given, else a per-user install, else a machine-wide one, else
    ISCC on the PATH."""
    places = [os.environ.get('ISCC_EXE', '')]
    if os.environ.get('LOCALAPPDATA'):
        places.append(str(Path(os.environ['LOCALAPPDATA']) / 'Programs' / 'Inno Setup 6' / 'ISCC.exe'))
    for env in ('ProgramFiles(x86)', 'ProgramFiles'):
        if os.environ.get(env):
            places.append(str(Path(os.environ[env]) / 'Inno Setup 6' / 'ISCC.exe'))
    places.append(shutil.which('ISCC') or '')
    for place in places:
        if place and Path(place).is_file():
            return Path(place)
    sys.exit('Inno Setup 6 (ISCC.exe) was not found. Install Inno Setup 6.6 or newer, or pass --iscc <path to ISCC.exe> '
             '(or set ISCC_EXE). Looked in:\n  ' + '\n  '.join(p for p in places if p))


def run(*command):
    print('>', ' '.join(str(c) for c in command), flush=True)
    subprocess.run([str(c) for c in command], check=True, cwd=ROOT)


def set_version(version):
    """The version resources (res/*.rc) and the update check's own number (src/version.h) equal the release number."""
    parts = version.split('.')
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        sys.exit('version must look like 1.0.0')
    commas, dots = ','.join(parts + ['0']), '.'.join(parts + ['0'])
    for rc in ('halo2.rc', 'config.rc', 'tools.rc'):
        path = ROOT / 'res' / rc
        text = path.read_text(encoding='utf-8')
        new = re.sub(r'(FILEVERSION|PRODUCTVERSION) [\d,]+', rf'\1 {commas}', text)
        new = re.sub(r'("(?:FileVersion|ProductVersion)", ")[\d.]+', rf'\g<1>{dots}', new)
        if new != text:
            path.write_text(new, encoding='utf-8')
    path = ROOT / 'src' / 'version.h'
    text = path.read_text(encoding='utf-8')
    new = re.sub(r'#define H2N_VERSION "[^"]*"', f'#define H2N_VERSION "{version}"', text)
    if new != text:
        path.write_text(new, encoding='utf-8')


# ---- the wizard's pictures -------------------------------------------------------------------------------------
def font(size, bold=True):
    for name in ('bahnschrift.ttf', 'segoeuib.ttf' if bold else 'segoeui.ttf', 'arialbd.ttf'):
        path = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Fonts' / name
        if path.is_file():
            f = ImageFont.truetype(str(path), size)
            if name == 'bahnschrift.ttf':
                try:
                    f.set_variation_by_name('Bold' if bold else 'Regular')
                except Exception:
                    pass
            return f
    return ImageFont.load_default()


def gradient(w, h):
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    mid = h * 2 // 5
    for y in range(h):
        if y < mid:
            a, b, t = BG_TOP, BG_MID, y / mid
        else:
            a, b, t = BG_MID, BG_BOT, (y - mid) / (h - mid)
        d.line([(0, y), (w, y)], fill=tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3)))
    return img


def chamfer(d, box, c, fill, outline, width=2):
    x0, y0, x1, y1 = box
    d.polygon([(x0 + c, y0), (x1, y0), (x1, y1 - c), (x1 - c, y1), (x0, y1), (x0, y0 + c)], fill=fill, outline=outline,
              width=width)


def wizard_images():
    icon = Image.open(ROOT / 'res' / 'halo2.png').convert('RGBA')

    # behind every page (497:360): the menus' gradient, darker so the page text stays easy to read, and the arcs
    w, h = 1491, 1080
    back = gradient(w, h)
    dark = Image.new('RGB', (w, h), (0, 0, 0))
    back = Image.blend(back, dark, 0.35)
    d = ImageDraw.Draw(back)
    d.arc([-w // 3, h // 5, w * 4 // 3, h * 2], 200, 330, fill=STREAK, width=4)
    d.arc([-w // 2, h // 3, w * 5 // 4, h * 9 // 4], 205, 335, fill=STREAK, width=3)
    back.save(INSTALLER / 'wizard_back.png')

    # the Welcome and Finished pages' side picture (164:314): an angled panel with the icon and the name
    w, h = 410, 785
    side = gradient(w, h)
    d = ImageDraw.Draw(side)
    chamfer(d, (14, 14, w - 15, h - 15), 44, None, EDGE, 3)
    chamfer(d, (30, 40, w - 31, 120), 22, BAND, EDGE, 2)
    title = font(54)
    tw = d.textlength('HALO 2', font=title)
    d.text(((w - tw) / 2, 50), 'HALO 2', font=title, fill=WHITE)
    size = 260
    side.paste(icon.resize((size, size), Image.LANCZOS), ((w - size) // 2, 190), icon.resize((size, size), Image.LANCZOS))
    sub = font(40)
    sw = d.textlength('RECOMP', font=sub)
    d.text(((w - sw) / 2, 480), 'RECOMP', font=sub, fill=CYAN)
    d.line([(70, 545), (w - 70, 545)], fill=EDGE, width=2)
    small = font(22, bold=False)
    for i, line in enumerate(('Original Xbox Halo 2,', 'title update 1.5,', 'native on Windows')):
        lw = d.textlength(line, font=small)
        d.text(((w - lw) / 2, 570 + i * 32), line, font=small, fill=(210, 224, 240))
    for i in range(3):        # the manuals' orange chevrons
        y = 700 + i * 16
        d.line([(w / 2 - 40, y), (w / 2, y + 14), (w / 2 + 40, y)], fill=ORANGE, width=6)
    side.save(INSTALLER / 'wizard_image.png')

    # top right of every inner page: the icon on the band's blue
    for size in SMALL_SIZES:
        canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        canvas.alpha_composite(icon.resize((size, size), Image.LANCZOS))
        canvas.save(INSTALLER / f'wizard_small_{size}.png')


def keep_symbols(version):
    """Each program and its .pdb, kept on this PC and never shipped: scripts/symbolize_log.py names the code offsets in
    a player's [FREEZE] or [CRASH] report with them. out/ is git-ignored."""
    keep = ROOT / 'out' / 'symbols' / version
    keep.mkdir(parents=True, exist_ok=True)
    for name in PROGRAMS:
        for ext in ('.exe', '.pdb'):
            src = BUILD / (name + ext)
            if not src.is_file():
                if ext == '.pdb' and name in ('halo2_recomp', 'halo2_downloader'):
                    sys.exit(f'{src} is missing: the release build makes it (CMakeLists.txt, H2_RELEASE)')
                continue
            shutil.copyfile(src, keep / (name + ext))
    print(f'Symbols kept in {keep}', flush=True)


def write_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 22), b''):
            h.update(block)
    digest = h.hexdigest()
    path.with_name(path.name + '.sha256').write_text(f'{digest}  {path.name}\n')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--version', required=True)
    parser.add_argument('--skip-build', action='store_true', help='use the programs already in build-release')
    parser.add_argument('--iscc', help="Inno Setup 6's ISCC.exe, if it is not found by itself")
    args = parser.parse_args()
    if args.iscc:
        os.environ['ISCC_EXE'] = args.iscc
    iscc = find_iscc()
    print(f'Inno Setup: {iscc}', flush=True)
    for gen in ('src/recomp/gen', 'src/downloader/gen'):
        if not any((ROOT / gen).glob('*.c')):
            sys.exit(f'{gen} is empty: run the recomp pipeline first (README, Building)')

    set_version(args.version)
    if not args.skip_build:
        run('cmd', '/c', ROOT / 'build_release.cmd')
    keep_symbols(args.version)
    # title update 1.5 for players without their own (installer/patches/*.h2p, in the Setup and the ZIP)
    run(sys.executable, ROOT / 'scripts' / 'convert_tu_patches.py')
    wizard_images()
    OUTPUT.mkdir(exist_ok=True)
    run(iscc, f'/DAppVersion={args.version}', f'/DBuildDir={BUILD}', INSTALLER / 'halo2.iss')
    setup = OUTPUT / f'Halo2-Recomp-Setup-{args.version}.exe'
    print(f'Setup: {setup}\nSHA-256: {write_sha256(setup)}')
    run(sys.executable, ROOT / 'scripts' / 'recomplauncher_manifest.py', '--version', args.version, '--build', BUILD)
    # The hi-res texture addon, once scripts/build_texture_pack.py has written texture_payload.iss. Its .bin parts
    # (out/texture-release/revision-<n>) go on the textures-<n> release; the game Setup above offers the same pack as
    # an optional task.
    payload = INSTALLER / 'texture_payload.iss'
    if payload.is_file():
        revision = re.search(r'#define TextureVersion "(\d+)"', payload.read_text(encoding='ascii')).group(1)
        run(iscc, f'/DBuildDir={BUILD}', INSTALLER / 'halo2_textures.iss')
        addon = OUTPUT / f'Halo2-Recomp-HiRes-Textures-{revision}-Setup.exe'
        print(f'Texture addon: {addon}\nSHA-256: {write_sha256(addon)}')
    else:
        print('No texture_payload.iss: the game Setup has no texture task and no addon was built '
              '(run scripts/build_texture_pack.py first).')


if __name__ == '__main__':
    main()
