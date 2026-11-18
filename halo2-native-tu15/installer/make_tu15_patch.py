"""Developer-side: build the retail 1.0 -> TU 1.5 (cert 0x803) binary patches.

Needs `pip install bsdiff4` (dev machine only). The end-user installer
(tu15_install.py) applies the patches with a pure-Python bspatch, so players
need nothing but Python 3 (or the frozen installer) and their retail disc.

    python make_tu15_patch.py <retail game dir> <tu15 dir with default.xbe/downloader.xbe>
"""
import bsdiff4, hashlib, json, sys
from pathlib import Path

here = Path(__file__).resolve().parent
retail, tu = Path(sys.argv[1]), Path(sys.argv[2])
sha = lambda b: hashlib.sha256(b).hexdigest()
files = []
for name in ("default.xbe", "downloader.xbe"):
    old, new = (retail / name).read_bytes(), (tu / name).read_bytes()
    patch = bsdiff4.diff(old, new)
    assert bsdiff4.patch(old, patch) == new
    out = here / "patches" / f"{name}.tu15.bsdiff"
    out.write_bytes(patch)
    files.append(dict(name=name, source_sha256=sha(old), source_size=len(old),
                      target_sha256=sha(new), target_size=len(new),
                      patch=f"patches/{out.name}", patch_sha256=sha(patch), patch_size=len(patch)))
    print(f"{name}: {len(old)} -> {len(new)} bytes, patch {len(patch)} bytes")
manifest = dict(
    title="Halo 2", title_id="4D530064",
    update="TU 0000000300000803 (cert version 0x803, April 2007, '1.5')",
    retail_id="Halo 2 retail 1.0 (cert version 3), USA/Europe disc",
    files=files,
    notes="The TU itself ships only default.xbe and downloader.xbe (+ContentMeta.xbx). "
          "patch_v3.lvl and the maps belong to the map packs (separate optional download). "
          "$u is NOT installed: the recompiled exe already is the 1.5 code.")
(here / "patches" / "tu15-manifest.json").write_text(json.dumps(manifest, indent=2))
