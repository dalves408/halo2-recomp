/*
 * kernel_crypto.c - Xbox Crypto Functions
 *
 * Implements SHA-1, RC4, HMAC-SHA1, and public key operations.
 *
 * The Xbox uses these for:
 *   - Save game signing (SHA-1 + HMAC)
 *   - Xbox Live authentication (RSA, RC4)
 *   - Content verification (SHA-1)
 *
 * Implementation uses Windows BCrypt API (available since Vista).
 * For save game compatibility, all crypto must produce identical outputs
 * to the original Xbox implementations.
 *
 * Note: RC4 is implemented manually since BCrypt deprecated it in newer
 * Windows versions. The Xbox RC4 is standard RC4 (ARC4).
 */

#include "kernel.h"
#include <string.h>
#include <stdint.h>

/* RECOMP_XC_TRACE=1: one line per kernel crypto call -- what was called and
 * with what lengths, never the data or keys. */
#include <stdio.h>
#include <stdlib.h>
static int xc_trace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("RECOMP_XC_TRACE"); on = e && e[0] == '1'; }
    return on;
}
#define XC_TRACE(...) do { if (xc_trace_on()) { fprintf(stderr, "  [XC] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* SHA-1 / RC4 / HMAC below are portable software implementations, so the
 * only platform dependency is BCrypt's types in the (unused) SHA_INTERNAL
 * struct. Keep it Windows-only. */
#if defined(_WIN32)
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

/* ============================================================================
 * SHA-1
 *
 * Xbox SHA-1 is standard FIPS 180-1 SHA-1. We use BCrypt for correctness.
 * The XBOX_SHA_CONTEXT is our own structure (defined in kernel.h).
 * We store the BCrypt hash handle inside it.
 * ============================================================================ */

/* We use a wrapper since XBOX_SHA_CONTEXT may not be large enough for
 * BCrypt's internal state. Store BCrypt handles in a separate mapping. */

#if defined(_WIN32)
typedef struct _SHA_INTERNAL {
    BCRYPT_ALG_HANDLE  hAlg;
    BCRYPT_HASH_HANDLE hHash;
    UCHAR              hashObject[256]; /* BCrypt hash object buffer */
} SHA_INTERNAL;
#endif

/*
 * Since XBOX_SHA_CONTEXT is only 92 bytes (State[5] + Count[2] + Buffer[64]),
 * we can't fit a BCrypt handle in it. We use a simple software SHA-1 instead.
 * This also ensures byte-exact compatibility with the Xbox implementation.
 */

/* Software SHA-1 implementation - matches Xbox/FIPS exactly */

