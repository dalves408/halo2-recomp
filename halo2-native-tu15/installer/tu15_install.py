r"""Halo 2 native (recomp) installer: retail disc -> title update 1.5.

The player supplies only their own retail Halo 2 disc (an .iso/.xiso image,
a mounted/physical drive, or an already-extracted folder). This script:
  1. locates default.xbe on the disc and checks its SHA-256 against the known
     retail 1.0 build (cert version 3);
  2. copies the disc's game data (maps, bink, media, ...) to <dest>;
  3. binary-patches default.xbe and downloader.xbe to the TU 0000000300000803
     ("1.5", cert 0x803) builds with the bundled bsdiff patches, and verifies
     the results byte-for-byte by SHA-256 before writing them;
  4. writes <dest>/tu15-install.json describing what was installed.

The recompiled halo2_recomp.exe of the TU build is compiled from the 1.5 code,
so nothing else is needed: the $u update folder is deliberately NOT created
(the 1.5 exe would otherwise try to relaunch into it). Map packs are a
separate, optional download/installer; the game runs at 1.5 without them.

Pure Python 3.8+, standard library only.

    python tu15_install.py --disc "Halo 2.iso" --dest ..\game
    python tu15_install.py --disc E:\ --dest ..\game
    python tu15_install.py --disc C:\extracted\halo2 --dest ..\game --xbe-only
"""
import argparse, bz2, hashlib, json, os, shutil, struct, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "patches" / "tu15-manifest.json"
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: plain xiso, XGD2 redump (Halo 2), XGD1, XGD3.
PARTITION_OFFSETS = (0, 0xFD90000, 0x18300000, 0x2080000)
SECTOR = 2048


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ---------- bspatch (BSDIFF40 format, as written by bsdiff/bsdiff4) ----------
def _offtin(b):
    v = int.from_bytes(b[:7] + bytes([b[7] & 0x7F]), "little")
    return -v if b[7] & 0x80 else v


def bspatch(old, patch):
    if patch[:8] != b"BSDIFF40":
        raise ValueError("not a BSDIFF40 patch")
    clen, dlen, nsize = _offtin(patch[8:16]), _offtin(patch[16:24]), _offtin(patch[24:32])
    ctrl = bz2.decompress(patch[32:32 + clen])
    diff = bz2.decompress(patch[32 + clen:32 + clen + dlen])
    extra = bz2.decompress(patch[32 + clen + dlen:])
    new = bytearray(nsize)
    op = np_ = dp = ep = cp = 0
    while np_ < nsize:
        x, y, z = (_offtin(ctrl[cp + i * 8:cp + i * 8 + 8]) for i in range(3))
        cp += 24
        # add x bytes of diff to old
        seg = bytearray(diff[dp:dp + x])
        lo = max(0, -op)
        hi = min(x, len(old) - op)
        for i in range(lo, hi):
            seg[i] = (seg[i] + old[op + i]) & 0xFF
        new[np_:np_ + x] = seg
        np_ += x; op += x; dp += x
        new[np_:np_ + y] = extra[ep:ep + y]
        np_ += y; ep += y; op += z
    return bytes(new)


# ---------- disc access ----------
class FolderDisc:
    def __init__(self, root):
        self.root = Path(root)
    def files(self):
        for p in sorted(self.root.rglob("*")):
            if p.is_file():
                yield p.relative_to(self.root).as_posix(), p.stat().st_size
    def read(self, rel):
        return (self.root / rel).read_bytes()
    def copy(self, rel, dst):
        shutil.copyfile(self.root / rel, dst)


