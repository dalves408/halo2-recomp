"""Extract every distinct Halo 2 bitmap from the Xbox .map files to PNG.

Each PNG is named  <hash16>_<W>x<H>_<FORMAT>_<tag-leaf>[_<n>][_face<f>].png
where hash16 is hash_bytes() from src/gpu/d3d11_backend.c (copied in
texhash.c) over the level-0 bytes of face 0 exactly as they sit in guest
memory: DXT blocks, Morton-swizzled texels, or linear rows at a 64-byte pitch,
and P8 indices without the palette. A runtime loader can compute the same value
from the texture snapshot it already holds, so keep the prefix when upscaling.

Output (all local, game-derived, never commit):
  <out>/png/<kind>/<category>/...png   kind = color | bump | dudv
  <out>/manifest.csv                   one row per written PNG
  <out>/occurrences.csv                every (map, tag, index) -> hash
  <out>/skipped.csv                    bitmaps not exported, with reason
  <out>/summary.json                   counts and sizes

Usage:
  python -I extract_textures.py --maps <dir> [--maps <dir> ...]
         --shared <dir with shared.map/mainmenu.map/single_player_shared.map>
         --xbe <default.xbe> --out <dir> [--jobs N]
"""
import argparse
import csv
import ctypes
import glob
import io
import json
import os
import struct
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from h2map import H2_FORMATS, FLAG_SWIZZLED, TYPE_NAMES, MapSet  # noqa: E402

BS = "\\"
DXT_FOURCC = {"DXT1": b"DXT1", "DXT3": b"DXT3", "DXT5": b"DXT5"}
BUMP_PALETTE_HEAD = bytes.fromhex("ff7e7eff ff7f7eff ff807eff ff817eff")


def load_hasher():
    dll = os.path.join(HERE, "texhash.dll")
    if not os.path.exists(dll):
        raise SystemExit(f"build {dll} first: gcc -O2 -shared -o texhash.dll texhash.c")
    lib = ctypes.CDLL(dll)
    lib.h2_hash_bytes.restype = ctypes.c_uint64
    lib.h2_hash_bytes.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
    return lambda b: lib.h2_hash_bytes(bytes(b), len(b))


def find_bump_palette(xbe):
    d = open(xbe, "rb").read()
    o = d.find(BUMP_PALETTE_HEAD)
    if o < 0:
        raise SystemExit(f"P8 bump palette not found in {xbe}")
    pal = np.frombuffer(d[o:o + 1024], np.uint8).reshape(256, 4)  # B,G,R,A
    v = (pal[:, :3].astype(np.float32) - 127.5) / 127.5
    if np.abs(np.sqrt((v * v).sum(1)) - 1).max() > 0.08:
        raise SystemExit("P8 bump palette candidate is not a unit-normal table")
    return o, pal


# ---------------------------------------------------------------- layout

