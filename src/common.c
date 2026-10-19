#include "common.h"
#include <stdarg.h>

void copy_str(char *dst, int dstsz, const char *src)
{
    int i;
    if (dstsz <= 0) return;
    for (i = 0; i < dstsz - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

void format_text(char *out, int outsz, const char *fmt, ...)
{
    va_list ap;

    if (outsz <= 0) return;
    out[0] = 0;
    va_start(ap, fmt);
    // The payload's _vsnprintf returns -1 and leaves the buffer unterminated
    // when the text does not fit, so the string itself is trusted for its
    // length afterwards rather than the return value.
    _vsnprintf(out, (size_t)outsz - 1, fmt, ap);
    va_end(ap);
    out[outsz - 1] = 0;
}

int event_is_quiet(const char *event)
{
    char copy[64];
    char *words[3];
    int count;
    int i = 0;

    copy_str(copy, sizeof copy, event);
    count = line_split(copy, words, 3);
    if (count < 1) return 0;
    if (_stricmp(words[0], "DONE") == 0 || _stricmp(words[0], "FAIL") == 0) i = 1;
    if (count <= i) return 0;

    // A transcript is put into the window in front of that device as text to
    // paste, and the text is nobody else's line: the event, and the word that
    // it went, are not news about the device. A device that would not take it -
    // an older build, which answers "unknown event" - is worth a line, so a
    // FAIL is not quiet.
    if (_stricmp(words[i], "INSERT") == 0)
        return i == 0 || _stricmp(words[0], "DONE") == 0;

    return _stricmp(words[i], "GETVOL") == 0 ||
           _stricmp(words[i], "GETDIM") == 0 ||
           _stricmp(words[i], "GETMUTE") == 0 ||
           // The window asks which build a device runs whenever it picks one,
           // so the answer is a poll rather than news about the device.
           _stricmp(words[i], "GETBUILD") == 0 ||
           // A click is not news about the device, and logging every one would
           // bury everything that is.
           _stricmp(words[i], "INPUT") == 0 ||
           // Asking how big a device's screen is is asked once, when it is
           // put into the window's arrangement, and is a look rather than
           // news about the device.
           _stricmp(words[i], "SCREENINFO") == 0 ||
           // Pasted text crosses the arrangement: what this machine or that one
           // had copied is not news about the device, and the text is nobody
           // else's line.
           _stricmp(words[i], "CLIPGET") == 0 ||
           _stricmp(words[i], "CLIPSET") == 0 ||
           _stricmp(words[i], "CLIPPASTE") == 0 ||
           _stricmp(words[i], "CLIPSEQ") == 0;
}

void event_for_log(const char *event, char *out, int outsz)
{
    int limit = 120;
    int n = (int)strlen(event);

    if (outsz <= 0) return;

    if (limit > outsz - 8) limit = outsz - 8;
    if (limit < 8) limit = 8;
    if (n <= limit) {
        copy_str(out, outsz, event);
        return;
    }
    memcpy(out, event, (size_t)limit);
    out[limit] = 0;
    strncat(out, " ...", (size_t)(outsz - limit - 1));
}

const char *computer_name_problem(const char *name)
{
    int i, digits = 1, len = (int)strlen(name);

    if (len == 0) return "Type a name for the computer.";
    // Keep the name within the 15-character NetBIOS limit.
    if (len > 15) return "A Windows computer name is at most 15 characters.";
    for (i = 0; i < len; i++) {
        char c = name[i];

        if (c >= '0' && c <= '9') continue;
        digits = 0;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')
            continue;
        return "Use letters, digits and hyphens only.";
    }
    if (digits) return "A computer name cannot be all digits.";
    return NULL;
}


void exe_dir(char *out, int outsz)
{
    char path[MAX_PATH];
    DWORD n;
    int i;

    out[0] = 0;
    n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path) return;

    for (i = (int)n - 1; i >= 0; i--) {
        if (path[i] == '\\' || path[i] == '/') {
            path[i + 1] = 0;
            break;
        }
    }
    if (i < 0) path[0] = 0;
    copy_str(out, outsz, path);
}

int random_bytes(void *buf, int n)
{
    HCRYPTPROV prov = 0;
    BOOL ok = FALSE;

    if (n < 0 || (n > 0 && !buf)) return 0;
    if (n == 0) return 1;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
        ok = CryptGenRandom(prov, (DWORD)n, (BYTE *)buf);
        CryptReleaseContext(prov, 0);
    }
    if (!ok) memset(buf, 0, (size_t)n);
    return ok ? 1 : 0;
}

