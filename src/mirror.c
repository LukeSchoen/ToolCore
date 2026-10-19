// mirror.c - Input Director
//
// INPUT puts a click, a wheel or a key into this session, a screen that has
// gone off coming back on for it.
//
// CLIPGET, CLIPSET and CLIPPASTE carry text pasting between arrangements: one
// line of base64 each way. CLIPGET reads the session's text to paste, CLIPSET
// prepares it, and CLIPPASTE prepares it only while this session has not
// copied something of its own since the arrangement last prepared or looked at
// it - that copy is newer than anything from the arrangement, and a paste on
// this machine uses it.
//
// INSERT is the text of a finished transcript: it is prepared as text to paste,
// and then put into the window the session has in front of it. A console host
// has it typed as Unicode, Tibia has it typed as scan-code keys because it does
// not accept pasted text, and any other window is sent Ctrl+V.

#include "common.h"
#include "TextPaste.h"
#include "mirror.h"

static SOCKET g_sock = INVALID_SOCKET;
static int g_cursor_hold;               // Input Director owns the pointer
// The paste sequence number as the arrangement last left this session's text:
// just after text was prepared, or just after it was read as what the
// arrangement knows is here. Windows moves the number on for every change, so a
// number that has moved on since is this session's own copy, made with the keys
// or the mouse, and is newer than anything the arrangement had. Comparing
// numbers rather than text means a copy that happens to match, or a re-read of
// a copy, cannot be mistaken for no copy at all.
static DWORD g_paste_seq;
static int g_paste_seq_set;

// Windows may keep the pointer hidden after touch input, and a keyboard can
// hide it too when the machine's mouse-vanish setting is on. Input Director
// still needs a pointer to aim, so put it back while the pointer is here. The
// display count is only raised, never lowered: leaving this screen must not
// hide a pointer the person at the machine is using.
static void cursor_show(void)
{
    CURSORINFO ci;
    int i;

    ci.cbSize = sizeof ci;
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) return;
    for (i = 0; i < 16; i++)
        if (ShowCursor(TRUE) >= 0) break;
}

void mirror_cursor_hold(int on)
{
    g_cursor_hold = on;
    if (on) cursor_show();
}

// Input is injected into the calling thread's desktop. The password provider
// runs on the secure input desktop rather than the default one, so a move sent
// from the endpoint's ordinary thread never reaches it. A clean worker thread
// can follow whichever desktop Windows has made active without moving the
// endpoint's windows with it.
static CRITICAL_SECTION g_input_lock;
static HANDLE g_input_go;
static HANDLE g_input_done;
static LONG g_input_once;
static int g_input_ready;
static INPUT *g_input_at;
static UINT g_input_count;
static UINT g_input_took;

static int input_desktop(void)
{
    HDESK mine = GetThreadDesktop(GetCurrentThreadId());
    HDESK active;
    char mine_name[64];
    char active_name[64];
    DWORD needed;

    active = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS |
                                          DESKTOP_WRITEOBJECTS);
    if (!active) return 1;      // use the desktop this thread already has
    if (!mine ||
        !GetUserObjectInformationA(mine, UOI_NAME, mine_name,
                                   sizeof mine_name, &needed) ||
        !GetUserObjectInformationA(active, UOI_NAME, active_name,
                                   sizeof active_name, &needed)) {
        CloseDesktop(active);
        return 1;
    }
    mine_name[sizeof mine_name - 1] = 0;
    active_name[sizeof active_name - 1] = 0;
    if (_stricmp(mine_name, active_name) == 0) {
        CloseDesktop(active);
        return 1;
    }
    if (!SetThreadDesktop(active)) {
        CloseDesktop(active);
        return 0;
    }
    CloseDesktop(active);
    return 1;
}

static DWORD WINAPI input_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (WaitForSingleObject(g_input_go, INFINITE) != WAIT_OBJECT_0) return 0;
        if (!input_desktop()) {
            g_input_took = 0;
        } else {
            if (g_cursor_hold) cursor_show();
            g_input_took = SendInput(g_input_count, g_input_at, sizeof *g_input_at);
            if (g_cursor_hold) cursor_show();
        }
        SetEvent(g_input_done);
    }
}

