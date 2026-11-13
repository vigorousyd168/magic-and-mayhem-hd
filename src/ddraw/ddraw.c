/* Minimal software DirectDraw for Chaos.exe (Magic & Mayhem).
 * Pretends to be a 16bpp display, keeps every surface in system memory and
 * presents the primary scaled to the window from a worker thread.
 * Build: i686-w64-mingw32-gcc -shared -O2 -s ddraw.c -o ddraw.dll -lgdi32 -luser32 -Wl,--kill-at
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __declspec(dllexport)
#define M WINAPI

/* ---- config and log ---- */
static int cfg_w, cfg_h;            /* client size override, 0 = whole monitor */
static int cfg_rw, cfg_rh;          /* game render size, 0 = same as the window */
static FILE *logf;
static int cfg_dump, cfg_script;
static char dump_dir[MAX_PATH];
static void lg(const char *fmt, ...) {
    va_list a;
    if (!logf) return;
    va_start(a, fmt); vfprintf(logf, fmt, a); va_end(a);
    fputc('\n', logf); fflush(logf);
}
static void load_cfg(void) {
    char p[MAX_PATH], *s;
    GetModuleFileNameA(GetModuleHandleA("ddraw.dll"), p, MAX_PATH);
    if ((s = strrchr(p, '\\'))) strcpy(s + 1, "ddraw.ini");
    cfg_w = GetPrivateProfileIntA("ddraw", "width", 0, p);
    cfg_h = GetPrivateProfileIntA("ddraw", "height", 0, p);
    cfg_rw = GetPrivateProfileIntA("ddraw", "renderw", 0, p);
    cfg_rh = GetPrivateProfileIntA("ddraw", "renderh", 0, p);
    cfg_dump = GetPrivateProfileIntA("ddraw", "dump", 0, p);
    cfg_script = GetPrivateProfileIntA("ddraw", "script", 0, p);
    strcpy(dump_dir, p); if ((s = strrchr(dump_dir, '\\'))) s[1] = 0;
    if (GetPrivateProfileIntA("ddraw", "log", 0, p)) {
        if ((s = strrchr(p, '\\'))) strcpy(s + 1, "ddraw.log");
        logf = fopen(p, "w");
    }
}

/* ---- objects ---- */
typedef struct Surf {
    void **vt; LONG ref;
    int w, h, pitch, bpp;
    BYTE *px;
    DWORD caps;
    int has_ck; DWORD ck;
    struct Surf *back;
    HDC dc; HBITMAP bmp, oldbmp; void *dcpx;
} Surf;
typedef struct { void **vt; LONG ref; } Obj;

static void **dd_vt, **surf_vt, **clip_vt, **pal_vt;
static Obj g_dd;
static int mode_w = 640, mode_h = 480, mode_bpp = 16;
static HWND g_hwnd;
static WNDPROC g_old;
static Surf *volatile g_primary;
static volatile LONG g_dirty;
static int view_x, view_y, view_w = 1, view_h = 1;   /* where the frame lands in the client area */
static int src_w = 640, src_h = 480;                 /* part of the primary being shown */
static int g_patched;

/* ---- presentation ---- */
static struct { BITMAPINFOHEADER h; DWORD mask[3]; } g_bmi;

static void compute_view(void) {
    RECT r; int cw, ch;
    if (!g_hwnd || !GetClientRect(g_hwnd, &r)) return;
    cw = r.right; ch = r.bottom;
    if (cw * src_h > ch * src_w) { view_h = ch; view_w = ch * src_w / src_h; }
    else                         { view_w = cw; view_h = cw * src_h / src_w; }
    view_x = (cw - view_w) / 2; view_y = (ch - view_h) / 2;
}
/* diagnostics: [ddraw] dump=1 writes shot_NNN.bmp every 2s and per-second call counts */
static int dump_n;
static volatile int trace_left;
static volatile LONG n_blt, n_prim, n_lock, n_msg;
static void dump_frame(void) {
    Surf *p = g_primary; char f[MAX_PATH]; FILE *o; int x, y, row, sz;
    BITMAPFILEHEADER fh; BITMAPINFOHEADER bh; BYTE *line;
    if (!p || p->bpp != 16) return;
    sprintf(f, "%sshot_%03d.bmp", dump_dir, dump_n++);
    if (!(o = fopen(f, "wb"))) return;
    row = (p->w * 3 + 3) & ~3; sz = row * p->h;
    memset(&fh, 0, sizeof fh); memset(&bh, 0, sizeof bh);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bh; fh.bfSize = fh.bfOffBits + sz;
    bh.biSize = sizeof bh; bh.biWidth = p->w; bh.biHeight = p->h; bh.biPlanes = 1; bh.biBitCount = 24;
    fwrite(&fh, sizeof fh, 1, o); fwrite(&bh, sizeof bh, 1, o);
    line = calloc(row, 1);
    for (y = p->h - 1; y >= 0; y--) {
        WORD *q = (WORD *)(p->px + y * p->pitch);
        for (x = 0; x < p->w; x++) {
            WORD c = q[x];
            line[x * 3] = (BYTE)((c & 31) * 255 / 31);
            line[x * 3 + 1] = (BYTE)(((c >> 5) & 63) * 255 / 63);
            line[x * 3 + 2] = (BYTE)((c >> 11) * 255 / 31);
        }
        fwrite(line, row, 1, o);
    }
    free(line); fclose(o);
}
/* diagnostics: what the window really shows, read back from its DC */
static void dump_view(void) {
    RECT r; HDC w, m; HBITMAP b, old; BITMAPINFOHEADER bh; BITMAPFILEHEADER fh; char f[MAX_PATH]; FILE *o; BYTE *px; int sz, row;
    if (!g_hwnd || !GetClientRect(g_hwnd, &r) || !r.right) return;
    w = GetDC(g_hwnd); m = CreateCompatibleDC(w);
    b = CreateCompatibleBitmap(w, r.right, r.bottom); old = SelectObject(m, b);
    BitBlt(m, 0, 0, r.right, r.bottom, w, 0, 0, SRCCOPY);
    SelectObject(m, old);
    memset(&bh, 0, sizeof bh); bh.biSize = sizeof bh; bh.biWidth = r.right; bh.biHeight = r.bottom; bh.biPlanes = 1; bh.biBitCount = 24;
    row = (r.right * 3 + 3) & ~3; sz = row * r.bottom; px = malloc(sz);
    GetDIBits(m, b, 0, r.bottom, px, (BITMAPINFO *)&bh, DIB_RGB_COLORS);
    sprintf(f, "%sview_%03d.bmp", dump_dir, dump_n);
    if ((o = fopen(f, "wb"))) {
        memset(&fh, 0, sizeof fh); fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bh; fh.bfSize = fh.bfOffBits + sz;
        fwrite(&fh, sizeof fh, 1, o); fwrite(&bh, sizeof bh, 1, o); fwrite(px, sz, 1, o); fclose(o);
    }
    free(px); DeleteObject(b); DeleteDC(m); ReleaseDC(g_hwnd, w);
}
/* Menus only paint the top-left 800x600; the battle HUD always paints the bottom-right corner. */
static int is_menu(Surf *p) {
    int x, y, lit = 0, total = 0;
    if (mode_w <= 800 && mode_h <= 600) return 0;
    for (y = p->h - 140; y < p->h; y += 3) {
        WORD *q = (WORD *)(p->px + y * p->pitch);
        for (x = p->w - 204; x < p->w; x += 3, total++) lit += q[x] != 0;
    }
    return lit * 20 < total;     /* under 5%: at most the mouse cursor is there, not the HUD */
}
/* The yes/no dialog centres itself on the screen size each time it opens, but menus only
 * show the top-left 800x600, so these two immediates follow what is on screen. */
