/*
 * console_id.c -- see console_id.h.
 *
 * EEPROM layout (256 bytes):
 *   0x00 HMAC-SHA1 of the decrypted 0x14..0x2F, under the BIOS's EEPROM key
 *   0x14 confounder (8), 0x1C hard-drive key (16), 0x2C game region (4):
 *        RC4-encrypted, key = HMAC-SHA1(EEPROM key, the 20-byte hash)
 *   0x30 factory checksum over 0x34..0x5F
 *   0x34 serial number (12 ASCII digits), 0x40 Ethernet address (6),
 *   0x48 online key (16), 0x58 AV region (video standard)
 *   0x60 user checksum over 0x64..0xBF, then the user settings
 */
#include "console_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

static int s_done, s_have_eeprom, s_have_hdkey, s_verified;
static uint8_t s_eeprom[256], s_hdkey[16];
static uint32_t s_game_region;

uint32_t xbox_EepromChecksum(const uint8_t *p, unsigned bytes)
{
    uint32_t high = 0, low = 0;
    unsigned i;
    for (i = 0; i + 4 <= bytes; i += 4) {
        uint64_t sum = ((uint64_t)high << 32 | low) + ((uint32_t)p[i] | (uint32_t)p[i + 1] << 8 |
                                                     (uint32_t)p[i + 2] << 16 | (uint32_t)p[i + 3] << 24);
        high = (uint32_t)(sum >> 32);
        low = (uint32_t)sum;
    }
    return ~(high + low);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int hex16(const char *s, uint8_t out[16])
{
    int i = 0;
    while (s && *s && i < 32) {
        int c = *s++, v;
        if (c == ' ' || c == '-' || c == ':') continue;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        if (i & 1) out[i / 2] |= (uint8_t)v; else out[i / 2] = (uint8_t)(v << 4);
        i++;
    }
    return i == 32;
}

#if defined(_WIN32)
static int hmac_sha1(const uint8_t *key, unsigned klen, const uint8_t *d, unsigned n, uint8_t out[20])
{
    BCRYPT_ALG_HANDLE alg;
    NTSTATUS st;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG))
        return 0;
    st = BCryptHash(alg, (PUCHAR)key, klen, (PUCHAR)d, n, out, 20);
    BCryptCloseAlgorithmProvider(alg, 0);
    return st == 0;
}

static void rc4(const uint8_t *key, unsigned klen, uint8_t *d, unsigned n)
{
    uint8_t s[256], t;
    unsigned i, j = 0, k;
    for (i = 0; i < 256; i++) s[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) { j = (j + s[i] + key[i % klen]) & 255; t = s[i]; s[i] = s[j]; s[j] = t; }
    i = j = 0;
    for (k = 0; k < n; k++) {
        i = (i + 1) & 255; j = (j + s[i]) & 255;
        t = s[i]; s[i] = s[j]; s[j] = t;
        d[k] ^= s[(s[i] + s[j]) & 255];
    }
}

/* The encrypted section, with the BIOS's EEPROM key the player supplied:
 * 1 and the hard-drive key in hd when it decrypts and its HMAC checks, which
 * only an EEPROM written by a real console's BIOS does. */
static int decrypt_section(const uint8_t key[16], uint8_t hd[16])
{
    uint8_t rkey[20], sec[28], check[20];
    if (!hmac_sha1(key, 16, s_eeprom, 20, rkey))
        return 0;
    memcpy(sec, s_eeprom + 0x14, sizeof sec);
    rc4(rkey, 20, sec, sizeof sec);
    if (!hmac_sha1(key, 16, sec, sizeof sec, check) || memcmp(check, s_eeprom, 20))
        return 0;
    memcpy(hd, sec + 8, 16);
    s_game_region = rd32(sec + 24);
    memset(sec, 0, sizeof sec);
    memset(rkey, 0, sizeof rkey);
    return 1;
}
#else
static int decrypt_section(const uint8_t key[16], uint8_t hd[16]) { (void)key; (void)hd; return 0; }
#endif

/* The EEPROM key: RECOMP_XBOX_EEPROM_KEY (hex) or RECOMP_XBOX_EEPROM_KEY_FILE
 * (16 bytes, written by the title's Insignia Setup from the player's BIOS). */