static int input_start(void)
{
    HANDLE thread;

    if (InterlockedCompareExchange(&g_input_once, 1, 0) == 0) {
        InitializeCriticalSection(&g_input_lock);
        g_input_go = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_input_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        thread = (g_input_go && g_input_done) ?
            CreateThread(NULL, 0, input_thread, NULL, 0, NULL) : NULL;
        if (thread) {
            CloseHandle(thread);
            g_input_ready = 1;
        } else {
            if (g_input_go) CloseHandle(g_input_go);
            if (g_input_done) CloseHandle(g_input_done);
            g_input_go = g_input_done = NULL;
        }
        InterlockedExchange(&g_input_once, 2);
    } else {
        while (g_input_once != 2) Sleep(0);
    }
    return g_input_ready;
}

// Send one group in the order it was built. The worker follows the input
// desktop for every group, so the password provider receives the same INPUT
// events as an ordinary logged-in desktop.
static int input_take(INPUT *in, UINT count, char *why, int whysz)
{
    UINT took;

    if (!input_start()) {
        if (g_cursor_hold) cursor_show();
        took = SendInput(count, in, sizeof *in);
        if (g_cursor_hold) cursor_show();
    } else {
        EnterCriticalSection(&g_input_lock);
        g_input_at = in;
        g_input_count = count;
        SetEvent(g_input_go);
        if (WaitForSingleObject(g_input_done, INFINITE) != WAIT_OBJECT_0) {
            g_input_at = NULL;
            g_input_count = 0;
            LeaveCriticalSection(&g_input_lock);
            copy_str(why, whysz, "the machine would not take it");
            return 0;
        }
        took = g_input_took;
        g_input_at = NULL;
        g_input_count = 0;
        LeaveCriticalSection(&g_input_lock);
    }
    if (took != count) {
        copy_str(why, whysz, "the machine would not take it");
        return 0;
    }
    return 1;
}

// The keys that are two keys on a keyboard but one key code on the wire, so
// the far end can tell the right one of the pair.
static int key_is_extended(int vk)
{
    switch (vk) {
    case VK_RMENU: case VK_RCONTROL: case VK_INSERT: case VK_DELETE:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_NUMLOCK: case VK_DIVIDE: case VK_SNAPSHOT:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return 1;
    }
    return 0;
}

// One of the two buttons a mouse with back and forward has. SendInput tells
// them apart by mouseData rather than by flag, so, unlike left, right and
// middle, each goes into an event of its own.
static void mouse_xbutton(INPUT *in, UINT *n, DWORD flags, DWORD button)
{
    in[*n].type = INPUT_MOUSE;
    in[*n].mi.dwFlags = flags;
    in[*n].mi.mouseData = button;
    (*n)++;
}

// A screen that has gone off comes back on for the input put into it, the way
// it does for the keys and the mouse at the machine: a person driving it from
// the arrangement is looking for the screen to come up under the pointer.
// There are two ways a screen goes off, and one request for each: the display
// idle timer, which asking for the display resets, and SLEEP's own
// SC_MONITORPOWER off, which the -1 undoes. The second is a broadcast, so it
// goes out when input starts again rather than for every step of the mouse.
//
// A broadcast is answered by every window on the machine in turn, and one that
// is busy or hung answers late. Waited on here, that is the pointer standing
// still for as long as the message takes to give up, on the same thread that
// carries every other event: on a machine with such a window every move is a
// move every second or two and everything behind it waits too. So the request
// that cannot wait - the display idle timer's - is made here, and the
// broadcast is handed to a thread of its own that the pointer never waits for.
static HANDLE g_wake_go;                // a broadcast is wanted
static volatile LONG g_wake_up;         // ... and the thread that reads it is up

// The broadcast, on a thread that carries nothing else, so a window that will
// not answer holds up nothing but the next wake.
static DWORD WINAPI wake_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (WaitForSingleObject(g_wake_go, INFINITE) != WAIT_OBJECT_0) return 0;
        SendMessageTimeout(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER,
                           (LPARAM)-1, SMTO_ABORTIFHUNG, 2000, NULL);
    }
}

// The event and its thread, made once: the event loop and the arrangement's
// own move socket both ask for a wake, and the first of them through makes
// them. 0 when there is no thread, so the caller takes the broadcast itself.
static int wake_start(void)
{
    HANDLE go, thread;

    if (g_wake_go) return g_wake_up != 0;
    go = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!go) return 0;
    if (InterlockedCompareExchangePointer((PVOID *)&g_wake_go, go, NULL)) {
        CloseHandle(go);                // another thread made it first
        return g_wake_up != 0;
    }
    thread = CreateThread(NULL, 0, wake_thread, NULL, 0, NULL);
    if (!thread) {
        g_wake_go = NULL;
        CloseHandle(go);
        return 0;
    }
    CloseHandle(thread);
    g_wake_up = 1;
    return 1;
}