static void dialog_follows_view(int w, int h) {
    DWORD old;
    if (!g_patched) return;
    VirtualProtect((void *)0x4c25fc, 0x32, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)0x4c25fc = 0xa0 + w - 800;
    *(DWORD *)0x4c262a = 0x1e0 + h - 600;
    VirtualProtect((void *)0x4c25fc, 0x32, old, &old);
}
static void present(void) {
    Surf *p = g_primary; HDC dc; int w, h;
    if (!p || !g_hwnd || !p->px) return;
    if (is_menu(p)) { w = 800; h = 600; } else { w = mode_w; h = mode_h; }
    if (w != src_w || h != src_h) {
        RECT r; HDC k = GetDC(g_hwnd);
        src_w = w; src_h = h;
        dialog_follows_view(w, h);
        GetClientRect(g_hwnd, &r); FillRect(k, &r, GetStockObject(BLACK_BRUSH)); ReleaseDC(g_hwnd, k);
    }
    compute_view();
    g_bmi.h.biSize = sizeof g_bmi.h; g_bmi.h.biWidth = p->pitch / 2; g_bmi.h.biHeight = -src_h;   /* only the rows shown: partial source rects are ambiguous on top-down DIBs */
    g_bmi.h.biPlanes = 1; g_bmi.h.biBitCount = 16; g_bmi.h.biCompression = BI_BITFIELDS;
    g_bmi.mask[0] = 0xF800; g_bmi.mask[1] = 0x07E0; g_bmi.mask[2] = 0x001F;
    dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, src_w == view_w ? COLORONCOLOR : HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchDIBits(dc, view_x, view_y, view_w, view_h, 0, 0, src_w, src_h, p->px,
                  (BITMAPINFO *)&g_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}
static HANDLE g_main;
static void sample_main(void) {
    CONTEXT c; DWORD *sp; int i, n = 0;
    c.ContextFlags = CONTEXT_FULL;
    SuspendThread(g_main);
    if (GetThreadContext(g_main, &c)) {
        lg("main eip=%08lx esp=%08lx", c.Eip, c.Esp);
        sp = (DWORD *)c.Esp;
        for (i = 0; i < 400 && n < 10 && !IsBadReadPtr(sp + i, 4); i++)
            if (sp[i] >= 0x401000 && sp[i] < 0x5a3000) { lg("   ret? %08lx", sp[i]); n++; }
    }
    ResumeThread(g_main);
}
static DWORD WINAPI present_thread(void *a) {
    (void)a;
    DWORD next = GetTickCount() + 1000; int sec = 0;
    for (;;) {
        Sleep(8);
        if (g_patched) *(volatile DWORD *)0x6bb37d = 1;
        if (InterlockedExchange(&g_dirty, 0)) present();
        if (GetTickCount() >= next) {
            next += 1000; sec++;
            lg("t=%d blt=%ld prim=%ld lock=%ld msg=%ld active=%d", sec, InterlockedExchange(&n_blt, 0),
               InterlockedExchange(&n_prim, 0), InterlockedExchange(&n_lock, 0), InterlockedExchange(&n_msg, 0),
               GetForegroundWindow() == g_hwnd);
            if (cfg_dump && sec <= 6 && g_main) sample_main();
            if (cfg_dump && sec % 2 == 0) { dump_view(); dump_frame(); }
            if (sec == 40) trace_left = 40;
        }
    }
    return 0;
}

/* ---- window and mouse mapping ---- */
static void to_game(LONG *x, LONG *y) {
    *x = (*x - view_x) * src_w / view_w;
    *y = (*y - view_y) * src_h / view_h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= src_w) *x = src_w - 1;
    if (*y >= src_h) *y = src_h - 1;
}
/* test automation: [ddraw] script=1 replays ddraw_script.txt with a virtual cursor
 * lines: "<ms> <x> <y>" click, "<ms> m <x> <y>" move only, "<ms> k <vk>" key press */
static volatile int fake_on, fake_x, fake_y, fake_lb, fake_rb;
static SHORT WINAPI hk_GetAsyncKeyState(int vk) {
    if (fake_on && vk == VK_LBUTTON) return fake_lb ? (SHORT)0x8000 : 0;
    if (fake_on && vk == VK_RBUTTON) return fake_rb ? (SHORT)0x8000 : 0;
    return GetAsyncKeyState(vk);
}
static SHORT WINAPI hk_GetKeyState(int vk) {
    if (fake_on && vk == VK_LBUTTON) return fake_lb ? (SHORT)0x8000 : 0;
    if (fake_on && vk == VK_RBUTTON) return fake_rb ? (SHORT)0x8000 : 0;
    return GetKeyState(vk);
}
static void post_mouse(UINT m, WPARAM w) {
    PostMessageA(g_hwnd, m, w, MAKELPARAM(view_x + fake_x * view_w / src_w, view_y + fake_y * view_h / src_h));
}
static DWORD WINAPI script_thread(void *a) {
    char line[128]; DWORD t0 = GetTickCount(); FILE *f = fopen((char *)a, "r");
    if (!f) { lg("no script"); return 0; }
    fake_on = 1;
    while (fgets(line, sizeof line, f)) {
        int ms, x, y; char c;
        if (sscanf(line, "%d %c %d %d", &ms, &c, &x, &y) == 4 && c == 'r') {
            while ((int)(GetTickCount() - t0) < ms) Sleep(10);
            fake_x = x; fake_y = y; post_mouse(WM_MOUSEMOVE, 0); Sleep(250);
            fake_rb = 1; post_mouse(WM_RBUTTONDOWN, MK_RBUTTON); Sleep(150);
            fake_rb = 0; post_mouse(WM_RBUTTONUP, 0);
        }
        else if (sscanf(line, "%d %c %d %d", &ms, &c, &x, &y) == 4 && c == 'm') { while ((int)(GetTickCount() - t0) < ms) Sleep(10); fake_x = x; fake_y = y; post_mouse(WM_MOUSEMOVE, 0); }
        else if (sscanf(line, "%d %c %d", &ms, &c, &x) == 3 && c == 'k') { while ((int)(GetTickCount() - t0) < ms) Sleep(10); PostMessageA(g_hwnd, WM_KEYDOWN, x, 1); Sleep(80); PostMessageA(g_hwnd, WM_KEYUP, x, 0xC0000001); }
        else if (sscanf(line, "%d %d %d", &ms, &x, &y) == 3) {
            while ((int)(GetTickCount() - t0) < ms) Sleep(10);
            lg("script click %d %d", x, y);
            fake_x = x; fake_y = y; post_mouse(WM_MOUSEMOVE, 0); Sleep(250);
            fake_lb = 1; post_mouse(WM_LBUTTONDOWN, MK_LBUTTON); Sleep(150);
            fake_lb = 0; post_mouse(WM_LBUTTONUP, 0);
        }
    }
    fclose(f);
    return 0;
}

