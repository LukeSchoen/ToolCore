// tool.exe - endpoint.
//
// One copy per device. It dials the server, proves it holds the same token,
// then answers events until it is stopped with Ctrl+C. Every line it prints
// also goes to client.log next to the exe, so the console can be closed and
// the record is still there.
//
//   tool.exe [--server <host>] [--port <n>]
//   tool.exe -run | -toast     daemon (no window) mode
//   tool.exe --message <text>  draws a simple toast message box
//
// This is ToolCore. It offers a local connection and the controls a person
// could operate only while they were near the machine (no screen mirroring)
//
// The events it answers:
//
//   INPUT M/W/K        mouse, wheel and keyboard input
//   SCREENINFO         screen dimensions
//   Copy Paste         text pasting
//   BEEP [freq] [ms]   pc beep
//   MUTE <on|off|toggle>
//   VOL <0-100>        master volume, unmuting above zero
//   DIM <0-100>        screen brightness
//   DIMENTER / DIMLEAVE
//                      screen brightness to 100% and the pointer kept showing,
//                      then the brightness back again
//   SLEEP              screen off
//   SAY <text>         a message to this machine's screen
//   SHUTDOWN / RESTART / CLOSE
//                      local machine basic controls
//   GETVOL / GETBRIGHTNESS
//                      Basic Setting Readings
//   GETBUILD           this exe's size and SHA-256
//   RENAME <name>      rename the Windows computer
#include "common.h"
#include "audio.h"
#include "mirror.h"
#include "tool_features.h"
#include <stdarg.h>

// Screen brightness lives in the active power scheme's "Display brightness"
// setting. The subgroup has a friendly alias (SUB_VIDEO) but the setting's
// alias, VIDEONORMALLEVEL, is not in every build's alias table, so the GUID is
// spelled out.
#define VIDEO_SUBGROUP_GUID "SUB_VIDEO"
#define BRIGHTNESS_GUID     "aded5e82-b909-4619-9949-f5d71dac0bcb"

static char g_name[64];
static char g_server[128];
static int g_port = TOOL_DEFAULT_PORT;
static char g_token[256];
static SOCKET g_sock = INVALID_SOCKET;
static CRITICAL_SECTION g_sock_lock;

// A device with more than one route to a server keeps them in servers.txt
// beside the exe, in the order they should be tried. The address here is the
// one the current attempt is using; everything about the live connection -
// its UDP registration and extension state - follows that.
#define SERVER_LIST_MAX 32
#define SERVER_CONNECT_MS 2500
#define SERVER_PING_MS 10000
#define SERVER_SILENCE_MS 30000
#define SERVER_IO_MS 10000

typedef struct {
    char host[128];
    int port;
} server_target;

static server_target g_servers[SERVER_LIST_MAX];
static int g_server_count;
static int g_server_at;
static int g_server_explicit;
static int g_port_explicit;
static char g_servers_problem[200];

// RENAME sets this: a connection cannot change the name it registered with, so
// the loop drops it and comes straight back under the new one. See do_rename.
static int g_reconnect;

static void log_line(const char *fmt, ...);

static void do_event(const char *event, lines_t *lr);

// Every log line also lands in client.log next to the exe, so the console can
// be closed and the record is still there afterwards. The supervisor starts a
// fresh file, and its child keeps appending after a restart.
static int g_log_file_warned;
static int g_log_append;

static void log_path(char *path, int size)
{
    exe_dir(path, size);
    strncat(path, "client.log", (size_t)(size - (int)strlen(path) - 1));
}

static void log_file_start(void)
{
    char path[MAX_PATH];
    FILE *f;

    log_path(path, sizeof path);
    f = fopen(path, "w");
    if (!f) {
        if (!g_log_file_warned) {
            g_log_file_warned = 1;
            printf("(cannot open %s - this log is in the console only)\n", path);
            fflush(stdout);
        }
        return;
    }
    fclose(f);
}

static void log_to_file(const char *line)
{
    char path[MAX_PATH];
    FILE *f;

    log_path(path, sizeof path);
    f = fopen(path, "a");
    if (!f) {
        if (!g_log_file_warned) {
            g_log_file_warned = 1;
            printf("(cannot open %s - this log is in the console only)\n", path);
            fflush(stdout);
        }
        return;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
}

static void log_line(const char *fmt, ...)
{
    SYSTEMTIME st;
    char text[1024];
    char line[1200];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(text, sizeof text - 1, fmt, ap);
    va_end(ap);
    text[sizeof text - 1] = 0;

    GetLocalTime(&st);
    format_text(line, sizeof line, "%02u:%02u:%02u  %s",
                st.wHour, st.wMinute, st.wSecond, text);
    if (GetConsoleWindow()) {
        printf("%s\n", line);
        fflush(stdout);
    }
    log_to_file(line);
}

void tool_core_log(const char *fmt, ...)
{
    char text[1024];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(text, sizeof text - 1, fmt, ap);
    va_end(ap);
    text[sizeof text - 1] = 0;
    log_line("%s", text);
}

int tool_core_send(const char *fmt, ...)
{
    char text[TOOL_MAXLINE];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(text, sizeof text - 1, fmt, ap);
    va_end(ap);
    text[sizeof text - 1] = 0;
    return line_send(g_sock, NULL, "%s", text);
}

SOCKET tool_core_socket(void)
{
    return g_sock;
}

const char *tool_core_server(void)
{
    return g_server;
}

int tool_core_server_port(void)
{
    return g_port;
}

// ------------------------------------------------------------------- the toast
//
// A message for whoever is standing at the device, drawn by this program
// rather than asked of Windows. A shell notification is at the mercy of
// notification settings, focus assist and full-screen apps - it is a call that
// can succeed and show nothing at all - and a message nobody sees is worse
// than one never sent. This is a window like any other: the night colours the
// console uses, in the corner, up for a few seconds, and it never takes the
// focus, so a device that is in the middle of something is not interrupted.
// Only one is up at a time: a message takes the corner from the box before it
// rather than stacking on top of it, so a burst of them leaves the newest.

#define TOAST_CLASS  "ToolsToast"
#define TOAST_MS     5000
#define TOAST_PAD    14
#define TOAST_MARGIN 16
#define TOAST_MAXW   420
#define TOAST_MAXH   320

// The words wrap to the box's own width and every line sits in the middle of
// it, so the short last line of a wrapped message is not left hanging on one
// side. The measuring and all of the painting below draw with this one set of
// flags, which is what keeps the window the size the words were measured at.
#define TOAST_WORDS  (DT_WORDBREAK | DT_NOPREFIX | DT_CENTER)

// The box: a face lit a little from the top, a one-pixel edge that is a
// different shade on each of its four sides, and a second such ring inside
// the first, so the raised look has depth instead of reading as one flat
// outline. The words are brighter than all of it, wrapped in a soft halo and
// dropped a shadow, which is what keeps them legible over whatever is behind.
#define TOAST_FACE    RGB(21, 24, 32)      // the top of the face
#define TOAST_BACK    RGB(9, 11, 16)       // ...and its bottom
#define TOAST_EDGE_HI RGB(78, 86, 102)     // outer bevel, the top
#define TOAST_EDGE_LT RGB(70, 77, 91)      // outer bevel, the left
#define TOAST_EDGE_RT RGB(60, 66, 78)      // outer bevel, the right
#define TOAST_EDGE_LO RGB(52, 57, 68)      // outer bevel, the bottom
#define TOAST_IN_HI   RGB(44, 49, 60)      // inner bevel, the top
#define TOAST_IN_LT   RGB(40, 44, 54)      // inner bevel, the left
#define TOAST_IN_RT   RGB(33, 37, 45)      // inner bevel, the right
#define TOAST_IN_LO   RGB(28, 31, 38)      // inner bevel, the bottom
#define TOAST_TEXT    RGB(242, 246, 252)   // the words
#define TOAST_GLOW    RGB(92, 103, 124)    // the halo around them
#define TOAST_SHADOW  RGB(0, 0, 0)         // and the shadow under them

static HFONT g_toast_font;
static HFONT g_toast_edge_font;

// One box's words, and whether it is still on the screen. The thread that made
// the window keeps this record on its own stack and runs a message loop until
// the box is gone or its few seconds are up; a window is only ever talked to
// by the thread that made it, so the pointer stays good for the box's life.
typedef struct toast {
    char *text;
    int gone;
} toast;

// How many pixels this screen has to the inch, as the program is being told
// about them; see dpi_aware, which is what makes that the screen's own answer
// rather than the one Windows gives a program that has not asked.
static int screen_dpi(void)
{
    HDC screen = GetDC(NULL);
    int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSY) : 96;

    if (screen) ReleaseDC(NULL, screen);
    return dpi > 0 ? dpi : 96;
}

// The toast is laid out in pixels, and its font already grows with the screen
// - so the room around the words has to grow with it too, or a machine with
// its display scaled up gets a box whose padding has stayed behind.
static int toast_px(int px)
{
    return MulDiv(px, screen_dpi(), 96);
}

