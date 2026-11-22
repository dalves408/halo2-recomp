"""Converts the title update 1.5 patches (installer/patches/*.tu15.bsdiff, BSDIFF40 with bzip2 blocks) to the
uncompressed form halo2_import.exe applies (*.tu15.h2p), and checks each one against the retail disc's files.

    python scripts/convert_tu_patches.py [--retail-dir <folder with the retail default.xbe and downloader.xbe>]

.h2p is BSDIFF40 with its three blocks stored as they are: "H2TUPAT1", then little-endian int64 control length,
diff length and new size, then the control block (offtin triples), the diff block and the extra block. Windows has
no bzip2, and the release ZIP and Setup compress the files anyway. The patches are derived from Microsoft's title
update, so neither form is committed (.gitignore); scripts/build_installer.py ships the .h2p files beside
halo2_import.exe. With --retail-dir every patch is applied and the result checked against
installer/patches/tu15-manifest.json.
"""
import argparse
import bz2
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PATCHES = ROOT / 'installer' / 'patches'


def offtin(b):
    v = struct.unpack('<q', b)[0]
    return -(v & 0x7FFFFFFFFFFFFFFF) if v < 0 else v


def read_bsdiff(path):
    data = path.read_bytes()
    if data[:8] != b'BSDIFF40':
        sys.exit(f'{path.name}: not a BSDIFF40 patch')
    clen, dlen, new_size = (offtin(data[8 + 8 * i:16 + 8 * i]) for i in range(3))
    ctrl = bz2.decompress(data[32:32 + clen])
    diff = bz2.decompress(data[32 + clen:32 + clen + dlen])
    extra = bz2.decompress(data[32 + clen + dlen:])
    return new_size, ctrl, diff, extra


def apply(old, new_size, ctrl, diff, extra):
    """bspatch, the same steps as halo2_import.c's."""
    new = bytearray(new_size)
    op = np = cp = dp = ep = 0
    while np < new_size:
        x, y, z = (offtin(ctrl[cp + 8 * i:cp + 8 * i + 8]) for i in range(3))
        cp += 24
        for i in range(x):
            new[np + i] = (diff[dp + i] + (old[op + i] if 0 <= op + i < len(old) else 0)) & 0xFF
        dp += x
        np += x
        op += x
        new[np:np + y] = extra[ep:ep + y]
        ep += y
        np += y
        op += z
    return bytes(new)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--retail-dir', type=Path)
    a = ap.parse_args()
    manifest = json.loads((PATCHES / 'tu15-manifest.json').read_text(encoding='utf-8'))
    for f in manifest['files']:
        src = ROOT / 'installer' / f['patch']
        if hashlib.sha256(src.read_bytes()).hexdigest() != f['patch_sha256']:
            sys.exit(f'{src.name}: does not match tu15-manifest.json')
        new_size, ctrl, diff, extra = read_bsdiff(src)
        if new_size != f['target_size']:
            sys.exit(f'{src.name}: new size {new_size}, manifest says {f["target_size"]}')
        out = PATCHES / (f['name'] + '.tu15.h2p')
        out.write_bytes(b'H2TUPAT1' + struct.pack('<qqq', len(ctrl), len(diff), new_size) + ctrl + diff + extra)
        print(f'{out.name}: {out.stat().st_size:,} bytes')
        if a.retail_dir:
            old = (a.retail_dir / f['name']).read_bytes()
            if hashlib.sha256(old).hexdigest() != f['source_sha256']:
                sys.exit(f'{f["name"]} in {a.retail_dir} is not the retail file')
            got = hashlib.sha256(apply(old, new_size, ctrl, diff, extra)).hexdigest()
            if got != f['target_sha256']:
                sys.exit(f'{f["name"]}: patched result does not match title update 1.5')
            print(f'  {f["name"]}: retail -> 1.5 checked')


if __name__ == '__main__':
    main()
