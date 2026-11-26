"""Packs the hi-res texture pack into the .bin parts the installers download (after Marvel vs. Capcom 2's
build_texture_pack.py).

    python scripts/build_texture_pack.py [--source <folder of .dds files>] --revision 1

The source is the upscaled BC7 pack (h2textures/upscaled_4x_dds: color/, bump/, dudv/ and mip_aliases.txt; see the
README's Hi-res textures). Each part is an ordinary 7z archive (LZMA2) with a textures/ root -- the folder the game
loads (src/gpu/tex_pack.c) -- and a .bin extension, and is under 2,000,000,000 bytes so GitHub serves it as a release
asset. The parts extract into the same folder in order; the last one carries manifest.txt, which lists every file as
"<sha256> <bytes> <path>" and is what Setup checks the finished folder against.

Output in out/texture-release/revision-<n>: texture-01.bin ..., SHA256SUMS.txt, texture-payload.json, and
installer/texture_payload.iss with the pins both installers compile in. The parts go on the textures-<n> release
(a prerelease, so the game's update check, which reads releases/latest, never sees it). The pack is made from the
game's own textures, so the parts stay out of git.

Later revisions:

    python scripts/build_texture_pack.py --revision 2 --overlay ../h2textures/remade/dds --patch-from 1

--overlay is a folder laid out like the source whose files replace (or add to) the source's. Revision 2 is the
first pack plus the remade zoom overlays and water masks (h2textures/remade). --patch-from <n> also builds
texture-update-<n>-to-<revision>.bin: every file that differs from revision <n> plus the new manifest.txt, for
players who already have revision <n>. Revision <n>'s file list is rebuilt from --base-source (default: the plain
source) and must match the manifest SHA-256 pinned in out/texture-release/revision-<n>/texture-payload.json.
installer/texture_patch_files.iss gets the update's files and sizes, which Setup uses to recognise a revision <n>
folder even when an earlier update was interrupted half way.
"""
import argparse
import hashlib
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SEVENZIP = Path(r'C:\Program Files\7-Zip\7z.exe')
REPO = 'dalves408/halo2-recomp'
PART_LIMIT = 2_000_000_000          # GitHub's per-asset ceiling is 2 GiB; stay under 2 GB
PART_TARGET = 1_850_000_000         # uncompressed bytes per part: BC7 barely compresses, so plan as if it did not
PACK_GUID = '7E3A9C15-4B2D-4F86-9A61-D58C0B7E2F43'
MAX_PARTS = 4                       # what texture_download_code.iss is written for
PACK_DIR = 'textures'               # the folder beside the game that src/gpu/tex_pack.c loads
SEVENZIP_ARGS = ['-t7z', '-m0=LZMA2', '-mx=7', '-md=64m', '-ms=on', '-mmt=on',
                 '-mtc=off', '-mta=off', '-mtm=off', '-bsp0', '-bso0']


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def pack(stage, files, out):
    """7z the listed files (paths relative to stage, under textures/) into out."""
    if out.exists():
        out.unlink()
    listfile = out.with_suffix('.list')
    listfile.write_text('\n'.join(files) + '\n', encoding='utf-8')
    tmp = out.with_suffix('.7z')
    if tmp.exists():
        tmp.unlink()
    subprocess.run([str(SEVENZIP), 'a', *SEVENZIP_ARGS, str(tmp), f'@{listfile}'], cwd=stage, check=True)
    tmp.rename(out)
    listfile.unlink()
    return out.stat().st_size


def pack_files(folder):
    """{relative posix path: file} for the pack's files under folder (.dds and mip_aliases.txt)."""
    return {p.relative_to(folder).as_posix(): p for p in folder.rglob('*') if p.is_file() and
            (p.suffix.lower() == '.dds' or p.name.lower() == 'mip_aliases.txt')}


def pack_order(rel):
    """Revision 1 sorted Windows Path objects, which compare case-insensitively with backslashes."""
    return rel.replace('/', '\\').lower()


def manifest_text(entries):
    return ''.join(f'{h} {n} {rel}\n' for h, n, rel in entries)


