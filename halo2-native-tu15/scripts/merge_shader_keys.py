"""Grows the shader starter list (installer/shader_keys.bin) from players' logs.

    python scripts/merge_shader_keys.py <log or keys.bin> [...] [--base installer/shader_keys.bin] [--out <file>]

At each start the game writes into its log every combiner shader the player's PC has built that the shipped
shader_keys.bin does not have, one "[SHADER-KEY] H2K1 <base64>" line each (src/gpu/d3d11_backend.c,
rc_log_extra_keys). RecompLauncher sends the newest .log with a report and players attach logs to issues, so the
logs from reports carry those shaders. This reads any number of logs (and keys.bin / shader_keys.bin lists, if you
have them), drops duplicates and keys of another size, and writes the base list followed by the new keys, in the
order they were first seen. The list is shipped in the Setup and the ZIP, never committed.

A key is only the register-combiner state the game set (no paths, names or hardware details).
"""
import argparse
import base64
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAGIC = b'H2RCKEY1'
HEADER = struct.Struct('<8sII')          # magic, key size, reserved (RcKeysHeader)
LINE_TAG = '[SHADER-KEY] H2K1 '


def read_list(path):
    """(key size, [keys]) from a keys.bin / shader_keys.bin."""
    data = path.read_bytes()
    if len(data) < HEADER.size:
        sys.exit(f'{path}: too short for a key list')
    magic, size, _ = HEADER.unpack_from(data)
    if magic != MAGIC or not size:
        sys.exit(f'{path}: not a shader key list')
    body = data[HEADER.size:]
    whole = len(body) // size * size
    if whole != len(body):
        print(f'{path}: {len(body) - whole} trailing bytes ignored (an interrupted write)')
    return size, [body[i:i + size] for i in range(0, whole, size)]


def read_log(path):
    """The keys in a log's [SHADER-KEY] lines."""
    keys = []
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            at = line.find(LINE_TAG)
            if at < 0:
                continue
            text = line[at + len(LINE_TAG):].strip()
            try:
                keys.append(base64.b64decode(text, validate=True))
            except ValueError:
                print(f'{path}: a damaged [SHADER-KEY] line skipped')
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('inputs', nargs='+', type=Path, help='game logs (.log) and/or key lists (.bin)')
    ap.add_argument('--base', type=Path, default=ROOT / 'installer' / 'shader_keys.bin',
                    help='the current list (default installer/shader_keys.bin); its keys come first')
    ap.add_argument('--out', type=Path, help='where to write (default: the base list)')
    args = ap.parse_args()

    size, base = read_list(args.base) if args.base.is_file() else (None, [])
    merged, seen = [], set()
    for key in base:
        if key not in seen:
            seen.add(key)
            merged.append(key)
    print(f'{args.base}: {len(base)} keys ({len(base) - len(merged)} duplicates dropped)'
          if base else f'{args.base}: none yet')

    added = 0
    for path in args.inputs:
        if path.suffix.lower() == '.bin':
            list_size, keys = read_list(path)
            if size is None:
                size = list_size
            if list_size != size:
                print(f'{path}: keys of {list_size} bytes, the list has {size}: from another build, skipped')
                continue
        else:
            keys = read_log(path)
        new = wrong = 0
        for key in keys:
            if size is None:
                size = len(key)
            if len(key) != size:
                wrong += 1
                continue
            if key not in seen:
                seen.add(key)
                merged.append(key)
                new += 1
        added += new
        note = f', {wrong} of another key size skipped (another build)' if wrong else ''
        print(f'{path}: {len(keys)} keys, {new} new{note}')

    if size is None:
        sys.exit('no keys found')
    out = args.out or args.base
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(HEADER.pack(MAGIC, size, 0) + b''.join(merged))
    print(f'{out}: {len(merged)} keys ({added} new)')


if __name__ == '__main__':
    main()
