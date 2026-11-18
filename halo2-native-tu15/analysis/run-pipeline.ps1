$ErrorActionPreference='Continue'
Set-Location (Join-Path $PSScriptRoot '..\..\xboxrecomp-toolkit')
$A='..\halo2-native-tu15\analysis'
python -m tools.xbe_parser "$A\default.xbe" --json "$A\default_analysis.json" *> "$A\parse.log"
python -m tools.disasm "$A\default.xbe" --analysis-json "$A\default_analysis.json" -o "$A\disasm" -v *> "$A\disasm.log"
python -m tools.func_id "$A\default.xbe" --functions "$A\disasm\functions.json" --strings "$A\disasm\strings.json" --xrefs "$A\disasm\xrefs.json" -o "$A\identified" -v *> "$A\identify.log"
python -m tools.abi_analysis "$A\default.xbe" --disasm-dir "$A\disasm" --func-id-dir "$A\identified" --output-dir "$A\abi" -v *> "$A\abi.log"
"PIPELINE-ANALYSIS-DONE" | Out-File "$A\pipeline.done"