def split(names, sizes, count):
    """The file list in order, cut into count groups of about equal size."""
    total = sum(sizes)
    groups, budget, cur, acc = [], total / count, [], 0
    for name, n in zip(names, sizes):
        if cur and acc + n > budget and len(groups) < count - 1:
            groups.append(cur)
            cur, acc = [], 0
        cur.append(name)
        acc += n
    groups.append(cur)
    return groups


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', default=str(ROOT.parent / 'h2textures' / 'upscaled_4x_dds'))
    ap.add_argument('--revision', type=int, default=1)
    ap.add_argument('--overlay', help='folder whose files replace or add to the source (same layout)')
    ap.add_argument('--patch-from', type=int, help='also build an update from this earlier revision')
    ap.add_argument('--base-source', help='the earlier revision\'s source (default: --source)')
    args = ap.parse_args()
    src = Path(args.source)
    files = pack_files(src)
    if args.overlay:
        overlay = pack_files(Path(args.overlay))
        replaced = sum(1 for rel in overlay if rel in files)
        files.update(overlay)
        print(f'overlay: {len(overlay)} files ({replaced} replace the source\'s, {len(overlay) - replaced} new)')
    if not any(rel.lower().endswith('.dds') for rel in files):
        sys.exit(f'no .dds files in {src}')
    out = ROOT / 'out' / 'texture-release' / f'revision-{args.revision}'
    out.mkdir(parents=True, exist_ok=True)
    stage = out / 'stage'
    if stage.exists():
        shutil.rmtree(stage)
    pack_root = stage / PACK_DIR

    entries = []
    for rel in sorted(files, key=pack_order):
        p = files[rel]
        link_or_copy(p, pack_root / rel)
        entries.append((sha256(p), p.stat().st_size, rel))
    manifest = manifest_text(entries)
    (pack_root / 'manifest.txt').write_text(manifest, encoding='ascii', newline='\n')
    (pack_root / 'README.txt').write_text(
        f'Halo 2 Recomp hi-res texture pack, revision {args.revision}.\n'
        f'{len(entries)} files, 4x upscaled, BC7'
        + (' (zoom overlays and water masks uncompressed, transparency exact)' if args.overlay else '') +
        '.\nThe game loads this folder at start; Hi-res textures in F1 settings switches it off.\n'
        'Made for the PC recompilation at https://github.com/' + REPO + '\n',
        encoding='ascii', newline='\n')
    total = sum(e[1] for e in entries)
    print(f'{len(entries)} files, {total:,} bytes', flush=True)

    # Sorted paths keep each folder (color/, bump/, dudv/) together; the last part carries the manifest.
    names = [f'{PACK_DIR}/{rel}' for _, _, rel in entries]
    sizes = [n for _, n, _ in entries]
    extras = [f'{PACK_DIR}/manifest.txt', f'{PACK_DIR}/README.txt']
    first = max(1, math.ceil(total / PART_TARGET))
    for count in range(first, MAX_PARTS + 1):
        groups = split(names, sizes, count)
        groups[-1] = groups[-1] + extras
        for old in out.glob('texture-*.bin'):
            old.unlink()
        parts = []
        for i, g in enumerate(groups, 1):
            path = out / f'texture-{i:02d}.bin'
            size = pack(stage, g, path)
            print(f'  {path.name}: {len(g)} files, {size:,} bytes', flush=True)
            parts.append(path)
        if all(p.stat().st_size < PART_LIMIT for p in parts):
            break
    else:
        sys.exit(f'the pack does not fit in {MAX_PARTS} parts under {PART_LIMIT:,} bytes')

    # Every part on its own, then all of them together, must give back exactly the staged pack.
    check = out / 'check'
    if check.exists():
        shutil.rmtree(check)
    for p in parts:
        subprocess.run([str(SEVENZIP), 'x', '-t7z', '-y', '-bsp0', '-bso0', f'-o{check}', str(p)], check=True)
    got = check / PACK_DIR
    if (got / 'manifest.txt').read_text(encoding='ascii') != manifest:
        sys.exit('manifest.txt did not survive the round trip')
    for h, n, rel in entries:
        f = got / rel
        if not f.is_file() or f.stat().st_size != n or sha256(f) != h:
            sys.exit(f'{rel} did not survive the round trip')
    shutil.rmtree(check)
    print('round trip: every texture matches', flush=True)

    tag = f'textures-{args.revision}'
    patch = build_patch(args, src, out, stage, entries, manifest, tag) if args.patch_from else None
    shutil.rmtree(stage)
    info = {
        'revision': args.revision,
        'tag': tag,
        'files': len(entries),
        'extracted_bytes': total + len(manifest.encode()),
        'manifest_sha256': hashlib.sha256(manifest.encode('ascii')).hexdigest(),
        'parts': [{'name': p.name, 'bytes': p.stat().st_size, 'sha256': sha256(p),
                   'url': f'https://github.com/{REPO}/releases/download/{tag}/{p.name}'} for p in parts],
    }
    if patch:
        info['update'] = patch
    (out / 'texture-payload.json').write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
    assets = info['parts'] + ([patch] if patch else [])
    (out / 'SHA256SUMS.txt').write_text(''.join(f"{x['sha256']}  {x['name']}\n" for x in assets),
                                        encoding='ascii', newline='\n')

    lines = [
        f'; Hi-res texture pack revision {args.revision}: what both installers download, check and extract.',
        '; Written by scripts/build_texture_pack.py; do not edit by hand. Each define can be overridden for tests.',
    ]

    def define(name, value):
        lines.extend([f'#ifndef {name}', f'#define {name} {value}', '#endif'])

    define('TextureVersion', f'"{args.revision}"')
    define('TextureDisplayVersion', f'"Revision {args.revision}"')
    define('PackGuid', f'"{PACK_GUID}"')
    define('TextureExtractedBytes', info['extracted_bytes'])
    define('TextureManifestSHA256', f'"{info["manifest_sha256"]}"')
    define('TextureFileCount', len(entries))
    define('TextureCacheRoot', '"{app}\\data\\Downloads"')
    define('TexturePartCount', len(parts))
    define('TextureDownloadBytes', sum(x['bytes'] for x in info['parts']))
    for i, x in enumerate(info['parts'], 1):
        define(f'TextureURL{i}', f'"{x["url"]}"')
        define(f'TextureSHA256{i}', f'"{x["sha256"]}"')
        define(f'TextureBytes{i}', x['bytes'])
    if patch:
        # The update from an earlier revision: Setup uses it when it finds that revision's folder.
        define('TexturePatchFrom', f'"{patch["from"]}"')
        define('TexturePatchName', f'"{patch["name"]}"')
        define('TexturePatchURL', f'"{patch["url"]}"')
        define('TexturePatchSHA256', f'"{patch["sha256"]}"')
        define('TexturePatchBytes', patch['bytes'])
        define('TexturePatchExtractedBytes', patch['extracted_bytes'])
        define('TexturePatchFileCount', len(patch['files']))
        define('TextureBaseManifestSHA256', f'"{patch["base_manifest_sha256"]}"')
        define('TextureBaseFileCount', patch['base_files'])
    # Earlier official revisions' manifests: a folder holding one of them is ours, so Setup deletes it after
    # installing this revision instead of keeping it as an unknown backup (a full revision 3 had no update
    # from 2, and a 6 GB revision 2 folder would have stayed beside it).
    older = []
    for r in range(1, args.revision):
        pj = ROOT / 'out' / 'texture-release' / f'revision-{r}' / 'texture-payload.json'
        if pj.exists():
            older.append(json.loads(pj.read_text(encoding='utf-8'))['manifest_sha256'])
    if older:
        define('TextureOlderManifests', '"' + ';'.join(older) + '"')
    (ROOT / 'installer' / 'texture_payload.iss').write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    write_patch_files_iss(patch)
    print(json.dumps(info, indent=2))


