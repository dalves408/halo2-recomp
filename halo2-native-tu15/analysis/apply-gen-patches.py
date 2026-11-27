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

# 5. Mouse aim stays out of Xbox Live (src/h2_mouse.c): entry hooks on the main menu's
#    choices (TU 1.5 addresses, matched by code bytes to the retail functions the Halo 2
#    decompilation documents): Xbox Live turns the mouse off, campaign, split screen and
#    System Link turn it back on.
for va, live in ((0x0023737C, 1), (0x00237324, 0), (0x00237488, 0), (0x00237500, 0)):
    p = find(f"void sub_{va:08X}(void)\n{{")
    patch(p, f"void sub_{va:08X}(void)\n{{\n",
          f"void sub_{va:08X}(void)\n{{\n"
          f"    extern void h2_mouse_live_menu(int live);\n"
          f"    h2_mouse_live_menu({live});   /* main menu choice {va:08X} (hand hook, src/h2_mouse.c) */\n",
          f"main menu choice {va:08X} (hand hook")

# 6. Widescreen 16:10 (src/settings.c): in the camera projection, sub_0002FB50, the
#    Xbox's widescreen mode widens the view by 4/3 (4:3 to 16:9). That factor becomes
#    g_h2_ws_aspect_mul: 4/3 for 16:9 (unchanged), 1.2 for 16:10. The same 4/3 constant
#    (0x46A514) is used elsewhere and is left alone.
p = find("void sub_0002FB50(void)")
patch(p, "    xmm5 = XMM_SCALAR(MEMF(0x46A514)); /* movss */\n    xmm3.f[0] = xmm3.f[0] * xmm5.f[0]; /* mulss */\n",
      "    xmm5 = XMM_SCALAR(MEMF(0x46A514)); /* movss */\n"
      "    { extern float g_h2_ws_aspect_mul;   /* widescreen 16:10 (hand fix, src/settings.c) */\n"
      "      xmm3.f[0] = xmm3.f[0] * g_h2_ws_aspect_mul; }\n",
      "widescreen 16:10 (hand fix, src/settings.c)")

# 7. True widescreen (src/settings.c): the camera projection, sub_0002FB50, computes the
#    view's aspect (width / height) at 0x0002FC34; it is multiplied by g_h2_wide_fov
#    (1.333 for 16:9, 1.2 for 16:10, 1 for 4:3), so the 3D view sees more at the sides
#    while the game stays in its 4:3 mode (HUD, menus and split screen unchanged).
p = find("void sub_0002FB50(void)")
patch(p, "    xmm3.f[0] = xmm3.f[0] / MEMF(eax + 0x14); /* divss */\n",
      "    xmm3.f[0] = xmm3.f[0] / MEMF(eax + 0x14); /* divss */\n"
      "    { extern float g_h2_wide_fov;   /* true widescreen (hand fix, src/settings.c) */\n"
      "      xmm3.f[0] = xmm3.f[0] * g_h2_wide_fov; }\n",
      "true widescreen (hand fix, src/settings.c)")

# 8. Switch at 0x002346F7 in sub_002346D9 (table 0x0023487D, index = arg - 0xE446): the table
#    has 16 cases, 8 zero slots, then a live 25th slot (index 24 -> 0x0023473D, `or dl, 0FFh`).
#    The lifter stops at the first zero slot, so index 24 fell to RECOMP_ITAIL ("[ICALL] Failed
#    to resolve VA 0x0023473D"), which returned with the 0x220-byte frame still on the stack and
#    crashed soon after (seen once in a System Link Slayer match).
p = find("    if (_jt == 0x00234739u) goto loc_00234739;\n")
patch(p, "    if (_jt == 0x00234739u) goto loc_00234739;\n",
      "    if (_jt == 0x00234739u) goto loc_00234739;\n"
      "    if (_jt == 0x0023473Du) goto loc_0023473D; /* slot 24 (hand-extended) */\n",
      "goto loc_0023473D; /* slot 24 (hand-extended)")
patch(p, "loc_00234739: ;\n    SET_LO8(edx, 0xF);\n    goto loc_00234740;\n\nloc_00234740: ;",
      "loc_00234739: ;\n    SET_LO8(edx, 0xF);\n    goto loc_00234740;\n\n"
      "loc_0023473D: ;\n    SET_LO8(edx, 0xFF); /* or dl, 0FFh (hand-extended slot 24) */\n\n"
      "loc_00234740: ;", "or dl, 0FFh (hand-extended slot 24)")