static void wake_display(void)
{
    static DWORD woke;
    DWORD now = GetTickCount();

    SetThreadExecutionState(ES_DISPLAY_REQUIRED);
    if (woke && (DWORD)(now - woke) < 2000) return;
    woke = now;
    if (!wake_start()) {
        SendMessageTimeout(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER,
                           (LPARAM)-1, SMTO_ABORTIFHUNG, 2000, NULL);
        return;
    }
    SetEvent(g_wake_go);
}

// One move of this session's pointer. The buttons are a mask: 1 a move, then
// 2/4 left, 8/16 right and 32/64 middle, the low half of each pair meaning
// pressed. A mouse with back and forward carries it on: 128/256 back and
// 512/1024 forward, the same way round. Coordinates are the window's own, over
// this session's virtual desktop. This is the whole of an INPUT M wherever it
// came from: the server's line below, or a datagram the window sent straight
// here, which is why it says why rather than answering over the connection.
int mirror_move(int px, int py, int buttons, char *why, int whysz)
{
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int ox = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int oy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    INPUT in[6];
    UINT n = 0;
    double x, y;
    DWORD flags;
    DWORD pressed = 0;

    if (vw < 2 || vh < 2) {
        copy_str(why, whysz, "no mouse to move");
        return 0;
    }
    // SendInput wants the whole screen as 0..65535, which is what this turns a
    // pixel into.
    x = (double)(px - ox) * 65535.0 / (vw - 1);
    y = (double)(py - oy) * 65535.0 / (vh - 1);
    ZeroMemory(&in, sizeof in);
    flags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    in[n].type = INPUT_MOUSE;
    in[n].mi.dx = (LONG)(x + 0.5);
    in[n].mi.dy = (LONG)(y + 0.5);
    if (in[n].mi.dx < 0) in[n].mi.dx = 0;
    if (in[n].mi.dy < 0) in[n].mi.dy = 0;
    if (in[n].mi.dx > 65535) in[n].mi.dx = 65535;
    if (in[n].mi.dy > 65535) in[n].mi.dy = 65535;
    in[n].mi.dwFlags = flags;
    n++;

    // The move is an event of its own so the button is always applied after
    // the pointer has reached its destination, including on the secure desktop.
    if (buttons & 2) pressed |= MOUSEEVENTF_LEFTDOWN;
    if (buttons & 4) pressed |= MOUSEEVENTF_LEFTUP;
    if (buttons & 8) pressed |= MOUSEEVENTF_RIGHTDOWN;
    if (buttons & 16) pressed |= MOUSEEVENTF_RIGHTUP;
    if (buttons & 32) pressed |= MOUSEEVENTF_MIDDLEDOWN;
    if (buttons & 64) pressed |= MOUSEEVENTF_MIDDLEUP;
    if (pressed) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = pressed;
        n++;
    }
    if (buttons & 128) mouse_xbutton(in, &n, MOUSEEVENTF_XDOWN, XBUTTON1);
    if (buttons & 256) mouse_xbutton(in, &n, MOUSEEVENTF_XUP, XBUTTON1);
    if (buttons & 512) mouse_xbutton(in, &n, MOUSEEVENTF_XDOWN, XBUTTON2);
    if (buttons & 1024) mouse_xbutton(in, &n, MOUSEEVENTF_XUP, XBUTTON2);

    // The screen is there to be looked at before the first event of it lands,
    // so the wake goes ahead of the input rather than after it.
    wake_display();
    return input_take(in, n, why, whysz);
}

