Set-Location (Join-Path $PSScriptRoot '..\..\xboxrecomp-toolkit')
$A='..\halo2-native-tu15\analysis'
python -m tools.recomp "$A\default.xbe" --all --split 150 --game-name "Halo 2" --gen-dir "..\halo2-native-tu15\src\recomp\gen" --disasm-dir "$A\disasm" --func-id-dir "$A\identified" --abi-dir "$A\abi" -o "$A\lift" *> "$A\lift.log"
"done" | Out-File "$A\lift.done"
