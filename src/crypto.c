// Tools - ChaCha20-Poly1305, HMAC-SHA256 and HKDF-SHA256.
//
// See crypto.h for what this is for and what it does not do. The two
// primitives are written from RFC 8439; SHA-256 itself is the system one,
// through the same CryptoAPI the rest of Tools already uses, so only
// ChaCha20 and Poly1305 are hand-written.

#include "crypto.h"

#include <windows.h>
#include <wincrypt.h>
#include <string.h>

// Long is 32 bits on Windows, so say what is meant.
typedef unsigned int u32;
typedef unsigned long long u64;

static u32 load32(const unsigned char *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void store32(unsigned char *p, u32 v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

// ------------------------------------------------------------- ChaCha20

#define ROTL32(v, c) ((u32)(((v) << (c)) | ((v) >> (32 - (c)))))

#define QUARTERROUND(a, b, c, d)     \
    do {                             \
        a += b; d ^= a; d = ROTL32(d, 16); \
        c += d; b ^= c; b = ROTL32(b, 12); \
        a += b; d ^= a; d = ROTL32(d, 8);  \
        c += d; b ^= c; b = ROTL32(b, 7);  \
    } while (0)

static void chacha20_setup(u32 st[16], const unsigned char key[32],
                           const unsigned char nonce[12])
{
    static const unsigned char sigma[16] = "expand 32-byte k";
    int i;

    for (i = 0; i < 4; i++) st[i] = load32(sigma + i * 4);
    for (i = 0; i < 8; i++) st[4 + i] = load32(key + i * 4);
    st[12] = 0;
    for (i = 0; i < 3; i++) st[13 + i] = load32(nonce + i * 4);
}

// The state is held in named locals rather than an array. This function is the
// whole cost of sealing a line; Poly1305 runs beside it.
static void chacha20_block(const u32 in[16], unsigned char out[64])
{
    u32 x0 = in[0],  x1 = in[1],  x2 = in[2],  x3 = in[3];
    u32 x4 = in[4],  x5 = in[5],  x6 = in[6],  x7 = in[7];
    u32 x8 = in[8],  x9 = in[9],  x10 = in[10], x11 = in[11];
    u32 x12 = in[12], x13 = in[13], x14 = in[14], x15 = in[15];
    int i;

    for (i = 0; i < 10; i++) {
        QUARTERROUND(x0, x4, x8,  x12);
        QUARTERROUND(x1, x5, x9,  x13);
        QUARTERROUND(x2, x6, x10, x14);
        QUARTERROUND(x3, x7, x11, x15);
        QUARTERROUND(x0, x5, x10, x15);
        QUARTERROUND(x1, x6, x11, x12);
        QUARTERROUND(x2, x7, x8,  x13);
        QUARTERROUND(x3, x4, x9,  x14);
    }

    // "adding the original input words", from the array the caller keeps.
    store32(out + 0,  x0  + in[0]);
    store32(out + 4,  x1  + in[1]);
    store32(out + 8,  x2  + in[2]);
    store32(out + 12, x3  + in[3]);
    store32(out + 16, x4  + in[4]);
    store32(out + 20, x5  + in[5]);
    store32(out + 24, x6  + in[6]);
    store32(out + 28, x7  + in[7]);
    store32(out + 32, x8  + in[8]);
    store32(out + 36, x9  + in[9]);
    store32(out + 40, x10 + in[10]);
    store32(out + 44, x11 + in[11]);
    store32(out + 48, x12 + in[12]);
    store32(out + 52, x13 + in[13]);
    store32(out + 56, x14 + in[14]);
    store32(out + 60, x15 + in[15]);
}

// XORs len bytes against the ChaCha20 stream, starting at the given block
// counter. in and out may be the same buffer.
static void chacha20_xor(const unsigned char key[32], const unsigned char nonce[12],
                         u32 counter, const unsigned char *in, unsigned char *out,
                         int len)
{
    u32 st[16];
    unsigned char block[64];
    int i;

    chacha20_setup(st, key, nonce);
    while (len >= 64) {
        st[12] = counter++;
        chacha20_block(st, block);
        for (i = 0; i < 16; i++)
            store32(out + i * 4, load32(in + i * 4) ^ load32(block + i * 4));
        in += 64;
        out += 64;
        len -= 64;
    }
    if (len > 0) {
        st[12] = counter;
        chacha20_block(st, block);
        for (i = 0; i < len; i++) out[i] = (unsigned char)(in[i] ^ block[i]);
    }
}

// ------------------------------------------------------------- Poly1305

// The 5 x 26-bit limb form: every product is at most 26+26 bits, so the
// arithmetic stays inside 64-bit integers on a compiler with no 128-bit type.

typedef struct {
    u32 r0, r1, r2, r3, r4;
    u32 s1, s2, s3, s4;
    u32 h0, h1, h2, h3, h4;
    unsigned char pad[16];
    unsigned char buf[16];
    int leftover;
} poly1305_ctx;

static void poly1305_init(poly1305_ctx *c, const unsigned char key[32])
{
    unsigned char r[16];

    // Clamp r the way the spec says, as a byte mask over the 16 key bytes,
    // and only then cut it into 26-bit limbs. Reading 4 bytes from byte 3 and
    // shifting right by 2 is how bit 26 onwards becomes limb 1; the same
    // trick gives the rest. Cutting the raw words up without the clamp is
    // easy to get wrong - it silently keeps bits 28 to 31.
    memcpy(r, key, 16);
    r[3] &= 15;
    r[7] &= 15;
    r[11] &= 15;
    r[15] &= 15;
    r[4] &= 252;
    r[8] &= 252;
    r[12] &= 252;

    c->r0 = load32(r + 0) & 0x3ffffffu;
    c->r1 = (load32(r + 3) >> 2) & 0x3ffffffu;
    c->r2 = (load32(r + 6) >> 4) & 0x3ffffffu;
    c->r3 = (load32(r + 9) >> 6) & 0x3ffffffu;
    c->r4 = (load32(r + 12) >> 8) & 0x3ffffffu;

    c->s1 = c->r1 * 5;
    c->s2 = c->r2 * 5;
    c->s3 = c->r3 * 5;
    c->s4 = c->r4 * 5;

    c->h0 = c->h1 = c->h2 = c->h3 = c->h4 = 0;
    c->leftover = 0;
    memcpy(c->pad, key + 16, 16);
}

static void poly1305_blocks(poly1305_ctx *c, const unsigned char *m, int bytes,
                            u32 hibit)
{
    u32 h0 = c->h0, h1 = c->h1, h2 = c->h2, h3 = c->h3, h4 = c->h4;
    u32 r0 = c->r0, r1 = c->r1, r2 = c->r2, r3 = c->r3, r4 = c->r4;
    u32 s1 = c->s1, s2 = c->s2, s3 = c->s3, s4 = c->s4;
    u64 d0, d1, d2, d3, d4;
    u32 b0, b1, b2, b3, carry;

    while (bytes >= 16) {
        b0 = load32(m + 0);
        b1 = load32(m + 4);
        b2 = load32(m + 8);
        b3 = load32(m + 12);

        h0 += b0 & 0x3ffffffu;
        h1 += ((b0 >> 26) | (b1 << 6)) & 0x3ffffffu;
        h2 += ((b1 >> 20) | (b2 << 12)) & 0x3ffffffu;
        h3 += ((b2 >> 14) | (b3 << 18)) & 0x3ffffffu;
        h4 += (b3 >> 8) | hibit;

        d0 = ((u64)h0 * r0) + ((u64)h1 * s4) + ((u64)h2 * s3) + ((u64)h3 * s2) +
             ((u64)h4 * s1);
        d1 = ((u64)h0 * r1) + ((u64)h1 * r0) + ((u64)h2 * s4) + ((u64)h3 * s3) +
             ((u64)h4 * s2);
        d2 = ((u64)h0 * r2) + ((u64)h1 * r1) + ((u64)h2 * r0) + ((u64)h3 * s4) +
             ((u64)h4 * s3);
        d3 = ((u64)h0 * r3) + ((u64)h1 * r2) + ((u64)h2 * r1) + ((u64)h3 * r0) +
             ((u64)h4 * s4);
        d4 = ((u64)h0 * r4) + ((u64)h1 * r3) + ((u64)h2 * r2) + ((u64)h3 * r1) +
             ((u64)h4 * r0);

        carry = (u32)(d0 >> 26); h0 = (u32)d0 & 0x3ffffffu;
        d1 += carry;             carry = (u32)(d1 >> 26); h1 = (u32)d1 & 0x3ffffffu;
        d2 += carry;             carry = (u32)(d2 >> 26); h2 = (u32)d2 & 0x3ffffffu;
        d3 += carry;             carry = (u32)(d3 >> 26); h3 = (u32)d3 & 0x3ffffffu;
        d4 += carry;             carry = (u32)(d4 >> 26); h4 = (u32)d4 & 0x3ffffffu;
        h0 += carry * 5;         carry = h0 >> 26;        h0 &= 0x3ffffffu;
        h1 += carry;

        m += 16;
        bytes -= 16;
    }

    c->h0 = h0;
    c->h1 = h1;
    c->h2 = h2;
    c->h3 = h3;
    c->h4 = h4;
}

static void poly1305_update(poly1305_ctx *c, const unsigned char *m, int bytes)
{
    if (c->leftover) {
        int want = 16 - c->leftover;

        if (want > bytes) want = bytes;
        memcpy(c->buf + c->leftover, m, (size_t)want);
        c->leftover += want;
        m += want;
        bytes -= want;
        if (c->leftover < 16) return;
        poly1305_blocks(c, c->buf, 16, 1u << 24);
        c->leftover = 0;
    }

    if (bytes >= 16) {
        int full = bytes & ~15;

        poly1305_blocks(c, m, full, 1u << 24);
        m += full;
        bytes -= full;
    }

    if (bytes > 0) {
        memcpy(c->buf, m, (size_t)bytes);
        c->leftover = bytes;
    }
}

static void poly1305_finish(poly1305_ctx *c, unsigned char tag[16])
{
    u32 h0 = c->h0, h1 = c->h1, h2 = c->h2, h3 = c->h3, h4 = c->h4;
    u32 g0, g1, g2, g3, g4, carry, mask;
    u64 t;

    if (c->leftover) {
        c->buf[c->leftover++] = 1;
        while (c->leftover < 16) c->buf[c->leftover++] = 0;
        poly1305_blocks(c, c->buf, 16, 0);
        h0 = c->h0; h1 = c->h1; h2 = c->h2; h3 = c->h3; h4 = c->h4;
    }

    // carry the limbs all the way up
    carry = h1 >> 26; h1 &= 0x3ffffffu; h2 += carry;
    carry = h2 >> 26; h2 &= 0x3ffffffu; h3 += carry;
    carry = h3 >> 26; h3 &= 0x3ffffffu; h4 += carry;
    carry = h4 >> 26; h4 &= 0x3ffffffu; h0 += carry * 5;
    carry = h0 >> 26; h0 &= 0x3ffffffu; h1 += carry;

    // g = h + 5 - 2^130, which is h - p
    g0 = h0 + 5;      carry = g0 >> 26; g0 &= 0x3ffffffu;
    g1 = h1 + carry;  carry = g1 >> 26; g1 &= 0x3ffffffu;
    g2 = h2 + carry;  carry = g2 >> 26; g2 &= 0x3ffffffu;
    g3 = h3 + carry;  carry = g3 >> 26; g3 &= 0x3ffffffu;
    g4 = h4 + carry - (1u << 26);

    // h < p leaves g negative, so keep h; otherwise keep g
    mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    // back to 128 bits, then add s
    h0 = (h0) | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    t = (u64)h0 + load32(c->pad + 0);               h0 = (u32)t;
    t = (u64)h1 + load32(c->pad + 4) + (t >> 32);   h1 = (u32)t;
    t = (u64)h2 + load32(c->pad + 8) + (t >> 32);   h2 = (u32)t;
    t = (u64)h3 + load32(c->pad + 12) + (t >> 32);  h3 = (u32)t;

    store32(tag + 0, h0);
    store32(tag + 4, h1);
    store32(tag + 8, h2);
    store32(tag + 12, h3);
}

// ------------------------------------------------------- ChaCha20-Poly1305

static void aead_mac(poly1305_ctx *mac, const unsigned char *aad, int aadlen,
                     const unsigned char *ct, int ctlen)
{
    static const unsigned char zeros[16] = { 0 };
    unsigned char lengths[16];
    int i;

    poly1305_update(mac, aad, aadlen);
    if (aadlen & 15) poly1305_update(mac, zeros, 16 - (aadlen & 15));

    poly1305_update(mac, ct, ctlen);
    if (ctlen & 15) poly1305_update(mac, zeros, 16 - (ctlen & 15));

    for (i = 0; i < 8; i++) lengths[i] = (unsigned char)(((u64)aadlen >> (i * 8)) & 0xff);
    for (i = 0; i < 8; i++) lengths[8 + i] = (unsigned char)(((u64)ctlen >> (i * 8)) & 0xff);
    poly1305_update(mac, lengths, 16);
}

static void poly1305_key_from(const unsigned char key[32],
                              const unsigned char nonce[12],
                              unsigned char otk[32])
{
    u32 st[16];
    unsigned char block[64];

    chacha20_setup(st, key, nonce);
    st[12] = 0;
    chacha20_block(st, block);
    memcpy(otk, block, 32);
}

int aead_seal(const unsigned char key[32], const unsigned char nonce[12],
              const unsigned char *aad, int aadlen,
              const unsigned char *pt, int ptlen, unsigned char *out)
{
    unsigned char otk[32];
    poly1305_ctx mac;

    poly1305_key_from(key, nonce, otk);
    chacha20_xor(key, nonce, 1, pt, out, ptlen);

    poly1305_init(&mac, otk);
    aead_mac(&mac, aad, aadlen, out, ptlen);
    poly1305_finish(&mac, out + ptlen);
    return 1;
}

int aead_open(const unsigned char key[32], const unsigned char nonce[12],
              const unsigned char *aad, int aadlen,
              const unsigned char *ct, int ctlen, unsigned char *out)
{
    unsigned char otk[32];
    unsigned char tag[16];
    poly1305_ctx mac;
    int i, diff = 0;

    if (ctlen < TOOL_TAG_LEN) return 0;
    ctlen -= TOOL_TAG_LEN;

    poly1305_key_from(key, nonce, otk);
    poly1305_init(&mac, otk);
    aead_mac(&mac, aad, aadlen, ct, ctlen);
    poly1305_finish(&mac, tag);

    // Compare in full: a tag is not a place to stop early.
    for (i = 0; i < TOOL_TAG_LEN; i++)
        diff |= (int)(tag[i] ^ ct[ctlen + i]);
    if (diff != 0) return 0;

    chacha20_xor(key, nonce, 1, ct, out, ctlen);
    return 1;
}

// ------------------------------------------------------ SHA-256 and friends

int sha256_buf(const unsigned char *in, int inlen, unsigned char out[TOOL_DIGEST_LEN])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[TOOL_DIGEST_LEN];
    DWORD dlen = sizeof digest;
    int ok = 0;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return 0;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return 0;
    }
    if (inlen > 0 && !CryptHashData(hash, in, (DWORD)inlen, 0)) goto done;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) goto done;
    memcpy(out, digest, TOOL_DIGEST_LEN);
    ok = 1;