static BOOL WINAPI hk_GetCursorPos(POINT *p) {
    BOOL ok;
    if (fake_on) { p->x = fake_x; p->y = fake_y; return TRUE; }
    ok = GetCursorPos(p);
    if (ok && g_hwnd) { ScreenToClient(g_hwnd, p); to_game(&p->x, &p->y); }
    return ok;
}
static BOOL WINAPI hk_SetCursorPos(int x, int y) {
    POINT p = { view_x + x * view_w / src_w, view_y + y * view_h / src_h };
    if (g_hwnd) ClientToScreen(g_hwnd, &p);
    return SetCursorPos(p.x, p.y);
}
static BOOL WINAPI hk_ClipCursor(const RECT *r) { (void)r; return TRUE; }
static BOOL WINAPI hk_ScreenToClient(HWND h, POINT *p) { (void)h; (void)p; return TRUE; }
static BOOL WINAPI hk_ClientToScreen(HWND h, POINT *p) { (void)h; (void)p; return TRUE; }
static BOOL WINAPI hk_GetClientRect(HWND h, RECT *r) {
    if (h != g_hwnd) return GetClientRect(h, r);
    r->left = r->top = 0; r->right = mode_w; r->bottom = mode_h; return TRUE;
}
static BOOL WINAPI hk_GetWindowRect(HWND h, RECT *r) {
    if (h != g_hwnd) return GetWindowRect(h, r);
    r->left = r->top = 0; r->right = mode_w; r->bottom = mode_h; return TRUE;
}

/* chaos95.dll (the Chinese font module) only draws glyphs when the screen is 640 or 800 wide */
static int WINAPI hk_GetSystemMetrics(int i) {
    if (i == SM_CXFULLSCREEN) return mode_w == 640 ? 640 : 800;
    return GetSystemMetrics(i);
}

static void patch_iat_in(HMODULE mod, const char *dll, const char *fn, void *hook);
static void patch_iat(const char *dll, const char *fn, void *hook) {
    patch_iat_in(GetModuleHandleA(NULL), dll, fn, hook);
}
static void patch_iat_in(HMODULE mod, const char *dll, const char *fn, void *hook) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    void *target = (void *)GetProcAddress(GetModuleHandleA(dll), fn);
    for (; d->Name; d++) {
        IMAGE_THUNK_DATA *t;
        if (_stricmp((char *)base + d->Name, dll)) continue;
        for (t = (IMAGE_THUNK_DATA *)(base + d->FirstThunk); t->u1.Function; t++) {
            if ((void *)t->u1.Function == target) {
                DWORD old;
                VirtualProtect(&t->u1.Function, 4, PAGE_READWRITE, &old);
                t->u1.Function = (DWORD)hook;
                VirtualProtect(&t->u1.Function, 4, old, &old);
                lg("hooked %s", fn);
            }
        }
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    InterlockedIncrement(&n_msg);
    if (m == WM_ACTIVATEAPP || m == WM_ACTIVATE || m == WM_KILLFOCUS || m == WM_SETFOCUS) lg("msg %x w=%x", m, (unsigned)w);
    /* the game pauses when it loses focus; a scripted run must keep going in the background */
    if (cfg_script && ((m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS))
        return 0;
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL) {
        LONG x = (short)LOWORD(l), y = (short)HIWORD(l);
        to_game(&x, &y);
        l = MAKELPARAM(x, y);
    }
    if (m == WM_SIZE || m == WM_ERASEBKGND) {
        RECT r; HDC dc = GetDC(h);
        GetClientRect(h, &r); FillRect(dc, &r, GetStockObject(BLACK_BRUSH)); ReleaseDC(h, dc);
        InterlockedExchange(&g_dirty, 1);
        if (m == WM_ERASEBKGND) return 1;
    }
    return CallWindowProcA(g_old, h, m, w, l);
}

static void take_window(HWND h) {
    int sw, sh, x = 0, y = 0;
    if (!h || g_hwnd == h) return;
    g_hwnd = h;
    if (cfg_w && cfg_h) {
        RECT r = { 0, 0, cfg_w, cfg_h };
        SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        sw = r.right - r.left; sh = r.bottom - r.top; x = 40; y = 40;
    } else {
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    }
    SetWindowPos(h, HWND_TOP, x, y, sw, sh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    compute_view();
    g_old = (WNDPROC)SetWindowLongA(h, GWL_WNDPROC, (LONG)wndproc);
    patch_iat("USER32.dll", "GetCursorPos", hk_GetCursorPos);
    patch_iat("USER32.dll", "SetCursorPos", hk_SetCursorPos);
    patch_iat("USER32.dll", "ClipCursor", hk_ClipCursor);
    patch_iat("USER32.dll", "ScreenToClient", hk_ScreenToClient);
    patch_iat("USER32.dll", "ClientToScreen", hk_ClientToScreen);
    patch_iat("USER32.dll", "GetClientRect", hk_GetClientRect);
    patch_iat("USER32.dll", "GetWindowRect", hk_GetWindowRect);
    if (cfg_script) {
        static char path[MAX_PATH];
        sprintf(path, "%sddraw_script.txt", dump_dir);
        patch_iat("USER32.dll", "GetAsyncKeyState", hk_GetAsyncKeyState);
        patch_iat("USER32.dll", "GetKeyState", hk_GetKeyState);
        CreateThread(NULL, 0, script_thread, path, 0, NULL);
    }
    if (GetModuleHandleA("chaos95.dll"))
        patch_iat_in(GetModuleHandleA("chaos95.dll"), "USER32.dll", "GetSystemMetrics", hk_GetSystemMetrics);
    CreateThread(NULL, 0, present_thread, NULL, 0, NULL);
}

/* ---- surfaces ---- */
static Surf *surf_new(int w, int h, int bpp, DWORD caps) {
    Surf *s = calloc(1, sizeof *s);
    s->vt = surf_vt; s->ref = 1; s->w = w; s->h = h; s->bpp = bpp; s->caps = caps;
    s->pitch = ((w * bpp / 8) + 3) & ~3;
    s->px = calloc(s->pitch, h ? h : 1);
    return s;
}
static void fill_pf(DDPIXELFORMAT *pf, int bpp) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    if (bpp == 8) { pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8; pf->dwRGBBitCount = 8; return; }
    pf->dwFlags = DDPF_RGB; pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F;
}
static void fill_desc(Surf *s, DDSURFACEDESC *d) {
    DWORD size = d->dwSize ? d->dwSize : sizeof *d;
    memset(d, 0, size);
    d->dwSize = size;
    d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwHeight = s->h; d->dwWidth = s->w; d->lPitch = s->pitch;
    d->ddsCaps.dwCaps = s->caps;
    fill_pf(&d->ddpfPixelFormat, s->bpp);
}
static void mark(Surf *s) { if (s == g_primary) InterlockedExchange(&g_dirty, 1); }
static BYTE *at(Surf *s, int x, int y) { return s->px + y * s->pitch + x * (s->bpp / 8); }