static HFONT toast_make_font(DWORD quality)
{
    int height = -MulDiv(12, screen_dpi(), 72);

    if (height > -16) height = -16;
    return CreateFontA(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, quality,
                       DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

// The words are subpixel-antialiased, the sharpest the screen can draw, and
// their halo and shadow are grayscale: the subpixel fringing of the first is
// a tint nobody wants around every edge of every letter.
static HFONT toast_font(void)
{
    if (!g_toast_font) {
        g_toast_font = toast_make_font(CLEARTYPE_QUALITY);
        if (!g_toast_font) g_toast_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return g_toast_font;
}

static HFONT toast_edge_font(void)
{
    if (!g_toast_edge_font) {
        g_toast_edge_font = toast_make_font(ANTIALIASED_QUALITY);
        if (!g_toast_edge_font) g_toast_edge_font = toast_font();
    }
    return g_toast_edge_font;
}

// How much room the words need, at the widest the box is allowed to be. The
// painting below asks the same question of the same font, so the window cannot
// come out a different size from the message it holds.
static void toast_measure(HDC dc, const char *text, RECT *out)
{
    HGDIOBJ old = SelectObject(dc, toast_font());

    out->left = 0;
    out->top = 0;
    out->right = toast_px(TOAST_MAXW);
    out->bottom = toast_px(TOAST_MAXH);
    DrawTextA(dc, text, -1, out, DT_CALCRECT | TOAST_WORDS);
    SelectObject(dc, old);
}

static BOOL CALLBACK toast_close_proc(HWND w, LPARAM lp)
{
    char cls[64];

    (void)lp;
    if (GetClassNameA(w, cls, sizeof cls) && strcmp(cls, TOAST_CLASS) == 0)
        PostMessageA(w, WM_CLOSE, 0, 0);
    return TRUE;
}

// Whatever box is up, wherever it came from: a message shown by the endpoint
// and one shown by tool.exe --message are different processes, so the search
// is the window manager's rather than a variable of ours.
static void toast_hide_previous(void)
{
    EnumWindows(toast_close_proc, 0);
}

static void toast_fill(HDC dc, const RECT *rc, COLORREF colour)
{
    HBRUSH brush = CreateSolidBrush(colour);

    FillRect(dc, rc, brush);
    DeleteObject(brush);
}

// A colour a fraction of the way from one to another, t running 0 to 255.
static COLORREF toast_mix(COLORREF from, COLORREF to, int t)
{
    int r = GetRValue(from) + (GetRValue(to) - GetRValue(from)) * t / 255;
    int g = GetGValue(from) + (GetGValue(to) - GetGValue(from)) * t / 255;
    int b = GetBValue(from) + (GetBValue(to) - GetBValue(from)) * t / 255;

    return RGB(r, g, b);
}

// The face, a scanline at a time, darker towards the bottom - the one thing
// that says the box is lit from above.
static void toast_face(HDC dc, const RECT *rc)
{
    int y, h = rc->bottom - rc->top;

    for (y = 0; y < h; y++) {
        RECT row = *rc;

        row.top = rc->top + y;
        row.bottom = row.top + 1;
        toast_fill(dc, &row,
                   toast_mix(TOAST_FACE, TOAST_BACK,
                             h > 1 ? y * 255 / (h - 1) : 0));
    }
}

// One pixel around the box, a colour of its own on each side: a light top and
// left against a dark right and bottom read as a raised bevel.
static void toast_bevel(HDC dc, const RECT *rc,
                        COLORREF top, COLORREF left,
                        COLORREF right, COLORREF bottom)
{
    RECT line;

    line = *rc;
    line.bottom = line.top + 1;
    toast_fill(dc, &line, top);
    line = *rc;
    line.right = line.left + 1;
    toast_fill(dc, &line, left);
    line = *rc;
    line.left = line.right - 1;
    toast_fill(dc, &line, right);
    line = *rc;
    line.top = line.bottom - 1;
    toast_fill(dc, &line, bottom);
}

// The words once, shifted by (dx, dy) and in one colour. The shift is the
// viewport's rather than the rectangle's, so every pass wraps exactly as the
// measuring did and the passes land on top of one another.
static void toast_text_at(HDC dc, HFONT font, const char *text, RECT *body,
                          int dx, int dy, COLORREF colour)
{
    HGDIOBJ was = SelectObject(dc, font);
    POINT old;

    SetViewportOrgEx(dc, dx, dy, &old);
    SetTextColor(dc, colour);
    DrawTextA(dc, text, -1, body, TOAST_WORDS);
    SetViewportOrgEx(dc, old.x, old.y, NULL);
    SelectObject(dc, was);
}

static LRESULT CALLBACK toast_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)lp;

        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        break;
    }
    case WM_DESTROY: {
        toast *t = (toast *)GetWindowLongPtrA(h, GWLP_USERDATA);

        if (t) t->gone = 1;
        break;
    }
    case WM_NCDESTROY: {
        toast *t = (toast *)GetWindowLongPtrA(h, GWLP_USERDATA);

        if (t) free(t->text);
        SetWindowLongPtrA(h, GWLP_USERDATA, 0);
        break;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, inner, body;
        int step;
        const toast *t = (const toast *)GetWindowLongPtrA(h, GWLP_USERDATA);

        GetClientRect(h, &rc);

        // The face, then its two bevelled rings: the first is the box's own
        // edge, the second sits inside it, and the two together give the edge
        // the depth a single outline does not have.
        toast_face(dc, &rc);
        toast_bevel(dc, &rc, TOAST_EDGE_HI, TOAST_EDGE_LT,
                    TOAST_EDGE_RT, TOAST_EDGE_LO);
        inner = rc;
        InflateRect(&inner, -1, -1);
        if (inner.right > inner.left && inner.bottom > inner.top)
            toast_bevel(dc, &inner, TOAST_IN_HI, TOAST_IN_LT,
                        TOAST_IN_RT, TOAST_IN_LO);

        if (t && t->text) {
            HFONT halo = toast_edge_font();

            step = toast_px(1);
            SetBkMode(dc, TRANSPARENT);
            body = rc;
            // The same padding the box was measured and sized with, at this
            // screen's scale, so the words sit as far in on one side as the
            // other whatever the display is running at.
            InflateRect(&body, -toast_px(TOAST_PAD), -toast_px(TOAST_PAD));

            // A shadow down and to the right, a halo all the way round, and
            // the bright words on top of both: the shadow lifts them off the
            // face, the halo holds them clear of it, and the last pass is
            // what is read.
            toast_text_at(dc, halo, t->text, &body, step, step * 2, TOAST_SHADOW);
            toast_text_at(dc, halo, t->text, &body, -step, -step, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, step, -step, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, -step, step, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, step, step, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, 0, -step, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, -step, 0, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, step, 0, TOAST_GLOW);
            toast_text_at(dc, halo, t->text, &body, 0, step, TOAST_GLOW);
            toast_text_at(dc, toast_font(), t->text, &body, 0, 0, TOAST_TEXT);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// One message, on the screen, until its few seconds are up. 0 when there is no
// screen to put it on.
static int toast_run(const char *text)
{
    WNDCLASSA wc;
    RECT work, box;
    HDC screen;
    HWND h;
    toast t;
    DWORD end;
    int width, height;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = toast_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassA(&wc);        // only ever fails as already registered

    screen = GetDC(NULL);
    if (!screen) return 0;
    toast_measure(screen, text, &box);
    ReleaseDC(NULL, screen);
    width = box.right - box.left + toast_px(TOAST_PAD) * 2;
    height = box.bottom - box.top + toast_px(TOAST_PAD) * 2;

    // The corner of the working area, so a taskbar is never covered.
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    t.text = (char *)malloc(strlen(text) + 1);
    if (!t.text) return 0;
    copy_str(t.text, (int)strlen(text) + 1, text);
    t.gone = 0;

    // The corner is the newest message's. The box still holding it is asked
    // to close, and goes as its own thread picks the request up.
    toast_hide_previous();

    h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                        TOAST_CLASS, text, WS_POPUP,
                        work.right - width - toast_px(TOAST_MARGIN),
                        work.bottom - height - toast_px(TOAST_MARGIN),
                        width, height, NULL, NULL, wc.hInstance, &t);
    if (!h) {
        free(t.text);
        return 0;
    }

    ShowWindow(h, SW_SHOWNOACTIVATE);
    UpdateWindow(h);

    end = GetTickCount() + TOAST_MS;
    while (!t.gone && (int)(end - GetTickCount()) > 0) {
        MSG msg;

        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(40);
    }
    if (!t.gone) DestroyWindow(h);
    return 1;
}

// The same on a thread of its own: an endpoint talking to the server has no
// message loop to lend, and must not stop answering while a message is up.
static DWORD WINAPI toast_thread(void *arg)
{
    toast_run((const char *)arg);
    free(arg);
    return 0;
}

static void toast_show(const char *text)
{
    char *words = (char *)malloc(strlen(text) + 1);
    HANDLE thread;

    if (!words) return;
    copy_str(words, (int)strlen(text) + 1, text);
    thread = CreateThread(NULL, 0, toast_thread, words, 0, NULL);
    if (!thread) {
        free(words);
        return;
    }
    CloseHandle(thread);
}

void tool_core_toast(const char *text)
{
    toast_show(text);
}

// SAY <text>: what a console asks a device to say. The box is drawn on a
// thread of its own, so the events behind this one are answered while the
// message is up.
static void do_say(const char *args)
{
    if (!*args) {
        line_send(g_sock, NULL, "FAIL SAY nothing to say");
        return;
    }
    toast_show(args);
    line_send(g_sock, NULL, "DONE SAY");
}

// The names the program starts from: the server list, the address one entry of
// it is using, and this computer's own name. --server and TOOL_SERVER name one
// explicit destination and take precedence over servers.txt. --port and
// TOOL_PORT change that destination's port; neither names a host by itself.
// Nothing overrides the computer name, which stops one machine turning up
// under a name of its own.
static void load_settings(void)
{
    DWORD n = sizeof g_name;
    const char *v;

    g_port = TOOL_DEFAULT_PORT;
    // The same two settings the window's boxes take, so one machine can be
    // pointed at a server of its own without a shortcut to change. The command
    // line wins over both.
    v = getenv("TOOL_SERVER");
    if (v && *v) {
        copy_str(g_server, sizeof g_server, v);
        g_server_explicit = 1;
    }
    v = getenv("TOOL_PORT");
    if (v && atoi(v) > 0 && atoi(v) <= 65535) {
        g_port = atoi(v);
        g_port_explicit = 1;
    }
    if (!GetComputerNameA(g_name, &n)) copy_str(g_name, sizeof g_name, "endpoint");
}

static char *trim_server_text(char *text)
{
    int n;

    while (*text == ' ' || *text == '\t') text++;
    n = (int)strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
                     text[n - 1] == '\r' || text[n - 1] == '\n'))
        text[--n] = 0;
    return text;
}

static int server_port_value(const char *text)
{
    int value = 0;
    int i;

    if (!*text) return 0;
    for (i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9') return 0;
        if (value > 6553) return 0;
        value = value * 10 + (text[i] - '0');
        if (value > 65535) return 0;
    }
    return value > 0 ? value : 0;
}

