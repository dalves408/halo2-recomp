"""Re-apply the hand edits that live inside generated code (TU 1.5 addresses).
Idempotent; run after every `tools.recomp` regeneration of src/recomp/gen.

1. Timing routine 0x0012DBF0 (1.0: 0x0012B0E0): optional RECOMP_HALO2_SINGLE_VBLANK
   override before `MEM16(0x4F2C88) = LO16(eax)` (1.0 data words 0x4E63B8/BA).
2. Switch at 0x0019397D (table 0x00193A0C; 1.0: 0x00190B65 / 0x00190BF4): the lifter
   stops at the first zero entry and misses the 4 live cases at index 12-15
   (0x1939D8/E1/EA/F3, which store 12..15). Same hand extension as 1.0 recomp_0038.c.
"""
import re, sys
from pathlib import Path
gen = Path(__file__).resolve().parents[1] / "src" / "recomp" / "gen"

def patch(path, old, new, marker):
    s = path.read_text(encoding="utf-8")
    if marker in s:
        print(f"{path.name}: already patched ({marker})"); return
    if s.count(old) != 1:
        sys.exit(f"{path.name}: anchor found {s.count(old)} times, regenerate/inspect")
    path.write_text(s.replace(old, new), encoding="utf-8"); print(f"{path.name}: patched ({marker})")

def find(needle):
    hits = [p for p in gen.glob("recomp_*.c") if needle in p.read_text(encoding="utf-8")]
    if len(hits) != 1: sys.exit(f"{needle!r} found in {len(hits)} files")
    return hits[0]

# 1. single vblank
p = find("    MEM16(0x4F2C88) = LO16(eax);")
patch(p, '#include <math.h>\n', '#include <math.h>\n\nextern int g_halo2_single_vblank;\n', 'extern int g_halo2_single_vblank;')
patch(p, "loc_0012DCB1: ;\n    MEM16(0x4F2C88) = LO16(eax);",
      "loc_0012DCB1: ;\n    /* Native timing override: a frame may target the next 60 Hz vblank.\n"
      "     * The game still derives elapsed time from the actual vblank counter. */\n"
      "    if (g_halo2_single_vblank && LO16(eax) > 1) {\n        SET_LO16(eax, 1);\n        MEM16(0x4F2C8A) = 1;\n    }\n"
      "    MEM16(0x4F2C88) = LO16(eax);", "g_halo2_single_vblank && LO16(eax)")

# 2. switch extension
p = find("MEM32(ecx * 4 + 0x193A0C)")
patch(p, "{ uint32_t _jt = MEM32(ecx * 4 + 0x193A0C); /* switch: 8 entries, 8 targets */",
      "{ uint32_t _jt = MEM32(ecx * 4 + 0x193A0C); /* switch: 12 live entries (hand-extended) */", "12 live entries (hand-extended)")
patch(p, "    if (_jt == 0x001939CFu) goto loc_001939CF;\n",
      "    if (_jt == 0x001939CFu) goto loc_001939CF;\n" + "".join(
          f"    if (_jt == 0x{a:08X}u) goto loc_{a:08X};\n" for a in (0x1939D8, 0x1939E1, 0x1939EA, 0x1939F3)),
      "goto loc_001939D8;")
patch(p, "    MEM32(ebp + -8) = 7;\n    goto loc_001939FA;\n\nloc_001939FA: ;",
      "    MEM32(ebp + -8) = 7;\n    goto loc_001939FA;\n\n" + "".join(
          f"loc_{a:08X}: ;\n    MEM32(ebp + -8) = {v};\n    goto loc_001939FA;\n\n" for a, v in ((0x1939D8, 12), (0x1939E1, 13), (0x1939EA, 14), (0x1939F3, 15)))
      + "loc_001939FA: ;", "loc_001939D8: ;")

# 3. XNet 0x003DE27B (secure address for a service, behind stub 0x003D73F6): the lifter took
#    0x003DE2BA -- the second byte of `jz 0x003DE2FF` at 0x003DE2B9 -- for a function start,
#    so sub_003DE27B stops at 0x003DE2BB and its success path (the code at 0x003DE2BB) never
#    runs: the function returned the lookup pointer, the logon read that as an error, and
#    Xbox Live sign-in ended in 0x80151001 with no packet sent to the security gateway.
#    sub_003DE2BA holds a correct translation of 0x003DE2BB on, after one bogus `inc esp`.
p = find("void sub_003DE2BA(void)")
patch(p, "loc_003DE2BA: ;\n    esp++;\n",
      "loc_003DE2BA: ;\n    /* 0x003DE2BA is mid-instruction (hand fix): the real code starts at\n"
      "     * 0x003DE2BB, reached only as sub_003DE27B's fall-through. */\n",
      "reached only as sub_003DE27B's fall-through")
p = find("void sub_003DE27B(void)")
patch(p, "    if (CMP_EQ(_fa, _fb)) { g_seh_ebp = ebp; sub_003DE2FF(); return; } /* je: equal / zero */\n\n}",
      "    if (CMP_EQ(_fa, _fb)) { g_seh_ebp = ebp; sub_003DE2FF(); return; } /* je: equal / zero */\n"
      "    /* fall through to 0x003DE2BB (hand fix, see sub_003DE2BA) */\n"
      "    g_seh_ebp = ebp; sub_003DE2BA(); return;\n}",
      "fall through to 0x003DE2BB (hand fix")

# 4. Mouse aim (src/h2_mouse.c): an entry hook at the top of sub_0018A300, the function that
#    turns a local player by the look stick's yaw/pitch deltas. The hook adds the mouse's
#    turn to those two stack arguments when the per-player control update calls it.
p = find("void sub_0018A300(void)\n{")
patch(p, "void sub_0018A300(void)\n{\n",
      "void sub_0018A300(void)\n{\n"
      "    extern void sub_0018A300_enter(void);\n"
      "    sub_0018A300_enter();   /* mouse aim (hand hook, src/h2_mouse.c) */\n",
      "sub_0018A300_enter();")
