"""Names the code offsets in a player's log: halo2_recomp+0x1A2B3C becomes halo2_recomp+0x1A2B3C [sub_002000A0+0x12]
(after Marvel vs. Capcom 2's symbolize_log.py).

    python scripts/symbolize_log.py <log> [--symbols out/symbols] [--out <file>]

A release prints no function names (its .pdb files are not shipped), only offsets, in the [FREEZE] and [CRASH]
reports. Each process writes a [BUILD] line with its program, version and the id of the .pdb it was linked with;
build_installer.py keeps each release's programs and .pdb files in out/symbols/<version>/. This finds the matching pair
for halo2_recomp and halo2_downloader (by that id, so a rebuilt exe with the same version number is not mistaken for
it) and asks Windows' dbghelp.dll for each name. Generated functions are named after the guest address they were
translated from (sub_XXXXXXXX), so a named stack is the game's own call chain.
"""
import argparse
import ctypes
import re
import struct
import sys
from ctypes import wintypes as W
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASE = 0x140000000
MODULES = ('halo2_recomp', 'halo2_downloader')
OFFSET = re.compile(r'\b(halo2_recomp|halo2_downloader)\+0x([0-9A-Fa-f]+)')
BUILD = re.compile(r'\[BUILD\] (halo2_recomp|halo2_downloader)\.exe (\S+), symbols (\w+)')


def pdb_id(exe):
    """The CodeView id (GUID + age, as the game prints it) of the .pdb an exe was linked with."""
    data = exe.read_bytes()
    at = data.find(b'RSDS')
    while at >= 0:
        guid = data[at + 4:at + 20]
        age = struct.unpack_from('<I', data, at + 20)[0]
        d1, d2, d3 = struct.unpack_from('<IHH', guid)
        text = f'{d1:08X}{d2:04X}{d3:04X}' + ''.join(f'{b:02X}' for b in guid[8:]) + f'{age:X}'
        if data[at + 24:at + 60].split(b'\0')[0].lower().endswith(b'.pdb'):
            return text
        at = data.find(b'RSDS', at + 4)
    return None


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', W.ULONG), ('TypeIndex', W.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', W.ULONG), ('Size', W.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', W.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', W.ULONG), ('Scope', W.ULONG),
                ('Tag', W.ULONG), ('NameLen', W.ULONG), ('MaxNameLen', W.ULONG), ('Name', ctypes.c_char * 256)]


class Symbols:
    _next = 0x48324E00

    def __init__(self, exe):
        self.dbg = ctypes.WinDLL('dbghelp.dll')
        self.dbg.SymLoadModuleEx.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleEx.argtypes = [W.HANDLE, W.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64,
                                             W.DWORD, ctypes.c_void_p, W.DWORD]
        self.dbg.SymFromAddr.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64),
                                         ctypes.POINTER(SYMBOL_INFO)]
        Symbols._next += 1
        self.proc = W.HANDLE(Symbols._next)           # any unique value: no live process is read
        self.dbg.SymSetOptions(0x2 | 0x40000)         # UNDNAME | FAIL_CRITICAL_ERRORS
        if not self.dbg.SymInitialize(self.proc, str(exe.parent).encode(), False):
            sys.exit('dbghelp could not start')
        if not self.dbg.SymLoadModuleEx(self.proc, None, str(exe).encode(), None, BASE, exe.stat().st_size * 4, None, 0):
            sys.exit(f'dbghelp could not load {exe}')

    def name(self, rva):
        info = SYMBOL_INFO()
        info.SizeOfStruct = 88
        info.MaxNameLen = 255
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddr(self.proc, BASE + rva, ctypes.byref(disp), ctypes.byref(info)):
            return f'{info.Name.decode(errors="replace")}+0x{disp.value:X}'
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('log', type=Path)
    parser.add_argument('--symbols', type=Path, default=ROOT / 'out' / 'symbols')
    parser.add_argument('--out', type=Path, help='write here instead of <log>.named.log')
    args = parser.parse_args()

    text = args.log.read_text(encoding='utf-8', errors='replace')
    loaded, names = {}, {}
    for module, version, wanted in BUILD.findall(text):
        if module in loaded:
            continue
        exes = [p for p in args.symbols.glob(f'*/{module}.exe') if pdb_id(p) == wanted]
        if exes:
            loaded[module] = Symbols(exes[0])
            print(f'{module} {version.rstrip(",")}: {exes[0].parent}')
        else:
            print(f'{module} {version.rstrip(",")}: no symbols with id {wanted} in {args.symbols}')
    if not loaded:
        sys.exit('no matching symbols: the log needs a [BUILD] line from a release whose files build_installer.py kept')

    def named(m):
        module, rva = m.group(1), int(m.group(2), 16)
        if module not in loaded:
            return m.group(0)
        key = (module, rva)
        if key not in names:
            names[key] = loaded[module].name(rva)
        return m.group(0) + (f' [{names[key]}]' if names[key] else '')

    out = args.out or args.log.with_suffix('.named.log')
    out.write_text(OFFSET.sub(named, text), encoding='utf-8')
    print(f'{len(names)} addresses named -> {out}')


if __name__ == '__main__':
    main()