int token_generate(char *out, int outsz)
{
    unsigned char raw[32];
    int i;

    if (!out || outsz < 65) {
        if (out && outsz > 0) out[0] = 0;
        return 0;
    }
    if (!random_bytes(raw, sizeof raw)) {
        out[0] = 0;
        return 0;
    }
    for (i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", (unsigned)raw[i]);
    out[i * 2] = 0;
    SecureZeroMemory(raw, sizeof raw);
    return 1;
}

int token_load(char *out, int outsz, int create_missing)
{
    char dir[MAX_PATH];
    char path[MAX_PATH];
    FILE *f;
    int n;

    exe_dir(dir, sizeof dir);
    copy_str(path, sizeof path, dir);
    strncat(path, "token.txt", sizeof path - strlen(path) - 1);

    f = fopen(path, "rb");
    if (!f) {
        if (!create_missing) {
            printf("error: no token file at %s\n", path);
            return 0;
        }
        if (!token_generate(out, outsz)) {
            printf("error: the crypto provider would not make a token\n");
            return 0;
        }
        f = fopen(path, "wb");
        if (!f) {
            printf("error: cannot create %s\n", path);
            return 0;
        }
        fprintf(f, "%s\n", out);
        fclose(f);
        printf("created %s\n", path);
        printf("copy that file next to tool.exe on every device.\n");
        return 1;
    }

    n = (int)fread(out, 1, outsz - 1, f);
    fclose(f);
    if (n < 0) n = 0;
    out[n] = 0;
    // Trim trailing whitespace and anything after it.
    while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == '\n' ||
                     out[n - 1] == ' '  || out[n - 1] == '\t')) {
        out[--n] = 0;
    }
    if (n == 0) {
        printf("error: %s is empty\n", path);
        return 0;
    }
    return 1;
}

// ------------------------------------------------------------ the key exchange
//
// The first two things on a connection are 32 random bytes each way. They are
// not a message and there is nothing to read in them: they exist so that both
// ends can derive a key that is different for every connection. An
// eavesdropper who keeps every byte of them still cannot derive the key,
// because the token is what the key is made of.
//
// After them, everything in both directions is a sealed record. There is no
// readable message anywhere in the protocol, at any point - not even a hello.

static int send_all(SOCKET s, const unsigned char *buf, int len)
{
    int sent, total = 0;

    while (total < len) {
        sent = send(s, (const char *)buf + total, len - total, 0);
        if (sent <= 0) return 0;
        total += sent;
    }
    return 1;
}

static int recv_all(SOCKET s, unsigned char *buf, int len, int timeout_ms)
{
    int got, total = 0;

    while (total < len) {
        if (sock_wait(s, timeout_ms) <= 0) return 0;
        got = recv(s, (char *)buf + total, len - total, 0);
        if (got <= 0) return 0;
        total += got;
    }
    return 1;
}

int session_start(SOCKET sock, int initiator, const char *token,
                  char *why, int whysz)
{
    static const char *label = "instruments session";
    int one = 1;
    unsigned char mine[TOOL_HANDSHAKE_LEN];
    unsigned char theirs[TOOL_HANDSHAKE_LEN];
    unsigned char salt[TOOL_HANDSHAKE_LEN * 2];
    unsigned char okm[TOOL_SESSION_LEN];

    why[0] = 0;
    // Every line on this connection is a small one of its own - a mouse move is
    // one short line - and Nagle would sit on the second of two until the first
    // was answered, which is a remote pointer that arrives in fits. The line
    // after this one is the first thing that cares, so it is set here, on both
    // ends of every connection, rather than only where frequent traffic goes
    // out.
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    if (!random_bytes(mine, sizeof mine)) {
        copy_str(why, whysz, "the crypto provider would not make random bytes");
        return 0;
    }

    if (initiator) {
        if (!send_all(sock, mine, sizeof mine)) {
            copy_str(why, whysz, "could not send the opening bytes");
            return 0;
        }
        if (!recv_all(sock, theirs, sizeof theirs, 10000)) {
            copy_str(why, whysz, "no answer from the other end");
            return 0;
        }
    } else {
        if (!recv_all(sock, theirs, sizeof theirs, 10000)) {
            copy_str(why, whysz, "nothing arrived from the other end");
            return 0;
        }
        if (!send_all(sock, mine, sizeof mine)) {
            copy_str(why, whysz, "could not answer with our own bytes");
            return 0;
        }
    }

    // Both ends build this the same way: the initiator's bytes first.
    if (initiator) {
        memcpy(salt, mine, TOOL_HANDSHAKE_LEN);
        memcpy(salt + TOOL_HANDSHAKE_LEN, theirs, TOOL_HANDSHAKE_LEN);
    } else {
        memcpy(salt, theirs, TOOL_HANDSHAKE_LEN);
        memcpy(salt + TOOL_HANDSHAKE_LEN, mine, TOOL_HANDSHAKE_LEN);
    }
    if (!hkdf_sha256((const unsigned char *)token, (int)strlen(token),
                     salt, (int)sizeof salt,
                     (const unsigned char *)label, (int)strlen(label),
                     okm, TOOL_SESSION_LEN)) {
        copy_str(why, whysz, "cannot derive this session's keys");
        return 0;
    }

    if (!secure_activate(sock, initiator ? 0 : 1, okm)) {
        copy_str(why, whysz, "cannot seal this session");
        return 0;
    }
    return 1;
}