# 9. Switch at 0x002445D3 (table 0x002446B0, index = MEM32(esi + 0xC14) - 1): 4 cases, 2 zero
#    slots, then slots 6-8 (0x002445ED `shr eax, 6`, 0x002445F2 `shr eax, 7`, 0x002445DF).
#    Same lifter stop at the first zero slot as patch 8; 0x002445ED/F2 had no translation.
#    (The similar table at 0x0021D498 needs nothing: its extra slots were lifted as small
#    functions, sub_0021D471/489/48F, which RECOMP_ITAIL finds.)
p = find("MEM32(esi * 4 + 0x2446B0)")
patch(p, "    if (_jt == 0x002445E8u) goto loc_002445E8;\n",
      "    if (_jt == 0x002445E8u) goto loc_002445E8;\n"
      "    if (_jt == 0x002445EDu) goto loc_002445ED; /* slot 6 (hand-extended) */\n"
      "    if (_jt == 0x002445F2u) goto loc_002445F2; /* slot 7 (hand-extended) */\n",
      "goto loc_002445ED; /* slot 6 (hand-extended)")
patch(p, "    eax = (uint32_t)(eax) >> ((2) & 31u);\n"
         "    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */\n"
         "    goto loc_002445F5;\n\nloc_002445F5: ;",
      "    eax = (uint32_t)(eax) >> ((2) & 31u);\n"
      "    _fa = (uint32_t)(eax) & 0xFFFFFFFFu; _fas = (int32_t)(int32_t)(_fa); /* shift result */\n"
      "    goto loc_002445F5;\n\n"
      "loc_002445ED: ;   /* shr eax, 6 (hand-extended slot 6) */\n"
      "    eax = (uint32_t)(eax) >> 6;\n    goto loc_002445F5;\n\n"
      "loc_002445F2: ;   /* shr eax, 7 (hand-extended slot 7) */\n"
      "    eax = (uint32_t)(eax) >> 7;\n\n"
      "loc_002445F5: ;", "shr eax, 6 (hand-extended slot 6)")

# 10. Test switch RECOMP_HALO2_UNLOCK_LEVELS (src/main.c): sub_0019337D (1.0 0x190565) gives the
#     highest campaign level any signed-in profile has reached; Select Level lists up to it and a
#     custom-game map list compares against it. With the switch on it answers 0x7FFF (every level).
#     It is a cdecl function with no arguments, so the hook returns before its prologue.
p = find("void sub_0019337D(void)\n{")
patch(p, "void sub_0019337D(void)\n{\n",
      "void sub_0019337D(void)\n{\n"
      "    extern int g_halo2_unlock_levels;   /* test switch (hand hook, src/main.c) */\n"
      "    if (g_halo2_unlock_levels) { eax = 0x7FFF; esp += 4; return; }\n",
      "extern int g_halo2_unlock_levels;")

# 11. Camera interpolation (src/h2_interp.c): the main loop sub_0012DF60 (1.0 0x12b450) draws the
#     frame with sub_00229C50 (1.0 0x223240) on both of its paths. The hooks around each call put a
#     blended camera position into the per-player view records for the draw and the game's own
#     position back after it.
p = find("PUSH32(esp, 0x0012E16Eu); RECOMP_ABI_CALL(0x00229C50u, sub_00229C50);")
patch(p, '#include <math.h>\n', '#include <math.h>\n\nvoid h2_interp_render_begin(void);\nvoid h2_interp_render_end(void);\n',
      'void h2_interp_render_begin(void);')
for ret in ("0x0012E16Eu", "0x0012E18Cu"):
    call = f"    PUSH32(esp, {ret}); RECOMP_ABI_CALL(0x00229C50u, sub_00229C50); /* call 0x00229C50 */\n"
    patch(p, call, "    h2_interp_render_begin();   /* camera interpolation (hand hook) */\n" + call +
          "    h2_interp_render_end();\n", f"h2_interp_render_begin();   /* camera interpolation (hand hook) */\n    PUSH32(esp, {ret})")

# 11b. Interpolation, after the world update: sub_0013AA50 runs the due 30 Hz ticks; the hook after
#      it runs before the per-frame update (sub_0013A9B0) that places the camera.
patch(p, 'void h2_interp_render_end(void);\n', 'void h2_interp_render_end(void);\nvoid h2_interp_world_updated(void);\n',
      'void h2_interp_world_updated(void);')
call = "    PUSH32(esp, 0x0012E147u); RECOMP_ABI_CALL(0x0013AA50u, sub_0013AA50); /* call 0x0013AA50 */\n"
patch(p, call, call + "    h2_interp_world_updated();   /* interpolation (hand hook) */\n",
      "h2_interp_world_updated();   /* interpolation (hand hook) */")

# 13. Interpolation, the main view: sub_0002B9F0 renders the main view from the camera at eax (and a
#     second copy from its second argument). The hook at its entry blends that camera for a
#     cutscene (src/h2_interp.c).
p13 = find("void sub_0002B9F0(void)\n")
patch(p13, "loc_0002B9F0: ;\n", "loc_0002B9F0: ;\n    h2_interp_view_render(eax, MEM32(esp + 8));   /* interpolation (hand hook) */\n",
      "h2_interp_view_render(eax, MEM32(esp + 8));")