def level_size(fmt, w, h):
    name, bpp, dxt = H2_FORMATS[fmt]
    if dxt:
        return ((w + 3) // 4) * ((h + 3) // 4) * bpp
    return w * h * bpp


def linear_pitch(fmt, w):
    return (w * H2_FORMATS[fmt][1] + 63) & ~63


def layout(e):
    """Return (level0 size, face stride, swizzled, pitch) as the runtime sees it."""
    fmt, w, h = e["format"], e["width"], e["height"]
    name, bpp, dxt = H2_FORMATS[fmt]
    swz = bool(e["flags"] & FLAG_SWIZZLED)
    if not dxt and not swz:
        p = linear_pitch(fmt, w)
        return p * h, p * h, False, p
    total, lw, lh = 0, w, h
    for _ in range(e["mips"] + 1):
        total += level_size(fmt, lw, lh)
        lw, lh = max(lw // 2, 1), max(lh // 2, 1)
    stride = (total + 127) & ~127
    return level_size(fmt, w, h), stride, swz, 0


_morton_cache = {}


def morton(w, h):
    key = (w, h)
    if key not in _morton_cache:
        y, x = np.mgrid[0:h, 0:w]
        out = np.zeros((h, w), np.int64)
        outbit, bit = 1, 1
        while bit < w or bit < h:
            if bit < w:
                out |= np.where(x & bit, outbit, 0)
                outbit <<= 1
            if bit < h:
                out |= np.where(y & bit, outbit, 0)
                outbit <<= 1
            bit <<= 1
        _morton_cache[key] = out
    return _morton_cache[key]


# ---------------------------------------------------------------- decode

def decode_dxt(raw, w, h, fourcc):
    pw, ph = (w + 3) & ~3, (h + 3) & ~3
    hdr = struct.pack("<4sIIIIIII44xII4s20xI16x", b"DDS ", 124, 0x1007, ph, pw,
                      len(raw), 0, 1, 32, 0x4, fourcc, 0x1000)
    img = Image.open(io.BytesIO(hdr + bytes(raw)))
    img.load()
    img = img.convert("RGBA")
    return np.asarray(img)[:h, :w].copy()


def texels(raw, e, swz, pitch, bpp):
    w, h = e["width"], e["height"]
    dt = {1: np.uint8, 2: np.uint16, 4: np.uint32}[bpp]
    if swz:
        flat = np.frombuffer(raw, dt, count=w * h)
        return flat[morton(w, h)]
    rows = np.frombuffer(raw, np.uint8, count=pitch * h).reshape(h, pitch)
    return rows[:, :w * bpp].copy().view(dt).reshape(h, w)


def decode(raw, e, swz, pitch, bump_pal):
    """Return (RGBA uint8 array, note)."""
    name, bpp, dxt = H2_FORMATS[e["format"]]
    w, h = e["width"], e["height"]
    if dxt:
        return decode_dxt(raw, w, h, DXT_FOURCC[name]), ""
    v = texels(raw, e, swz, pitch, bpp).astype(np.uint32)
    out = np.empty((h, w, 4), np.uint8)
    r = g = b = None
    a = np.full((h, w), 255, np.uint32)
    note = ""
    if name in ("A8R8G8B8", "X8R8G8B8"):
        r, g, b = (v >> 16) & 255, (v >> 8) & 255, v & 255
        if name == "A8R8G8B8":
            a = v >> 24
    elif name == "R5G6B5":
        r, g, b = ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
    elif name == "A1R5G5B5":
        a = np.where(v & 0x8000, 255, 0)
        r, g, b = ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31
    elif name == "A4R4G4B4":
        a = ((v >> 12) & 15) * 17
        r, g, b = ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17
    elif name == "Y8":
        r = g = b = v
    elif name == "A8":
        r = g = b = np.full_like(v, 255)
        a = v
    elif name == "AY8":
        r = g = b = a = v
    elif name == "A8Y8":
        a, r = v >> 8, v & 255
        g = b = r
    elif name == "P8_BUMP":
        p = bump_pal[v]
        b, g, r, a = (p[..., i].astype(np.uint32) for i in range(4))
        note = "decoded with the fixed P8 bump palette from default.xbe"
    elif name == "V8U8":
        r, g = (v & 255) ^ 0x80, (v >> 8) ^ 0x80
        b = np.full_like(v, 255)
        note = "signed U/V stored biased (byte XOR 0x80) in R/G; B=255"
    elif name == "G8B8":
        r = np.zeros_like(v)
        g, b = v >> 8, v & 255
        note = "G8B8: high byte in G, low byte in B, R=0"
    else:
        raise ValueError(f"no decoder for {name}")
    out[..., 0], out[..., 1], out[..., 2], out[..., 3] = r, g, b, a
    return out, note


# ---------------------------------------------------------------- naming

def categorize(tag):
    t = tag.lower()
    parts = t.split(BS)
    if t.startswith("ui" + BS + "hud"):
        return "hud"
    if parts[0] == "ui":
        return "interface"
    if parts[0] == "effects":
        return "effects"
    if parts[0] == "objects":
        sub = parts[1] if len(parts) > 1 else ""
        return {"characters": "characters", "weapons": "weapons",
                "vehicles": "vehicles"}.get(sub, "objects")
    if parts[0] == "scenarios":
        if len(parts) > 1 and parts[1] == "skies":
            return "sky"
        if any(p in ("decals",) for p in parts):
            return "decals"
        return "environment"
    if parts[0] in ("shaders", "rasterizer"):
        return "engine"
    return "other"


def kind_of(fmt_name, tag):
    leaf = tag.lower().rsplit(BS, 1)[-1]
    if fmt_name == "P8_BUMP" or "bump" in leaf or "normal" in leaf or leaf.endswith("_vector"):
        return "bump"
    if fmt_name in ("V8U8", "G8B8"):
        return "dudv"
    return "color"


def slug(tag, k, n):
    leaf = tag.rsplit(BS, 1)[-1]
    leaf = "".join(c if c.isalnum() or c in "-_" else "_" for c in leaf)[:72]
    return f"{leaf}_{k}" if n > 1 else leaf


# ---------------------------------------------------------------- phase 1

def scan(map_paths, shared_dir, hasher):
    ms = MapSet(shared_dir)
    unique = {}          # (hash, w, h, fmt) -> job
    occurrences = []
    skipped = []
    for path in map_paths:
        m = ms.get(path)
        for tag, tid, k, n, e in m.bitmaps():
            row = {"map": m.name, "tag": tag, "index": k, "count": n}
            if "error" in e:
                skipped.append({**row, "reason": e["error"]})
                continue
            fmt = H2_FORMATS.get(e["format"], (f"fmt{e['format']}",))[0]
            row.update(width=e["width"], height=e["height"], format=fmt,
                       type=TYPE_NAMES.get(e["type"], str(e["type"])))
            if e["format"] not in H2_FORMATS:
                skipped.append({**row, "reason": "unknown format"})
                continue
            if fmt == "P8":
                skipped.append({**row, "reason": "P8 lightmap: palette lives in the ltmp tag; not an upscale target"})
                continue
            if e["type"] == 1:
                skipped.append({**row, "reason": "3D volume texture: runtime has no 3D texture path"})
                continue
            if fmt in ("ARGBFP32", "RGBFP32", "RGBFP16"):
                skipped.append({**row, "reason": "float format"})
                continue
            ptr, avail = e["lod_offsets"][0], e["lod_sizes"][0]
            if ptr == 0xFFFFFFFF or not avail:
                skipped.append({**row, "reason": "no LOD0 raw data"})
                continue
            l0, stride, swz, pitch = layout(e)
            faces = 6 if e["type"] == 2 else 1
            need = stride * (faces - 1) + l0
            if need > avail:
                skipped.append({**row, "reason": f"LOD0 holds {avail} bytes, layout needs {need}"})
                continue
            try:
                src, off, raw0 = ms.read_raw(m, ptr, l0)
            except (ValueError, KeyError, FileNotFoundError) as ex:
                skipped.append({**row, "reason": f"raw read failed: {ex}"})
                continue
            h = hasher(raw0)
            key = (h, e["width"], e["height"], fmt)
            occurrences.append({**row, "hash": f"{h:016x}", "raw_map": src,
                                "raw_offset": f"{off:#x}"})
            if key not in unique:
                unique[key] = {
                    "hash": f"{h:016x}", "tag": tag, "index": k, "count": n,
                    "map": path, "raw_map": src, "ptr": ptr, "need": need,
                    "entry": e, "fmt": fmt, "faces": faces, "stride": stride,
                    "swizzled": swz, "pitch": pitch, "level0_bytes": l0,
                }
    return unique, occurrences, skipped


# ---------------------------------------------------------------- phase 2

_W = {}


def _init(shared_dir, xbe, out):
    _W["ms"] = MapSet(shared_dir)
    _W["pal"] = find_bump_palette(xbe)[1]
    _W["out"] = out


def write_job(job):
    ms, out = _W["ms"], _W["out"]
    e, fmt = job["entry"], job["fmt"]
    try:
        owner = ms.get(job["map"])
        _, _, raw = ms.read_raw(owner, job["ptr"], job["need"])
        kind, cat = kind_of(fmt, job["tag"]), categorize(job["tag"])
        base = f"{job['hash']}_{e['width']}x{e['height']}_{fmt}_{slug(job['tag'], job['index'], job['count'])}"
        files, notes, alpha = [], "", False
        for f in range(job["faces"]):
            seg = raw[f * job["stride"]:f * job["stride"] + job["level0_bytes"]]
            rgba, notes = decode(seg, e, job["swizzled"], job["pitch"], _W["pal"])
            has_alpha = bool((rgba[..., 3] != 255).any())
            alpha |= has_alpha
            img = Image.fromarray(rgba, "RGBA") if has_alpha else Image.fromarray(rgba[..., :3], "RGB")
            name = base + (f"_face{f}" if job["faces"] > 1 else "") + ".png"
            rel = os.path.join("png", kind, cat, name)
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            img.save(dst, optimize=False, compress_level=6)
            files.append((rel, os.path.getsize(dst)))
        return {"ok": True, "job": job, "kind": kind, "category": cat,
                "files": files, "alpha": alpha, "note": notes}
    except Exception as ex:  # record and continue
        return {"ok": False, "job": job, "error": f"{type(ex).__name__}: {ex}"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--maps", action="append", required=True,
                    help="directory (or glob) of .map files; repeatable")
    ap.add_argument("--shared", required=True)
    ap.add_argument("--xbe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    a = ap.parse_args()

    paths = []
    for m in a.maps:
        paths += sorted(glob.glob(os.path.join(m, "*.map")) if os.path.isdir(m) else glob.glob(m))
    paths = [p for p in paths if not os.path.basename(p).startswith("cache")]
    if not paths:
        raise SystemExit("no .map files found")
    os.makedirs(a.out, exist_ok=True)
    pal_off, _ = find_bump_palette(a.xbe)
    t0 = time.time()
    unique, occ, skipped = scan(paths, a.shared, load_hasher())
    print(f"scanned {len(paths)} maps: {len(occ)} exportable bitmap entries, "
          f"{len(unique)} distinct, {len(skipped)} skipped ({time.time() - t0:.0f}s)", flush=True)

    results = []
    with ProcessPoolExecutor(a.jobs, initializer=_init, initargs=(a.shared, a.xbe, a.out)) as ex:
        for i, r in enumerate(ex.map(write_job, unique.values(), chunksize=16)):
            results.append(r)
            if (i + 1) % 2000 == 0:
                print(f"  {i + 1}/{len(unique)} written", flush=True)

    failures = [r for r in results if not r["ok"]]
    good = [r for r in results if r["ok"]]
    with open(os.path.join(a.out, "manifest.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["file", "hash", "width", "height", "format", "type", "faces", "mips",
                    "swizzled", "pitch", "level0_bytes", "alpha", "kind", "category",
                    "tag", "index", "raw_map", "bytes", "note"])
        for r in good:
            j, e = r["job"], r["job"]["entry"]
            for rel, size in r["files"]:
                w.writerow([rel.replace(os.sep, "/"), j["hash"], e["width"], e["height"], j["fmt"],
                            TYPE_NAMES.get(e["type"]), j["faces"], e["mips"] + 1,
                            int(j["swizzled"]), j["pitch"], j["level0_bytes"], int(r["alpha"]),
                            r["kind"], r["category"], j["tag"], j["index"], j["raw_map"], size,
                            r["note"]])
    for name, rows in (("occurrences.csv", occ), ("skipped.csv", skipped)):
        with open(os.path.join(a.out, name), "w", newline="") as fh:
            if rows:
                keys = list(dict.fromkeys(k for r in rows for k in r))
                w = csv.DictWriter(fh, keys)
                w.writeheader()
                w.writerows(rows)
    with open(os.path.join(a.out, "failures.csv"), "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["hash", "tag", "index", "format", "width", "height", "error"])
        for r in failures:
            j = r["job"]
            w.writerow([j["hash"], j["tag"], j["index"], j["fmt"], j["entry"]["width"],
                        j["entry"]["height"], r["error"]])

    files = [f for r in good for f in r["files"]]
    summary = {
        "maps": [os.path.basename(p) for p in paths],
        "bump_palette_xbe_offset": pal_off,
        "bitmap_entries_exportable": len(occ),
        "distinct_textures": len(unique),
        "written_textures": len(good),
        "written_png_files": len(files),
        "png_bytes": sum(s for _, s in files),
        "decode_failures": len(failures),
        "skipped_entries": len(skipped),
        "skipped_by_reason": Counter(s["reason"].split(":")[0] for s in skipped),
        "skipped_distinct_by_reason": {
            k: len(v) for k, v in _distinct(skipped).items()},
        "by_format": Counter(r["job"]["fmt"] for r in good),
        "by_kind": Counter(r["kind"] for r in good),
        "by_category": Counter(f"{r['kind']}/{r['category']}" for r in good),
        "with_alpha": sum(r["alpha"] for r in good),
        "cube_maps": sum(r["job"]["faces"] == 6 for r in good),
        "by_size": Counter(f"{r['job']['entry']['width']}x{r['job']['entry']['height']}"
                           for r in good).most_common(15),
        "seconds": round(time.time() - t0),
    }
    with open(os.path.join(a.out, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)
    print(json.dumps({k: v for k, v in summary.items() if k != "maps"}, indent=2))


def _distinct(skipped):
    d = defaultdict(set)
    for s in skipped:
        d[s["reason"].split(":")[0]].add((s["tag"], s["index"]))
    return d


if __name__ == "__main__":
    main()
