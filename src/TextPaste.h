// Tools - the text pasting that Input Director carries between endpoints.

#ifndef TOOL_TEXT_PASTE_H
#define TOOL_TEXT_PASTE_H

#include "common.h"

// Text one paste carries, in UTF-8 bytes. The base64 of it rides one line that
// also carries "CMD", the endpoint's name and the event, or the "RESULT" line
// the answer comes back on - 96 bytes with room to spare - so this is what is
// left of a line once those are on it.
#define TEXT_PASTE_MAX ((TOOL_MAXLINE - 96) / 4 * 3)

// This session's text to paste, as UTF-8 text in out. 1 = out holds it, 0 =
// there is nothing to paste, -1 = the text is longer than outsz. The Windows
// text-paste format is CF_UNICODETEXT, so text that is not plain ASCII crosses
// too.
int text_paste_get(char *out, int outsz);

// Prepares UTF-8 text for this session to paste. 1 = it is there.
int text_paste_put(const char *text);

#endif
