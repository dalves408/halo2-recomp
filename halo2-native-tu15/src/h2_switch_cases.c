/*
 * h2_switch_cases.c -- case blocks of a switch the lifter did not make into
 * functions (TU 1.5 addresses).
 *
 * sub_0005B0B0 (thiscall, ecx = an object, one stack argument; `ret 4`) is a
 * switch on the object's state at +0x741C:
 *
 *     jmp dword ptr [(state - 1) * 4 + 0x5B1A0]
 *
 * The table holds 0x5B0EC, 0, 0x5B0DD, 0, 0x5B0F9, 0, 0x5B0F9, 0x5B115,
 * 0x5B138, 0x5B182. The disassembler made functions of 0x5B0EC, 0x5B0F9 and
 * 0x5B138 (tail_jump_alias), but 0x5B0DD, 0x5B115 and 0x5B182 sit inside
 * those and have no function of their own, so the generated indirect tail jump
 * (RECOMP_ITAIL) found nothing: "[ICALL] Failed to resolve VA 0x0005B0DD",
 * then an access violation in the same thread (seen on Xbox Live through
 * Insignia, 2026-11-20). recomp_lookup_manual (recomp_manual.c) returns these.
 *
 * Each case runs in sub_0005B0B0's frame: esi and ebx pushed, 0x34 bytes of
 * locals, ebx = the object, the stack argument at [esp + 0x40]. They end with
 * the shared epilogue (pop esi, pop ebx, add esp 0x34, ret 4), written the way
 * the generated code writes it (see sub_0005B0EC in recomp_0006.c).
 */
#include <stdint.h>
#define RECOMP_GENERATED_CODE   /* register names (eax, esp, ...) as the generated files use them */
#include "gen/recomp_funcs.h"

#define CASE_EPILOGUE() do { \
        POP32(esp, esi); \
        POP32(esp, ebx); \
        esp = esp + 0x34; \
        g_ebp = ebp; g_seh_ebp = ebp; esp += 8; return; /* ret 4 */ \
    } while (0)

/* state 3, 0x5B0DD: mov eax, ebx; call 0x62110; epilogue */
void h2_case_0005B0DD(void)
{
    uint32_t ebp = g_seh_ebp;   /* fpo: the switch's frame */
    eax = ebx;
    g_ebp = ebp; g_seh_ebp = ebp;
    PUSH32(esp, 0x0005B0E4u); RECOMP_ABI_CALL(0x00062110u, sub_00062110);
    CASE_EPILOGUE();
}

/* state 8, 0x5B115: pop esi; mov byte [ebx + 0x7420], 1; pop ebx; add esp, 0x34; ret 4 */
void h2_case_0005B115(void)
{
    uint32_t ebp = g_seh_ebp;
    POP32(esp, esi);
    MEM8(ebx + 0x7420) = 1;
    POP32(esp, ebx);
    esp = esp + 0x34;
    g_ebp = ebp; g_seh_ebp = ebp; esp += 8; return; /* ret 4 */
}

/* state 10, 0x5B182: mov al, [esp + 0x40]; test al, al; jne 0x5B0E4 (epilogue);
 * mov eax, ebx; call 0x5B1D0; epilogue */
void h2_case_0005B182(void)
{
    uint32_t ebp = g_seh_ebp;
    SET_LO8(eax, MEM8(esp + 0x40));
    if (LO8(eax) != 0)
        CASE_EPILOGUE();
    eax = ebx;
    g_ebp = ebp; g_seh_ebp = ebp;
    PUSH32(esp, 0x0005B195u); RECOMP_ABI_CALL(0x0005B1D0u, sub_0005B1D0);
    CASE_EPILOGUE();
}