int run_capture_ex(const char *cmdline, const char *dir, char *out, int outsz,
                   int timeout_ms, int *exit_code)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    HANDLE nul = INVALID_HANDLE_VALUE;
    char *cmd;
    int cmdlen;
    DWORD deadline;
    int got = 0;
    int started = 0;
    int timed_out = 0;
    int done = 0;

    out[0] = 0;
    if (exit_code) *exit_code = 0;

    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        copy_str(out, outsz, "cannot create pipe");
        return 0;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = (nul == INVALID_HANDLE_VALUE) ? NULL : nul;

    cmdlen = (int)strlen(cmdline) + 1;
    cmd = (char *)malloc(cmdlen);
    if (!cmd) {
        CloseHandle(rd);
        CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        copy_str(out, outsz, "out of memory");
        return 0;
    }
    copy_str(cmd, cmdlen, cmdline);

    memset(&pi, 0, sizeof pi);
    started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             NULL, dir, &si, &pi);
    free(cmd);
    CloseHandle(wr);            // our copy; the child holds the write end now
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!started) {
        CloseHandle(rd);
        sprintf(out, "cannot run (error %lu)", (unsigned long)GetLastError());
        return 0;
    }

    // Drain the pipe while the child runs, so a chatty command cannot block.
    deadline = GetTickCount() + (DWORD)(timeout_ms > 0 ? timeout_ms : 0);
    while (!done) {
        DWORD avail = 0;
        DWORD gotn = 0;
        char chunk[1024];

        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;

        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            if ((int)(GetTickCount() - deadline) >= 0) {
                timed_out = 1;
                break;
            }
            Sleep(10);
            continue;
        }

        if (avail > sizeof chunk) avail = (DWORD)sizeof chunk;
        if (!ReadFile(rd, chunk, avail, &gotn, NULL) || gotn == 0) break;
        if (got + (int)gotn < outsz - 1) {
            memcpy(out + got, chunk, gotn);
            got += (int)gotn;
            out[got] = 0;
        }
    }

    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        copy_str(out, outsz, "timed out");
    } else {
        DWORD code = 0;
        WaitForSingleObject(pi.hProcess, 5000);
        if (exit_code && GetExitCodeProcess(pi.hProcess, &code))
            *exit_code = (int)code;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);

    // One protocol line only: newlines and control bytes become spaces.
    {
        int i;
        for (i = 0; i < got; i++) {
            unsigned char c = (unsigned char)out[i];
            if (c < 32 || c == 127) out[i] = ' ';
        }
        while (got > 0 && out[got - 1] == ' ') out[--got] = 0;
        while (got > 0 && out[0] == ' ') {
            memmove(out, out + 1, (size_t)got);
            got--;
        }
    }
    return !timed_out;
}

int run_capture(const char *cmdline, char *out, int outsz, int timeout_ms)
{
    return run_capture_ex(cmdline, NULL, out, outsz, timeout_ms, NULL);
}