static void fill_rect(Surf *d, RECT r, DWORD c) {
    int x, y;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > d->w) r.right = d->w;
    if (r.bottom > d->h) r.bottom = d->h;
    for (y = r.top; y < r.bottom; y++) {
        if (d->bpp == 16) { WORD *p = (WORD *)at(d, r.left, y); for (x = r.left; x < r.right; x++) *p++ = (WORD)c; }
        else memset(at(d, r.left, y), (int)c, r.right - r.left);
    }
}
static void blit(Surf *d, RECT dr, Surf *s, RECT sr, int key, DWORD ck) {
    int dw = dr.right - dr.left, dh = dr.bottom - dr.top;
    int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
    int x, y, bytes = d->bpp / 8;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || d->bpp != s->bpp) return;
    if (dw == sw && dh == sh) {                       /* clip 1:1 copies */
        if (dr.left < 0) { sr.left -= dr.left; dr.left = 0; }
        if (dr.top < 0) { sr.top -= dr.top; dr.top = 0; }
        if (sr.left < 0) { dr.left -= sr.left; sr.left = 0; }
        if (sr.top < 0) { dr.top -= sr.top; sr.top = 0; }
        dw = min(dr.right, d->w) - dr.left; dh = min(dr.bottom, d->h) - dr.top;
        dw = min(dw, s->w - sr.left); dh = min(dh, s->h - sr.top);
        if (dw <= 0 || dh <= 0) return;
        if (!key) {
            int overlap = d == s && dr.top > sr.top;
            for (y = 0; y < dh; y++) {
                int yy = overlap ? dh - 1 - y : y;
                memmove(at(d, dr.left, dr.top + yy), at(s, sr.left, sr.top + yy), dw * bytes);
            }
        } else if (bytes == 2) {
            for (y = 0; y < dh; y++) {
                WORD *a = (WORD *)at(d, dr.left, dr.top + y), *b = (WORD *)at(s, sr.left, sr.top + y);
                for (x = 0; x < dw; x++, a++, b++) if (*b != (WORD)ck) *a = *b;
            }
        } else {
            for (y = 0; y < dh; y++) {
                BYTE *a = at(d, dr.left, dr.top + y), *b = at(s, sr.left, sr.top + y);
                for (x = 0; x < dw; x++, a++, b++) if (*b != (BYTE)ck) *a = *b;
            }
        }
        return;
    }
    for (y = 0; y < dh; y++) {                        /* stretched, nearest neighbour */
        int dy = dr.top + y, sy = sr.top + y * sh / dh;
        if (dy < 0 || dy >= d->h || sy < 0 || sy >= s->h) continue;
        for (x = 0; x < dw; x++) {
            int dx = dr.left + x, sx = sr.left + x * sw / dw;
            if (dx < 0 || dx >= d->w || sx < 0 || sx >= s->w) continue;
            if (bytes == 2) { WORD c = *(WORD *)at(s, sx, sy); if (!key || c != (WORD)ck) *(WORD *)at(d, dx, dy) = c; }
            else { BYTE c = *at(s, sx, sy); if (!key || c != (BYTE)ck) *at(d, dx, dy) = c; }
        }
    }
}

/* ---- battle minimap ("scanner") at twice the size ----
 * The scanner draws itself straight into the locked back buffer, centred at x = W - mapW - 8
 * from row 6, edges included. Its lock is redirected to a copy of the back buffer; on Unlock
 * every pixel it changed goes back at 2x, anchored to the top-right corner (x -> 2x - W, y -> 2y).
 * Its mouse rect is doubled to match and the handler gets the coordinates halved back. */
static BYTE *scan_buf, *scan_bb;
static size_t scan_size;
static int scan_pending, scan_x0, scan_rows;
static void *__fastcall scan_lock(void *self) {
    BYTE *p = ((BYTE *(__fastcall *)(void *))0x56fdb0)(self);
    int pitch = *(int *)0x6816a0, mw = *(int *)0x6a213c, mh = *(int *)0x6a2140, y;
    if (!p || pitch <= 0) return p;
    if ((size_t)pitch * mode_h > scan_size) {
        free(scan_buf); scan_size = (size_t)pitch * mode_h;
        if (!(scan_buf = malloc(scan_size))) { scan_size = 0; return p; }
    }
    scan_x0 = max(0, mode_w - 2 * mw - 48);
    scan_rows = min(mode_h / 2, mh + 40);
    for (y = 0; y < scan_rows; y++)
        memcpy(scan_buf + y * pitch + scan_x0 * 2, p + y * pitch + scan_x0 * 2, (mode_w - scan_x0) * 2);
    scan_bb = p; scan_pending = 1;
    return scan_buf;
}
static void scan_compose(Surf *t) {
    static BYTE *mask; static size_t mask_size;
    int pw = t->pitch / 2, W = t->w, cols = W - scan_x0, x, y;
    WORD *bb = (WORD *)t->px, *pv = (WORD *)scan_buf;
    if ((size_t)cols * scan_rows > mask_size) {
        free(mask); mask_size = (size_t)cols * scan_rows;
        if (!(mask = malloc(mask_size))) { mask_size = 0; return; }
    }
    for (y = 0; y < scan_rows; y++)
        for (x = scan_x0; x < W; x++)
            mask[y * cols + x - scan_x0] = pv[y * pw + x] != bb[y * pw + x];
    for (y = 0; y < scan_rows; y++)
        for (x = scan_x0; x < W; x++)
            if (mask[y * cols + x - scan_x0]) {
                WORD c = pv[y * pw + x], *d = bb + 2 * y * pw + 2 * x - W;
                d[0] = d[1] = d[pw] = d[pw + 1] = c;
            }
}
static int __fastcall scan_register(BYTE *self, int edx, int *rect, int b) {
    int r = ((int (__fastcall *)(void *, int, int *, int))0x49b630)(self, edx, rect, b);
    int *q = (int *)(self + 0x11);      /* registered copy: left, top, right, bottom */
    q[0] = 2 * q[0] - mode_w; q[1] *= 2; q[2] = 2 * q[2] - mode_w; q[3] *= 2;
    return r;
}
void scan_fix_xy(int *p) { p[0] = (p[0] + mode_w) / 2; p[1] /= 2; }
extern void scan_mouse(void);
/* the handler is thiscall (flags, 40-byte mouse record starting with x, y): halve x, y and go on */
__asm__(".text\n_scan_mouse:\n"
        "  push %ecx\n  lea 12(%esp), %eax\n  push %eax\n  call _scan_fix_xy\n"
        "  add $4, %esp\n  pop %ecx\n  mov $0x53f3c0, %eax\n  jmp *%eax\n");