// INPUT M <x> <y> <buttons> | INPUT W <delta> | INPUT K <vk> <down|up>.
void mirror_input(const char *args)
{
    char work[128];
    char *words[8];
    INPUT in[5];
    UINT n = 0;
    int count;

    copy_str(work, sizeof work, args);
    count = line_split(work, words, 8);
    ZeroMemory(&in, sizeof in);

    if (count >= 4 && _stricmp(words[0], "M") == 0) {
        char why[80];

        if (!mirror_move(atoi(words[1]), atoi(words[2]), atoi(words[3]),
                         why, sizeof why)) {
            line_send(g_sock, NULL, "FAIL INPUT %s", why);
            return;
        }
        line_send(g_sock, NULL, "DONE INPUT");
        return;
    } else if (count >= 2 && _stricmp(words[0], "W") == 0) {
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[0].mi.mouseData = (DWORD)(LONG)atoi(words[1]);
        n = 1;
    } else if (count >= 3 && _stricmp(words[0], "K") == 0) {
        int vk = atoi(words[1]);
        UINT scan;

        if (vk <= 0 || vk > 255) {
            line_send(g_sock, NULL, "FAIL INPUT that is not a key");
            return;
        }
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.dwFlags = atoi(words[2]) ? 0 : KEYEVENTF_KEYUP;
        if (key_is_extended(vk)) in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        // Games commonly read the keyboard by scan code. A virtual key alone
        // is not enough for them, even though ordinary windows accept it.
        scan = MapVirtualKeyA((UINT)vk, 0);     // MAPVK_VK_TO_VSC
        if (scan) {
            in[0].ki.wScan = (WORD)scan;
            in[0].ki.dwFlags |= KEYEVENTF_SCANCODE;
        } else {
            in[0].ki.wVk = (WORD)vk;
        }
        n = 1;
    } else {
        line_send(g_sock, NULL, "FAIL INPUT M x y buttons, W delta or K vk down");
        return;
    }

    // The screen is there to be looked at before the first event of it lands,
    // so the wake goes ahead of the input rather than after it.
    wake_display();
    {
        char why[80];

        if (!input_take(in, n, why, sizeof why)) {
            line_send(g_sock, NULL, "FAIL INPUT %s", why);
            return;
        }
    }
    line_send(g_sock, NULL, "DONE INPUT");
}

// SCREENINFO. The size of the virtual desktop, and nothing else. It lets the
// window arrange devices by the shape of their screens without needing any
// additional screen data.
void mirror_screeninfo(void)
{
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (w <= 0 || h <= 0) {
        line_send(g_sock, NULL, "FAIL SCREENINFO no screen to read");
        return;
    }
    line_send(g_sock, NULL, "DONE SCREENINFO %d %d", w, h);
}

// The arrangement has just prepared text to paste, or is taking what is there
// as what it knows is here: remember where the sequence number stands, so a
// change from here on is this session's own copy.
static void text_paste_note(void)
{
    g_paste_seq = GetClipboardSequenceNumber();
    g_paste_seq_set = 1;
}

// Whether this session has copied something of its own since the arrangement
// last prepared text or looked at it.
static int text_paste_moved(void)
{
    return g_paste_seq_set && GetClipboardSequenceNumber() != g_paste_seq;
}

// CLIPSEQ. Whether this session's text to paste has moved on from what the
// arrangement last prepared or looked at, as one word and nothing else: a cheap
// question a console asks now and then so that a copy made here is noticed
// without reading the text, and without the reading itself looking like a copy
// of the arrangement's making.
void mirror_clipseq(void)
{
    line_send(g_sock, NULL, "DONE CLIPSEQ %s",
              text_paste_moved() ? "new" : "same");
}

// CLIPGET [wait]. What this session has to paste, as one line of base64, and
// whether this session copied something of its own since the arrangement last
// prepared or looked at the text - "new" or "same" before the text. A look
// leaves the text as what the arrangement now knows is here. The read behind a
// Ctrl+C does not: "wait" gives the chord a moment to arrive, and what it read
// is about to become this PC's, so a paste that races ahead of it must still
// see this session's copy as its own and leave it.
void mirror_clipget(const char *args)
{
    char text[TEXT_PASTE_MAX + 1];
    char b64[TEXT_PASTE_MAX * 4 / 3 + 8];
    int wait = args && strstr(args, "wait");
    int moved, n;

    if (wait) Sleep(120);
    moved = text_paste_moved();
    n = text_paste_get(text, sizeof text);
    if (n < 0) {
        line_send(g_sock, NULL, "FAIL CLIPGET more text than a copy carries");
        return;
    }
    if (n == 0) {
        if (!wait) text_paste_note();
        line_send(g_sock, NULL, "FAIL CLIPGET nothing to paste");
        return;
    }
    if (!wait) text_paste_note();
    b64_encode((const unsigned char *)text, n, b64, sizeof b64);
    line_send(g_sock, NULL, "DONE CLIPGET %s %s", moved ? "new" : "same", b64);
}