// One entry is "host [port]" or "host:port". The port is 7901 when omitted.
// An IPv6 address cannot be used by this endpoint's IPv4 socket, so a line
// with more than one colon is refused rather than half-read.
static int server_parse_line(char *line, server_target *target, int default_port)
{
    char *words[3];
    char *host;
    char *colon;
    int count;
    int port = default_port;

    count = line_split(line, words, 3);
    if (count < 1 || count > 2) return 0;

    host = words[0];
    colon = strrchr(host, ':');
    if (colon) {
        if (colon == host || !colon[1] || strchr(host, ':') != colon) return 0;
        port = server_port_value(colon + 1);
        if (!port || count > 1) return 0;
        *colon = 0;
    } else if (count == 2) {
        port = server_port_value(words[1]);
        if (!port) return 0;
    }
    if (!*host || strlen(host) >= sizeof target->host) return 0;

    copy_str(target->host, sizeof target->host, host);
    target->port = port;
    return 1;
}

static void servers_path(char *path, int size)
{
    exe_dir(path, size);
    strncat(path, "servers.txt", (size_t)(size - (int)strlen(path) - 1));
}

static int server_add(const char *host, int port)
{
    int i;

    for (i = 0; i < g_server_count; i++)
        if (g_servers[i].port == port &&
            _stricmp(g_servers[i].host, host) == 0)
            return 1;
    if (g_server_count >= SERVER_LIST_MAX) return 0;
    copy_str(g_servers[g_server_count].host,
             sizeof g_servers[g_server_count].host, host);
    g_servers[g_server_count].port = port;
    g_server_count++;
    return 1;
}

static int servers_read(void)
{
    char path[MAX_PATH];
    char line[256];
    server_target target;
    FILE *f;
    int bad = 0;
    int overflow = 0;

    servers_path(path, sizeof path);
    f = fopen(path, "r");
    if (!f) {
        format_text(g_servers_problem, sizeof g_servers_problem,
                    "cannot read %s - add a server address or use --server",
                    path);
        return 0;
    }

    while (fgets(line, sizeof line, f)) {
        char *text = line;
        char *comment;

        comment = strchr(text, '#');
        if (comment) *comment = 0;
        text = trim_server_text(text);
        if (!*text) continue;
        if (!server_parse_line(text, &target, TOOL_DEFAULT_PORT)) {
            bad++;
            continue;
        }
        if (!server_add(target.host, target.port)) overflow = 1;
    }
    fclose(f);

    if (overflow)
        copy_str(g_servers_problem, sizeof g_servers_problem,
                 "servers.txt has more than 32 servers - the rest were ignored");
    else if (!g_server_count && bad)
        format_text(g_servers_problem, sizeof g_servers_problem,
                    "%s has %d invalid line(s) and no server addresses",
                    path, bad);
    else if (!g_server_count)
        format_text(g_servers_problem, sizeof g_servers_problem,
                    "%s has no server addresses - add a host or use --server",
                    path);
    else if (bad)
        format_text(g_servers_problem, sizeof g_servers_problem,
                    "servers.txt has %d invalid line(s) - they were ignored", bad);
    return g_server_count;
}

static void servers_use(int index)
{
    if (!g_server_count) return;
    if (index < 0 || index >= g_server_count) index = 0;
    g_server_at = index;
    copy_str(g_server, sizeof g_server, g_servers[index].host);
    g_port = g_servers[index].port;
}

static int servers_load(void)
{
    g_server_count = 0;
    g_server_at = 0;
    g_servers_problem[0] = 0;

    if (g_port_explicit && !g_server_explicit) {
        copy_str(g_servers_problem, sizeof g_servers_problem,
                 "--port and TOOL_PORT require --server or TOOL_SERVER");
        return 0;
    }
    if (g_server_explicit) {
        if (!*g_server) {
            copy_str(g_servers_problem, sizeof g_servers_problem,
                     "--server and TOOL_SERVER require a host");
            return 0;
        }
        server_add(g_server, g_port);
    } else {
        g_server[0] = 0;
        servers_read();
    }
    if (g_server_count) servers_use(0);
    return g_server_count;
}

static int servers_failed(int no_window, int child_mode)
{
    const char *why = g_servers_problem[0] ? g_servers_problem
                                           : "no server is configured";

    if (!no_window) {
        printf("tool: %s\n", why);
    } else {
        if (child_mode) g_log_append = 1;
        else log_file_start();
        log_line("%s", why);
    }
    return 1;
}

// 1 means another entry is ready now; 0 means the list has come round to its
// first entry and the caller should wait before trying the cycle again.
static int servers_next(void)
{
    if (g_server_count > 1 && g_server_at + 1 < g_server_count) {
        servers_use(g_server_at + 1);
        return 1;
    }
    // Pick up an edit to servers.txt at the end of each pass without needing
    // the endpoint restarted.
    servers_load();
    return 0;
}

// Options are written with one dash or two, whichever the person typed.
static int is_opt(const char *arg, const char *name)
{
    if (arg[0] != '-') return 0;
    arg++;
    if (*arg == '-') arg++;
    return strcmp(arg, name) == 0;
}

// --message <text> and --message=<text>: the one option whose value is a
// sentence rather than a word. Everything after it on the command line is the
// message, so it needs no quotes at a prompt and survives an extension
// command. --message=<word> is the same thing with the first word run into the
// option, which is how one word goes when it has been quoted.
static int take_message(int argc, char **argv, int *i, char *out, int outsz)
{
    const char *arg = argv[*i];
    int k;

    if (arg[0] != '-') return 0;
    arg++;
    if (*arg == '-') arg++;
    if (_strnicmp(arg, "message", 7) != 0) return 0;
    if (arg[7] != 0 && arg[7] != '=') return 0;

    copy_str(out, outsz, arg[7] == '=' ? arg + 8 : "");
    for (k = *i + 1; k < argc; k++) {
        if ((int)strlen(out) + (int)strlen(argv[k]) + 2 > outsz) break;
        if (*out) strncat(out, " ", outsz - strlen(out) - 1);
        strncat(out, argv[k], outsz - strlen(out) - 1);
    }
    *i = argc;      // the rest of the line was the message
    return 1;
}

// ------------------------------------------------------------- the console
//
// The manager is the only part of this program meant to be looked at, so it is
// the only part that wants a console. One it is handed is used as it is; if the
// launcher gave it none, or gave it one it hid, this is what puts a window in
// front of somebody.

static int open_console(void)
{
    HWND con = GetConsoleWindow();

    if (!con) {
        if (!AllocConsole()) return 0;
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        SetConsoleTitleA("tools - endpoint manager");
        con = GetConsoleWindow();
    }
    if (con) {
        ShowWindow(con, SW_SHOW);
        SetForegroundWindow(con);
    }
    return 1;
}

// The endpoint has no console, and must not be attached to somebody's: closing
// a console kills everything attached to it, so a copy started from a command
// prompt would die with that prompt's window. Detaching it, first thing, is
// what makes closing the console the harmless thing it should be. A copy the
// manager started was created detached and has nothing to leave.
static void detach_console(void)
{
    if (GetConsoleWindow()) FreeConsole();
}

// ------------------------------------------- the running copy, and stopping

// Windows will happily run two endpoints, and then two of them share one name
// and one log. The copy that is running holds this mutex for as long as it
// lives, so anything else can ask whether an endpoint is up - that is what the
// manager's "running:" line is - without guessing from a process name.
//
// Local\ because an endpoint runs in the session that started it, which is the
// session a person typing tool.exe at that machine is in too.
#define TOOLS_SINGLE  "Local\\ToolsEndpointRunning"

// The parent that watches the endpoint and starts it again if it exits without
// being asked to. One supervisor is enough for one endpoint.
#define TOOLS_SUPERVISOR "Local\\ToolsEndpointSupervisor"

// How the manager stops an endpoint: set this, and the loop notices and leaves
// the way it would leave anything else - closing its socket and writing its
// log. A copy from a build older than this one has no such event, which is the
// one case where stopping means ending it the hard way; see stop_endpoint.
#define TOOLS_STOP    "Local\\ToolsEndpointStop"

// A program that does not say how it wants to be told about DPI is handed a
// virtualised desktop instead: Windows reports the screen as if it were 96 DPI
// and scales what it hands back. That is fine for a window somebody reads, but
// it would distort the dimensions and coordinates this endpoint reports.
//
// Nothing here is drawn by hand, so there is nothing for the answer to upset:
// the one window is the message, and its own sizes go through toast_px, which
// is the same DPI this sets. Called before anything with a screen in it.
static void dpi_aware(void)
{
    typedef BOOL (WINAPI *set_ctx_fn)(HANDLE);
    typedef BOOL (WINAPI *set_aware_fn)(void);
    typedef HRESULT (WINAPI *set_shcore_fn)(int);

    HMODULE user = GetModuleHandleA("user32.dll");
    HMODULE shcore;
    FARPROC fn;

    // Per monitor and the second version of it where there is one: a device
    // whose screens run at different scales is the case this is for.
    if (user) {
        fn = GetProcAddress(user, "SetProcessDpiAwarenessContext");
        if (fn && ((set_ctx_fn)fn)((HANDLE)(LONG_PTR)-4)) return;
        fn = GetProcAddress(user, "SetProcessDPIAware");
        if (fn && ((set_aware_fn)fn)()) return;
    }
    shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        fn = GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (fn) ((set_shcore_fn)fn)(2);         // PROCESS_PER_MONITOR_DPI_AWARE
        FreeLibrary(shcore);
    }
}