static HRESULT M s_QI(Surf *t, REFIID i, void **o) {
    if (i->Data1 != 0 && i->Data1 != 0x6C14DB81 && i->Data1 != 0x57805885 && i->Data1 != 0xDA044E00 && i->Data1 != 0x0B2B8630) { *o = NULL; return E_NOINTERFACE; }
    t->ref++; *o = t; return 0;
}
static ULONG M s_AddRef(Surf *t) { return ++t->ref; }
static ULONG M s_Release(Surf *t) {
    if (t->ref <= 0) { lg("double release %p %dx%d", t, t->w, t->h); return 0; }
    if (--t->ref > 0) return t->ref;
    lg("free surface %p %dx%d", t, t->w, t->h);
    if (t == g_primary) g_primary = NULL;
    free(t->px); t->px = NULL;   /* header stays: the game releases some surfaces twice */
    return 0;
}
#define STUB(n, ...) static HRESULT M n(Surf *t, __VA_ARGS__) { (void)t; lg("stub " #n); return 0; }
#define STUB0(n) static HRESULT M n(Surf *t) { (void)t; lg("stub " #n); return 0; }
STUB(s_AddAtt, void *a) STUB(s_AddOv, RECT *a) STUB(s_BltBatch, void *a, DWORD b, DWORD c)
STUB(s_DelAtt, DWORD a, void *b) STUB(s_EnumAtt, void *a, void *b) STUB(s_EnumOv, DWORD a, void *b, void *c)
STUB(s_GetClipper, void **a) STUB(s_GetOvPos, LONG *a, LONG *b) STUB(s_GetPal, void **a)
STUB(s_Init, void *a, void *b) STUB(s_SetOvPos, LONG a, LONG b)
STUB(s_UpdOv, RECT *a, void *b, RECT *c, DWORD d, void *e) STUB(s_UpdOvD, DWORD a)
STUB(s_UpdOvZ, DWORD a, void *b) STUB(s_SetDesc, void *a, DWORD b)
STUB(s_SetPriv, REFGUID a, void *b, DWORD c, DWORD d) STUB(s_GetPriv, REFGUID a, void *b, DWORD *c)
STUB(s_FreePriv, REFGUID a) STUB(s_GetUniq, DWORD *a) STUB0(s_ChgUniq)
static HRESULT M s_Blt(Surf *d, RECT *dr, Surf *s, RECT *sr, DWORD fl, DDBLTFX *fx) {
    RECT a, b;
    InterlockedIncrement(&n_blt); if (d == g_primary) InterlockedIncrement(&n_prim);
    if (trace_left > 0) { trace_left--; lg("Blt d=%p(%dx%d) s=%p(%dx%d) fl=%lx dr=%ld,%ld,%ld,%ld sr=%ld,%ld,%ld,%ld", d, d->w, d->h, s, s ? s->w : 0, s ? s->h : 0, fl, dr?dr->left:-1, dr?dr->top:-1, dr?dr->right:-1, dr?dr->bottom:-1, sr?sr->left:-1, sr?sr->top:-1, sr?sr->right:-1, sr?sr->bottom:-1); }
    if (dr) a = *dr; else SetRect(&a, 0, 0, d->w, d->h);
    if (fl & DDBLT_COLORFILL) { fill_rect(d, a, fx->dwFillColor); mark(d); return 0; }
    if (!s) return 0;
    if (sr) b = *sr; else SetRect(&b, 0, 0, s->w, s->h);
    if (fl & DDBLT_KEYSRCOVERRIDE) blit(d, a, s, b, 1, fx->ddckSrcColorkey.dwColorSpaceLowValue);
    else blit(d, a, s, b, (fl & DDBLT_KEYSRC) && s->has_ck, s->ck);
    mark(d);
    return 0;
}
static HRESULT M s_BltFast(Surf *d, DWORD x, DWORD y, Surf *s, RECT *sr, DWORD fl) {
    RECT a, b;
    InterlockedIncrement(&n_blt); if (d == g_primary) InterlockedIncrement(&n_prim);
    if (trace_left > 0) { trace_left--; lg("BltFast d=%p(%dx%d) s=%p(%dx%d) fl=%lx at %lu,%lu sr=%ld,%ld,%ld,%ld ck=%d,%lx", d, d->w, d->h, s, s->w, s->h, fl, x, y, sr?sr->left:-1, sr?sr->top:-1, sr?sr->right:-1, sr?sr->bottom:-1, s->has_ck, s->ck); }
    if (sr) b = *sr; else SetRect(&b, 0, 0, s->w, s->h);
    SetRect(&a, x, y, x + (b.right - b.left), y + (b.bottom - b.top));
    blit(d, a, s, b, (fl & DDBLTFAST_SRCCOLORKEY) && s->has_ck, s->ck);
    mark(d);
    return 0;
}
static HRESULT M s_Flip(Surf *t, Surf *o, DWORD f) {
    (void)o; (void)f;
    if (t->back) { BYTE *p = t->px; t->px = t->back->px; t->back->px = p; }
    mark(t);
    return 0;
}
static HRESULT M s_GetAtt(Surf *t, DDSCAPS *c, Surf **o) {
    (void)c;
    if (!t->back) return DDERR_NOTFOUND;
    t->back->ref++; *o = t->back; return 0;
}
static HRESULT M s_GetBltStatus(Surf *t, DWORD f) { (void)t; (void)f; return 0; }
static HRESULT M s_GetFlipStatus(Surf *t, DWORD f) { (void)t; (void)f; return 0; }
static HRESULT M s_GetCaps(Surf *t, DDSCAPS *c) { c->dwCaps = t->caps; return 0; }
static HRESULT M s_GetColorKey(Surf *t, DWORD f, DDCOLORKEY *k) {
    (void)f; if (!t->has_ck) return DDERR_NOCOLORKEY;
    k->dwColorSpaceLowValue = k->dwColorSpaceHighValue = t->ck; return 0;
}
static HRESULT M s_SetColorKey(Surf *t, DWORD f, DDCOLORKEY *k) {
    (void)f;
    if (!k) { t->has_ck = 0; return 0; }
    t->has_ck = 1; t->ck = k->dwColorSpaceLowValue; return 0;
}
static HRESULT M s_GetDC(Surf *t, HDC *o) {
    struct { BITMAPINFOHEADER h; DWORD m[3]; } b;
    memset(&b, 0, sizeof b);
    b.h.biSize = sizeof b.h; b.h.biWidth = t->pitch / 2; b.h.biHeight = -t->h; b.h.biPlanes = 1;
    b.h.biBitCount = 16; b.h.biCompression = BI_BITFIELDS;
    b.m[0] = 0xF800; b.m[1] = 0x07E0; b.m[2] = 0x001F;
    t->dc = CreateCompatibleDC(NULL);
    t->bmp = CreateDIBSection(t->dc, (BITMAPINFO *)&b, DIB_RGB_COLORS, &t->dcpx, NULL, 0);
    t->oldbmp = SelectObject(t->dc, t->bmp);
    memcpy(t->dcpx, t->px, t->pitch * t->h);
    *o = t->dc;
    return 0;
}
static HRESULT M s_ReleaseDC(Surf *t, HDC h) {
    (void)h;
    GdiFlush();
    memcpy(t->px, t->dcpx, t->pitch * t->h);
    SelectObject(t->dc, t->oldbmp); DeleteObject(t->bmp); DeleteDC(t->dc);
    t->dc = NULL; mark(t);
    return 0;
}
static HRESULT M s_GetPF(Surf *t, DDPIXELFORMAT *pf) { fill_pf(pf, t->bpp); return 0; }
static HRESULT M s_GetDesc(Surf *t, DDSURFACEDESC *d) { fill_desc(t, d); return 0; }
static HRESULT M s_IsLost(Surf *t) { (void)t; return 0; }
static HRESULT M s_Restore(Surf *t) { (void)t; return 0; }
static HRESULT M s_Lock(Surf *t, RECT *r, DDSURFACEDESC *d, DWORD f, HANDLE e) {
    (void)f; (void)e;
    InterlockedIncrement(&n_lock);
    if (trace_left > 0) { trace_left--; lg("Lock %p(%dx%d) rect=%d", t, t->w, t->h, r != NULL); }
    fill_desc(t, d);
    d->dwFlags |= DDSD_LPSURFACE;
    d->lpSurface = r ? at(t, r->left, r->top) : t->px;
    return 0;
}
static HRESULT M s_Unlock(Surf *t, void *r) {
    (void)r;
    if (scan_pending && t->px == scan_bb) { scan_pending = 0; scan_compose(t); }
    mark(t); return 0;
}
static HRESULT M s_SetClipper(Surf *t, void *c) { (void)t; (void)c; return 0; }
static HRESULT M s_SetPal(Surf *t, void *c) { (void)t; (void)c; return 0; }
static HRESULT M s_GetDD(Surf *t, void **o) { (void)t; g_dd.ref++; *o = &g_dd; return 0; }
static HRESULT M s_PageLock(Surf *t, DWORD f) { (void)t; (void)f; return 0; }

