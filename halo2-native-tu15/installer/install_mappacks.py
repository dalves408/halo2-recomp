r"""STAGED, NOT YET RUN: install the optional Halo 2 map packs into a recomp SaveData.

Separate from the 1.5 install: the game runs at 1.5 without any packs.
Each pack folder (Bonus / Killtacular / Maptacular / Blastacular) holds
ContentMeta.xbx, ContentImage.xbx, patch.lvl, patch_v3.lvl and its .map files.
It is copied to SaveData\TDATA\4d530064\$c\<offering id, 16 lowercase hex>\ and
its ContentMeta.xbx digest is re-signed for the toolkit's XboxHDKey (16 zero
bytes) in the installed copy only. The source files are never modified.

    python install_mappacks.py --packs <dir with pack folders> --savedata ..\SaveData [--uninstall]

Pack folders are recognised by an offering id 4D5300640000000N in their name
(as in tu15-content\dlc\1-bonus-4D53006400000001). UNVERIFIED: whether the
recompiled XAPI accepts the signature, mounts F: and streams the maps is phase 3.
"""
import argparse, hashlib, hmac, re, shutil, struct
from pathlib import Path

TITLE = "4d530064"

def sign(meta, hdkey=bytes(16)):
    d = bytearray(meta)
    assert d[20:24] == b"XCSF", "not an XCSF ContentMeta"
    # XAPI content enumeration (TU 0x33A739) skips any package whose header flags (+0x20)
    # lack bit 0, the "install complete" bit an installer sets when it finishes. The disc
    # copies of packs 1-3 have it clear (pack 4 has it set), so set it, then sign.
    struct.pack_into("<I", d, 0x20, struct.unpack_from("<I", d, 0x20)[0] | 1)
    hdrsize, = struct.unpack_from("<I", d, 0x18); tid, = struct.unpack_from("<I", d, 0x24)
    key = hmac.new(hdkey, struct.pack("<I", tid), hashlib.sha1).digest()
    d[:20] = hmac.new(key, bytes(d[0x14:hdrsize]), hashlib.sha1).digest()
    return bytes(d)

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--packs", required=True); ap.add_argument("--savedata", required=True)
    ap.add_argument("--uninstall", action="store_true"); a = ap.parse_args()
    root = Path(a.savedata) / "TDATA" / TITLE / "$c"
    for src in sorted(Path(a.packs).iterdir()):
        m = re.search(r"(4d53006400000[0-9a-f]{3})", src.name, re.I)
        if not src.is_dir() or not m or not (src / "ContentMeta.xbx").exists():
            continue
        dst = root / m.group(1).lower()
        if a.uninstall:
            if dst.exists(): shutil.rmtree(dst); print(f"removed {dst}")
            continue
        dst.mkdir(parents=True, exist_ok=True)
        for f in src.iterdir():
            if f.is_file() and f.name.lower() != "contentmeta.xbx":
                shutil.copyfile(f, dst / f.name)
        (dst / "ContentMeta.xbx").write_bytes(sign((src / "ContentMeta.xbx").read_bytes()))
        print(f"installed {src.name} -> {dst}")

if __name__ == "__main__":
    main()
