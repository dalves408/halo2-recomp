$ErrorActionPreference='Continue'
Set-Location (Join-Path $PSScriptRoot '..\..\xboxrecomp-toolkit')
$A='..\halo2-native-tu15\analysis-downloader'
python -m tools.xbe_parser "$A\downloader.xbe" --json "$A\downloader_analysis.json" *> "$A\parse.log"
python -m tools.disasm "$A\downloader.xbe" --analysis-json "$A\downloader_analysis.json" -o "$A\disasm" -v *> "$A\disasm.log"
python -m tools.func_id "$A\downloader.xbe" --functions "$A\disasm\functions.json" --strings "$A\disasm\strings.json" --xrefs "$A\disasm\xrefs.json" -o "$A\identified" -v *> "$A\identify.log"
python -m tools.abi_analysis "$A\downloader.xbe" --disasm-dir "$A\disasm" --func-id-dir "$A\identified" --output-dir "$A\abi" -v *> "$A\abi.log"
"PIPELINE-ANALYSIS-DONE" | Out-File "$A\pipeline.done"
python -m tools.recomp "$A\downloader.xbe" --all --split 150 --game-name "Halo 2 Downloader" --gen-dir "..\halo2-native-tu15\src\downloader\gen" --disasm-dir "$A\disasm" --func-id-dir "$A\identified" --abi-dir "$A\abi" -o "$A\lift" *> "$A\lift.log"
"done" | Out-File "$A\lift.done"
