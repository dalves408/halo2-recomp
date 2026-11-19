r"""Derive the per-title keys a retail Xbox kernel makes when it starts a game.

A real kernel computes, from the running title's certificate and the CERT key
in the console's BIOS second-stage loader (2BL):
  XboxLANKey                  = HMAC-SHA1(CERT key, cert.LanKey)[:16]
  XboxSignatureKey            = HMAC-SHA1(CERT key, cert.SignatureKey)[:16]
  XboxAlternateSignatureKeys  = HMAC-SHA1(CERT key, cert.AltSignatureKey[i])[:16], i = 0..15
Xbox Live checks the title with them, so a recompiled title needs them to sign in.

Inputs are the player's own dumps only (nothing is built in): their console's
BIOS, its MCPX boot ROM and its EEPROM, plus the game's default.xbe. The 2BL is
decrypted as the MCPX does (1.0: RC4 with the ROM key at 0x1A5; 1.1: RC4 with
SHA-1(ROM key 0x19C | flash key | ROM key ^ 0x5C)); the EEPROM key is the 16
bytes that verify the EEPROM's own HMAC, and the CERT key the 16 after it
(the same layout CounterStrikeXB's Insignia Setup uses for XboxLANKey).

Writes <out> = LAN key (16) + signature key (16) + 16 alternate keys (256),
288 bytes. No key is printed.

    python derive_title_keys.py --bios bios.bin --mcpx mcpx_1.0.bin --mcpx mcpx_1.1.bin \
        --eeprom eeprom.bin --xbe game\default.xbe --out SaveData\Identity\titlekeys.bin
"""
import argparse
import hashlib
import hmac
import os
import struct
import sys


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 255
        s[i], s[j] = s[j], s[i]
    out = bytearray(data)
    i = j = 0
    for k in range(len(out)):
        i = (i + 1) & 255
        j = (j + s[i]) & 255
        s[i], s[j] = s[j], s[i]
        out[k] ^= s[(s[i] + s[j]) & 255]
    return bytes(out)


def hmac1(key, data):
    return hmac.new(key, data, hashlib.sha1).digest()


def eeprom_key_ok(k, eeprom):
    rk = hmac1(k, eeprom[:20])
    sec = rc4(rk, eeprom[0x14:0x30])
    return hmac1(k, sec) == eeprom[:20]


def find_cert_key(bios, mcpx_files, eeprom):
    im = bios[len(bios) - 0x40000:]           # flash is mirrored up to the top of memory
    for path in mcpx_files:
        mcpx = open(path, 'rb').read()
        if len(mcpx) != 512:
            continue
        candidates = []
        blk = mcpx[0x19C:0x1AC] + im[0x3FDF0:0x3FE00] + bytes(b ^ 0x5C for b in mcpx[0x19C:0x1AC])
        candidates.append(('1.1', rc4(hashlib.sha1(blk).digest(), im[0x39E00:0x39E00 + 0x3600])))
        candidates.append(('1.0', rc4(mcpx[0x1A5:0x1B5], im[0x39E00:0x39E00 + 0x6000])))
        for rev, dec in candidates:
            for p in range(len(dec) - 32):
                if eeprom_key_ok(dec[p:p + 16], eeprom):
                    return rev, os.path.basename(path), dec[p + 16:p + 32]
    return None, None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bios', required=True)
    ap.add_argument('--mcpx', required=True, action='append')
    ap.add_argument('--eeprom', required=True)
    ap.add_argument('--xbe', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    bios = open(a.bios, 'rb').read()
    eeprom = open(a.eeprom, 'rb').read()
    xbe = open(a.xbe, 'rb').read()
    if len(bios) < 0x40000 or len(eeprom) != 256 or xbe[:4] != b'XBEH':
        sys.exit('ERROR: need a BIOS dump (>= 256 KB), a 256-byte EEPROM and an XBE')
    rev, which, cert_key = find_cert_key(bios, a.mcpx, eeprom)
    if not cert_key:
        sys.exit('ERROR: these BIOS/MCPX dumps do not unlock this EEPROM (not the same console?)')
    base = struct.unpack_from('<I', xbe, 0x104)[0]
    cert = struct.unpack_from('<I', xbe, 0x118)[0] - base
    lan = hmac1(cert_key, xbe[cert + 0xB0:cert + 0xC0])[:16]
    sig = hmac1(cert_key, xbe[cert + 0xC0:cert + 0xD0])[:16]
    alt = b''.join(hmac1(cert_key, xbe[cert + 0xD0 + 16 * i:cert + 0xE0 + 16 * i])[:16] for i in range(16))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'wb') as f:
        f.write(lan + sig + alt)
    title, version = struct.unpack_from('<I', xbe, cert + 8)[0], struct.unpack_from('<I', xbe, cert + 0xAC)[0]
    print('OK: MCPX %s (%s) unlocked the EEPROM; title %08X version %X; wrote %d bytes to %s'
          % (rev, which, title, version, len(lan + sig + alt), a.out))


if __name__ == '__main__':
    main()