static void *surf_vtbl[] = {
    s_QI, s_AddRef, s_Release, s_AddAtt, s_AddOv, s_Blt, s_BltBatch, s_BltFast, s_DelAtt, s_EnumAtt,
    s_EnumOv, s_Flip, s_GetAtt, s_GetBltStatus, s_GetCaps, s_GetClipper, s_GetColorKey, s_GetDC,
    s_GetFlipStatus, s_GetOvPos, s_GetPal, s_GetPF, s_GetDesc, s_Init, s_IsLost, s_Lock, s_ReleaseDC,
    s_Restore, s_SetClipper, s_SetColorKey, s_SetOvPos, s_SetPal, s_Unlock, s_UpdOv, s_UpdOvD, s_UpdOvZ,
    s_GetDD, s_PageLock, s_PageLock, s_SetDesc, s_SetPriv, s_GetPriv, s_FreePriv, s_GetUniq, s_ChgUniq,
};

/* ---- clipper and palette: accepted and ignored, Blt already clips to the surface ---- */
static HRESULT M o_QI(Obj *t, REFIID i, void **o) { (void)i; t->ref++; *o = t; return 0; }
static ULONG M o_AddRef(Obj *t) { return ++t->ref; }
static ULONG M o_Release(Obj *t) { if (--t->ref > 0) return t->ref; if (t != &g_dd) free(t); return 0; }
static HRESULT M c_GetList(Obj *t, RECT *a, void *b, DWORD *c) { (void)t; (void)a; (void)b; if (c) *c = 0; return 0; }
static HRESULT M c_GetHWnd(Obj *t, HWND *h) { (void)t; *h = g_hwnd; return 0; }
static HRESULT M c_Init(Obj *t, void *a, DWORD b) { (void)t; (void)a; (void)b; return 0; }
static HRESULT M c_Changed(Obj *t, BOOL *b) { (void)t; *b = FALSE; return 0; }
static HRESULT M c_SetList(Obj *t, void *a, DWORD b) { (void)t; (void)a; (void)b; return 0; }
static HRESULT M c_SetHWnd(Obj *t, DWORD a, HWND b) { (void)t; (void)a; (void)b; return 0; }
static void *clip_vtbl[] = { o_QI, o_AddRef, o_Release, c_GetList, c_GetHWnd, c_Init, c_Changed, c_SetList, c_SetHWnd };
static HRESULT M p_GetCaps(Obj *t, DWORD *c) { (void)t; *c = DDPCAPS_8BIT; return 0; }
static HRESULT M p_Get(Obj *t, DWORD f, DWORD b, DWORD n, PALETTEENTRY *e) { (void)t; (void)f; (void)b; memset(e, 0, n * 4); return 0; }
static HRESULT M p_Init(Obj *t, void *a, DWORD b, void *c) { (void)t; (void)a; (void)b; (void)c; return 0; }
static HRESULT M p_Set(Obj *t, DWORD f, DWORD b, DWORD n, void *e) { (void)t; (void)f; (void)b; (void)n; (void)e; return 0; }
static void *pal_vtbl[] = { o_QI, o_AddRef, o_Release, p_GetCaps, p_Get, p_Init, p_Set };

static Obj *obj_new(void **vt) { Obj *o = calloc(1, sizeof *o); o->vt = vt; o->ref = 1; return o; }