// ------------------------------------------------------------------- base64

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_encode(const unsigned char *in, int n, char *out, int outsz)
{
    int i = 0;
    int o = 0;

    while (i < n) {
        int rem = n - i;
        unsigned long v = (unsigned long)in[i] << 16;

        if (rem > 1) v |= (unsigned long)in[i + 1] << 8;
        if (rem > 2) v |= (unsigned long)in[i + 2];
        if (o + 4 >= outsz) break;

        out[o++] = b64_chars[(v >> 18) & 63];
        out[o++] = b64_chars[(v >> 12) & 63];
        out[o++] = rem > 1 ? b64_chars[(v >> 6) & 63] : '=';
        out[o++] = rem > 2 ? b64_chars[v & 63] : '=';
        i += 3;
    }
    out[o] = 0;
    return o;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, unsigned char *out, int outsz)
{
    int quad[4];
    int n = 0;
    int o = 0;

    for (; *in; in++) {
        int v;

        if (*in == '=') break;
        v = b64_value(*in);
        if (v < 0) continue;
        quad[n++] = v;
        if (n == 4) {
            unsigned long w = ((unsigned long)quad[0] << 18) |
                              ((unsigned long)quad[1] << 12) |
                              ((unsigned long)quad[2] << 6) |
                              (unsigned long)quad[3];
            if (o + 3 > outsz) break;
            out[o++] = (unsigned char)((w >> 16) & 0xff);
            out[o++] = (unsigned char)((w >> 8) & 0xff);
            out[o++] = (unsigned char)(w & 0xff);
            n = 0;
        }
    }
    if (n == 2) {
        unsigned long w = ((unsigned long)quad[0] << 18) | ((unsigned long)quad[1] << 12);
        if (o + 1 <= outsz) out[o++] = (unsigned char)((w >> 16) & 0xff);
    } else if (n == 3) {
        unsigned long w = ((unsigned long)quad[0] << 18) | ((unsigned long)quad[1] << 12) |
                          ((unsigned long)quad[2] << 6);
        if (o + 2 <= outsz) {
            out[o++] = (unsigned char)((w >> 16) & 0xff);
            out[o++] = (unsigned char)((w >> 8) & 0xff);
        }
    }
    return o;
}

// ------------------------------------------------------- streaming SHA-256

