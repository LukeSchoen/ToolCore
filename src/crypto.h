// Tools - what the line protocol rides inside once a session is sealed.
//
// Each connection derives a session key from the shared token and the two
// handshake nonces, and every line after the handshake is sealed with
// ChaCha20-Poly1305 (RFC 8439). The sealing lives under line_read/line_send in
// common.c, so the protocol's shape does not change: one line in, one line
// out, and the event code and the server's routing never see the difference.
//
// What this gives, and what it does not:
//   - confidentiality and integrity of every line after the handshake, and
//     replay protection (each record carries an implicit counter),
//   - both ends prove they know the token, so a stranger cannot stand in for
//     the server either,
//   - no forward secrecy: the key comes from the token, so somebody who
//     records a session and only later learns the token can decrypt it. That
//     would need an ephemeral key exchange, and this compiler has no 128-bit
//     type, so it would mean hand-rolled field arithmetic.
//
// Nothing here is a general-purpose crypto library. It is the two primitives
// this protocol needs, checked against the RFC 8439 test vectors.

#ifndef TOOL_CRYPTO_H
#define TOOL_CRYPTO_H

#define TOOL_KEY_LEN     32
#define TOOL_TAG_LEN     16
#define TOOL_NONCE_LEN   12
#define TOOL_DIGEST_LEN  32
#define TOOL_SESSION_LEN 64

// ChaCha20-Poly1305 (RFC 8439 section 2.8). out receives the ciphertext with
// the 16-byte tag appended, so it must hold ptlen + TOOL_TAG_LEN bytes.
// aad may be NULL when aadlen is 0. Seal always returns 1; open returns 0 when
// the tag does not match, and then writes nothing.
int aead_seal(const unsigned char key[TOOL_KEY_LEN],
              const unsigned char nonce[TOOL_NONCE_LEN],
              const unsigned char *aad, int aadlen,
              const unsigned char *pt, int ptlen, unsigned char *out);

int aead_open(const unsigned char key[TOOL_KEY_LEN],
              const unsigned char nonce[TOOL_NONCE_LEN],
              const unsigned char *aad, int aadlen,
              const unsigned char *ct, int ctlen, unsigned char *out);

// SHA-256 of one buffer.
int sha256_buf(const unsigned char *in, int inlen, unsigned char out[TOOL_DIGEST_LEN]);

// HMAC-SHA256 (RFC 2104) and HKDF-SHA256 (RFC 5869, extract then expand).
int hmac_sha256(const unsigned char *key, int keylen,
                const unsigned char *msg, int msglen,
                unsigned char out[TOOL_DIGEST_LEN]);

int hkdf_sha256(const unsigned char *ikm, int ikmlen,
                const unsigned char *salt, int saltlen,
                const unsigned char *info, int infolen,
                unsigned char *okm, int okmlen);

#endif