// 1 = this copy is the one. 0 = another copy is already running.
static int claim_single_instance(void)
{
    HANDLE held;

    SetLastError(ERROR_SUCCESS);
    held = CreateMutexA(NULL, FALSE, TOOLS_SINGLE);
    if (!held) return 0;
    (void)held;     // kept for the life of the process, on purpose
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

// The endpoint's own stop button, watched by the loop below.
static HANDLE g_stop_event;

static int stop_requested(void)
{
    return g_stop_event && WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0;
}

// Waiting that a stop can cut short. The endpoint backs off up to 30 seconds
// between dials, and a manager that set the stop event should not have to sit
// through the rest of one before the copy goes.
static int wait_or_stop(int ms)
{
    if (!g_stop_event) {
        Sleep((DWORD)ms);
        return 0;
    }
    return WaitForSingleObject(g_stop_event, (DWORD)ms) == WAIT_OBJECT_0;
}

// A blocked recv does not return because the loop behind it saw an event. The
// watcher owns the one operation that does wake those calls: shutting the
// connection down when the manager asks to stop. The lock keeps it from using
// a socket handle the main thread is closing and setting up for another dial.
static DWORD WINAPI stop_io_thread(void *arg)
{
    (void)arg;
    if (!g_stop_event) return 0;
    if (WaitForSingleObject(g_stop_event, INFINITE) != WAIT_OBJECT_0) return 0;

    EnterCriticalSection(&g_sock_lock);
    if (g_sock != INVALID_SOCKET) shutdown(g_sock, SD_BOTH);
    LeaveCriticalSection(&g_sock_lock);
    return 0;
}

static int stop_io_watch_start(void)
{
    HANDLE thread;

    if (!g_stop_event) return 0;
    thread = CreateThread(NULL, 0, stop_io_thread, NULL, 0, NULL);
    if (!thread) return 0;
    CloseHandle(thread);
    return 1;
}

static void endpoint_socket_set(SOCKET s)
{
    EnterCriticalSection(&g_sock_lock);
    g_sock = s;
    LeaveCriticalSection(&g_sock_lock);
}

static void endpoint_socket_close(void)
{
    SOCKET s;

    EnterCriticalSection(&g_sock_lock);
    s = g_sock;
    g_sock = INVALID_SOCKET;
    if (s != INVALID_SOCKET) closesocket(s);
    LeaveCriticalSection(&g_sock_lock);
}

// Whether an endpoint is running right now. The mutex object lives exactly as
// long as the process holding it does, so a handle that opens at all is the
// answer - no process list, no name matching, nothing to go stale.
static int endpoint_is_running(void)
{
    HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, TOOLS_SINGLE);

    if (!m) return 0;
    CloseHandle(m);
    return 1;
}

static void append_arg(char *out, int outsz, const char *arg)
{
    int quote = strchr(arg, ' ') != NULL || strchr(arg, '\t') != NULL;
    int len = (int)strlen(out);
    const char *p;

    strncat(out, " ", (size_t)(outsz - len - 1));
    len = (int)strlen(out);
    if (quote) strncat(out, "\"", (size_t)(outsz - len - 1));
    for (p = arg; *p; p++) {
        len = (int)strlen(out);
        if (len >= outsz - 1) break;
        out[len] = *p;
        out[len + 1] = 0;
    }
    len = (int)strlen(out);
    if (quote) strncat(out, "\"", (size_t)(outsz - len - 1));
}

// The supervisor's child is this same exe in endpoint mode. Its original
// arguments are kept, including a server and port a person typed, so a restart
// goes back to the same place.
static int child_command_line(char *cmd, int cmdsz, int argc, char **argv)
{
    char exe[MAX_PATH];
    int i;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) return 0;
    copy_str(cmd, cmdsz, "\"");
    strncat(cmd, exe, (size_t)(cmdsz - (int)strlen(cmd) - 1));
    strncat(cmd, "\" -endpoint", (size_t)(cmdsz - (int)strlen(cmd) - 1));
    for (i = 1; i < argc; i++)
        append_arg(cmd, cmdsz, argv[i]);
    return 1;
}

static int supervise_endpoint(int argc, char **argv)
{
    HANDLE held;
    HANDLE child;
    char cmd[TOOL_MAXLINE + MAX_PATH];
    DWORD backoff_ms = 1000;

    SetLastError(ERROR_SUCCESS);
    held = CreateMutexA(NULL, FALSE, TOOLS_SUPERVISOR);
    if (!held) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(held);
        return 0;
    }
    if (endpoint_is_running()) {
        CloseHandle(held);
        return 0;
    }
    if (!child_command_line(cmd, sizeof cmd, argc, argv)) {
        CloseHandle(held);
        return 1;
    }

    g_stop_event = CreateEventA(NULL, TRUE, FALSE, TOOLS_STOP);
    log_file_start();
    log_line("endpoint supervisor starting");

    for (;;) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        DWORD started;
        DWORD code = 0;
        DWORD wait;
        HANDLE waits[2];
        DWORD count = 1;

        if (stop_requested()) break;
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        ZeroMemory(&pi, sizeof pi);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                            DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                            NULL, NULL, &si, &pi)) {
            log_line("cannot start the endpoint (error %lu), retrying in %lus",
                     (unsigned long)GetLastError(),
                     (unsigned long)(backoff_ms / 1000));
            if (wait_or_stop((int)backoff_ms)) break;
            if (backoff_ms < 30000) backoff_ms *= 2;
            continue;
        }
        CloseHandle(pi.hThread);
        child = pi.hProcess;
        started = GetTickCount();

        waits[0] = child;
        if (g_stop_event) waits[count++] = g_stop_event;
        wait = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (count == 2 && wait == WAIT_OBJECT_0 + 1) {
            WaitForSingleObject(child, 10000);
            GetExitCodeProcess(child, &code);
            CloseHandle(child);
            break;
        }
        if (wait != WAIT_OBJECT_0) {
            log_line("could not wait for the endpoint (error %lu)",
                     (unsigned long)GetLastError());
            TerminateProcess(child, 1);
            WaitForSingleObject(child, 2000);
            CloseHandle(child);
            break;
        }

        GetExitCodeProcess(child, &code);
        CloseHandle(child);

        // A second copy may have won the endpoint mutex between our check and
        // this child's start. That is not a crash, and it is not ours to watch.
        if (endpoint_is_running()) break;
        if (stop_requested()) break;
        if ((DWORD)(GetTickCount() - started) >= 60000) backoff_ms = 1000;
        log_line("endpoint exited unexpectedly (0x%08lX), restarting in %lus",
                 (unsigned long)code, (unsigned long)(backoff_ms / 1000));
        if (wait_or_stop((int)backoff_ms)) break;
        if (backoff_ms < 30000) backoff_ms *= 2;
    }

    log_line("endpoint supervisor stopped");
    CloseHandle(held);
    return 0;
}

// ------------------------------------------- coming back after the next logon
//
// Nothing is registered behind the user's back: the entry that starts this
// endpoint with Windows exists only because somebody typed i at the manager or
// ran --install, and u or --uninstall takes it away again. It is one per-user
// value, so neither needs an administrator, and it starts the same exe it was
// asked from, wherever that exe happens to live.
//
// It says -toast because that is what this value has always said, on machines
// that already have it and in the update script. -toast and -run are the same
// thing: the endpoint, with no window at all.

#define RUN_KEY  "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_NAME "Tools"

static int autostart_read(char *out, int outsz)
{
    HKEY key;
    DWORD type = 0, len = (DWORD)outsz;
    LONG rc;

    out[0] = 0;
    rc = RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegQueryValueExA(key, RUN_NAME, NULL, &type, (BYTE *)out, &len);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = 0;
        return 0;
    }
    out[outsz - 1] = 0;
    return 1;
}

static int autostart_add(void)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH + 32];
    HKEY key;
    LONG rc;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) return 0;
    format_text(cmd, sizeof cmd, "\"%s\" -toast", exe);

    rc = RegCreateKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, NULL,
                         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegSetValueExA(key, RUN_NAME, 0, REG_SZ, (const BYTE *)cmd,
                        (DWORD)strlen(cmd) + 1);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static int autostart_remove(void)
{
    HKEY key;
    LONG rc;

    rc = RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) return 0;
    rc = RegDeleteValueA(key, RUN_NAME);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

static void do_install(void)
{
    char now[512];

    printf("tool %s\n", TOOL_VERSION);
    if (!autostart_add()) {
        printf("could not write %s\\%s (error %lu)\n", RUN_KEY, RUN_NAME,
               (unsigned long)GetLastError());
        printf("no change was made.\n");
        return;
    }
    if (autostart_read(now, sizeof now))
        printf("starts with Windows now: %s\n", now);
    printf("the endpoint will come up at the next logon: no window.\n");
    printf("this copy is not started by installing - run tool.exe and\n"
           "type s to start it now.\n");
}

static void do_uninstall(void)
{
    char had[512];

    printf("tool %s\n", TOOL_VERSION);
    if (!autostart_read(had, sizeof had)) {
        printf("nothing to remove - it is not set to start with Windows.\n");
        return;
    }
    if (!autostart_remove()) {
        printf("could not remove %s\\%s (error %lu)\n", RUN_KEY, RUN_NAME,
               (unsigned long)GetLastError());
        return;
    }
    printf("removed: %s\n", had);
    printf("an endpoint that is running keeps running: run tool.exe and\n"
           "type s to stop it.\n");
}

static SOCKET dial(void)
{
    struct sockaddr_in sa;
    struct hostent *he;
    SOCKET s;
    u_long nonblocking = 1;
    u_long blocking = 0;
    int error;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    sa.sin_addr.s_addr = inet_addr(g_server);
    if (sa.sin_addr.s_addr == INADDR_NONE) {
        he = gethostbyname(g_server);
        if (!he) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        memcpy(&sa.sin_addr, he->h_addr, (size_t)he->h_length);
    }

    // A blocking connect can sit on an unreachable LAN address for many
    // seconds before Windows reports failure. Each entry gets one short try,
    // so the next one in servers.txt is reached promptly.
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) == SOCKET_ERROR) {
        fd_set writable, failed;
        struct timeval wait;
        int length = sizeof error;

        error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS &&
            error != WSAEALREADY) {
            closesocket(s);
            WSASetLastError(error);
            return INVALID_SOCKET;
        }

        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        wait.tv_sec = SERVER_CONNECT_MS / 1000;
        wait.tv_usec = (SERVER_CONNECT_MS % 1000) * 1000;
        error = select(0, NULL, &writable, &failed, &wait);
        if (error == SOCKET_ERROR) {
            error = WSAGetLastError();
            closesocket(s);
            WSASetLastError(error);
            return INVALID_SOCKET;
        }
        if (error == 0) {
            closesocket(s);
            WSASetLastError(WSAETIMEDOUT);
            return INVALID_SOCKET;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &length) != 0) {
            error = WSAGetLastError();
            closesocket(s);
            WSASetLastError(error);
            return INVALID_SOCKET;
        }
        if (error != 0) {
            closesocket(s);
            WSASetLastError(error);
            return INVALID_SOCKET;
        }
    }
    ioctlsocket(s, FIONBIO, &blocking);
    // A peer that stops midway through a record must not leave line_read
    // waiting forever. Silence between records is handled by the connection
    // loop below.
    error = SERVER_IO_MS;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&error, sizeof error);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&error, sizeof error);
    return s;
}

