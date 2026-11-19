"""Read-only parser for Halo 2 Xbox (cache version 8) .map files: tag index,
tag names and 'bitm' bitmap tags with their raw-pixel pointers.

Layout notes (verified against the retail maps in this project):
  header  0x010 index offset, 0x014 index size, 0x2C0 file-name count,
          0x2C4 name table offset, 0x2C8 name table size, 0x2CC name index offset
  index   +0x00 tag-class array (virtual), +0x04 class count,
          +0x08 tag array (virtual), +0x18 tag count, +0x1C 'tags'
          class array starts at index+0x20 (12 bytes each); tag entry is
          class, id, meta virtual address, meta size (16 bytes)
  meta    virtual address - (first tag's address - (index offset + index size))
  bitm    +0x44 reflexive (count, virtual pointer) of 116-byte bitmap entries
  bitmap  +0x04 u16 width, +0x06 u16 height, +0x08 u8 depth, +0x0A u16 type
          (0 2D, 1 3D, 2 cube), +0x0C u16 format, +0x0E u16 flags,
          +0x14 u16 extra mip count, +0x1C six LOD raw offsets, +0x34 six sizes
  raw     offset & 0x3FFFFFFF in the map selected by bits 30-31:
          0 this map, 1 mainmenu.map, 2 shared.map, 3 single_player_shared.map
          LOD0 holds the full chain (level 0 first); later LODs start lower.
"""
import mmap
import os
import struct

LOCATION_MAPS = {1: "mainmenu.map", 2: "shared.map", 3: "single_player_shared.map"}

# Halo 2 bitmap format enum -> (name, bytes per texel or DXT block bytes, is_dxt)
H2_FORMATS = {
    0: ("A8", 1, False), 1: ("Y8", 1, False), 2: ("AY8", 1, False),
    3: ("A8Y8", 2, False), 6: ("R5G6B5", 2, False), 8: ("A1R5G5B5", 2, False),
    9: ("A4R4G4B4", 2, False), 10: ("X8R8G8B8", 4, False),
    11: ("A8R8G8B8", 4, False), 14: ("DXT1", 8, True), 15: ("DXT3", 16, True),
    16: ("DXT5", 16, True), 17: ("P8_BUMP", 1, False), 18: ("P8", 1, False),
    19: ("ARGBFP32", 16, False), 20: ("RGBFP32", 12, False),
    21: ("RGBFP16", 6, False), 22: ("V8U8", 2, False), 23: ("G8B8", 2, False),
}
FLAG_SWIZZLED = 0x0008
TYPE_NAMES = {0: "2d", 1: "3d", 2: "cube"}


class H2Map:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self._f = open(path, "rb")
        self.d = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.d
        if d[0:4] != b"daeh" or struct.unpack_from("<I", d, 4)[0] != 8:
            raise ValueError(f"{path}: not a Halo 2 Xbox cache file")
        io, isz = struct.unpack_from("<2I", d, 0x10)
        cls_ptr, cls_count, tag_ptr = struct.unpack_from("<3I", d, io)
        count = struct.unpack_from("<I", d, io + 0x18)[0]
        idx_magic = cls_ptr - (io + 0x20)
        tags_at = tag_ptr - idx_magic
        name_count, name_off, _, name_idx = struct.unpack_from("<4I", d, 0x2C0)
        self.tags = []
        for i in range(count):
            cl, tid, addr, size = struct.unpack_from("<4I", d, tags_at + 16 * i)
            name = ""
            if i < name_count:
                o = name_off + struct.unpack_from("<I", d, name_idx + 4 * i)[0]
                name = d[o:d.find(b"\0", o)].decode("latin1")
            self.tags.append((cl.to_bytes(4, "little")[::-1].decode("latin1"),
                              tid, addr, size, name))
        first = next(t[2] for t in self.tags if t[2])
        self.meta_magic = first - (io + isz)

    def v2f(self, addr):
        return addr - self.meta_magic

    def bitmaps(self):
        """Yield (tag_name, tag_id, index, count, entry dict) for every bitmap."""
        d = self.d
        for cl, tid, addr, size, name in self.tags:
            if cl != "bitm" or not addr or size < 0x4C:
                continue
            m = self.v2f(addr)
            n, ptr = struct.unpack_from("<2I", d, m + 0x44)
            if n == 0 or n > 4096:
                continue
            base = self.v2f(ptr)
            for k in range(n):
                e = base + 116 * k
                if d[e:e + 4] != b"mtib":
                    yield name, tid, k, n, {"error": "bad bitmap signature"}
                    continue
                w, h = struct.unpack_from("<2H", d, e + 4)
                depth = d[e + 8]
                btype, fmt, flags = struct.unpack_from("<3H", d, e + 0x0A)
                mips = struct.unpack_from("<H", d, e + 0x14)[0]
                offs = struct.unpack_from("<6I", d, e + 0x1C)
                sizes = struct.unpack_from("<6I", d, e + 0x34)
                yield name, tid, k, n, {
                    "width": w, "height": h, "depth": depth, "type": btype,
                    "format": fmt, "flags": flags, "mips": mips,
                    "lod_offsets": offs, "lod_sizes": sizes,
                }


class MapSet:
    """Opens maps lazily and resolves raw pointers across the shared maps."""

    def __init__(self, shared_dir):
        self.shared_dir = shared_dir
        self.open = {}

    def get(self, path):
        if path not in self.open:
            self.open[path] = H2Map(path)
        return self.open[path]

    def read_raw(self, owner, pointer, size):
        loc = pointer >> 30
        off = pointer & 0x3FFFFFFF
        if loc == 0:
            src = owner
        else:
            src = self.get(os.path.join(self.shared_dir, LOCATION_MAPS[loc]))
        if off + size > len(src.d):
            raise ValueError(f"raw {off:#x}+{size:#x} beyond {src.name}")
        return src.name, off, src.d[off:off + size]
