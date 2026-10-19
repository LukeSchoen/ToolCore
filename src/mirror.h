// Tools - Input Director (INPUT and pointer moves), INSERT and text pasting
// that crosses with the keys.

#ifndef TOOL_MIRROR_H
#define TOOL_MIRROR_H

#include "common.h"

// The connection Input Director's clicks and text pasting ride.
void mirror_attach(SOCKET sock);
void mirror_detach(void);

void mirror_screeninfo(void);           // SCREENINFO
void mirror_input(const char *args);    // INPUT M x y buttons | W delta | K vk down
void mirror_cursor_hold(int on);         // DIMENTER / DIMLEAVE
// One move of this session's pointer, to a point on its own screen: what an
// INPUT M carries, however it arrived - the server's line or a datagram
// straight from the window. 1 when the machine took it, and why says what
// stopped it when it did not.
int mirror_move(int px, int py, int buttons, char *why, int whysz);
void mirror_clipget(const char *args);  // CLIPGET [wait]
void mirror_clipset(const char *args);  // CLIPSET <base64>
void mirror_clippaste(const char *args); // CLIPPASTE <base64>
void mirror_clipseq(void);              // CLIPSEQ
void mirror_insert(const char *args);   // INSERT <base64>

#endif
