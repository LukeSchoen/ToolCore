// Tools - the line protocol, shared by the server, the endpoint and the
// console.
//
// One message is one line: space-separated fields, the last of which runs to
// the end of the line. Nothing on a connection is readable until the two ends
// have derived their session keys from the shared token (see crypto.h); after
// that every line travels as a sealed record.

#ifndef TOOL_COMMON_H
#define TOOL_COMMON_H

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"

#define TOOL_VERSION "0.1"
#define TOOL_MAXLINE 8192
#define TOOL_DEFAULT_PORT 7901

// A sealed line on the wire is a 4-byte big-endian length, then that many
// bytes of ciphertext with the 16-byte tag on the end. The buffer a reader
// fills has to hold the longest of those: the longest plaintext line, plus the
// tag, plus the length.
#define TOOL_RECORD_MAX (TOOL_MAXLINE + 32)

// A DATA line carries TOOL_CHUNK raw bytes, which is 4096 base64
// characters - comfortably inside one protocol line.
#define TOOL_CHUNK 3072

// Every file size that is counted, sent or printed is an tool_size. It is
// 64-bit on purpose: "unsigned long" is 32 bits on Windows, where 4 GB written
// as an unsigned long folds to 0.
typedef unsigned long long tool_size;

// A transfer may be up to 4 GB. The endpoint turns away anything larger, and
// the console does not start one: the .part file is written in full before it
// is renamed, so the device needs room for two copies.
#define TOOL_MAX_FILE (4ULL * 1024ULL * 1024ULL * 1024ULL)

// ---------------------------------------------------------------- processes

// Directory the running exe sits in, with a trailing backslash.
void exe_dir(char *out, int outsz);

// Reads <exe_dir>token.txt. With create_missing set, a fresh random token is
// written there when the file is absent. Returns 1 on success.
int token_load(char *out, int outsz, int create_missing);

// Random bytes from the crypto provider. Returns 1 only when the provider
// filled the buffer; on failure the buffer is cleared and the return is 0.
int random_bytes(void *buf, int n);

// Generates a fresh 64-character hex token into out. Returns 1 on success.
int token_generate(char *out, int outsz);

// Runs a command line with no window and collects its output. dir, when given,
// is the child's working directory (the endpoint passes its own folder, so a
// relative path in an extension command means what it means in a GET or a PUT:
// next to the exe). Newlines become spaces so the answer fits on one protocol
// line.
// Returns 1 when the child exited (out holds its output), 0 on timeout or
// failure (out holds a reason). exit_code, when given, receives the child's
// exit code - 0 when it never ran or had to be killed, so only trust it on a 1
// return.
int run_capture_ex(const char *cmdline, const char *dir, char *out, int outsz,
                   int timeout_ms, int *exit_code);

// Same thing for callers that only care whether it finished.
int run_capture(const char *cmdline, char *out, int outsz, int timeout_ms);

// ------------------------------------------------------------------- base64

// Fills out with base64 text. Returns the character count written.
int b64_encode(const unsigned char *in, int n, char *out, int outsz);

// Decodes base64 back to bytes, ignoring stray characters. Returns the byte
// count written, which is 0 when the input held nothing useful.
int b64_decode(const char *in, unsigned char *out, int outsz);

// ------------------------------------------------------- streaming SHA-256

// So a file can be hashed in the same pass that reads or writes it.
typedef struct {
    HCRYPTPROV prov;
    HCRYPTHASH hash;
    int ok;
} sha256_t;

void sha256_begin(sha256_t *h);
void sha256_update(sha256_t *h, const void *buf, int len);

// Writes the lowercase hex digest and releases the hash. 1 = the digest is
// valid, 0 = hashing failed somewhere along the way.
int sha256_end(sha256_t *h, char *outhex, int outsz);

// Size and SHA-256 of one whole file. 1 = size/outhex are set, 0 = it could
// not be read or hashed, with a short reason in why. Also refuses anything
// over TOOL_MAX_FILE.
int file_size_sha256(const char *path, tool_size *size, char *outhex,
                     int outsz, char *why, int whysz);

// ------------------------------------------------------------- line protocol

typedef struct {
    SOCKET sock;
    unsigned char buf[TOOL_RECORD_MAX];
    int len;
    DWORD last_rx;
} lines_t;

// ------------------------------------------------------------- sealed lines
//
// Every line on every connection is a sealed record. line_send and line_read
// require a socket with a sealed session.
//
// The state lives here rather than in the callers because line_send is given a
// socket and nothing else, and the server sends to one endpoint from several
// threads.
//
// secure_startup must be called once, from main, before any connection is
// made: nothing else initialises the lock these use.

void secure_startup(void);

// How many random bytes each end contributes at the start of a connection.
#define TOOL_HANDSHAKE_LEN 32

// The first thing on a connection, and the only thing that is not a sealed
// record: 32 random bytes each way, which is all the two ends need to derive a
// key that is different for every connection. There is nothing to read in
// them - no version, no name, no token.
//
//   initiator -> other   32 random bytes
//   other -> initiator   32 random bytes
//   then                 sealed records, both ways, for ever
//
// initiator is the end that dialled (an endpoint, ServCLI, ServGUI); the
// server is the other. 1 = the session is on. 0 = it is not, and why says so.
int session_start(SOCKET sock, int initiator, const char *token,
                  char *why, int whysz);

// Turns sealing on for this socket. server_side decides which half of the key
// material is the sending key. Returns 0 when there is no room.
int secure_activate(SOCKET sock, int server_side,
                    const unsigned char okm[TOOL_SESSION_LEN]);

// Forgets a socket. Call it wherever a connection is closed, or a reused
// socket number would inherit the last connection's keys.
void secure_forget(SOCKET sock);