class XisoDisc:
    def __init__(self, path):
        self.f = open(path, "rb")
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            hdr = self.f.read(0x1C)
            if hdr[:20] == XDVDFS_MAGIC:
                self.base = off
                self.root_sector, self.root_size = struct.unpack_from("<II", hdr, 20)
                break
        else:
            raise ValueError(f"{path}: no XDVDFS game partition found (not an Xbox disc image?)")
        self.entries = {}
        self._walk(self.root_sector, self.root_size, "")

    def _walk(self, sector, size, prefix):
        if size == 0:
            return
        self.f.seek(self.base + sector * SECTOR)
        table = self.f.read(size)
        seen, stack = set(), [0]
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
            if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
                continue
            name = table[off + 14:off + 14 + nlen].decode("latin-1")
            path = prefix + name
            if attr & 0x10:
                self._walk(start, length, path + "/")
            else:
                self.entries[path] = (start, length)
            if left and left != 0xFFFF:
                stack.append(left * 4)
            if right and right != 0xFFFF:
                stack.append(right * 4)

    def files(self):
        for k in sorted(self.entries):
            yield k, self.entries[k][1]

    def _find(self, rel):
        for k in self.entries:
            if k.lower() == rel.lower():
                return self.entries[k]
        raise FileNotFoundError(rel)

    def read(self, rel):
        start, length = self._find(rel)
        self.f.seek(self.base + start * SECTOR)
        return self.f.read(length)

    def copy(self, rel, dst):
        start, length = self._find(rel)
        self.f.seek(self.base + start * SECTOR)
        with open(dst, "wb") as out:
            left = length
            while left:
                chunk = self.f.read(min(left, 8 << 20))
                if not chunk:
                    raise IOError(f"short read in {rel}")
                out.write(chunk); left -= len(chunk)


def open_disc(path):
    p = Path(path)
    if p.is_dir():
        return FolderDisc(p)
    return XisoDisc(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--disc", required=True, help="retail disc: .iso/.xiso image, drive (E:\\) or extracted folder")
    ap.add_argument("--dest", required=True, help="output game folder (the recomp's game\\)")
    ap.add_argument("--xbe-only", action="store_true", help="only write the patched xbe files (data already present)")
    ap.add_argument("--force", action="store_true", help="overwrite existing files")
    a = ap.parse_args()

    manifest = json.loads(MANIFEST.read_text())
    disc = open_disc(a.disc)
    listing = dict(disc.files())
    dest = Path(a.dest); dest.mkdir(parents=True, exist_ok=True)

    # 1. verify retail xbes and build the 1.5 ones in memory first (nothing written on failure)
    patched = {}
    for f in manifest["files"]:
        src = disc.read(f["name"])
        h = sha256(src)
        if h == f["target_sha256"]:
            print(f"{f['name']}: disc already carries the 1.5 build")
            patched[f["name"]] = src
            continue
        if h != f["source_sha256"]:
            sys.exit(f"ERROR: {f['name']} on the disc has SHA-256 {h}, expected the retail Halo 2 1.0 build "
                     f"{f['source_sha256']}. This disc/dump is not supported.")
        patch = (HERE / f["patch"]).read_bytes()
        if sha256(patch) != f["patch_sha256"]:
            sys.exit(f"ERROR: patch file {f['patch']} is corrupt")
        t0 = time.time()
        new = bspatch(src, patch)
        if sha256(new) != f["target_sha256"] or len(new) != f["target_size"]:
            sys.exit(f"ERROR: patched {f['name']} does not match the 1.5 build")
        print(f"{f['name']}: retail 1.0 verified, patched to 1.5 ({time.time()-t0:.1f}s), SHA-256 OK")
        patched[f["name"]] = new

    # 2. copy disc data
    copied = 0
    if not a.xbe_only:
        total = sum(listing.values())
        done = 0
        for rel, size in listing.items():
            if rel.lower() in (n.lower() for n in patched):
                continue
            out = dest / rel
            if out.exists() and out.stat().st_size == size and not a.force:
                done += size; continue
            out.parent.mkdir(parents=True, exist_ok=True)
            disc.copy(rel, out)
            copied += 1; done += size
            print(f"  [{done * 100 // max(total, 1):3d}%] {rel}")

    # 3. write the 1.5 xbes
    for name, data in patched.items():
        (dest / name).write_bytes(data)
    info = dict(installed=time.strftime("%Y-%m-%d %H:%M:%S"), version="1.5 (TU 0000000300000803)",
                files={n: sha256(d) for n, d in patched.items()}, copied_data_files=copied,
                map_packs="not part of this install (separate optional download)")
    (dest / "tu15-install.json").write_text(json.dumps(info, indent=2))
    print(f"Done: {dest} is Halo 2 1.5.")


if __name__ == "__main__":
    main()