static int eeprom_key(uint8_t k[16])
{
    const char *v = getenv("RECOMP_XBOX_EEPROM_KEY");
    FILE *f;
    if (v && *v) {
        if (hex16(v, k)) return 1;
        fprintf(stderr, "  [CONSOLE] RECOMP_XBOX_EEPROM_KEY is not 32 hex digits: not used\n");
        return 0;
    }
    v = getenv("RECOMP_XBOX_EEPROM_KEY_FILE");
    if (v && *v && (f = fopen(v, "rb")) != NULL) {
        size_t n = fread(k, 1, 16, f);
        fclose(f);
        if (n == 16) return 1;
        fprintf(stderr, "  [CONSOLE] the EEPROM key file is not 16 bytes: not used\n");
    }
    return 0;
}

void xbox_ConsoleIdInit(void)
{
    const char *path, *v;
    FILE *f;
    if (s_done) return;
    s_done = 1;

    path = getenv("RECOMP_XBOX_EEPROM");
    if (path && *path && (f = fopen(path, "rb")) != NULL) {
        size_t n = fread(s_eeprom, 1, sizeof s_eeprom, f);
        fclose(f);
        if (n != sizeof s_eeprom)
            fprintf(stderr, "  [CONSOLE] eeprom.bin is %u bytes, not 256: not used\n", (unsigned)n);
        else if (xbox_EepromChecksum(s_eeprom + 0x34, 0x2C) != rd32(s_eeprom + 0x30) ||
                 xbox_EepromChecksum(s_eeprom + 0x64, 0x5C) != rd32(s_eeprom + 0x60))
            fprintf(stderr, "  [CONSOLE] eeprom.bin checksums are wrong: not used\n");
        else
            s_have_eeprom = 1;
    } else if (path && *path) {
        fprintf(stderr, "  [CONSOLE] cannot open the EEPROM file\n");
    }

    if ((v = getenv("RECOMP_XBOX_HDKEY")) != NULL && *v) {
        if (hex16(v, s_hdkey)) s_have_hdkey = 1;
        else fprintf(stderr, "  [CONSOLE] RECOMP_XBOX_HDKEY is not 32 hex digits: not used\n");
    }
    /* Verified: the BIOS's EEPROM key opens the EEPROM's sealed section (a
     * real console wrote it) and the hard-drive key given is the one sealed
     * inside. Only a verified identity may reach Xbox Live. */
    if (s_have_eeprom) {
        uint8_t k[16], hd[16];
        if (eeprom_key(k)) {
            if (!decrypt_section(k, hd))
                fprintf(stderr, "  [CONSOLE] the EEPROM key does not unlock this EEPROM: identity not verified\n");
            else if (!s_have_hdkey) {
                memcpy(s_hdkey, hd, 16);
                s_have_hdkey = s_verified = 1;
            } else if (memcmp(hd, s_hdkey, 16) == 0)
                s_verified = 1;
            else
                fprintf(stderr, "  [CONSOLE] the hard-drive key given is not this EEPROM's: identity not verified\n");
        }
        memset(k, 0, sizeof k);
        memset(hd, 0, sizeof hd);
    }
    fprintf(stderr, "  [CONSOLE] identity: %s, hard-drive key %s, %s\n",
            s_have_eeprom ? "the player's EEPROM" : "none (no EEPROM)",
            s_have_hdkey ? (getenv("RECOMP_XBOX_HDKEY") ? "given" : "decrypted from the EEPROM") : "none",
            s_verified ? "verified against the console's BIOS" : "not verified");
}

int xbox_ConsoleIdentityVerified(void) { xbox_ConsoleIdInit(); return s_have_eeprom && s_have_hdkey && s_verified; }

int xbox_ConsoleHasEeprom(void) { xbox_ConsoleIdInit(); return s_have_eeprom; }
const uint8_t *xbox_ConsoleEeprom(void) { xbox_ConsoleIdInit(); return s_eeprom; }
uint32_t xbox_ConsoleGameRegion(void) { xbox_ConsoleIdInit(); return s_game_region; }

int xbox_ConsoleHdKey(uint8_t out[16])
{
    xbox_ConsoleIdInit();
    if (!s_have_hdkey) return 0;
    memcpy(out, s_hdkey, 16);
    return 1;
}