// Opens the session and says who we are. 1 = accepted.
//
// Nothing is readable until after session_start has sealed the connection, so
// the server's first readable word is what proves it holds the token too.
static int handshake(lines_t *lr)
{
    char line[TOOL_MAXLINE];
    char why[256];

    // The session is sealed before anything is said; see session_start. The
    // first thing said on it is who we are, and the server's answer proves it
    // holds the token - a line that decodes at all could only have been made
    // by somebody who does.
    if (!session_start(g_sock, 1, g_token, why, sizeof why)) {
        log_line("%s", why);
        return 0;
    }
    if (!line_send(g_sock, NULL, "HELLO endpoint %s", g_name)) {
        log_line("could not send the handshake");
        return 0;
    }
    if (line_read(lr, line, sizeof line) != 1) {
        log_line("no answer to the handshake");
        return 0;
    }
    if (strncmp(line, "OK", 2) != 0) {
        log_line("server refused us: %s", line);
        return 0;
    }
    log_line("session sealed");
    return 1;
}

// ---------------------------------------------------------------- the events

static void do_beep(const char *args)
{
    int freq = 1000;
    int ms = 500;
    char shown[32];
    char *sp;

    if (*args) {
        freq = atoi(args);
        sp = strchr(args, ' ');
        if (sp) ms = atoi(sp + 1);
    }
    if (freq < 37) freq = 37;
    if (freq > 32767) freq = 32767;
    if (ms < 1) ms = 1;
    if (ms > 5000) ms = 5000;

    if (!Beep((DWORD)freq, (DWORD)ms)) {
        line_send(g_sock, NULL, "FAIL BEEP cannot beep");
        return;
    }
    // The same two numbers the answer carries, on the device's own screen.
    format_text(shown, sizeof shown, "beep: %d Hz %d ms", freq, ms);
    toast_show(shown);
    line_send(g_sock, NULL, "DONE BEEP %d %d", freq, ms);
}

static void do_mute(const char *args)
{
    int on;

    if (!*args || !_stricmp(args, "toggle")) {
        if (!audio_get_mute(&on)) {
            line_send(g_sock, NULL, "FAIL MUTE no audio device");
            return;
        }
        on = !on;
    } else {
        on = _stricmp(args, "off") != 0;
    }
    if (!audio_set_mute(on)) {
        line_send(g_sock, NULL, "FAIL MUTE no audio device");
        return;
    }
    // Whether the sound is off, on the device's own screen.
    toast_show(on ? "mute: on" : "mute: off");
    line_send(g_sock, NULL, "DONE MUTE %s", on ? "on" : "off");
}

// VOL <0-100>: the master volume, and the one change that also says itself on
// the device: the slider is somewhere else, so the number is all there is to
// see, and it is what the toast carries. Setting it above zero is asking to
// hear it, so the sound is unmuted as well.
static void do_volume(const char *args)
{
    int percent = atoi(args);
    char shown[32];

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!audio_set_volume(percent)) {
        line_send(g_sock, NULL, "FAIL VOL no audio device");
        return;
    }
    if (percent > 0 && !audio_set_mute(0)) {
        line_send(g_sock, NULL, "FAIL VOL no audio device");
        return;
    }
    format_text(shown, sizeof shown, "volume: %d%%", percent);
    toast_show(shown);
    line_send(g_sock, NULL, "DONE VOL %d", percent);
}

