from pathlib import Path
import csv
import json
import re
import sys

project = Path(__file__).resolve().parents[1]
analysis = project / 'analysis'
sys.path.insert(0, str(project.parent / 'xboxrecomp-toolkit'))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator, load_label_db

def load(path):
    with path.open(encoding='utf-8') as file:
        return json.load(file)

# TU 1.5: arguments are ADDR:SIZE (hex addr, decimal size); there is no TU decomp inventory.
sizes = {int(a.split(':')[0], 16): int(a.split(':')[1]) for a in sys.argv[1:]}
addresses = list(sizes)
xbe = project / 'game/default.xbe'
config.configure_from_xbe(str(xbe))
functions = {int(info['start'], 16): dict(info, end=int(info['end'], 16))
             for info in load(analysis / 'disasm/functions.json')}
classes = {int(info['start'], 16): info
           for info in load(analysis / 'identified/identified_functions.json')}
abi = {int(info['address'], 16): info for info in load(analysis / 'abi/abi_functions.json')}
labels = load_label_db(str(analysis / 'disasm/labels.json'))
inventory = {a: {'size': s} for a, s in sizes.items()}

gen = project / 'src/recomp/gen'
dispatcher = gen / 'recomp_dispatch.c'
dispatch = dispatcher.read_text(encoding='utf-8')
for address in addresses:
    row = inventory[address]
    name = f'sub_{address:08X}'
    info = dict(start=f'0x{address:08X}', end=address + int(row['size']), name=name,
                size=int(row['size']), detection_method='seed_vtable_thunk')
    functions[address] = info
    labels[address] = name
translator = FunctionTranslator(xbe.read_bytes(), functions, labels, classes, abi,
                                seh_prolog=0x32A6E4, seh_epilog=0x32A71F)
for address in addresses:
    name = f'sub_{address:08X}'
    code = translator.translate_function(address, functions[address])
    if not code or f'void {name}(void)' not in code:
        raise SystemExit(f'Failed to lift {name}')
    entry = f'    {{ 0x{address:08X}u, (recomp_func_t){name} }},'
    if entry not in dispatch:
        entries = list(re.finditer(r'^    { 0x([0-9A-F]+)u, .* },$', dispatch, flags=re.M))
        next_entry = next((item for item in entries if int(item[1],16) > address), None)
        if next_entry is None:
            raise SystemExit(f'No following dispatch entry for {name}')
        dispatch = dispatch[:next_entry.start()] + entry + '\n' + dispatch[next_entry.start():]
        dispatch = dispatch.replace('static const recomp_entry_t g_recomp_table[] = {',
                                    f'void {name}(void);\nstatic const recomp_entry_t g_recomp_table[] = {{', 1)
    (gen / f'recovered_{address:08X}.c').write_text(
        '#define RECOMP_GENERATED_CODE\n#include "recomp_funcs.h"\n#include <math.h>\n\n' + code,
        encoding='utf-8')
    print(f'Recovered {name}')
dispatcher.write_text(dispatch, encoding='utf-8')