def write_patch_files_iss(patch):
    """installer/texture_patch_files.iss: TexturePatchFileBytes(relative path) gives the new size of a file the update
    replaces (-1 for any other), so Setup still recognises the earlier revision's folder after an interrupted
    update. Included inside [Code] by texture_download_code.iss."""
    lines = ['{ Written by scripts/build_texture_pack.py; do not edit by hand. The files the texture update replaces',
             '  or adds, with their new sizes; -1 for any other file. }',
             'function TexturePatchFileBytes(Relative: String): Int64;',
             'begin',
             '  Result := -1;']
    if patch:
        lines.append('  Relative := Lowercase(Relative);')
        for i, (rel, n) in enumerate(sorted(patch['files'].items())):
            win = rel.replace('/', '\\').lower()
            lines.append(f"  {'if' if i == 0 else 'else if'} Relative = '{win}' then Result := {n}")
        lines[-1] += ';'
    lines.append('end;')
    (ROOT / 'installer' / 'texture_patch_files.iss').write_text('\n'.join(lines) + '\n', encoding='ascii',
                                                                newline='\n')


def build_patch(args, src, out, stage, entries, manifest, tag):
    """texture-update-<from>-to-<revision>.bin: the files that differ from revision <from>, plus manifest.txt and
    README.txt (written last by Setup). Revision <from>'s list is rebuilt and checked against its pinned SHA-256."""
    base_json = ROOT / 'out' / 'texture-release' / f'revision-{args.patch_from}' / 'texture-payload.json'
    if not base_json.is_file():
        sys.exit(f'{base_json} is missing: it pins revision {args.patch_from}\'s manifest')
    base_pin = json.loads(base_json.read_text(encoding='utf-8'))['manifest_sha256']
    base_src = Path(args.base_source or args.source)
    base_files = pack_files(base_src)
    print(f'rebuilding revision {args.patch_from}\'s file list from {base_src} ...', flush=True)
    base_entries = [(sha256(base_files[rel]), base_files[rel].stat().st_size, rel)
                    for rel in sorted(base_files, key=pack_order)]
    base_manifest = manifest_text(base_entries)
    if hashlib.sha256(base_manifest.encode('ascii')).hexdigest() != base_pin:
        sys.exit(f'the rebuilt revision {args.patch_from} manifest does not match its pinned SHA-256 {base_pin}')
    old = {rel.lower(): h for h, _, rel in base_entries}
    new = {rel.lower() for _, _, rel in entries}
    gone = sorted(set(old) - new)
    if gone:
        sys.exit(f'revision {args.revision} drops {len(gone)} files of revision {args.patch_from} '
                 f'(e.g. {gone[0]}); Setup\'s update only adds and replaces files')
    changed = [(h, n, rel) for h, n, rel in entries if old.get(rel.lower()) != h]
    if not changed:
        sys.exit(f'revision {args.revision} has no changes from revision {args.patch_from}')
    name = f'texture-update-{args.patch_from}-to-{args.revision}.bin'
    path = out / name
    size = pack(stage, [f'{PACK_DIR}/{rel}' for _, _, rel in changed] +
                [f'{PACK_DIR}/manifest.txt', f'{PACK_DIR}/README.txt'], path)
    if size >= PART_LIMIT:
        sys.exit(f'{name} is {size:,} bytes, over the {PART_LIMIT:,} byte asset limit')
    check = out / 'check-update'
    if check.exists():
        shutil.rmtree(check)
    subprocess.run([str(SEVENZIP), 'x', '-t7z', '-y', '-bsp0', '-bso0', f'-o{check}', str(path)], check=True)
    for h, n, rel in changed:
        f = check / PACK_DIR / rel
        if not f.is_file() or f.stat().st_size != n or sha256(f) != h:
            sys.exit(f'{rel} did not survive the update\'s round trip')
    if (check / PACK_DIR / 'manifest.txt').read_text(encoding='ascii') != manifest:
        sys.exit('manifest.txt did not survive the update\'s round trip')
    shutil.rmtree(check)
    print(f'  {name}: {len(changed)} changed files, {size:,} bytes (round trip ok)', flush=True)
    return {'name': name, 'from': args.patch_from, 'bytes': size, 'sha256': sha256(path),
            'url': f'https://github.com/{REPO}/releases/download/{tag}/{name}',
            'extracted_bytes': sum(n for _, n, _ in changed) + len(manifest.encode()),
            'base_manifest_sha256': base_pin, 'base_files': len(base_entries),
            'files': {rel: n for _, n, rel in changed}}


if __name__ == '__main__':
    main()