void sha256_begin(sha256_t *h)
{
    h->prov = 0;
    h->hash = 0;
    h->ok = 0;

    if (!CryptAcquireContextA(&h->prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        return;
    if (!CryptCreateHash(h->prov, CALG_SHA_256, 0, 0, &h->hash)) {
        CryptReleaseContext(h->prov, 0);
        h->prov = 0;
        return;
    }
    h->ok = 1;
}

void sha256_update(sha256_t *h, const void *buf, int len)
{
    if (!h->ok || len <= 0) return;
    if (!CryptHashData(h->hash, (BYTE *)buf, (DWORD)len, 0)) h->ok = 0;
}

int sha256_end(sha256_t *h, char *outhex, int outsz)
{
    BYTE digest[32];
    DWORD dlen = sizeof digest;
    int i;
    int ok = 0;

    if (outhex && outsz > 0) outhex[0] = 0;
    if (h->ok && CryptGetHashParam(h->hash, HP_HASHVAL, digest, &dlen, 0)) {
        for (i = 0; i < 32 && (i * 2 + 2) < outsz; i++)
            sprintf(outhex + i * 2, "%02x", (unsigned)digest[i]);
        outhex[i * 2] = 0;
        ok = 1;
    }
    if (h->hash) CryptDestroyHash(h->hash);
    if (h->prov) CryptReleaseContext(h->prov, 0);
    h->hash = 0;
    h->prov = 0;
    h->ok = 0;
    return ok;
}

int file_size_sha256(const char *path, tool_size *size, char *outhex,
                     int outsz, char *why, int whysz)
{
    FILE *f;
    sha256_t h;
    unsigned char buf[TOOL_CHUNK];
    size_t got;
    tool_size total = 0;

    why[0] = 0;
    if (size) *size = 0;
    if (outhex && outsz > 0) outhex[0] = 0;

    f = fopen(path, "rb");
    if (!f) {
        copy_str(why, whysz, "cannot open that local file");
        return 0;
    }
    sha256_begin(&h);
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
        total += (tool_size)got;
        sha256_update(&h, buf, (int)got);
        if (total > TOOL_MAX_FILE) {
            fclose(f);
            sha256_end(&h, outhex, outsz);
            copy_str(why, whysz, "that file is over the size limit");
            return 0;
        }
    }
    fclose(f);
    if (!sha256_end(&h, outhex, outsz)) {
        copy_str(why, whysz, "cannot hash that local file");
        return 0;
    }
    if (size) *size = total;
    return 1;
}

void lines_init(lines_t *lr, SOCKET sock)
{
    lr->sock = sock;
    lr->len = 0;
    lr->last_rx = GetTickCount();
}

// ------------------------------------------------------------- sealed lines

#define SECURE_SLOTS 64

typedef struct {
    SOCKET sock;
    int on;
    unsigned char tx[TOOL_KEY_LEN];
    unsigned char rx[TOOL_KEY_LEN];
    unsigned long long tx_seq;
    unsigned long long rx_seq;
} secure_slot;

static secure_slot g_secure[SECURE_SLOTS];
static CRITICAL_SECTION g_secure_lock;
static int g_secure_started;

void secure_startup(void)
{
    if (g_secure_started) return;
    InitializeCriticalSection(&g_secure_lock);
    memset(g_secure, 0, sizeof g_secure);
    g_secure_started = 1;
}

// Caller holds g_secure_lock. -1 when this socket is not sealed.
static int secure_slot_for(SOCKET sock)
{
    int i;

    if (!g_secure_started) return -1;
    for (i = 0; i < SECURE_SLOTS; i++) {
        if (g_secure[i].on && g_secure[i].sock == sock) return i;
    }
    return -1;
}

int secure_is_on(SOCKET sock)
{
    int found;

    if (!g_secure_started) return 0;
    EnterCriticalSection(&g_secure_lock);
    found = secure_slot_for(sock) >= 0;
    LeaveCriticalSection(&g_secure_lock);
    return found;
}

int secure_activate(SOCKET sock, int server_side,
                    const unsigned char okm[TOOL_SESSION_LEN])
{
    int i, slot = -1;

    if (!g_secure_started) return 0;
    EnterCriticalSection(&g_secure_lock);
    for (i = 0; i < SECURE_SLOTS; i++) {
        if (g_secure[i].on && g_secure[i].sock == sock) {
            slot = i;
            break;
        }
        if (slot < 0 && !g_secure[i].on) slot = i;
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_secure_lock);
        return 0;
    }
    g_secure[slot].sock = sock;
    g_secure[slot].on = 1;
    // One key each way, so the same counter can be used in both directions.
    if (server_side) {
        memcpy(g_secure[slot].tx, okm, TOOL_KEY_LEN);
        memcpy(g_secure[slot].rx, okm + TOOL_KEY_LEN, TOOL_KEY_LEN);
    } else {
        memcpy(g_secure[slot].rx, okm, TOOL_KEY_LEN);
        memcpy(g_secure[slot].tx, okm + TOOL_KEY_LEN, TOOL_KEY_LEN);
    }
    g_secure[slot].tx_seq = 0;
    g_secure[slot].rx_seq = 0;
    LeaveCriticalSection(&g_secure_lock);
    return 1;
}

void secure_forget(SOCKET sock)
{
    int i;

    if (!g_secure_started) return;
    EnterCriticalSection(&g_secure_lock);
    for (i = 0; i < SECURE_SLOTS; i++) {
        if (g_secure[i].on && g_secure[i].sock == sock) {
            g_secure[i].on = 0;
            g_secure[i].sock = INVALID_SOCKET;
        }
    }
    LeaveCriticalSection(&g_secure_lock);
}

static void put_seq(unsigned char *nonce, unsigned long long seq)
{
    int i;

    memset(nonce, 0, 4);
    for (i = 0; i < 8; i++)
        nonce[4 + i] = (unsigned char)(seq >> (56 - i * 8));
}

// Seals one line into out, which must hold len + 20 bytes: the length, the
// ciphertext and the tag. Returns the wire length, or 0 when this socket has
// no sealed session.
static int secure_seal(SOCKET sock, const unsigned char *line, int len,
                       unsigned char *out)
{
    unsigned char nonce[TOOL_NONCE_LEN];
    unsigned char aad[4];
    int slot, n;

    if (!g_secure_started) return 0;
    EnterCriticalSection(&g_secure_lock);
    slot = secure_slot_for(sock);
    if (slot < 0) {
        LeaveCriticalSection(&g_secure_lock);
        return 0;
    }
    n = len + TOOL_TAG_LEN;
    aad[0] = (unsigned char)(n >> 24);
    aad[1] = (unsigned char)(n >> 16);
    aad[2] = (unsigned char)(n >> 8);
    aad[3] = (unsigned char)n;
    memcpy(out, aad, 4);
    put_seq(nonce, g_secure[slot].tx_seq);
    aead_seal(g_secure[slot].tx, nonce, aad, 4, line, len, out + 4);
    g_secure[slot].tx_seq++;
    LeaveCriticalSection(&g_secure_lock);
    return n + 4;
}