// GETVOL: what the volume is right now, so a console's slider can show the
// truth rather than whatever it last set.
static void do_get_volume(void)
{
    int percent = 0;

    if (!audio_get_volume(&percent)) {
        line_send(g_sock, NULL, "FAIL GETVOL no audio device");
        return;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    line_send(g_sock, NULL, "DONE GETVOL %d", percent);
}

// GETMUTE: whether the sound is muted right now, for the same reason - a
// console's mute button should show what the device is doing, not what the
// button was last clicked to.
static void do_get_mute(void)
{
    int on = 0;

    if (!audio_get_mute(&on)) {
        line_send(g_sock, NULL, "FAIL GETMUTE no audio device");
        return;
    }
    line_send(g_sock, NULL, "DONE GETMUTE %s", on ? "on" : "off");
}

// What the screen says it is at, or -1 when nothing can say. Reading works
// through WMI even on machines where setting through WMI does nothing.
static int brightness_now(void)
{
    char out[512];
    char *p;

    if (!run_capture("powershell -NoProfile -Command "
                     "\"(Get-CimInstance -Namespace root/wmi "
                     "-ClassName WmiMonitorBrightness).CurrentBrightness\"",
                     out, sizeof out, 20000))
        return -1;
    for (p = out; *p && (*p < '0' || *p > '9'); p++) ;
    if (!*p) return -1;
    return atoi(p);
}

// GETDIM: the brightness the screen reports, so a console's slider can sit
// where the device actually is.
static void do_get_dim(void)
{
    int percent = brightness_now();

    if (percent < 0) {
        line_send(g_sock, NULL, "FAIL GETDIM no brightness reading");
        return;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    line_send(g_sock, NULL, "DONE GETDIM %d", percent);
}

// Change the screen brightness. WmiSetBrightness is the documented way to do
// this and it reports success on a Surface while the screen does not move an
// inch; moving the setting in the active power scheme and re-applying the
// scheme is what they actually obey. So: the scheme first, WMI as the fallback
// for machines whose scheme has no such setting.
static int brightness_set(int percent, char *why, int whysz)
{
    char cmd[512];
    char out[1024];

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    format_text(cmd, sizeof cmd,
                "powercfg /setacvalueindex SCHEME_CURRENT %s %s %d",
                VIDEO_SUBGROUP_GUID, BRIGHTNESS_GUID, percent);
    if (run_capture(cmd, out, sizeof out, 15000)) {
        // Both AC and DC, so unplugging the charger does not jump it back.
        format_text(cmd, sizeof cmd,
                    "powercfg /setdcvalueindex SCHEME_CURRENT %s %s %d",
                    VIDEO_SUBGROUP_GUID, BRIGHTNESS_GUID, percent);
        run_capture(cmd, out, sizeof out, 15000);
        run_capture("powercfg /setactive SCHEME_CURRENT", out, sizeof out, 15000);
    } else {
        format_text(cmd, sizeof cmd,
                    "powershell -NoProfile -Command \"(Get-CimInstance -Namespace "
                    "root/wmi -ClassName WmiMonitorBrightnessMethods)"
                    ".WmiSetBrightness(1,%d)\"", percent);
        if (!run_capture(cmd, out, sizeof out, 20000)) {
            copy_str(why, whysz, *out ? out : "no brightness control");
            return 0;
        }
    }
    return 1;
}

// DIM <0-100>. The answer and the toast both carry what the screen really
// reads, because a panel may land on a step of its own rather than the number
// asked for.
static void do_dim(const char *args)
{
    char why[256];
    char shown[32];
    int percent = atoi(args);
    int now;

    if (!brightness_set(percent, why, sizeof why)) {
        line_send(g_sock, NULL, "FAIL DIM %s", why);
        return;
    }

    now = brightness_now();
    if (now < 0) now = percent;

    // The same number the answer carries, on the screen whose brightness it
    // is: a change made from somewhere else is the one thing there is to see.
    format_text(shown, sizeof shown, "brightness: %d%%", now);
    toast_show(shown);
    line_send(g_sock, NULL, "DONE DIM %d", now);
}

// Input Director entering a screen raises it to full brightness until the
// pointer leaves, then puts back what the screen was at on the way in. Neither
// change is a notification of its own, so neither shows a toast.
static int g_dim_leave = -1;

static void do_dim_enter(void)
{
    char why[256];
    int before;

    mirror_cursor_hold(1);
    if (g_dim_leave >= 0) {
        line_send(g_sock, NULL, "DONE DIMENTER");
        return;
    }
    before = brightness_now();
    if (before < 0) {
        line_send(g_sock, NULL, "FAIL DIMENTER no brightness reading");
        return;
    }
    if (!brightness_set(100, why, sizeof why)) {
        line_send(g_sock, NULL, "FAIL DIMENTER %s", why);
        return;
    }
    g_dim_leave = before;
    line_send(g_sock, NULL, "DONE DIMENTER");
}

static void do_dim_leave(void)
{
    char why[256];

    mirror_cursor_hold(0);
    if (g_dim_leave < 0) {
        line_send(g_sock, NULL, "DONE DIMLEAVE");
        return;
    }
    if (!brightness_set(g_dim_leave, why, sizeof why)) {
        line_send(g_sock, NULL, "FAIL DIMLEAVE %s", why);
        return;
    }
    g_dim_leave = -1;
    line_send(g_sock, NULL, "DONE DIMLEAVE");
}

// GETBUILD: this program's own exe, as a size and a SHA-256, which is what
// says whether the device is running the build a console holds. It is the
// running image's own file that is hashed rather than one found by name, so a
// copy started from anywhere answers about itself, and a file in use can be
// read like any other, so none of this needs the endpoint stopped first.
static void do_getbuild(void)
{
    char exe[MAX_PATH];
    char why[256];
    char hex[80];
    tool_size size = 0;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) {
        line_send(g_sock, NULL, "FAIL GETBUILD cannot tell where this program is");
        return;
    }
    if (!file_size_sha256(exe, &size, hex, sizeof hex, why, sizeof why)) {
        line_send(g_sock, NULL, "FAIL GETBUILD %s", why);
        return;
    }
    line_send(g_sock, NULL, "DONE GETBUILD %llu %s", size, hex);
}


static void do_sleep(void)
{
    SendMessageTimeout(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, (LPARAM)2,
                       SMTO_ABORTIFHUNG, 2000, NULL);
    line_send(g_sock, NULL, "DONE SLEEP");
}

// ---------------------------------------------------------------- one file

// ---------------------------------------------------------------- the name
//
// RENAME <newname>: the one event that changes what the device is rather than
// what it is doing. There is only one name to move, which is the whole point -
// a device is called what Windows calls it - so this is a Windows rename and
// nothing else. Nothing is written down: the endpoint answers as the new name
// straight away by holding it for this run, and the machine itself takes it at
// the restart, after which the computer name is where the name comes from
// anyway.
static void do_rename(const char *args)
{
    char machine[64];
    char cmd[TOOL_MAXLINE];
    char out[2048];
    char old[64];
    const char *why_not;
    DWORD n = sizeof machine;
    int code = 0;

    if (!*args) {
        line_send(g_sock, NULL, "FAIL RENAME no new name");
        return;
    }
    why_not = computer_name_problem(args);
    if (why_not) {
        line_send(g_sock, NULL, "FAIL RENAME %s", why_not);
        return;
    }

    if (!GetComputerNameA(machine, &n)) machine[0] = 0;
    if (_stricmp(machine, args) != 0) {
        format_text(cmd, sizeof cmd,
                    "powershell -NoProfile -Command Rename-Computer "
                    "-NewName '%s' -Force", args);
        if (!run_capture_ex(cmd, NULL, out, sizeof out, 60000, &code) || code != 0) {
            line_send(g_sock, NULL, "FAIL RENAME %s",
                      *out ? out : "Windows would not rename this computer");
            return;
        }
        log_line("renamed this computer from '%s' to '%s'", machine, args);
    }

    copy_str(old, sizeof old, g_name);
    copy_str(g_name, sizeof g_name, args);
    log_line("this endpoint answers as '%s' now (was '%s'); Windows finishes "
             "the rename at the next restart", g_name, old);
    line_send(g_sock, NULL, "DONE RENAME %s", g_name);

    // A live connection cannot change the name it registered with, so the
    // server hears the new one the only way it can: this connection goes and
    // the loop dials again. What is written above has already gone out.
    g_reconnect = 1;
}

// Relative paths mean "next to the exe", so a transfer can name
// tool_new.exe without knowing where the endpoint was started from.
static void resolve_path(const char *path, char *out, int outsz)
{
    if (!*path) {
        out[0] = 0;
        return;
    }
    if (path[1] == ':' || path[0] == '\\' || path[0] == '/') {
        copy_str(out, outsz, path);
        return;
    }
    exe_dir(out, outsz);
    strncat(out, path, outsz - strlen(out) - 1);
}

void tool_core_resolve_path(const char *path, char *out, int outsz)
{
    resolve_path(path, out, outsz);
}

// ------------------------------------------------------ Input Director UDP
//
// The arrangement's pointer moves ride UDP rather than the sealed line,
// because the whole of a move is wanting to be quick: one move is one
// datagram, and one a busy socket drops is one the next replaces. The socket
// is opened from this device's own side and a registration goes out to the
// server's port, so the server knows where this device is and the console
// driving it can send straight here as well as through the server. Nothing
// else rides it: the clicks, the wheel and the keys are the sealed line's,
// because those are the ones that must not go missing.
//
// A move datagram does what an INPUT M over the line does - both go through
// the feature hook, so the two cannot drift apart. A probe datagram is answered
// so the server or console that sent it knows this device can receive the
// moves it is about to send.

static SOCKET g_udp = INVALID_SOCKET;

// Who has sent a move, so a datagram that arrived twice or out of order is not
// taken a second time. A handful of them: the server and the console or two
// driving this device.
#define UDP_FROM_MAX 8
static datagram_peer g_udp_from[UDP_FROM_MAX];

// Where this device is, said on the connection it already holds rather than
// from the move socket itself: the server has to be able to hear it whatever
// the device's network can be reached from outside, and this way the address
// is the one on the network the two of them actually share.
static void udp_announce(void)
{
    char local[32];
    char text[160];
    struct sockaddr_in me;
    int len = sizeof me;
    int port = 0;

    if (g_udp == INVALID_SOCKET || g_sock == INVALID_SOCKET) return;
    if (getsockname(g_udp, (struct sockaddr *)&me, &len) == 0)
        port = (int)ntohs(me.sin_port);
    datagram_local_ip(g_server, g_port, local, sizeof local);
    if (!port || !local[0]) return;
    format_text(text, sizeof text, "UDP %s %d", local, port);
    line_send(g_sock, NULL, "%s", text);
}

static DWORD WINAPI udp_thread(void *arg)
{
    unsigned char wire[TOOL_MAXLINE];

    (void)arg;
    for (;;) {
        char text[TOOL_MAXLINE];
        char move[TOOL_MAXLINE];
        int pending = 0;

        if (stop_requested()) break;
        if (datagram_wait(g_udp, 1000) <= 0) continue;

        // Whatever is waiting is read, and only the last move in the pile is
        // applied: a move is a place rather than a step in a history, so a
        // backlog applied one by one would be a pointer dragging behind the
        // hand.
        for (;;) {
            struct sockaddr_in peer;
            datagram_from *from;
            int got = datagram_recv(g_udp, wire, sizeof wire, &peer);
            int plain;

            if (got < 0) break;
            from = datagram_peer_for(g_udp_from, UDP_FROM_MAX, &peer);
            plain = datagram_open(from, wire, got, text, sizeof text);
            if (plain > 0 && text[0] == 'P') {
                char ip[32];
                char ack[96];
                unsigned char *b = (unsigned char *)&peer.sin_addr;

                format_text(ip, sizeof ip, "%u.%u.%u.%u",
                            b[0], b[1], b[2], b[3]);
                format_text(ack, sizeof ack, "A %s", g_name);
                datagram_sendto(g_udp, ip, ntohs(peer.sin_port), ack);
            } else if (plain > 0 && text[0] == 'M') {
                copy_str(move, sizeof move, text);
                pending = 1;
            }
            if (datagram_wait(g_udp, 0) <= 0) break;
        }
        if (pending) {
            char *words[4];
            char why[80];

            if (line_split(move, words, 4) >= 3)
                mirror_move(atoi(words[1]), atoi(words[2]), 0,
                            why, sizeof why);
        }
    }
    return 0;
}

// The socket and the thread come up with the endpoint and live as long as it
// does. A device that is not connected yet is not one anybody is moving the
// pointer on, and the registration finds nowhere to go until it is.
static void udp_start(void)
{
    HANDLE thread;

    if (g_udp != INVALID_SOCKET) return;
    if (!datagram_startup(g_token)) {
        log_line("the crypto provider would not make random bytes; "
                 "Input Director moves are unavailable");
        return;
    }
    g_udp = datagram_socket();
    if (g_udp == INVALID_SOCKET) return;
    thread = CreateThread(NULL, 0, udp_thread, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    else {
        closesocket(g_udp);
        g_udp = INVALID_SOCKET;
    }
}


// The direct lifecycle events run these fixed machine controls. They take no
// command text from the server.
static void do_control(const char *what, const char *command)
{
    char dir[MAX_PATH];
    char out[2048];
    int code = 0;
    int ok;

    if (!*command) {
        line_send(g_sock, NULL, "FAIL %s nothing to run", what);
        return;
    }
    exe_dir(dir, sizeof dir);
    ok = run_capture_ex(command, dir, out, sizeof out, 30000, &code);
    if (ok)
        line_send(g_sock, NULL, "DONE %s %d %s", what, code,
                  *out ? out : "(no output)");
    else
        line_send(g_sock, NULL, "FAIL %s %s", what,
                  *out ? out : "did not run");
}

static void do_event(const char *event, lines_t *lr)
{
    char buf[TOOL_MAXLINE];
    char args[TOOL_MAXLINE];
    char *words[8];
    int count;

    copy_str(buf, sizeof buf, event);
    copy_str(args, sizeof args, line_rest(buf, 1));
    count = line_split(buf, words, 8);
    if (count < 1) return;

    if (!_stricmp(words[0], "BEEP")) do_beep(args);
    else if (!_stricmp(words[0], "MUTE")) do_mute(args);
    else if (!_stricmp(words[0], "VOL")) do_volume(args);
    else if (!_stricmp(words[0], "DIM")) do_dim(args);
    else if (!_stricmp(words[0], "SLEEP")) do_sleep();
    else if (!_stricmp(words[0], "SHUTDOWN"))
        do_control("SHUTDOWN", "shutdown.exe /s /t 3 /f");
    else if (!_stricmp(words[0], "RESTART"))
        do_control("RESTART", "shutdown.exe /r /t 3 /f");
    else if (!_stricmp(words[0], "CLOSE"))
        do_control("CLOSE", "taskkill /f /im tool.exe");
    else if (!_stricmp(words[0], "SAY")) do_say(args);
    else if (!_stricmp(words[0], "GETVOL")) do_get_volume();
    else if (!_stricmp(words[0], "GETDIM")) do_get_dim();
    else if (!_stricmp(words[0], "DIMENTER")) do_dim_enter();
    else if (!_stricmp(words[0], "DIMLEAVE")) do_dim_leave();
    else if (!_stricmp(words[0], "GETMUTE")) do_get_mute();
    else if (!_stricmp(words[0], "GETBUILD")) do_getbuild();
    else if (!_stricmp(words[0], "RENAME")) do_rename(args);
    else if (!_stricmp(words[0], "SCREENINFO")) mirror_screeninfo();
    else if (!_stricmp(words[0], "INPUT")) mirror_input(args);
    else if (!_stricmp(words[0], "CLIPGET")) mirror_clipget(args);
    else if (!_stricmp(words[0], "CLIPSET")) mirror_clipset(args);
    else if (!_stricmp(words[0], "CLIPPASTE")) mirror_clippaste(args);
    else if (!_stricmp(words[0], "CLIPSEQ")) mirror_clipseq();
    else if (!_stricmp(words[0], "INSERT")) mirror_insert(args);
    // Extension events have no core handler. A full build receives them through
    // this optional hook; a ToolCore-only build rejects them.
    else if (!tool_extension_event(event, lr))
        line_send(g_sock, NULL, "FAIL %s unknown event", words[0]);
}

void tool_core_dispatch_event(const char *event, lines_t *lr)
{
    do_event(event, lr);
}

// ----------------------------------------------------------------- the manager
//
// tool.exe with no arguments is this: what the device is doing, and one
// letter at a time to change it. It is not the endpoint and never becomes the
// endpoint - it starts one as a separate, detached process - so closing this
// console cannot stop a copy that is running. That is the whole difference
// from a start that is the endpoint itself.

// Where the endpoint comes from: this same exe, in its -run form, so the copy
// the manager starts is the build the manager is. Detached, so it has no
// console of its own and is not attached to this one, and it reads the same
// token.txt in the same folder and is pointed at the same server this manager
// is, because the settings are what the exe and the environment say. -run
// starts a supervisor, which starts the endpoint itself.
static int start_endpoint(DWORD *pid_out)
{
    char exe[MAX_PATH];
    char cmd[MAX_PATH + 16];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    int i;

    if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0) {
        printf("cannot tell where this exe is, so there is nothing to start\n");
        return 0;
    }
    format_text(cmd, sizeof cmd, "\"%s\" -run", exe);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                        NULL, NULL, &si, &pi)) {
        printf("could not start the endpoint (error %lu)\n",
               (unsigned long)GetLastError());
        return 0;
    }
    *pid_out = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Give it a moment to claim the name, so "running:" is never a guess about
    // a process that died on the way up - a missing token, say.
    for (i = 0; i < 50; i++) {
        if (endpoint_is_running()) return 1;
        Sleep(100);
    }
    printf("it started (pid %lu) but is not running - client.log next to the exe\n"
           "will say why\n", (unsigned long)*pid_out);
    return 0;
}

