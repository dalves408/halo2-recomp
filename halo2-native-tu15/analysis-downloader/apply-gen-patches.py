"""Re-apply the hand edits that live inside the downloader's generated code
(src/downloader/gen, from analysis-downloader/run-pipeline.ps1). Idempotent;
run after every regeneration.

1. XNet pool allocator 0x00072AB3: the lifter took 0x00072BAA -- the second byte
   of `mov [esi+2], ax` (66 89 46 02 at 0x00072BA9) -- for a function start, so
   sub_00072AB3 stops after that store and never runs its last three
   instructions (`mov [esi], bl`, `mov esi, edx`, `jmp 0x00072AF3`). It fell off
   its end with the stack 0x1C low and ebx/esi/edi wrong, and XNet's first
   timer DPC then used 0xC0000000 as its object and faulted.
   (Same class of bug as halo2-native-tu15 analysis/apply-gen-patches.py #3.)
"""
import sys
from pathlib import Path

gen = Path(__file__).resolve().parents[1] / "src" / "downloader" / "gen"


def patch(path, old, new, marker, func=None):
    """Replace `old` once; with `func`, only inside that function's body (the
    lifter emits extra entry points that repeat a function's blocks)."""
    s = path.read_text(encoding="utf-8")
    lo, hi = 0, len(s)
    if func:
        lo = s.index(f"void {func}(void)\n{{")
        hi = s.index("\n}\n", lo) + 2
    body = s[lo:hi]
    if marker in body:
        print(f"{path.name}: already patched ({marker})")
        return
    if body.count(old) != 1:
        sys.exit(f"{path.name}: anchor found {body.count(old)} times, regenerate/inspect")
    path.write_text(s[:lo] + body.replace(old, new) + s[hi:], encoding="utf-8")
    print(f"{path.name}: patched ({marker})")


def find(needle):
    hits = [p for p in gen.glob("recomp_*.c") if needle in p.read_text(encoding="utf-8")]
    if len(hits) != 1:
        sys.exit(f"{needle!r} found in {len(hits)} files")
    return hits[0]


# 1. sub_00072AB3: the jump target the lost tail returns to, then the lost tail.
p = find("void sub_00072AB3(void)")
patch(p,
      "loc_00072AF1: ;\n"
      "    esi = 0; /* xor self */\n"
      "    _fa = (uint32_t)(esi) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* xor result */\n"
      "    SET_LO8(ecx, MEM8(ebp + -12));\n",
      "loc_00072AF1: ;\n"
      "    esi = 0; /* xor self */\n"
      "    _fa = (uint32_t)(esi) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* xor result */\n"
      "loc_00072AF3: ; /* hand fix: target of the restored jmp at 0x00072BB1 */\n"
      "    SET_LO8(ecx, MEM8(ebp + -12));\n",
      "loc_00072AF3: ; /* hand fix", func="sub_00072AB3")
patch(p,
      "    MEM16(esi + 2) = LO16(eax);\n\n}",
      "    MEM16(esi + 2) = LO16(eax);\n"
      "    /* 0x00072BAD.. (hand fix: cut off by the false start at 0x00072BAA) */\n"
      "    MEM8(esi) = LO8(ebx);   /* mov [esi], bl */\n"
      "    esi = edx;              /* mov esi, edx */\n"
      "    goto loc_00072AF3;      /* jmp 0x00072AF3 */\n"
      "}",
      "cut off by the false start at 0x00072BAA", func="sub_00072AB3")