/* ---- IDirectDraw / 2 / 4 ---- */
static HRESULT M d_QI(Obj *t, REFIID i, void **o) {
    lg("DD QI %08lx", i->Data1);
    if (i->Data1 != 0 && i->Data1 != 0x6C14DB80 && i->Data1 != 0xB3A6F3E0 && i->Data1 != 0x9C59509A) { *o = NULL; return E_NOINTERFACE; }
    t->ref++; *o = t; return 0;
}
static HRESULT M d_Compact(Obj *t) { (void)t; return 0; }
static HRESULT M d_CreateClipper(Obj *t, DWORD f, void **o, void *u) { (void)t; (void)f; (void)u; *o = obj_new(clip_vt); return 0; }
static HRESULT M d_CreatePalette(Obj *t, DWORD f, void *e, void **o, void *u) { (void)t; (void)f; (void)e; (void)u; *o = obj_new(pal_vt); return 0; }
static HRESULT M d_CreateSurface(Obj *t, DDSURFACEDESC *d, Surf **o, void *u) {
    DWORD caps = (d->dwFlags & DDSD_CAPS) ? d->ddsCaps.dwCaps : 0;
    int bpp = mode_bpp;
    (void)t; (void)u;
    if ((d->dwFlags & DDSD_PIXELFORMAT) && d->ddpfPixelFormat.dwRGBBitCount) bpp = d->ddpfPixelFormat.dwRGBBitCount;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        Surf *s = surf_new(mode_w, mode_h, mode_bpp, caps);
        if ((d->dwFlags & DDSD_BACKBUFFERCOUNT) && d->dwBackBufferCount)
            s->back = surf_new(mode_w, mode_h, mode_bpp, DDSCAPS_BACKBUFFER | DDSCAPS_OFFSCREENPLAIN);
        g_primary = s; *o = s;
        lg("CreateSurface primary %dx%d back=%d", s->w, s->h, s->back != NULL);
        return 0;
    }
    *o = surf_new(d->dwWidth, d->dwHeight, bpp, caps);
    lg("CreateSurface %lux%lu bpp=%d caps=%lx flags=%lx -> %p", d->dwWidth, d->dwHeight, bpp, caps, d->dwFlags, *o);
    return 0;
}
static HRESULT M d_Dup(Obj *t, Surf *a, Surf **b) { (void)t; a->ref++; *b = a; return 0; }
typedef HRESULT (WINAPI *ModeCb)(DDSURFACEDESC *, void *);
static HRESULT M d_EnumModes(Obj *t, DWORD f, void *x, void *ctx, ModeCb cb) {
    static const int m[][2] = { {640, 480}, {800, 600} };
    int i;
    (void)t; (void)f; (void)x;
    for (i = 0; i < 2; i++) {
        DDSURFACEDESC d; memset(&d, 0, sizeof d); d.dwSize = sizeof d;
        d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        d.dwWidth = m[i][0]; d.dwHeight = m[i][1]; d.lPitch = m[i][0] * 2;
        fill_pf(&d.ddpfPixelFormat, 16);
        if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
    }
    return 0;
}
static HRESULT M d_EnumSurf(Obj *t, DWORD f, void *a, void *b, void *c) { (void)t; (void)f; (void)a; (void)b; (void)c; return 0; }
static HRESULT M d_FlipToGDI(Obj *t) { (void)t; return 0; }
static HRESULT M d_GetCaps(Obj *t, DDCAPS *a, DDCAPS *b) {
    (void)t;
    if (a) { DWORD s = a->dwSize; memset(a, 0, s); a->dwSize = s; a->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY; a->dwVidMemTotal = a->dwVidMemFree = 64 << 20; }
    if (b) { DWORD s = b->dwSize; memset(b, 0, s); b->dwSize = s; }
    return 0;
}
static HRESULT M d_GetMode(Obj *t, DDSURFACEDESC *d) {
    DWORD s = d->dwSize ? d->dwSize : sizeof *d;
    (void)t; memset(d, 0, s); d->dwSize = s;
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    d->dwWidth = mode_w; d->dwHeight = mode_h; d->lPitch = mode_w * 2;
    fill_pf(&d->ddpfPixelFormat, mode_bpp);
    return 0;
}
static HRESULT M d_FourCC(Obj *t, DWORD *a, DWORD *b) { (void)t; (void)b; *a = 0; return 0; }
static HRESULT M d_GDISurf(Obj *t, Surf **o) { (void)t; if (!g_primary) return DDERR_NOTFOUND; g_primary->ref++; *o = g_primary; return 0; }
static HRESULT M d_MonFreq(Obj *t, DWORD *a) { (void)t; *a = 60; return 0; }
static HRESULT M d_ScanLine(Obj *t, DWORD *a) { (void)t; *a = 0; return 0; }
static HRESULT M d_VBlank(Obj *t, BOOL *a) { (void)t; *a = TRUE; return 0; }
static HRESULT M d_Init(Obj *t, void *a) { (void)t; (void)a; return 0; }
static HRESULT M d_RestoreMode(Obj *t) { (void)t; return 0; }
static HRESULT M d_SetCoop(Obj *t, HWND h, DWORD f) {
    (void)t;
    lg("SetCooperativeLevel %p %lx", h, f);
    if (f & DDSCL_FULLSCREEN) take_window(h);
    return 0;
}
static HRESULT M d_SetMode2(Obj *t, DWORD w, DWORD h, DWORD bpp, DWORD r, DWORD f) {
    (void)t; (void)r; (void)f;
    lg("SetDisplayMode %lu %lu %lu", w, h, bpp);
    mode_w = w; mode_h = h; mode_bpp = bpp;
    compute_view();
    return 0;
}
static HRESULT M d_SetMode1(Obj *t, DWORD w, DWORD h, DWORD bpp) { return d_SetMode2(t, w, h, bpp, 0, 0); }
static HRESULT M d_WaitVB(Obj *t, DWORD f, HANDLE e) { (void)t; (void)f; (void)e; Sleep(1); return 0; }
static HRESULT M d_VidMem(Obj *t, void *c, DWORD *a, DWORD *b) { (void)t; (void)c; if (a) *a = 64 << 20; if (b) *b = 64 << 20; return 0; }
static HRESULT M d_FromDC(Obj *t, HDC h, void **o) { (void)t; (void)h; (void)o; return DDERR_NOTFOUND; }
static HRESULT M d_RestoreAll(Obj *t) { (void)t; return 0; }
static HRESULT M d_TestCoop(Obj *t) { (void)t; return 0; }
static HRESULT M d_DevId(Obj *t, void *a, DWORD f) { (void)t; (void)f; memset(a, 0, 1060); return 0; }

/* Identical layout for all versions; only SetDisplayMode differs between v1 and v2+.
 * Chaos.exe talks through IDirectDraw2, and v1 objects never call it directly. */
