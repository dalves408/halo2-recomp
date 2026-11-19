"""Write mip_aliases.txt for the hi-res texture pack (src/gpu/tex_pack.c).

Halo 2 also binds textures whose level 0 is the second or third mip of a
bitmap (distant objects, streaming). Those uploads hash differently from the
pack key, so this lists, for every exported bitmap, the hash of each smaller
mip and the pack key it belongs to:

    <alias hash> <w> <h> <base hash> <W> <H>

Mips under 8 texels on a side or 128 bytes are left out (tiny blocks collide
across textures), and so is any alias that matches a real texture's key or two
different bitmaps.

    python -I tools\\make_mip_aliases.py --maps <dir> [--maps <glob>] --shared <dir> --out mip_aliases.txt
"""
import argparse
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_textures as X  # noqa: E402
import h2map as H  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", action="append", required=True)
    ap.add_argument("--shared", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    paths = []
    for m in a.maps:
        paths += sorted(glob.glob(os.path.join(m, "*.map")) if os.path.isdir(m) else glob.glob(m))
    hasher = X.load_hasher()
    ms = H.MapSet(a.shared)
    bases = set()
    aliases = {}            # (hash, w, h) -> set of base keys
    for path in paths:
        m = ms.get(path)
        for tag, tid, k, n, e in m.bitmaps():
            if "error" in e or e["format"] not in H.H2_FORMATS:
                continue
            fmt = H.H2_FORMATS[e["format"]][0]
            if fmt in ("P8", "ARGBFP32", "RGBFP32", "RGBFP16") or e["type"] == 1:
                continue
            ptr, avail = e["lod_offsets"][0], e["lod_sizes"][0]
            if ptr == 0xFFFFFFFF or not avail:
                continue
            l0, stride, swz, pitch = X.layout(e)
            if not swz and H.H2_FORMATS[e["format"]][2] is False:
                continue    # linear (UI) textures have no mip chain to alias
            try:
                _, _, chain = ms.read_raw(m, ptr, min(avail, stride))
            except (ValueError, KeyError, FileNotFoundError):
                continue
            if len(chain) < l0:
                continue
            base = (hasher(chain[:l0]), e["width"], e["height"])
            bases.add(base)
            off, w, h = l0, e["width"], e["height"]
            for _ in range(e["mips"]):
                w, h = max(w // 2, 1), max(h // 2, 1)
                size = X.level_size(e["format"], w, h)
                if off + size > len(chain) or min(w, h) < 8 or size < 128:
                    break
                key = (hasher(chain[off:off + size]), w, h)
                aliases.setdefault(key, set()).add(base)
                off += size
        print(f"{os.path.basename(path)}: {len(aliases)} aliases so far", file=sys.stderr)
    kept = dropped = 0
    with open(a.out, "w", newline="\n") as f:
        for key in sorted(aliases):
            targets = aliases[key]
            if key in bases or len(targets) != 1:
                dropped += 1
                continue
            b = next(iter(targets))
            f.write(f"{key[0]:016x} {key[1]} {key[2]} {b[0]:016x} {b[1]} {b[2]}\n")
            kept += 1
    print(f"wrote {kept} aliases to {a.out} ({dropped} ambiguous dropped)")


if __name__ == "__main__":
    main()
