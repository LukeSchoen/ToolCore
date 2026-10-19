#include "TextPaste.h"

// ------------------------------------------------------------------ pasting
//
// Both ends do the same two things with the text a Ctrl+C or a Ctrl+V
// carries: read this session's text to paste, and prepare text to paste.

// Windows keeps pasted text in one place per desktop and only one program may
// have it open, so an open a passing program is holding is waited for rather
// than taken as no text to paste. The programs that hold it open let go in far
// less than this.
static int text_paste_open(void)
{
    int i;

    for (i = 0; i < 12; i++) {
        if (OpenClipboard(NULL)) return 1;
        Sleep(10);
    }
    return 0;
}

int text_paste_get(char *out, int outsz)
{
    HANDLE mem;
    const wchar_t *wide;
    int n;

    if (!text_paste_open()) return 0;
    mem = GetClipboardData(CF_UNICODETEXT);
    wide = mem ? (const wchar_t *)GlobalLock(mem) : NULL;
    if (!wide) {
        CloseClipboard();
        return 0;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outsz, NULL, NULL);
    GlobalUnlock(mem);
    CloseClipboard();
    // A buffer too small leaves nothing written, which is different from having
    // no text to paste.
    if (n <= 0) return -1;
    return n - 1;                       // without the terminating zero
}

int text_paste_put(const char *text)
{
    HGLOBAL mem;
    wchar_t *wide;
    int chars;
    int ok = 0;

    chars = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (chars <= 0) return 0;
    mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)chars * sizeof(wchar_t));
    if (!mem) return 0;
    wide = (wchar_t *)GlobalLock(mem);
    if (wide) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, chars);
        GlobalUnlock(mem);
        if (text_paste_open()) {
            EmptyClipboard();
            // Windows keeps the text it was handed, so a mem that got there is
            // not this program's to free.
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
            CloseClipboard();
        }
    }
    if (!ok) GlobalFree(mem);
    return ok;
}
