"""x87 FXTRACT lifts to exponent-in-st1, significand-in-st0.

It used to fall through to the `/* FPU: ... */` comment, so nothing was pushed.
The XDK voice codec's _fLog2 is `fld x; fxtract; faddp st(1); fld1; fsubp`,
so log2 came out as garbage, the x87 stack was left one deeper on every call,
and Halo 2's voice chat encoded noise. Checked here: the emitted statement, and
(when a C compiler is on PATH) a compiled run of that _fLog2 sequence.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from .disasm import Instruction
from .lifter import Lifter
from .translator import FP_STACK_MACROS


def _lift(mnemonic, op_str=""):
    insn = Instruction(0, 2, mnemonic, op_str, "")
    insn.operands = []
    return "\n".join(Lifter().lift_instruction(insn))


class FxtractEmitTest(unittest.TestCase):
    def test_not_left_as_a_comment(self):
        out = _lift("fxtract")
        self.assertNotIn("FPU: fxtract", out)
        self.assertIn("frexp(_v, &_e)", out)
        self.assertIn("fp_top() = (double)(_e - 1); fp_push(_s);", out)

    def test_special_values_push_too(self):
        out = _lift("fxtract")
        self.assertIn("if (_v == 0.0) { fp_top() = -INFINITY; fp_push(_v); }", out)
        self.assertIn("else if (isinf(_v)) { fp_top() = INFINITY; fp_push(_v); }", out)
        self.assertIn("else if (_v != _v) { fp_push(_v); }", out)


_PROGRAM = r"""
#include <math.h>
#include <stdio.h>
#include <stdint.h>
static double g_fp_stack[8];
static unsigned g_fp_top;
%(macros)s
static double flog2(double x) {
    unsigned depth = g_fp_top;
    double r;
    fp_push(x);
    %(fxtract)s
    fp_st1() = fp_st1() + fp_top(); fp_pop();
    fp_push(1.0);
    fp_st1() = fp_st1() - fp_top(); fp_pop();
    r = fp_top(); fp_pop();
    if (g_fp_top != depth) return 1e300;    /* stack not balanced */
    return r;
}
int main(void) {
    printf("%%.6f %%.6f %%.6f %%.6f\n", flog2(1.0), flog2(8.0), flog2(0.25), flog2(12.0));
    return 0;
}
"""


class FxtractRunTest(unittest.TestCase):
    def test_flog2_sequence(self):
        cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
        if not cc:
            self.skipTest("no C compiler on PATH")
        src = _PROGRAM % {"macros": "\n".join(FP_STACK_MACROS), "fxtract": _lift("fxtract")}
        with tempfile.TemporaryDirectory() as tmp:
            c = os.path.join(tmp, "t.c")
            exe = os.path.join(tmp, "t.exe" if os.name == "nt" else "t")
            Path(c).write_text(src)
            subprocess.run([cc, "-O1", "-o", exe, c, "-lm"], check=True)
            out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout.split()
        # exponent + significand - 1: exact at powers of two, the codec's
        # linear approximation between them (12 = 1.5 * 2^3 -> 3.5)
        self.assertEqual([float(v) for v in out], [0.0, 3.0, -2.0, 3.5])


if __name__ == "__main__":
    unittest.main()