// Opens one record into out. Returns the plaintext length, or -1 when the
// record does not check out.
static int secure_open(SOCKET sock, const unsigned char *record, int len,
                       unsigned char *out)
{
    unsigned char nonce[TOOL_NONCE_LEN];
    int slot, plain;

    if (!g_secure_started) return -1;
    EnterCriticalSection(&g_secure_lock);
    slot = secure_slot_for(sock);
    if (slot < 0) {
        LeaveCriticalSection(&g_secure_lock);
        return -1;
    }
    put_seq(nonce, g_secure[slot].rx_seq);
    plain = len - 4 - TOOL_TAG_LEN;
    if (plain < 0 ||
        !aead_open(g_secure[slot].rx, nonce, record, 4, record + 4,
                   len - 4, out)) {
        LeaveCriticalSection(&g_secure_lock);
        return -1;
    }
    g_secure[slot].rx_seq++;
    LeaveCriticalSection(&g_secure_lock);
    return plain;
}

int line_read(lines_t *lr, char *out, int outsz)
{
    unsigned char plain[TOOL_MAXLINE];

    for (;;) {
        int n;
        int need, plainlen;

        // A connection has no lines until its session is sealed.
        if (!secure_is_on(lr->sock)) return -1;

        while (lr->len < 4) {
            n = recv(lr->sock, (char *)lr->buf + lr->len,
                     sizeof lr->buf - lr->len, 0);
            if (n == 0) return 0;
            if (n < 0) return -1;
            lr->len += n;
        }
        need = ((int)lr->buf[0] << 24) | ((int)lr->buf[1] << 16) |
               ((int)lr->buf[2] << 8) | (int)lr->buf[3];
        if (need < TOOL_TAG_LEN ||
            need > TOOL_MAXLINE + TOOL_TAG_LEN)
            return -1;                      // not something this can hold

        while (lr->len < 4 + need) {
            n = recv(lr->sock, (char *)lr->buf + lr->len,
                     sizeof lr->buf - lr->len, 0);
            if (n == 0) return 0;
            if (n < 0) return -1;
            lr->len += n;
        }
        plainlen = secure_open(lr->sock, lr->buf, 4 + need, plain);
        if (plainlen < 0) return -1;        // a changed byte, or a replay

        memmove(lr->buf, lr->buf + 4 + need, (size_t)(lr->len - 4 - need));
        lr->len -= 4 + need;

        while (plainlen > 0 && (plain[plainlen - 1] == '\n' ||
                                plain[plainlen - 1] == '\r'))
            plainlen--;
        if (plainlen >= outsz) plainlen = outsz - 1;
        memcpy(out, plain, (size_t)plainlen);
        out[plainlen] = 0;
        lr->last_rx = GetTickCount();
        return 1;
    }
}

int line_pending(lines_t *lr)
{
    int need;

    if (!secure_is_on(lr->sock)) return 0;
    if (lr->len < 4) return 0;
    need = ((int)lr->buf[0] << 24) | ((int)lr->buf[1] << 16) |
           ((int)lr->buf[2] << 8) | (int)lr->buf[3];
    if (need < TOOL_TAG_LEN ||
        need > TOOL_MAXLINE + TOOL_TAG_LEN)
        return 0;
    return lr->len >= 4 + need;
}

int line_send(SOCKET sock, CRITICAL_SECTION *lock, const char *fmt, ...)
{
    char line[TOOL_MAXLINE];
    unsigned char record[TOOL_RECORD_MAX];
    va_list ap;
    int n;
    int sent;
    int total = 0;

    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 3, fmt, ap);
    va_end(ap);
    line[sizeof line - 3] = 0;
    n = (int)strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';

    if (lock) EnterCriticalSection(lock);

    {
        int wired = secure_seal(sock, (const unsigned char *)line, n, record);

        // A sealed session is required, so refuse the line if it could not
        // be sealed.
        if (wired <= 0) {
            if (lock) LeaveCriticalSection(lock);
            return 0;
        }
        n = wired;
        while (total < n) {
            sent = send(sock, (const char *)record + total, n - total, 0);
            if (sent <= 0) {
                if (lock) LeaveCriticalSection(lock);
                return 0;
            }
            total += sent;
        }
    }

    if (lock) LeaveCriticalSection(lock);
    return 1;
}

