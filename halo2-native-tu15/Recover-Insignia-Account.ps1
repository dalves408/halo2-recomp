# Recover an Insignia account into this install, through Halo 2's own Xbox Live library.
#
# Asks for the gamertag and the 16-digit recovery number shown on insignia.live, runs the
# game briefly with H2_ACCT=recover, and hands the two over an anonymous pipe. The number
# is never written to disk, a log, an argument or an environment variable, and the game
# wipes it once sent. The account is saved into SaveData\DiskConfig.bin (a copy of the
# file is kept first). Only one copy of this console identity may be online at a time:
# close the console, xemu and other copies of the game first.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$id = Join-Path $root 'SaveData\Identity'
$exe = if ($env:H2_EXE) { Join-Path $root $env:H2_EXE } else { Join-Path $root 'build-ninja\halo2_recomp.exe' }

foreach ($f in 'eeprom.bin', 'hddinfo.txt') {
    if (-not (Test-Path (Join-Path $id $f))) { Write-Host "Missing SaveData\Identity\$f. Run Insignia Setup first."; exit 1 }
}
if (-not (Test-Path $exe)) { Write-Host "Game not found: $exe"; exit 1 }

# The console identity, as Launch-Halo2-Insignia.cmd loads it. The key is never printed.
$line = Select-String -Path (Join-Path $id 'hddinfo.txt') -Pattern 'XboxHDKey' | Select-Object -First 1
if (-not $line) { Write-Host 'No XboxHDKey line in SaveData\Identity\hddinfo.txt'; exit 1 }
$env:RECOMP_XBOX_HDKEY = (($line.Line -replace '.*XboxHDKey', '') -replace '[^0-9A-Fa-f]', '').Substring(0, 32)
$env:RECOMP_XBOX_EEPROM = Join-Path $id 'eeprom.bin'
if (Test-Path (Join-Path $id 'titlekeys.bin')) { $env:RECOMP_XBOX_TITLE_KEYS = Join-Path $id 'titlekeys.bin' }
$env:RECOMP_NET = '1'
$env:RECOMP_FB_WINDOW = '1'
# A controller (the keyboard pad) as the launcher gives it: without one the game waits on
# a screen that never reaches the point where the recovery starts.
if (-not $env:RECOMP_KEYBOARD) { $env:RECOMP_KEYBOARD = '1' }
if (-not $env:RECOMP_GPU) { $env:RECOMP_GPU = 'd3d11' }
$env:H2_ACCT = 'recover'

$gamertag = (Read-Host 'Gamertag').Trim()
if (-not $gamertag -or $gamertag.Length -gt 15) { Write-Host 'A gamertag is 1 to 15 characters.'; exit 1 }
$secure = Read-Host 'Recovery number (16 digits, input hidden)' -AsSecureString

# Keep a copy of the drive's account area before anything is written.
$cfg = Join-Path $root 'SaveData\DiskConfig.bin'
if (Test-Path $cfg) {
    $copy = Join-Path $root ('SaveData\DiskConfig.before-recovery-{0}.bin' -f (Get-Date -Format yyyyMMdd_HHmmss))
    Copy-Item $cfg $copy
    Write-Host "Saved a copy of DiskConfig.bin as $(Split-Path $copy -Leaf)"
}

$logs = Join-Path $root 'build-ninja\screenshots'
New-Item -ItemType Directory -Force $logs | Out-Null
$stamp = Get-Date -Format yyyyMMdd_HHmmss
$log = Join-Path $logs "recovery_$stamp.log"

$pipe = New-Object System.IO.Pipes.AnonymousPipeServerStream([System.IO.Pipes.PipeDirection]::Out,
                                                            [System.IO.HandleInheritability]::Inheritable)
$env:H2_ACCT_INPUT = $pipe.GetClientHandleAsString()
Write-Host 'Starting Halo 2 to recover the account (about a minute; the window closes by itself)...'
# Started the way Launch-Halo2-Insignia.cmd starts it (cmd with file redirection; started
# directly with PowerShell's redirection the game stalled while loading). No shell execute,
# so the pipe's inheritable handle reaches cmd and, through it, the game.
$si = New-Object System.Diagnostics.ProcessStartInfo
$si.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
$si.Arguments = '/c ""{0}" 1>"{1}" 2>"{2}""' -f $exe, (Join-Path $logs "recovery_${stamp}_stdout.log"), $log
$si.WorkingDirectory = $root
$si.UseShellExecute = $false
$p = [System.Diagnostics.Process]::Start($si)
$pipe.DisposeLocalCopyOfClientHandle()
Remove-Item Env:H2_ACCT_INPUT, Env:H2_ACCT

# Gamertag and number, one line each. The plain number only exists inside this block.
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
    $bytes = [Text.Encoding]::ASCII.GetBytes("$gamertag`n$plain`n")
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    [Array]::Clear($bytes, 0, $bytes.Length)
    $plain = $null
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    $pipe.Dispose()
    $secure.Dispose()
}

$p.WaitForExit()
$messages = @{
    0  = 'Account recovered and saved. Start the game with Launch-Halo2-Insignia.cmd and sign in.'
    10 = 'The game did not receive the gamertag and number.'
    11 = 'The console could not sign in to Insignia (check the network and the console identity).'
    12 = 'No answer from Insignia in time. Try again in a few minutes.'
    13 = 'Insignia refused: check the gamertag and the recovery number on insignia.live.'
    14 = 'The account could not be saved to SaveData\DiskConfig.bin.'
    15 = 'All eight account slots are in use. Remove an account first.'
}
$code = $p.ExitCode
$msg = $messages[[int]$code]
if (-not $msg) { $msg = "The game stopped with code $code. See $log." }
Write-Host $msg
exit $code