static ULONG sha1_rotate_left(ULONG value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_transform(ULONG state[5], const UCHAR buffer[64])
{
    ULONG a, b, c, d, e, f, k, temp;
    ULONG w[80];
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        w[i] = ((ULONG)buffer[i * 4] << 24)
             | ((ULONG)buffer[i * 4 + 1] << 16)
             | ((ULONG)buffer[i * 4 + 2] << 8)
             | ((ULONG)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rotate_left(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = sha1_rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

VOID __stdcall xbox_XcSHAInit(PXBOX_SHA_CONTEXT ShaContext)
{
    XC_TRACE("SHAInit");
    if (!ShaContext)
        return;

    ShaContext->State[0] = 0x67452301;
    ShaContext->State[1] = 0xEFCDAB89;
    ShaContext->State[2] = 0x98BADCFE;
    ShaContext->State[3] = 0x10325476;
    ShaContext->State[4] = 0xC3D2E1F0;
    ShaContext->Count[0] = 0;
    ShaContext->Count[1] = 0;
    memset(ShaContext->Buffer, 0, 64);
}

VOID __stdcall xbox_XcSHAUpdate(PXBOX_SHA_CONTEXT ShaContext, const UCHAR* Input, ULONG InputLength)
{
    ULONG i, index, part_len;

    if (!ShaContext || !Input || InputLength == 0)
        return;

    index = (ShaContext->Count[0] >> 3) & 0x3F;

    /* Update bit count */
    ShaContext->Count[0] += (InputLength << 3);
    if (ShaContext->Count[0] < (InputLength << 3))
        ShaContext->Count[1]++;
    ShaContext->Count[1] += (InputLength >> 29);

    part_len = 64 - index;

    if (InputLength >= part_len) {
        memcpy(&ShaContext->Buffer[index], Input, part_len);
        sha1_transform(ShaContext->State, ShaContext->Buffer);

        for (i = part_len; i + 63 < InputLength; i += 64) {
            sha1_transform(ShaContext->State, &Input[i]);
        }

        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ShaContext->Buffer[index], &Input[i], InputLength - i);
}

VOID __stdcall xbox_XcSHAFinal(PXBOX_SHA_CONTEXT ShaContext, UCHAR* Digest)
{
    UCHAR finalcount[8];
    UCHAR pad;
    ULONG i;

    if (!ShaContext || !Digest)
        return;

    /* Store bit count (big-endian) */
    for (i = 0; i < 4; i++) {
        finalcount[i]     = (UCHAR)((ShaContext->Count[1] >> ((3 - i) * 8)) & 0xFF);
        finalcount[i + 4] = (UCHAR)((ShaContext->Count[0] >> ((3 - i) * 8)) & 0xFF);
    }

    /* Pad to 56 mod 64 */
    pad = 0x80;
    xbox_XcSHAUpdate(ShaContext, &pad, 1);
    pad = 0x00;
    while ((ShaContext->Count[0] >> 3) % 64 != 56) {
        xbox_XcSHAUpdate(ShaContext, &pad, 1);
    }

    /* Append length */
    xbox_XcSHAUpdate(ShaContext, finalcount, 8);

    /* Output digest (big-endian) */
    for (i = 0; i < 5; i++) {
        Digest[i * 4]     = (UCHAR)((ShaContext->State[i] >> 24) & 0xFF);
        Digest[i * 4 + 1] = (UCHAR)((ShaContext->State[i] >> 16) & 0xFF);
        Digest[i * 4 + 2] = (UCHAR)((ShaContext->State[i] >> 8) & 0xFF);
        Digest[i * 4 + 3] = (UCHAR)((ShaContext->State[i]) & 0xFF);
    }
}

/* ============================================================================
 * RC4 (ARC4)
 *
 * Standard RC4 stream cipher. Used for Xbox Live encryption and some
 * save game protection. Implemented in software for portability.
 * ============================================================================ */

VOID __stdcall xbox_XcRC4Key(PXBOX_RC4_CONTEXT Rc4Context, ULONG KeyLength, const UCHAR* Key)
{
    XC_TRACE("RC4Key len %lu", (unsigned long)KeyLength);
    UCHAR temp;
    ULONG i;
    UCHAR j = 0;

    if (!Rc4Context || !Key || KeyLength == 0)
        return;

    /* KSA (Key-Scheduling Algorithm) */
    for (i = 0; i < 256; i++)
        Rc4Context->S[i] = (UCHAR)i;

    for (i = 0; i < 256; i++) {
        j = j + Rc4Context->S[i] + Key[i % KeyLength];
        temp = Rc4Context->S[i];
        Rc4Context->S[i] = Rc4Context->S[j];
        Rc4Context->S[j] = temp;
    }

    Rc4Context->i = 0;
    Rc4Context->j = 0;
}

VOID __stdcall xbox_XcRC4Crypt(PXBOX_RC4_CONTEXT Rc4Context, ULONG Length, UCHAR* Data)
{
    UCHAR temp;
    ULONG n;

    if (!Rc4Context || !Data || Length == 0)
        return;

    /* PRGA (Pseudo-Random Generation Algorithm) */
    for (n = 0; n < Length; n++) {
        Rc4Context->i++;
        Rc4Context->j += Rc4Context->S[Rc4Context->i];

        temp = Rc4Context->S[Rc4Context->i];
        Rc4Context->S[Rc4Context->i] = Rc4Context->S[Rc4Context->j];
        Rc4Context->S[Rc4Context->j] = temp;

        Data[n] ^= Rc4Context->S[(UCHAR)(Rc4Context->S[Rc4Context->i] + Rc4Context->S[Rc4Context->j])];
    }
}

/* ============================================================================
 * HMAC-SHA1
 *
 * Standard RFC 2104 HMAC using SHA-1. Used for save game authentication.
 * Xbox XcHMAC takes two data buffers to hash (Data1 and Data2, concatenated).
 * ============================================================================ */

VOID __stdcall xbox_XcHMAC(
    const UCHAR* Key, ULONG KeyLength,
    const UCHAR* Data1, ULONG Data1Length,
    const UCHAR* Data2, ULONG Data2Length,
    UCHAR* Digest)
{
    XC_TRACE("HMAC key %lu data %lu+%lu", (unsigned long)KeyLength, (unsigned long)Data1Length, (unsigned long)Data2Length);
    XBOX_SHA_CONTEXT sha;
    UCHAR key_block[64];
    UCHAR ipad[64];
    UCHAR opad[64];
    UCHAR inner_digest[20];
    ULONG i;

    if (!Key || !Digest)
        return;

    /* If key is longer than block size, hash it first */
    memset(key_block, 0, 64);
    if (KeyLength > 64) {
        xbox_XcSHAInit(&sha);
        xbox_XcSHAUpdate(&sha, Key, KeyLength);
        xbox_XcSHAFinal(&sha, key_block);
    } else {
        memcpy(key_block, Key, KeyLength);
    }

    /* Inner padding */
    for (i = 0; i < 64; i++)
        ipad[i] = key_block[i] ^ 0x36;

    /* Outer padding */
    for (i = 0; i < 64; i++)
        opad[i] = key_block[i] ^ 0x5C;

    /* Inner hash: SHA1(ipad || data1 || data2) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, ipad, 64);
    if (Data1 && Data1Length > 0)
        xbox_XcSHAUpdate(&sha, Data1, Data1Length);
    if (Data2 && Data2Length > 0)
        xbox_XcSHAUpdate(&sha, Data2, Data2Length);
    xbox_XcSHAFinal(&sha, inner_digest);

    /* Outer hash: SHA1(opad || inner_digest) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, opad, 64);
    xbox_XcSHAUpdate(&sha, inner_digest, 20);
    xbox_XcSHAFinal(&sha, Digest);

    /* A signed record usually keeps its signature beside the data: say whether
     * this digest matches the 12 bytes before Data1 or the 8 after it (neither
     * is printed). That is how a key that does not verify shows up. */
    if (xc_trace_on() && Data1 && Data1Length >= 16) {
        int before = memcmp(Digest, Data1 - 12, 12) == 0;
        int after = memcmp(Digest, Data1 + Data1Length, 8) == 0;
        if (before || after)
            XC_TRACE("HMAC result matches the signature %s the data", before ? "before" : "after");
    }

    /* Clean up sensitive data */
    SecureZeroMemory(key_block, sizeof(key_block));
    SecureZeroMemory(ipad, sizeof(ipad));
    SecureZeroMemory(opad, sizeof(opad));
    SecureZeroMemory(inner_digest, sizeof(inner_digest));
}

/* ============================================================================
 * Public Key Operations
 *
 * Used for Xbox Live authentication and content signing verification.
 * These are not needed for PC operation (no Xbox Live), so they're
 * stubbed to return failure/zero.
 * ============================================================================ */

ULONG __stdcall xbox_XcPKGetKeyLen(PVOID PublicKey)
{
    XC_TRACE("PKGetKeyLen (stub: 0)");
    (void)PublicKey;
    return 0;
}

ULONG __stdcall xbox_XcPKDecPrivate(PVOID PrivateKey, PVOID Input, PVOID Output)
{
    XC_TRACE("PKDecPrivate (stub: fails)");
    (void)PrivateKey;
    (void)Input;
    (void)Output;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcPKDecPrivate: stubbed (no Xbox Live)");
    return 0; /* Failure */
}

ULONG __stdcall xbox_XcPKEncPublic(PVOID PublicKey, PVOID Input, PVOID Output)
{
    XC_TRACE("PKEncPublic (stub: fails)");
    (void)PublicKey;
    (void)Input;
    (void)Output;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcPKEncPublic: stubbed (no Xbox Live)");
    return 0;
}

BOOLEAN __stdcall xbox_XcVerifyPKCS1Signature(PVOID Hash, PVOID PublicKey, PVOID Signature)
{
    XC_TRACE("VerifyPKCS1Signature (stub: TRUE)");
    (void)Hash;
    (void)PublicKey;
    (void)Signature;
    /* Return TRUE to bypass signature checks */
    return TRUE;
}

/* XcModExp(A, B, C, D, n): A = B^C mod D, each n little-endian DWORDs.
 *
 * XNet's key exchange is Diffie-Hellman over this (system link included):
 * each console sends g^x mod p and keys the session with (g^y)^x mod p. With
 * this stubbed the two ends never agreed on a key, and a system link join
 * found the host and then reported "This game session is no longer
 * available". Square-and-multiply with a shift-and-add modular multiply:
 * slow next to Montgomery, but a 768-bit exchange is a few milliseconds and
 * happens once per connection. */
#define MODEXP_MAX 64           /* DWORDs: 2048-bit */

static int bn_cmp(const uint32_t *a, const uint32_t *b, ULONG n)
{
    ULONG i = n;
    while (i--) {
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

static void bn_sub(uint32_t *a, const uint32_t *b, ULONG n)     /* a -= b */
{
    uint64_t borrow = 0;
    ULONG i;
    for (i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
}

/* r = 2r + bit, then reduced: r < m on entry, so one subtraction suffices
 * (with the carry out of the top word standing for the bit above it). */
static void bn_dbl_mod(uint32_t *r, const uint32_t *m, ULONG n)
{
    uint32_t carry = 0;
    ULONG i;
    for (i = 0; i < n; i++) {
        uint32_t c = r[i] >> 31;
        r[i] = (r[i] << 1) | carry;
        carry = c;
    }
    if (carry || bn_cmp(r, m, n) >= 0)
        bn_sub(r, m, n);
}

static void bn_add_mod(uint32_t *r, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint64_t carry = 0;
    ULONG i;
    for (i = 0; i < n; i++) {
        uint64_t s = (uint64_t)r[i] + b[i] + carry;
        r[i] = (uint32_t)s;
        carry = s >> 32;
    }
    if (carry || bn_cmp(r, m, n) >= 0)
        bn_sub(r, m, n);
}

/* out = a * b mod m; a, b < m. */
static void bn_mulmod(uint32_t *out, const uint32_t *a, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint32_t r[MODEXP_MAX];
    int bit;
    memset(r, 0, n * 4u);
    for (bit = (int)(n * 32u) - 1; bit >= 0; bit--) {
        bn_dbl_mod(r, m, n);
        if ((a[bit >> 5] >> (bit & 31)) & 1u)
            bn_add_mod(r, b, m, n);
    }
    memcpy(out, r, n * 4u);
}

ULONG __stdcall xbox_XcModExp(PULONG Result, PULONG Base, PULONG Exponent, PULONG Modulus, ULONG ModulusLength)
{
    XC_TRACE("ModExp %lu dwords", (unsigned long)ModulusLength);
    uint32_t m[MODEXP_MAX], b[MODEXP_MAX], r[MODEXP_MAX], one[MODEXP_MAX];
    ULONG n = ModulusLength, top;
    int bit;

    if (!Result || !Base || !Exponent || !Modulus || n == 0 || n > MODEXP_MAX)
        return 0;
    memcpy(m, Modulus, n * 4u);
    for (top = n; top && m[top - 1] == 0; top--)
        ;
    if (!top)
        return 0;
    /* b = Base mod m (Base * 1), r = 1 mod m */
    memset(one, 0, sizeof one); one[0] = 1;
    {
        uint32_t base[MODEXP_MAX];
        memcpy(base, Base, n * 4u);
        memset(r, 0, n * 4u);
        for (bit = (int)(n * 32u) - 1; bit >= 0; bit--) {      /* r = base mod m, bit by bit */
            bn_dbl_mod(r, m, n);
            if ((base[bit >> 5] >> (bit & 31)) & 1u)
                bn_add_mod(r, one, m, n);
        }
        memcpy(b, r, n * 4u);
    }
    memset(r, 0, n * 4u);
    bn_add_mod(r, one, m, n);                                   /* r = 1 mod m */
    for (bit = (int)(n * 32u) - 1; bit >= 0; bit--) {
        bn_mulmod(r, r, r, m, n);
        if ((Exponent[bit >> 5] >> (bit & 31)) & 1u)
            bn_mulmod(r, r, b, m, n);
    }
    memcpy(Result, r, n * 4u);
    return 1;
}

/* ============================================================================
 * DES / 3DES
 *
 * XNet encrypts secured traffic (system link and Xbox Live) with DES or
 * triple DES in CBC mode through these. The key table is the title's opaque
 * buffer (XC_SERVICE_DES_TABLESIZE 0x80, DES3 0x180); here it holds a tag,
 * the cipher and the raw key, and the work is done by BCrypt in ECB mode
 * with the chaining done here. Key handles are cached by key.
 * Operation: 1 encrypts, 0 decrypts. Cipher: 0 DES, 1 triple DES.
 * ============================================================================ */

VOID __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength)
{
    ULONG i;
    if (!Key)
        return;
    for (i = 0; i < KeyLength; i++) {
        UCHAR v = (UCHAR)(Key[i] & 0xFE), c = v;
        c ^= c >> 4; c ^= c >> 2; c ^= c >> 1;
        Key[i] = (UCHAR)(v | ((c & 1u) ? 0u : 1u));             /* odd parity */
    }
}

#define DES_TAG 0x53454458u     /* "XDES" */

typedef struct { uint32_t tag, cipher; UCHAR key[24]; } DesTable;

VOID __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR* Key)
{
    XC_TRACE("KeyTable cipher %lu", (unsigned long)CipherSelect);
    DesTable *t = (DesTable *)KeyTable;
    if (!t || !Key)
        return;
    t->tag = DES_TAG;
    t->cipher = CipherSelect ? 1u : 0u;
    memset(t->key, 0, sizeof t->key);
    memcpy(t->key, Key, CipherSelect ? 24u : 8u);
}

#if defined(_WIN32)
typedef struct { uint32_t cipher; UCHAR key[24]; BCRYPT_KEY_HANDLE h; } DesKeyEnt;

static BCRYPT_KEY_HANDLE des_key(const DesTable *t)
{
    static BCRYPT_ALG_HANDLE alg[2];
    static DesKeyEnt cache[16];
    static unsigned next;
    static SRWLOCK lock = SRWLOCK_INIT;
    BCRYPT_KEY_HANDLE h = NULL;
    unsigned i;

    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < 16; i++)
        if (cache[i].h && cache[i].cipher == t->cipher && !memcmp(cache[i].key, t->key, sizeof t->key)) {
            h = cache[i].h;
            goto out;
        }
    if (!alg[t->cipher]) {
        if (BCryptOpenAlgorithmProvider(&alg[t->cipher], t->cipher ? BCRYPT_3DES_ALGORITHM : BCRYPT_DES_ALGORITHM, NULL, 0) != 0)
            goto out;
        BCryptSetProperty(alg[t->cipher], BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                          sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
    }
    if (BCryptGenerateSymmetricKey(alg[t->cipher], &h, NULL, 0, (PUCHAR)t->key, t->cipher ? 24u : 8u, 0) != 0) {
        h = NULL;
        goto out;
    }
    i = next++ % 16u;
    if (cache[i].h)
        BCryptDestroyKey(cache[i].h);
    cache[i].cipher = t->cipher;
    memcpy(cache[i].key, t->key, sizeof t->key);
    cache[i].h = h;
out:
    ReleaseSRWLockExclusive(&lock);
    return h;
}

static void des_block(const DesTable *t, UCHAR *out, const UCHAR *in, int encrypt)
{
    BCRYPT_KEY_HANDLE h = (t && t->tag == DES_TAG) ? des_key(t) : NULL;
    UCHAR buf[8];
    ULONG got = 0;
    memcpy(buf, in, 8);
    if (!h) { memcpy(out, buf, 8); return; }
    if (encrypt)
        BCryptEncrypt(h, buf, 8, NULL, NULL, 0, out, 8, &got, 0);
    else
        BCryptDecrypt(h, buf, 8, NULL, NULL, 0, out, 8, &got, 0);
}
#else
static void des_block(const DesTable *t, UCHAR *out, const UCHAR *in, int encrypt)
{
    (void)t; (void)encrypt;
    memmove(out, in, 8);
}
#endif

VOID __stdcall xbox_XcBlockCrypt(ULONG CipherSelect, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation)
{
    (void)CipherSelect;
    if (!Output || !Input)
        return;
    des_block((const DesTable *)KeyTable, (UCHAR *)Output, (const UCHAR *)Input, Operation != 0);
}

VOID __stdcall xbox_XcBlockCryptCBC(
    ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input,
    PVOID KeyTable, ULONG Operation, PVOID FeedbackVector)
{
    XC_TRACE("BlockCryptCBC cipher %lu len %lu op %lu", (unsigned long)CipherSelect, (unsigned long)OutputLength, (unsigned long)Operation);
    UCHAR *out = (UCHAR *)Output, *fb = (UCHAR *)FeedbackVector;
    const UCHAR *in = (const UCHAR *)Input;
    const DesTable *t = (const DesTable *)KeyTable;
    ULONG off, k;
    (void)CipherSelect;
    if (!out || !in || !fb)
        return;
    for (off = 0; off + 8 <= OutputLength; off += 8) {
        UCHAR blk[8], save[8];
        if (Operation) {
            for (k = 0; k < 8; k++) blk[k] = (UCHAR)(in[off + k] ^ fb[k]);
            des_block(t, out + off, blk, 1);
            memcpy(fb, out + off, 8);
        } else {
            memcpy(save, in + off, 8);
            des_block(t, blk, save, 0);
            for (k = 0; k < 8; k++) out[off + k] = (UCHAR)(blk[k] ^ fb[k]);
            memcpy(fb, save, 8);
        }
    }
}

VOID __stdcall xbox_XcCryptService(ULONG Operation, PVOID Param)
{
    XC_TRACE("CryptService op %lu (stub)", (unsigned long)Operation);
    (void)Operation;
    (void)Param;
}

VOID __stdcall xbox_XcUpdateCrypto(PVOID Param1, PVOID Param2)
{
    (void)Param1;
    (void)Param2;
}