// --------------------------------------------------------- sealed datagrams
//
// The shape is written down in common.h. The key is the token's, derived once;
// the number under each datagram is this process's own, so the lock is here
// because a server relaying for several consoles seals from all of them.

static unsigned char g_dgram_key[TOOL_KEY_LEN];
static unsigned g_dgram_epoch;
static unsigned g_dgram_seq;
static int g_dgram_on;
static CRITICAL_SECTION g_dgram_lock;

int datagram_startup(const char *token)
{
    static const unsigned char label[] = "instruments datagram";
    unsigned char epoch[4];

    if (g_dgram_on) return 1;
    if (!random_bytes(epoch, sizeof epoch)) return 0;
    if (!hkdf_sha256((const unsigned char *)token, (int)strlen(token),
                     label, (int)sizeof label - 1,
                     label, (int)sizeof label - 1,
                     g_dgram_key, TOOL_KEY_LEN)) {
        memset(g_dgram_key, 0, sizeof g_dgram_key);
        return 0;
    }
    g_dgram_epoch = ((unsigned)epoch[0] << 24) | ((unsigned)epoch[1] << 16) |
                    ((unsigned)epoch[2] << 8) | (unsigned)epoch[3];
    if (g_dgram_epoch == 0) g_dgram_epoch = 1;      // 0 means "no sender yet"
    g_dgram_seq = 0;
    InitializeCriticalSection(&g_dgram_lock);
    g_dgram_on = 1;
    return 1;
}

static void dgram_number(unsigned char *out, unsigned epoch, unsigned seq)
{
    out[0] = (unsigned char)(epoch >> 24);
    out[1] = (unsigned char)(epoch >> 16);
    out[2] = (unsigned char)(epoch >> 8);
    out[3] = (unsigned char)epoch;
    out[4] = (unsigned char)(seq >> 24);
    out[5] = (unsigned char)(seq >> 16);
    out[6] = (unsigned char)(seq >> 8);
    out[7] = (unsigned char)seq;
}

static unsigned dgram_read(const unsigned char *in)
{
    return ((unsigned)in[0] << 24) | ((unsigned)in[1] << 16) |
           ((unsigned)in[2] << 8) | (unsigned)in[3];
}

int datagram_seal(const char *text, unsigned char *out, int outsz)
{
    unsigned char nonce[TOOL_NONCE_LEN];
    unsigned char number[8];
    int len = (int)strlen(text);

    if (!g_dgram_on) return 0;
    if (len + 8 + TOOL_TAG_LEN > outsz) return 0;

    EnterCriticalSection(&g_dgram_lock);
    if (g_dgram_seq == 0xffffffffu) {
        // The count has come the whole way round, so the number is a new one:
        // a fresh epoch keeps it from ever being one this key has sealed.
        unsigned char epoch[4];

        if (!random_bytes(epoch, sizeof epoch)) {
            g_dgram_on = 0;
            LeaveCriticalSection(&g_dgram_lock);
            return 0;
        }
        g_dgram_epoch = ((unsigned)epoch[0] << 24) | ((unsigned)epoch[1] << 16) |
                        ((unsigned)epoch[2] << 8) | (unsigned)epoch[3];
        if (g_dgram_epoch == 0) g_dgram_epoch = 1;
        g_dgram_seq = 1;
    } else {
        g_dgram_seq++;
    }
    dgram_number(number, g_dgram_epoch, g_dgram_seq);
    LeaveCriticalSection(&g_dgram_lock);

    memset(nonce, 0, 4);
    memcpy(nonce + 4, number, 8);
    memcpy(out, number, 8);
    aead_seal(g_dgram_key, nonce, number, 8,
              (const unsigned char *)text, len, out + 8);
    return len + 8 + TOOL_TAG_LEN;
}

datagram_from *datagram_peer_for(datagram_peer *table, int count,
                                 const struct sockaddr_in *peer)
{
    int free = -1;
    int i;

    for (i = 0; i < count; i++) {
        if (table[i].used &&
            table[i].addr.sin_addr.s_addr == peer->sin_addr.s_addr &&
            table[i].addr.sin_port == peer->sin_port)
            return &table[i].from;
        if (!table[i].used && free < 0) free = i;
    }
    if (free < 0) return NULL;
    table[free].used = 1;
    table[free].addr = *peer;
    return &table[free].from;
}