// Ending an endpoint the hard way. Reached when there is no stop event to set
// (a copy from a build older than this one) or when one has been set and the
// copy is wedged inside a long command. This ends the supervisor with its
// endpoint. Never this process: the manager is a tool.exe too, so the tool that
// kills by image name has to spare it.
static int kill_endpoint_other_than_me(void)
{
    char cmd[160];
    char out[512];
    int code = 0;

    format_text(cmd, sizeof cmd,
                "taskkill /f /im tool.exe /fi \"PID ne %lu\"",
                (unsigned long)GetCurrentProcessId());
    run_capture_ex(cmd, NULL, out, sizeof out, 20000, &code);
    return !endpoint_is_running();      // the mutex, not taskkill, is the judge
}

// Asking the endpoint to stop, the polite way first: it watches this event
// between socket reads, so it closes its connection and writes its log the way
// it would on any other way out, rather than vanishing mid-line.
static int stop_endpoint(void)
{
    HANDLE ev = OpenEventA(EVENT_MODIFY_STATE, FALSE, TOOLS_STOP);
    int i;

    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
        for (i = 0; i < 100; i++) {         // up to ten seconds
            if (!endpoint_is_running()) return 1;
            Sleep(100);
        }
    }
    return kill_endpoint_other_than_me();
}

// Whether the device has a token file at all. Read rather than loaded: the
// manager connects to nothing, and this line only ever shows when something is
// wrong - an endpoint here could never reach the server without it.
static int token_present(void)
{
    char path[MAX_PATH];
    FILE *f;

    exe_dir(path, sizeof path);
    strncat(path, "token.txt", sizeof path - strlen(path) - 1);
    f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

// The status block. The manager always offers exactly two things to do, which
// are the two that are not already the state of the device: stop/start and
// uninstall/install.
static void manager_status(void)
{
    char value[512];
    int installed = autostart_read(value, sizeof value);
    int running = endpoint_is_running();

    printf("installed: %s\n", installed ? "Yes" : "No");
    printf("running: %s\n", running ? "Yes" : "No");
    if (!token_present())
        printf("token: missing - no token.txt next to tool.exe, so this\n"
               "       device cannot reach the server\n");
    printf("\n");
    printf("type %s or type %s: ",
           running ? "s to stop" : "s to start",
           installed ? "u to uninstall" : "i to install");
    fflush(stdout);
}

// One key. Returns 0 when the console should close.
static int manager_key(int key)
{
    char value[512];
    DWORD pid = 0;

    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';

    if (key == 's') {
        if (endpoint_is_running()) {
            printf("stopping the endpoint ...\n");
            fflush(stdout);         // the answer can take ten seconds
            if (stop_endpoint()) printf("the endpoint has stopped\n");
            else printf("it would not stop - a command may be running in it\n");
        } else {
            printf("starting the endpoint ...\n");
            fflush(stdout);
            if (start_endpoint(&pid))
                printf("the endpoint is running (pid %lu)\n", (unsigned long)pid);
        }
        return 1;
    }
    if (key == 'i') {
        if (autostart_read(value, sizeof value))
            printf("it is already set to start with Windows: %s\n", value);
        else if (autostart_add() && autostart_read(value, sizeof value))
            printf("installed: the endpoint comes up at the next logon (%s)\n", value);
        else
            printf("could not write the logon entry (error %lu)\n",
                   (unsigned long)GetLastError());
        return 1;
    }
    if (key == 'u') {
        if (!autostart_read(value, sizeof value))
            printf("nothing to remove - it is not set to start with Windows\n");
        else if (autostart_remove())
            printf("uninstalled: it will not come up at the next logon any more\n");
        else
            printf("could not remove the logon entry (error %lu)\n",
                   (unsigned long)GetLastError());
        return 1;
    }
    if (key == '?') {
        printf("  s  stop the endpoint, or start it if it is not running\n"
               "  i  start it with Windows at the next logon\n"
               "  u  take that back out\n"
               "  q  close this console, or press Esc (the endpoint carries on)\n"
               "  ?  this list\n");
        return 1;
    }
    // q or Esc, and Esc is a whole key by the time it gets here: read_key
    // swallows the sequences the arrow and function keys arrive as.
    if (key == 'q' || key == 27 || key == -1) return 0;
    return 1;       // a key that means nothing: ask again, say nothing
}

// The console this manager is typing at, in the one state it must never be
// left in: line input and echo off, waiting for a key. Ctrl+C during that read
// would end the process with the console still in it, and the shell it came
// from would then look like it had stopped listening. The handler puts it back
// before letting the default one end us.
static HANDLE g_typing_in;
static DWORD g_typing_mode;
static int g_typing_raw;

static BOOL WINAPI typing_ctrl_handler(DWORD type)
{
    (void)type;
    if (g_typing_raw && g_typing_in) SetConsoleMode(g_typing_in, g_typing_mode);
    return FALSE;       // and then the default handler ends this process
}

// Whether an Esc that has just been read was the whole key. An arrow key, a
// function key and most other things on that side of the keyboard arrive as an
// Esc and then a sequence, and the rest of it is already on its way; a person
// pressing Esc presses it on its own. So: whatever follows an Esc is swallowed
// here, which is what stops an arrow key closing the console.
static int escape_stands_alone(HANDLE in)
{
    char junk = 0;
    DWORD got = 0;
    int i;

    for (i = 0; i < 8; i++) {
        if (WaitForSingleObject(in, 40) != WAIT_OBJECT_0) break;
        if (!ReadFile(in, &junk, 1, &got, NULL) || got != 1) break;
    }
    return i == 0;      // true only when nothing followed it
}

// One key, no Enter. The console turns every character into a line until line
// input is turned off, so it is turned off for the read and put back straight
// after - the shell that started this is left exactly as it was. Keys that are
// not keys on their own are read past rather than handed back.
static int read_key(HANDLE in, DWORD normal, int *out)
{
    DWORD no_line = (normal & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT))
                    | ENABLE_PROCESSED_INPUT;
    char ch = 0;
    DWORD got = 0;

    g_typing_in = in;
    g_typing_mode = normal;
    g_typing_raw = 1;
    SetConsoleMode(in, no_line);

    for (;;) {
        ch = 0;
        got = 0;
        if (!ReadFile(in, &ch, 1, &got, NULL) || got != 1) {
            SetConsoleMode(in, normal);
            g_typing_raw = 0;
            return 0;
        }
        if ((unsigned char)ch == 27 && !escape_stands_alone(in)) continue;
        break;
    }

    SetConsoleMode(in, normal);
    g_typing_raw = 0;
    *out = (unsigned char)ch;
    return 1;
}

// Not a console: one command per line instead, so a script can drive the same
// letters. End of input means there is nobody left to ask - the signal to go,
// with the endpoint left running exactly as closing a window would leave it.
static int read_command_line(int *out)
{
    char line[64];
    char *p;

    if (!fgets(line, sizeof line, stdin)) return 0;
    for (p = line; *p == ' ' || *p == '\t'; p++) ;
    *out = *p ? (unsigned char)*p : '\n';
    return 1;
}

static int run_manager(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD normal = 0;
    int typing = GetConsoleMode(in, &normal) != 0;
    int key;

    if (typing) {
        open_console();             // a console nobody can see is not a window
        SetConsoleCtrlHandler(typing_ctrl_handler, TRUE);
    }

    if (*g_server)
        printf("tool %s  '%s'  ->  %s:%d\n",
               TOOL_VERSION, g_name, g_server, g_port);
    else
        printf("tool %s  '%s'  ->  servers.txt\n", TOOL_VERSION, g_name);
    printf("the endpoint runs on its own: closing this console does not stop it\n\n");

    for (;;) {
        manager_status();
        if (typing) {
            if (!read_key(in, normal, &key)) break;
            // The console will not echo it for us. Only printable keys are
            // echoed: an arrow key's escape bytes are not a word.
            if (key >= 32 && key < 127) printf("%c\n", key);
            else printf("\n");
        } else if (!read_command_line(&key)) {
            break;
        }
        if (!manager_key(key)) break;
        printf("\n");
    }

    if (endpoint_is_running())
        printf("closing the console - the endpoint keeps running\n");
    else
        printf("closing the console - nothing is running\n");
    return 0;
}

// ------------------------------------------------------------- the command line

static void do_help(void)
{
    printf("tool %s - the endpoint, and the console that manages it\n"
           "\n"
           "  tool.exe                         what this device is doing, and\n"
           "                                   one letter at a time to change\n"
           "                                   it: s stops the endpoint or starts\n"
           "                                   it, i installs it at logon, u\n"
           "                                   takes that back out, q or Esc\n"
           "                                   closes this console\n"
           "  tool.exe       -run | -toast     the supervised endpoint, no window\n"
           "  tool.exe       --server <host> [--port <n>]\n"
           "                                   dial exactly one server,\n"
           "                                   ignoring servers.txt\n"
           "  tool.exe       --message <text>  that text on this machine's\n"
           "                                   screen for a few seconds\n"
           "  tool.exe       --install         start it at the next logon\n"
           "  tool.exe       --uninstall       stop doing that\n"
           "  tool.exe       --help | --version\n"
           "\n"
           "The endpoint dials the server, proves it holds the same token.txt,\n"
           "then answers events until it is stopped. It runs as a process of\n"
           "its own: closing the console that started it does not stop it.\n"
           "Every line it prints also goes to client.log next to the exe. It\n"
           "answers as this computer's name.\n"
           "\n"
           "Without --server or TOOL_SERVER it tries the servers in\n"
           "servers.txt beside the exe, in order. That file must contain at\n"
           "least one host; no server is assumed.\n",
           TOOL_VERSION);
}