// CLIPSET <base64>. The other half: the text a Ctrl+V on the arrangement brings
// with it becomes this session's text to paste, and the key that follows pastes
// it wherever the pointer is.
void mirror_clipset(const char *args)
{
    char text[TEXT_PASTE_MAX + 1];
    int n = b64_decode(args, (unsigned char *)text, sizeof text - 1);

    text[n] = 0;
    if (!text_paste_put(text)) {
        line_send(g_sock, NULL, "FAIL CLIPSET text pasting would not take it");
        return;
    }
    text_paste_note();
    line_send(g_sock, NULL, "DONE CLIPSET");
}

// CLIPPASTE <base64>. The text a Ctrl+V on the arrangement carries. It becomes
// this session's text to paste only while this session has not copied something
// of its own since the arrangement last prepared or looked at it: that copy is
// newer, it is what the paste is for, and it is left in place for the
// arrangement to read when the pointer leaves.
void mirror_clippaste(const char *args)
{
    char text[TEXT_PASTE_MAX + 1];
    int n = b64_decode(args, (unsigned char *)text, sizeof text - 1);

    text[n] = 0;
    if (text_paste_moved()) {
        line_send(g_sock, NULL, "DONE CLIPPASTE");
        return;
    }
    if (!text_paste_put(text)) {
        line_send(g_sock, NULL, "FAIL CLIPPASTE text pasting would not take it");
        return;
    }
    text_paste_note();
    line_send(g_sock, NULL, "DONE CLIPPASTE");
}

// The window classes that take a Ctrl+V as their own key rather than as a
// paste: a console host hands the keyboard to the program inside it, so what
// arrives there is that program's own Ctrl+V. The same three the speak program
// names, so a transcript lands the same way at either end.
static int front_is_console(HWND front)
{
    static const char *console_classes[] = {
        "ConsoleWindowClass",            // conhost and its hosted programs
        "CASCADIA_HOSTING_WINDOW_CLASS", // Windows Terminal
        "mintty"                         // Git Bash / MSYS2
    };
    char cls[128];
    int i;

    if (!front || !GetClassNameA(front, cls, sizeof cls)) return 0;
    for (i = 0; i < (int)(sizeof console_classes / sizeof console_classes[0]); i++) {
        if (_stricmp(cls, console_classes[i]) == 0) return 1;
    }
    return 0;
}

// Tibia's client reads the keyboard as keys rather than as text, and the
// version here does not accept pasted text at all. Its title is the stable part
// of the window a person sees: "Tibia Version 7.6".
static int front_is_tibia(HWND front)
{
    char title[256];

    if (!front || !GetWindowTextA(front, title, sizeof title)) return 0;
    return _strnicmp(title, "Tibia", 5) == 0 &&
           (title[5] == 0 || title[5] == ' ' || title[5] == '-');
}

// The text into the session one character at a time, the way the program in
// the window takes it typed. A newline would submit a half-finished command
// line, so the ones inside a transcript are flattened into spaces. 0 = the
// machine would not take even the first character.
static int type_text(const char *text)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide;
    INPUT in[2];
    int i, typed = 0;

    if (wlen <= 0) return 0;
    wide = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wide) return 0;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wlen);
    for (i = 0; i < wlen - 1; i++) {
        wchar_t ch = wide[i];

        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (ch < 0x20 || ch == 0x7f) continue;
        ZeroMemory(in, sizeof in);
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = ch;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        if (SendInput(2, in, sizeof in[0]) != 2) break;
        typed++;
        Sleep(2);
    }
    free(wide);
    return typed;
}

// One key as the keyboard reports it, using the scan code games commonly read
// where there is one and the virtual key ordinary windows use otherwise. The
// endpoint's INPUT path already uses scan codes for the same reason.
static int type_tibia_key(UINT vk, int down)
{
    INPUT in;
    UINT scan = MapVirtualKeyA(vk, 0);

    ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    if (scan) {
        in.ki.wScan = (WORD)scan;
        in.ki.dwFlags = KEYEVENTF_SCANCODE;
    } else {
        in.ki.wVk = (WORD)vk;
    }
    if (key_is_extended((int)vk)) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) in.ki.dwFlags |= KEYEVENTF_KEYUP;
    return SendInput(1, &in, sizeof in) == 1;
}