int datagram_open(datagram_from *from, const unsigned char *wire, int len,
                  char *out, int outsz)
{
    unsigned char nonce[TOOL_NONCE_LEN];
    unsigned epoch, seq;
    int plain;

    if (!g_dgram_on || len < 8 + TOOL_TAG_LEN || outsz < 1) return -1;
    epoch = dgram_read(wire);
    seq = dgram_read(wire + 4);
    // Epoch 0 is what an untouched sender record holds, so it is never one a
    // sender seals under.
    if (epoch == 0) return -1;
    if (from && from->any && from->epoch == epoch && seq <= from->seq)
        return -1;

    plain = len - 8 - TOOL_TAG_LEN;
    if (plain > outsz - 1) return -1;
    memset(nonce, 0, 4);
    memcpy(nonce + 4, wire, 8);
    if (!aead_open(g_dgram_key, nonce, wire, 8, wire + 8, len - 8,
                   (unsigned char *)out))
        return -1;
    out[plain] = 0;
    if (from) {
        from->epoch = epoch;
        from->seq = seq;
        from->any = 1;
    }
    return plain;
}

SOCKET datagram_socket(void)
{
    struct sockaddr_in sa;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s == INVALID_SOCKET) return s;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = 0;
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

int datagram_addr(const char *host, int port, struct sockaddr_in *sa)
{
    struct hostent *he;

    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons((unsigned short)port);
    sa->sin_addr.s_addr = inet_addr(host);
    if (sa->sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname(host);
        if (!he) return 0;
        memcpy(&sa->sin_addr, he->h_addr, (size_t)he->h_length);
    }
    return 1;
}

int datagram_sendto(SOCKET s, const char *ip, int port, const char *text)
{
    unsigned char wire[TOOL_MAXLINE];
    struct sockaddr_in sa;
    int len;

    if (s == INVALID_SOCKET) return 0;
    if (!datagram_addr(ip, port, &sa)) return 0;
    len = datagram_seal(text, wire, sizeof wire);
    if (len <= 0) return 0;
    return sendto(s, (const char *)wire, len, 0,
                  (struct sockaddr *)&sa, sizeof sa) == len;
}

int datagram_wait(SOCKET s, int ms)
{
    return sock_wait(s, ms);
}

int datagram_recv(SOCKET s, unsigned char *wire, int wiresz,
                  struct sockaddr_in *peer)
{
    struct sockaddr_in from;
    int fromlen = sizeof from;
    int got;

    got = recvfrom(s, (char *)wire, wiresz, 0,
                   (struct sockaddr *)&from, &fromlen);
    if (got < 0) return -1;
    if (peer) *peer = from;
    return got;
}

void datagram_local_ip(const char *host, int port, char *out, int outsz)
{
    struct sockaddr_in sa;
    unsigned char *b;
    SOCKET s;
    int len = sizeof sa;

    if (outsz > 0) out[0] = 0;
    if (!datagram_addr(host, port, &sa)) return;
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0 &&
        getsockname(s, (struct sockaddr *)&sa, &len) == 0) {
        b = (unsigned char *)&sa.sin_addr;
        format_text(out, outsz, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    closesocket(s);
}

int line_split(char *line, char *words[], int maxwords)
{
    int count = 0;
    char *p = line;

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (count < maxwords) words[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p) break;
        *p++ = 0;
    }
    return count;
}

char *line_rest(char *line, int n)
{
    int count = 0;
    char *p = line;

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return line + strlen(line);
        if (count == n) return p;
        while (*p && *p != ' ' && *p != '\t') p++;
        count++;
    }
    return line + strlen(line);
}

void sock_text(SOCKET sock, char *out, int outsz)
{
    struct sockaddr_in sa;
    int len = sizeof sa;

    copy_str(out, outsz, "?");
    if (getpeername(sock, (struct sockaddr *)&sa, &len) == 0) {
        unsigned char *b = (unsigned char *)&sa.sin_addr;
        sprintf(out, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
                (unsigned)ntohs(sa.sin_port));
    }
}

int sock_wait(SOCKET sock, int ms)
{
    fd_set rd;
    struct timeval tv;
    int rc;

    FD_ZERO(&rd);
    FD_SET(sock, &rd);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    rc = select(0, &rd, NULL, NULL, &tv);
    if (rc <= 0) return rc;
    return 1;
}