// ------------------------------------------------------------------ the loop

int main(int argc, char **argv)
{
    WSADATA wsa;
    char line[TOOL_MAXLINE];
    char message[1024];
    int backoff_ms = 1000;
    int no_window = 0;
    int manage = 1;
    int said = 0;
    int supervise = 0;
    int child_mode = 0;
    int i;

    dpi_aware();
    load_settings();
    message[0] = 0;

    // --install and --uninstall are about this machine rather than about
    // talking to a server, so they are answered before anything reads the
    // token, opens a socket or starts a copy.
    for (i = 1; i < argc; i++) {
        if (is_opt(argv[i], "install")) { do_install(); return 0; }
        if (is_opt(argv[i], "uninstall")) { do_uninstall(); return 0; }
    }

    for (i = 1; i < argc; i++) {
        if (is_opt(argv[i], "server") && i + 1 < argc) {
            copy_str(g_server, sizeof g_server, argv[++i]);
            g_server_explicit = 1;
            manage = 0;     // naming the server says you mean the endpoint
        } else if (is_opt(argv[i], "port") && i + 1 < argc) {
            g_port = atoi(argv[++i]);
            if (g_port <= 0 || g_port > 65535) {
                printf("tool: --port wants 1..65535\n");
                return 2;
            }
            g_port_explicit = 1;
            manage = 0;
        } else if (is_opt(argv[i], "help") || is_opt(argv[i], "?")) {
            do_help();
            return 0;
        } else if (is_opt(argv[i], "version")) {
            printf("tool %s\n", TOOL_VERSION);
            return 0;
        } else if (is_opt(argv[i], "run") || is_opt(argv[i], "toast")) {
            // What a logon entry and the update scripts say, and the one thing
            // they still ask for: the endpoint with nothing on the desktop.
            // Both names have always meant the same run.
            no_window = 1;
            manage = 0;
            supervise = 1;
        } else if (is_opt(argv[i], "endpoint")) {
            // The supervisor's own child. It is not a second way to ask for a
            // run: this copy is already watched.
            no_window = 1;
            manage = 0;
            child_mode = 1;
        } else if (take_message(argc, argv, &i, message, sizeof message)) {
            said = 1;
            manage = 0;
        } else {
            printf("tool: I do not know the argument '%s'\n\n", argv[i]);
            do_help();
            return 2;
        }
    }

    // A message is the whole job: nothing is dialled, and no token is wanted.
    if (said) {
        if (!*message) {
            printf("tool: --message wants some text\n");
            return 2;
        }
        if (!toast_run(message)) {
            printf("tool: there is no screen to put a message on\n");
            return 1;
        }
        log_line("toast: %s", message);
        return 0;
    }

    // No arguments is the manager, and the manager connects to nothing: it
    // reads the logon entry, asks the mutex whether a copy is running, and
    // takes one letter at a time. It never claims the name for itself, so
    // closing this console can never be closing the endpoint.
    if (manage) return run_manager();

    if (!servers_load()) return servers_failed(no_window, child_mode);

    // A supervised run is a parent process whose job is to keep the endpoint
    // up. The child is the endpoint itself, and keeps appending to the log the
    // supervisor started so a crash leaves its reason behind.
    if (no_window) detach_console();
    if (supervise && !child_mode) return supervise_endpoint(argc, argv);
    g_log_append = child_mode;

    // Nothing to run if a copy is already up. Asked before the console is left
    // behind, because this is the one case where somebody who typed -run at a
    // prompt is still looking at that console and should be told where to go.
    if (endpoint_is_running()) {
        printf("tool: an endpoint is already running on this machine\n"
               "run tool.exe with no arguments to see it and stop it\n");
        return 0;
    }

    if (!claim_single_instance()) {
        // Two copies started at the same moment: the other one won the race.
        // Nothing is wrong.
        log_line("another copy came up first - leaving it alone");
        return 0;
    }
    // The supervisor owns client.log for the life of the run. A child appends
    // to it, so a restart keeps the line that says why the last one left.
    if (!g_log_append) log_file_start();
    g_stop_event = CreateEventA(NULL, TRUE, FALSE, TOOLS_STOP);

    if (!token_load(g_token, sizeof g_token, 0)) {
        // token_load has said why on stdout, which a run with no console has
        // nowhere to put: the log gets the one line that matters instead.
        if (no_window) log_line("no token.txt next to the exe - cannot connect");
        return 1;
    }
    if (WSAStartup(0x0202, &wsa) != 0) {
        if (no_window) log_line("winsock will not start");
        else printf("tool: winsock will not start\n");
        return 1;
    }
    secure_startup();

    InitializeCriticalSection(&g_sock_lock);
    if (!stop_io_watch_start())
        log_line("could not start the stop watcher; a stop may take longer");

    if (g_servers_problem[0]) log_line("%s", g_servers_problem);
    log_line("endpoint %s as '%s', %d server(s); first %s:%d",
             TOOL_VERSION, g_name, g_server_count, g_server, g_port);

    tool_extension_start();

    // The arrangement's moves have a socket of their own, which is up whether
    // or not the server is: it starts answering as soon as the registration
    // reaches the server.
    udp_start();

    for (;;) {
        lines_t lr;
        DWORD last_ping;
        DWORD last_busy;
        SOCKET next;

        if (stop_requested()) break;
        if (!g_server_count) {
            log_line("%s", g_servers_problem[0] ? g_servers_problem
                                                : "no server is configured");
            servers_load();
            if (!g_server_count) {
                if (wait_or_stop(backoff_ms)) break;
                if (backoff_ms < 30000) backoff_ms *= 2;
            }
            continue;
        }

        if (g_server_count > 1)
            log_line("connecting to %s:%d (%d/%d) ...",
                     g_server, g_port, g_server_at + 1, g_server_count);
        else
            log_line("connecting to %s:%d ...", g_server, g_port);
        next = dial();
        endpoint_socket_set(next);
        if (next == INVALID_SOCKET) {
            int error = WSAGetLastError();

            log_line("cannot reach %s:%d (error %d)", g_server, g_port, error);
            if (servers_next()) {
                log_line("trying the next server, %s:%d", g_server, g_port);
                continue;
            }
            log_line("no server answered; trying the list again in %ds",
                     backoff_ms / 1000);
            if (wait_or_stop(backoff_ms)) break;
            if (backoff_ms < 30000) backoff_ms *= 2;
            continue;
        }

        lines_init(&lr, g_sock);
        // A fresh connection may be handed a socket number a sealed session
        // used before it, so start from nothing.
        secure_forget(g_sock);
        if (!handshake(&lr)) {
            endpoint_socket_close();
            if (servers_next()) {
                log_line("trying the next server, %s:%d", g_server, g_port);
                continue;
            }
            if (wait_or_stop(15000)) break;
            continue;
        }

        backoff_ms = 1000;
        last_ping = GetTickCount();
        last_busy = last_ping;
        tool_extension_connected();
        mirror_attach(g_sock);
        // Where this device takes the arrangement's moves, told afresh on
        // every connection: the address is only good while the connection it
        // was said on is, and a server that has just started knows nothing.
        udp_announce();
        // What this device is doing, said afresh for the same reason, and then
        // again whenever it changes.
        tool_extension_state_note();
        for (;;) {
            int ready = sock_wait(g_sock, 1000);

            if (ready < 0) break;
            if (ready == 0) {
                if (stop_requested()) break;
                if ((GetTickCount() - lr.last_rx) >= SERVER_SILENCE_MS) {
                    log_line("no server traffic for %ds",
                             SERVER_SILENCE_MS / 1000);
                    break;
                }
                tool_extension_idle();
                if ((GetTickCount() - last_busy) >= 5000) {
                    tool_extension_state_note();
                    last_busy = GetTickCount();
                }
                if ((GetTickCount() - last_ping) >= SERVER_PING_MS) {
                    if (!line_send(g_sock, NULL, "PING")) break;
                    last_ping = GetTickCount();
                }
                continue;
            }

            if (line_read(&lr, line, sizeof line) != 1) break;
            if (strncmp(line, "PING", 4) == 0) {
                if (!line_send(g_sock, NULL, "PONG")) break;
            } else if (strncmp(line, "PONG", 4) == 0) {
                // nothing to do
            } else if (strncmp(line, "CMD ", 4) == 0) {
                if (!event_is_quiet(line + 4) &&
                    !tool_extension_event_quiet(line + 4)) {
                    char shown[160];

                    if (!tool_extension_event_log(line + 4, shown,
                                                  sizeof shown))
                        event_for_log(line + 4, shown, sizeof shown);
                    log_line("event: %s", shown);
                }
                do_event(line + 4, &lr);
                // A rename cannot be registered on the connection it arrived
                // on, so leave the loop; it drops and dials again below.
                if (g_reconnect) break;
            }
        }

        mirror_detach();
        tool_extension_disconnect();

        // The manager asked for this one, which is not the same as losing the
        // server: no redial, no backoff, just the way out.
        if (stop_requested()) break;

        if (g_reconnect) {
            // Deliberate: a rename asked for this, and it is not a failure, so
            // it comes back at the first delay rather than at the backoff.
            g_reconnect = 0;
            log_line("reconnecting so the server sees the new name");
            backoff_ms = 1000;
            endpoint_socket_close();
            if (wait_or_stop(backoff_ms)) break;
            if (backoff_ms < 30000) backoff_ms *= 2;
            continue;
        }

        log_line("lost the server");
        endpoint_socket_close();
        if (servers_next()) {
            log_line("trying the next server, %s:%d", g_server, g_port);
            continue;
        }
        if (wait_or_stop(backoff_ms)) break;
        if (backoff_ms < 30000) backoff_ms *= 2;
    }

    endpoint_socket_close();
    tool_extension_stop();
    log_line("stopped");
    WSACleanup();
    return 0;
}