done:
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return ok;
}

// SHA-256 of two buffers back to back, without copying them together first.
static int sha256_two(const unsigned char *a, int alen,
                      const unsigned char *b, int blen,
                      unsigned char out[TOOL_DIGEST_LEN])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[TOOL_DIGEST_LEN];
    DWORD dlen = sizeof digest;
    int ok = 0;

    if (!CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return 0;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptReleaseContext(prov, 0);
        return 0;
    }
    if (alen > 0 && !CryptHashData(hash, a, (DWORD)alen, 0)) goto done;
    if (blen > 0 && !CryptHashData(hash, b, (DWORD)blen, 0)) goto done;
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) goto done;
    memcpy(out, digest, TOOL_DIGEST_LEN);
    ok = 1;

done:
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return ok;
}

int hmac_sha256(const unsigned char *key, int keylen,
                const unsigned char *msg, int msglen,
                unsigned char out[TOOL_DIGEST_LEN])
{
    unsigned char k[64];
    unsigned char ipad[64];
    unsigned char opad[64];
    unsigned char inner[TOOL_DIGEST_LEN];
    int i;

    // A key longer than the block is hashed down to one, as the RFC says.
    if (keylen > 64) {
        if (!sha256_buf(key, keylen, k)) return 0;
        keylen = TOOL_DIGEST_LEN;
    } else {
        memcpy(k, key, (size_t)keylen);
    }
    memset(k + keylen, 0, 64 - (size_t)keylen);

    for (i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    if (!sha256_two(ipad, 64, msg, msglen, inner)) return 0;
    return sha256_two(opad, 64, inner, TOOL_DIGEST_LEN, out);
}

int hkdf_sha256(const unsigned char *ikm, int ikmlen,
                const unsigned char *salt, int saltlen,
                const unsigned char *info, int infolen,
                unsigned char *okm, int okmlen)
{
    static const unsigned char nothing[1] = { 0 };
    unsigned char prk[TOOL_DIGEST_LEN];
    unsigned char t[TOOL_DIGEST_LEN];
    unsigned char block[256];
    int done = 0, step = 1, n, copy, i;

    if (okmlen <= 0) return 1;
    if (infolen < 0) infolen = 0;
    if (infolen + 33 > (int)sizeof block) return 0;
    if (!salt || saltlen <= 0) {
        salt = nothing;
        saltlen = 0;
    }

    if (!hmac_sha256(salt, saltlen, ikm, ikmlen, prk)) return 0;

    while (done < okmlen) {
        n = 0;
        if (step > 1) {
            memcpy(block, t, TOOL_DIGEST_LEN);
            n = TOOL_DIGEST_LEN;
        }
        if (infolen > 0) {
            memcpy(block + n, info, (size_t)infolen);
            n += infolen;
        }
        block[n++] = (unsigned char)step;

        if (!hmac_sha256(prk, TOOL_DIGEST_LEN, block, n, t)) return 0;

        copy = okmlen - done;
        if (copy > TOOL_DIGEST_LEN) copy = TOOL_DIGEST_LEN;
        for (i = 0; i < copy; i++) okm[done + i] = t[i];
        done += copy;
        step++;
    }
    return 1;
}