static void *dd_vtbl[] = {
    d_QI, o_AddRef, o_Release, d_Compact, d_CreateClipper, d_CreatePalette, d_CreateSurface, d_Dup,
    d_EnumModes, d_EnumSurf, d_FlipToGDI, d_GetCaps, d_GetMode, d_FourCC, d_GDISurf, d_MonFreq,
    d_ScanLine, d_VBlank, d_Init, d_RestoreMode, d_SetCoop, d_SetMode2, d_WaitVB, d_VidMem,
    d_FromDC, d_RestoreAll, d_TestCoop, d_DevId,
};

__declspec(dllexport) HRESULT WINAPI DirectDrawCreate(GUID *g, LPDIRECTDRAW *o, IUnknown *u) {
    (void)g; (void)u;
    (void)d_SetMode1;
    g_dd.vt = dd_vt; g_dd.ref = 1; *o = (LPDIRECTDRAW)&g_dd;
    lg("DirectDrawCreate");
    return 0;
}
__declspec(dllexport) HRESULT WINAPI DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) { (void)cb; (void)ctx; return 0; }
__declspec(dllexport) HRESULT WINAPI DirectDrawCreateClipper(DWORD f, LPDIRECTDRAWCLIPPER *o, IUnknown *u) { (void)f; (void)u; *o = (LPDIRECTDRAWCLIPPER)obj_new(clip_vt); return 0; }

/* Hooks for the 2x minimap (see scan_lock). The icon column under it moves down 116 pixels,
 * from y=100 to 216, below the doubled scanner rect of the largest (100x100) maps: the
 * column's position (set at 0x4cbb51, again at 0x4cbc4b) and the icons' hit rects
 * (top 100, bottom 136, next row while bottom <= 280, at 0x4cb8d4..0x4cb9dd). */
static void patch_minimap(void) {
    static const struct { DWORD addr, old; } icons[] = {
        {0x4cbb54, 100}, {0x4cbc4e, 100}, {0x4cb8d8, 100}, {0x4cb8eb, 136}, {0x4cb9ae, 280}, {0x4cb9c8, 100}, {0x4cb9dd, 136},
    };
    BYTE *call = (BYTE *)0x53f088; DWORD *vt = (DWORD *)0x5a49c4, old; size_t i;
    int ok = call[0] == 0xe8 && *(DWORD *)(call + 1) == 0x56fdb0 - 0x53f08d && vt[0] == 0x53f3c0 && vt[2] == 0x49b630;
    for (i = 0; i < sizeof icons / sizeof *icons; i++) ok = ok && *(DWORD *)icons[i].addr == icons[i].old;
    if (!ok) { lg("minimap: unexpected code, not patched"); return; }
    VirtualProtect((void *)0x4cb000, 0x75000, PAGE_EXECUTE_READWRITE, &old);
    *(DWORD *)(call + 1) = (DWORD)scan_lock - 0x53f08d;
    for (i = 0; i < sizeof icons / sizeof *icons; i++) *(DWORD *)icons[i].addr = icons[i].old + 116;
    VirtualProtect((void *)0x4cb000, 0x75000, old, &old);
    VirtualProtect(vt, 12, PAGE_READWRITE, &old);
    vt[0] = (DWORD)scan_mouse; vt[2] = (DWORD)scan_register;
    VirtualProtect(vt, 12, old, &old);
}

#include "patches.h"
/* Moves the game's high-res screen size to the render size by editing immediates in memory. */
static void apply_patches(void) {
    int W = cfg_rw ? cfg_rw : cfg_w ? cfg_w : GetSystemMetrics(SM_CXSCREEN);
    int H = cfg_rh ? cfg_rh : cfg_h ? cfg_h : GetSystemMetrics(SM_CYSCREEN);
    int dW = W - 800, dH = H - 600; size_t i; DWORD old;
    int grow = (2 * dH + dW) / 8 + 1;                 /* extra cells in the 45-degree occlusion grid */
    int rows = 339 + grow, pitch = 43 + grow / 8 + 1;
    char exe[MAX_PATH], *s;
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    s = strrchr(exe, 0x5c);
    if (!s || _stricmp(s + 1, "Chaos.exe")) return;
    for (i = 0; i < sizeof patches / sizeof *patches; i++)
        if (*(DWORD *)patches[i].addr != patches[i].old) { lg("patch mismatch at %08lx: %08lx", patches[i].addr, *(DWORD *)patches[i].addr); return; }
    VirtualProtect((void *)0x401000, 0x1a2000, PAGE_EXECUTE_READWRITE, &old);
    for (i = 0; i < sizeof patches / sizeof *patches; i++) {
        DWORD *p = (DWORD *)patches[i].addr, v = 0;
        switch (patches[i].kind) {
        case K_ADD_DW: v = patches[i].old + dW; break;
        case K_ADD_DH: v = patches[i].old + dH; break;
        case K_ADD_DW2: v = patches[i].old + dW / 2; break;
        case K_ADD_DH2: v = patches[i].old + dH / 2; break;
        case K_SET_W: v = W; break;
        case K_SET_W2: v = W / 2; break;
        case K_SET_H: v = H; break;
        case K_SET_H_140: v = H - 140; break;
        case K_OCC_ROWS: v = rows; break;
        case K_OCC_BITS: v = pitch * 8; break;
        case K_OCC_PITCH: v = pitch; break;
        case K_OCC_TOTAL: v = rows * pitch * 8; break;
        case K_OCC_BYTES: v = rows * pitch; break;
        case K_SUB_3: v = patches[i].old - 3; break;
        }
        *p = v;
    }
    VirtualProtect((void *)0x401000, 0x1a2000, old, &old);
    g_patched = 1;
    lg("patched %d sites for %dx%d", (int)i, W, H);
    patch_minimap();
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *e) {
    static int n;
    CONTEXT *c = e->ContextRecord;
    DWORD *sp = (DWORD *)c->Esp; int i;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || n++ > 3) return EXCEPTION_CONTINUE_SEARCH;
    lg("AV eip=%08lx addr=%08lx write=%lu eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
       c->Eip, e->ExceptionRecord->ExceptionInformation[1], e->ExceptionRecord->ExceptionInformation[0],
       c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    for (i = 0; i < 600 && !IsBadReadPtr(sp + i, 4); i++)
        if ((sp[i] >= 0x401000 && sp[i] < 0x5a3000) || (sp[i] >= (DWORD)GetModuleHandleA("ddraw.dll") && sp[i] < (DWORD)GetModuleHandleA("ddraw.dll") + 0x10000))
            lg("  [esp+%03x] %08lx", i * 4, sp[i]);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, void *x) {
    (void)h; (void)x;
    if (r == DLL_PROCESS_ATTACH) {
        dd_vt = dd_vtbl; surf_vt = surf_vtbl; clip_vt = clip_vtbl; pal_vt = pal_vtbl;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
        load_cfg();
        if (logf) AddVectoredExceptionHandler(1, on_exception);
        apply_patches();
    }
    return TRUE;
}