// Tibia 7.6 predates Unicode text input, and reads typed keys itself. Common
// punctuation that speech recognition emits is folded to the key a person
// would have typed; anything else the keyboard layout cannot produce becomes
// a question mark instead of silently disappearing.
static int type_tibia_text(const char *text, HWND front)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    wchar_t *wide;
    int i, typed = 0;

    if (wlen <= 0) return 0;
    wide = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wide) return 0;
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wlen);
    for (i = 0; i < wlen - 1; i++) {
        wchar_t ch = wide[i];
        SHORT mapped;
        UINT vk;
        BYTE mods;
        int shift, caps;

        if (GetForegroundWindow() != front) break;
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (ch == L'\x2018' || ch == L'\x2019') ch = L'\'';
        else if (ch == L'\x201c' || ch == L'\x201d') ch = L'"';
        else if (ch == L'\x2013' || ch == L'\x2014') ch = L'-';
        else if (ch == L'\x2026') ch = L'.';
        if (ch < 0x20 || ch == 0x7f) continue;

        mapped = VkKeyScanW(ch);
        if (mapped == -1) mapped = VkKeyScanW(L'?');
        if (mapped == -1) continue;
        vk = (UINT)(mapped & 0xff);
        mods = (BYTE)((mapped >> 8) & 0xff);
        // Ctrl and Alt combinations are not ordinary typed characters and
        // could trigger a game command instead of entering text.
        if (mods & 6) {
            mapped = VkKeyScanW(L'?');
            if (mapped == -1) continue;
            vk = (UINT)(mapped & 0xff);
            mods = (BYTE)((mapped >> 8) & 0xff);
        }
        shift = (mods & 1) != 0;
        caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
        if (vk >= 'A' && vk <= 'Z')
            shift = (ch >= L'A' && ch <= L'Z') ? !caps : caps;

        if (shift && !type_tibia_key(VK_SHIFT, 1)) break;
        if (!type_tibia_key(vk, 1)) {
            if (shift) type_tibia_key(VK_SHIFT, 0);
            break;
        }
        Sleep(6);
        if (!type_tibia_key(vk, 0)) {
            if (shift) type_tibia_key(VK_SHIFT, 0);
            break;
        }
        if (shift && !type_tibia_key(VK_SHIFT, 0)) break;
        typed++;
        Sleep(6);
    }
    free(wide);
    return typed;
}

// INSERT <base64>. A finished transcript, put into this session the way paste
// would land: prepared as text to paste, and then put into the window in front
// of the session - typed into a console host, which binds Ctrl+V itself and
// would take that key as the program's own, typed as scan-code keys into Tibia,
// which does not accept pasted text, and pasted with a Ctrl+V anywhere else.
// The arrangement cannot see which window this session has in front of it, and
// this end can, so the choice is made here.
void mirror_insert(const char *args)
{
    char text[TEXT_PASTE_MAX + 1];
    int n = b64_decode(args, (unsigned char *)text, sizeof text - 1);
    HWND front = GetForegroundWindow();

    text[n] = 0;
    if (n <= 0) {
        line_send(g_sock, NULL, "FAIL INSERT nothing to put in");
        return;
    }
    if (!text_paste_put(text)) {
        line_send(g_sock, NULL, "FAIL INSERT text pasting would not take it");
        return;
    }
    text_paste_note();
    // The screen is there to be looked at before the first event of it lands,
    // the way it is for the keys and the mouse.
    wake_display();
    if (front_is_console(front)) {
        if (type_text(text) <= 0) {
            line_send(g_sock, NULL, "FAIL INSERT the machine would not take it");
            return;
        }
    } else if (front_is_tibia(front)) {
        if (type_tibia_text(text, front) <= 0) {
            line_send(g_sock, NULL, "FAIL INSERT the machine would not take it");
            return;
        }
    } else {
        INPUT in[4];

        ZeroMemory(in, sizeof in);
        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_CONTROL;
        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'V';
        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'V';
        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_CONTROL;
        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
        if (SendInput(4, in, sizeof in[0]) != 4) {
            line_send(g_sock, NULL, "FAIL INSERT the machine would not take it");
            return;
        }
    }
    line_send(g_sock, NULL, "DONE INSERT");
}

// The connection Input Director's clicks and text pasting ride.
void mirror_attach(SOCKET sock)
{
    g_sock = sock;
    // The text this session starts with is what the arrangement has not seen,
    // so the first look at it takes the sequence number from where it already
    // stands rather than from an empty one: a copy made before the arrangement
    // ever asked is still a copy.
    if (!g_paste_seq_set) text_paste_note();
}

// That connection is gone: no clicks or paste traffic to carry.
void mirror_detach(void)
{
    g_sock = INVALID_SOCKET;
}