patch(p13, "#include <math.h>\n", "#include <math.h>\n\nvoid h2_interp_view_render(uint32_t camera, uint32_t camera2);\n",
      "void h2_interp_view_render(uint32_t camera, uint32_t camera2);")

# 12. Camera interpolation, the copies: every render pass copies a view (0x1D dwords) into the
#     render camera 0x4C65C0 with rep movsd, then builds its view matrix (0x4C6634) from it. The
#     hook after each copy moves the camera to the frame's blended position (src/h2_interp.c); the
#     view record itself is filled inside the draw, after the hooks of patch 11 run.
copy_head = "    edi = 0x4C65C0;\n    { uint64_t _n = (uint64_t)ecx * 4u;\n"
copy_tail = "    ecx = 0; } /* rep movsd */\n"
hook = "    h2_interp_camera_copied();   /* camera interpolation (hand hook) */\n"
for path in sorted(gen.glob("recomp_*.c")):
    s = path.read_text(encoding="utf-8")
    if copy_head not in s:
        continue
    out, pos, n = [], 0, 0
    while True:
        i = s.find(copy_head, pos)
        if i < 0:
            out.append(s[pos:]); break
        j = s.index(copy_tail, i) + len(copy_tail)
        out.append(s[pos:j])
        if not s.startswith(hook, j):
            out.append(hook); n += 1
        pos = j
    if n:
        s = "".join(out)
        if "void h2_interp_camera_copied(void);" not in s:
            s = s.replace("#include <math.h>\n", "#include <math.h>\n\nvoid h2_interp_camera_copied(void);\n", 1)
        path.write_text(s, encoding="utf-8")
        print(f"{path.name}: patched (camera copy hook x{n})")
    else:
        print(f"{path.name}: already patched (camera copy hook)")

# 14. Interpolation, the first-person model: inside the main view, sub_0003D2B0 has sub_00168DED
#     build the local player's first-person node sets (up to 4 x (12-byte header + 64 nodes) at
#     0x4CE3F8, count to 0x4CE3F4) from the view's camera. Their animation steps at 30 Hz; the hook
#     after the build blends them in the camera's space (src/h2_interp.c).
p14 = find("void sub_0003D2B0(void)\n")
patch(p14, "loc_0003D35F: ;\n    MEM32(0x4CE3F4) = eax;\n",
      "loc_0003D35F: ;\n    MEM32(0x4CE3F4) = eax;\n    h2_interp_first_person(0x4CE3F8, eax);   /* interpolation (hand hook) */\n",
      "h2_interp_first_person(0x4CE3F8, eax);")
patch(p14, "#include <math.h>\n", "#include <math.h>\n\nvoid h2_interp_first_person(uint32_t sets, uint32_t count);\n",
      "void h2_interp_first_person(uint32_t sets, uint32_t count);")

# 15. Re-poses between ticks: inside the main view, sub_0002B970 (retail 0x2b990) sets up the view's
#     visible objects and re-poses some cutscene characters itself, mostly on frames with no new
#     tick, which the Xbox (one draw a tick) never showed. Hooks around the call put changed node
#     blocks back on such frames (src/h2_interp.c, h2_interp_view_objects).
p15 = find("void sub_0002B9F0(void)\n")
call15 = "    PUSH32(esp, 0x0002BC57u); RECOMP_ABI_CALL(0x0002B970u, sub_0002B970); /* call 0x0002B970 */\n"
patch(p15, call15, "    h2_interp_view_objects(0);   /* re-pose guard (hand hook) */\n" + call15 +
      "    h2_interp_view_objects(1);   /* re-pose guard (hand hook) */\n", "h2_interp_view_objects(0);")
patch(p15, "#include <math.h>\n", "#include <math.h>\n\nvoid h2_interp_view_objects(int after);\n",
      "void h2_interp_view_objects(int after);")

# 16. D3D's PFB flush kick (sub_00404EC0, the XDK KickOff wait): it sets 0x100410 bit 16 and spins
#     until the "GPU" clears it. Our pushbuffer thread clears it, so the spin burned a whole core
#     (47% of the game thread in Outskirts, 2026-11-26) that weak PCs need for that thread. With
#     RECOMP_PFB_FLUSH_WAIT=1 (main.c default) it waits on the toolkit's acknowledgement event
#     instead (nv2a_pfb_flush.c); =0 keeps the original spin.
p16 = find("void sub_00404EC0(void)\n")
patch(p16, "    MEM32(eax + 0x100410) = ecx;\n\nloc_00404EE0: ;\n",
      "    MEM32(eax + 0x100410) = ecx;\n"
      "    if (xbox_Nv2aPfbFlushWait()) { esp += 4; return; }   /* event wait (hand hook) */\n\nloc_00404EE0: ;\n",
      "xbox_Nv2aPfbFlushWait()) { esp += 4; return; }")
patch(p16, "#include <math.h>\n", "#include <math.h>\n\nint xbox_Nv2aPfbFlushWait(void);\n",
      "int xbox_Nv2aPfbFlushWait(void);")