// 1 when this socket's lines are being sealed.
int secure_is_on(SOCKET sock);

void lines_init(lines_t *lr, SOCKET sock);

// 1 = line read (CR/LF stripped), 0 = peer closed, -1 = socket error.
int line_read(lines_t *lr, char *out, int outsz);

// 1 when a complete line is already sitting in the buffer, so a caller that
// waits for the socket to become readable would miss it.
int line_pending(lines_t *lr);

// Sends one formatted line plus "\r\n". lock may be NULL.
int line_send(SOCKET sock, CRITICAL_SECTION *lock, const char *fmt, ...);

// Splits a line in place on spaces. Returns the word count.
int line_split(char *line, char *words[], int maxwords);

// The rest of the line after the first n words, or "" when there is none.
char *line_rest(char *line, int n);

// --------------------------------------------------------- sealed datagrams

// Input Director's mouse moves ride UDP: one move to a datagram, sealed the
// way a line is - ChaCha20-Poly1305 under a key derived from the token - with
// the one difference UDP forces. A line's nonce is a count both ends keep,
// because a TCP stream neither drops nor reorders; a datagram carries its own
// number instead, since the next one is not promised to follow this one. The
// number is the nonce and the check against a repeat: a receiver keeps the
// highest it has seen from one sender and refuses anything not above it, so a
// move that arrived late or twice does not drag the pointer back.
//
// On the wire a datagram is that 8-byte big-endian number, then the same
// ciphertext and tag a line has. The high four bytes are a random epoch,
// different every time a sender starts, and the low four count from one; a new
// epoch is a sender that has started again, and its count is taken from one
// whatever the last one reached. "P <name>" and its answer "A <name>" use the
// same socket to say a path works; only a move changes the pointer.

// Derives the datagram key from the token and chooses its random epoch. Call
// once, after token_load. Returns 0 and leaves datagrams off when the crypto
// provider cannot supply random bytes.
int datagram_startup(const char *token);

// One sealed datagram for one short line of text. out must hold
// 8 + strlen(text) + TOOL_TAG_LEN bytes. Returns the wire length.
int datagram_seal(const char *text, unsigned char *out, int outsz);

// What one receiver remembers of one sender, so a repeat is refused.
typedef struct {
    unsigned epoch;
    unsigned seq;
    int any;
} datagram_from;

// One sender a receiver remembers: who it is, and that count. A receiver keeps
// a small table of these - the server, and whichever consoles are driving it -
// so that one sender's count is not read as another's.
typedef struct {
    struct sockaddr_in addr;
    int used;
    datagram_from from;
} datagram_peer;

// The record this sender's own count belongs in, made when it first writes.
// NULL when the table is full.
datagram_from *datagram_peer_for(datagram_peer *table, int count,
                                 const struct sockaddr_in *peer);

// Opens one datagram. Returns the text length, or -1 when it does not check
// out or is a repeat of one already taken from that sender.
int datagram_open(datagram_from *from, const unsigned char *wire, int len,
                  char *out, int outsz);

// One UDP socket for the moves, bound to nothing in particular. INVALID_SOCKET
// when it will not open.
SOCKET datagram_socket(void);

// The sealed datagram for one line, sent to ip:port. Returns 1 on success.
int datagram_sendto(SOCKET s, const char *ip, int port, const char *text);

// Waits for the socket to become readable: 1 readable, 0 nothing yet, -1 the
// socket failed.
int datagram_wait(SOCKET s, int ms);

// One datagram read whole, with who sent it. Returns its length, or -1.
int datagram_recv(SOCKET s, unsigned char *wire, int wiresz,
                  struct sockaddr_in *peer);

// The address this machine has on the network that reaches host:port, as
// "a.b.c.d", or "" when there is none. A datagram socket connected to the far
// address knows which of this machine's own addresses it would go out of, and
// asking is what tells the endpoint where it is on the network it shares with
// the server.
void datagram_local_ip(const char *host, int port, char *out, int outsz);

// host:port into a sockaddr_in. 1 on success.
int datagram_addr(const char *host, int port, struct sockaddr_in *sa);

// --------------------------------------------------------------- small utils

// "192.168.1.5:55321" for logs and for the console's list.
void sock_text(SOCKET sock, char *out, int outsz);

// Waits for up to ms for the socket to become readable. 1 = readable, 0 = no,
// -1 = the peer closed or the socket failed.
int sock_wait(SOCKET sock, int ms);

// Safe strcpy into a fixed buffer.
void copy_str(char *dst, int dstsz, const char *src);

// 1 for events that are polled constantly and say nothing worth reading -
// GETVOL, GETDIM and GETMUTE - so the logs stay about things that happened.
// Accepts either the event ("GETVOL") or a whole answer line ("DONE GETVOL 100").
int event_is_quiet(const char *event);

// How much of an event belongs in a log: a long event is carrying bytes rather
// than words, and what a person wants to read is the first fields and an
// ellipsis. A file transfer never comes this way: its DATA lines are routed,
// not logged.
void event_for_log(const char *event, char *out, int outsz);

// The rules a Windows computer name has to satisfy, in one place because two
// programs need them: the console, which asks somebody for a name, and the
// endpoint, which is the one that has to live with it. Letters, digits and
// hyphens, never all digits, and no more than 15 characters.
//
// Returns NULL when Windows will take the name, otherwise a short reason it
// will not, in words a person can read.
const char *computer_name_problem(const char *name);

// Like sprintf, but it never writes past the buffer and always terminates the
// string. Long text is cut short rather than walking off the end.
void format_text(char *out, int outsz, const char *fmt, ...);

#endif
