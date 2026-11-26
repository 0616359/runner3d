/* ============================================================
 *  Dusk Runner 3D   (第三人称 3D 跑酷)
 *  Pure C + Win32 GDI software 3D renderer. No external assets.
 *
 *  Build (TCC):
 *    tcc runner3d.c -lgdi32 -luser32 -Wl,-subsystem=windows -o runner3d.exe
 *  Build (MSVC):
 *    cl /nologo /utf-8 runner3d.c user32.lib gdi32.lib /link /SUBSYSTEM:WINDOWS
 *
 *  Controls:
 *    A / Left, D / Right  -> strafe
 *    Space / Up           -> jump
 *    R                    -> restart
 *    Esc                  -> quit
 * ============================================================ */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* TCC minimal winapi lacks winnls.h */
#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
int WINAPI MultiByteToWideChar(unsigned int cp, unsigned long flags,
                               const char *mb, int cb, wchar_t *wc, int cch);

/* ---------------- config ---------------- */
#define WIN_W      1024
#define WIN_H      640
#define PI_        3.14159265358979
#define ZFAR       70.0
#define SEG        4.0
#define TRACK_HALF 4.2
#define LANE_W     2.2
#define FOCAL      560.0
#define CX         (WIN_W / 2)
#define CY         (WIN_H * 54 / 100)

/* ---------------- math ---------------- */
typedef struct { double x, y, z; } V3;
static V3 v3(double x, double y, double z) { V3 p; p.x = x; p.y = y; p.z = z; return p; }
static V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 vmul(V3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static double vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static V3 vnorm(V3 a) {
    double l = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    if (l < 1e-9) return v3(0, 1, 0);
    return v3(a.x / l, a.y / l, a.z / l);
}

/* ---------------- color ---------------- */
static int cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
static COLORREF mulc(COLORREF c, double s) {
    return RGB(cl((int)(GetRValue(c) * s)), cl((int)(GetGValue(c) * s)), cl((int)(GetBValue(c) * s)));
}
static COLORREF lerpc(COLORREF a, COLORREF b, double t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t),
               (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}

/* ---------------- camera ---------------- */
static double g_px = 0, g_py = 0, g_vy = 0;   /* player x, height, vertical vel */
static V3 g_camPos, g_camF, g_camR, g_camU;
static V3 g_light;
static double g_focal = FOCAL, g_cx = CX, g_cy = CY;

static void setupCamera(void) {
    V3 target = v3(g_px * 0.5, 1.05 + g_py * 0.15, 6.0);
    V3 up = v3(0, 1, 0);
    g_camPos = v3(g_px * 0.25, 3.45 + g_py * 0.12, -7.6);
    g_camF = vnorm(vsub(target, g_camPos));
    g_camR = vnorm(vcross(up, g_camF));
    g_camU = vcross(g_camF, g_camR);
}
static int project(V3 p, double *sx, double *sy, double *sz) {
    V3 d = vsub(p, g_camPos);
    double z = vdot(d, g_camF);
    double x, y;
    if (z < 0.25) return 0;
    x = vdot(d, g_camR); y = vdot(d, g_camU);
    *sx = g_cx + x / z * g_focal;
    *sy = g_cy - y / z * g_focal;
    *sz = z;
    return 1;
}

/* ---------------- polygon buffer ---------------- */
typedef struct { POINT p[4]; int n; COLORREF col; double depth; } Poly;
static Poly g_poly[1200];
static int g_pn = 0;

#define FOGCOL RGB(238, 188, 150)

static COLORREF shadeFog(COLORREF base, V3 n, double depth) {
    double d = vdot(n, g_light); double sh;
    double ft;
    if (d < 0) d = 0;
    sh = 0.42 + 0.58 * d;
    base = mulc(base, sh);
    ft = (depth - 16.0) / 46.0;
    if (ft < 0) ft = 0; if (ft > 0.85) ft = 0.85;
    return lerpc(base, FOGCOL, ft);
}

static void addFace(V3 a, V3 b, V3 c, V3 d, V3 hint, COLORREF base, int cull) {
    V3 nrm = vnorm(vcross(vsub(b, a), vsub(c, a)));
    V3 cen = vmul(vadd(vadd(a, b), vadd(c, d)), 0.25);
    double sx[4], sy[4], sz[4], dep = 0;
    int i;
    Poly *pl;
    if (vdot(nrm, hint) < 0) nrm = vmul(nrm, -1);
    if (cull && vdot(nrm, vsub(g_camPos, cen)) <= 0) return;
    for (i = 0; i < 4; i++) {
        V3 pt = (i == 0) ? a : (i == 1 ? b : (i == 2 ? c : d));
        if (!project(pt, &sx[i], &sy[i], &sz[i])) return;
        dep += sz[i];
    }
    dep *= 0.25;
    if (g_pn >= 1200) return;
    pl = &g_poly[g_pn];
    pl->n = 4;
    for (i = 0; i < 4; i++) { pl->p[i].x = (LONG)sx[i]; pl->p[i].y = (LONG)sy[i]; }
    pl->col = shadeFog(base, nrm, dep);
    pl->depth = dep;
    g_pn++;
}

static void addBoxOr(V3 c, V3 ax, V3 ay, V3 az, double hx, double hy, double hz, COLORREF base) {
    V3 P[8]; int i;
    for (i = 0; i < 8; i++) {
        double sx = (i & 1) ? hx : -hx;
        double sy = (i & 2) ? hy : -hy;
        double sz = (i & 4) ? hz : -hz;
        P[i] = vadd(c, vadd(vmul(ax, sx), vadd(vmul(ay, sy), vmul(az, sz))));
    }
    addFace(P[1], P[3], P[7], P[5], ax, base, 1);
    addFace(P[0], P[4], P[6], P[2], vmul(ax, -1), base, 1);
    addFace(P[2], P[3], P[7], P[6], ay, base, 1);
    addFace(P[0], P[1], P[5], P[4], vmul(ay, -1), base, 1);
    addFace(P[4], P[5], P[7], P[6], az, base, 1);
    addFace(P[0], P[2], P[3], P[1], vmul(az, -1), base, 1);
}
static void addBox(V3 c, double hx, double hy, double hz, COLORREF base) {
    addBoxOr(c, v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1), hx, hy, hz, base);
}

static int cmpPoly(const void *a, const void *b) {
    const Poly *pa = (const Poly *)a, *pb = (const Poly *)b;
    if (pa->depth < pb->depth) return 1;
    if (pa->depth > pb->depth) return -1;
    return 0;
}

/* immediate quad (for track / lanes, not depth sorted) */
static void drawQuadNow(HDC dc, V3 a, V3 b, V3 c, V3 d, V3 hint, COLORREF base) {
    V3 nrm = vnorm(vcross(vsub(b, a), vsub(c, a)));
    V3 cen = vmul(vadd(vadd(a, b), vadd(c, d)), 0.25);
    double sx[4], sy[4], sz[4], dep = 0; int i; POINT pts[4];
    HBRUSH br;
    if (vdot(nrm, hint) < 0) nrm = vmul(nrm, -1);
    for (i = 0; i < 4; i++) {
        V3 pt = (i == 0) ? a : (i == 1 ? b : (i == 2 ? c : d));
        if (!project(pt, &sx[i], &sy[i], &sz[i])) return;
        dep += sz[i]; pts[i].x = (LONG)sx[i]; pts[i].y = (LONG)sy[i];
    }
    dep *= 0.25;
    br = CreateSolidBrush(shadeFog(base, nrm, dep));
    SelectObject(dc, br);
    Polygon(dc, pts, 4);
    DeleteObject(br);
}

/* ---------------- text (UTF-8) ---------------- */
static void textU8(HDC dc, int x, int y, const char *s8) {
    WCHAR w[512]; int len = (int)strlen(s8);
    int n = MultiByteToWideChar(CP_UTF8, 0, s8, len, w, 511);
    if (n > 0) TextOutW(dc, x, y, w, n);
}
static void textU8Box(HDC dc, int l, int t, int rr, int b, const char *s8, UINT fmt) {
    WCHAR w[512]; RECT rc; int len = (int)strlen(s8);
    int n = MultiByteToWideChar(CP_UTF8, 0, s8, len, w, 511);
    if (n > 0) { rc.left = l; rc.top = t; rc.right = rr; rc.bottom = b; DrawTextW(dc, w, n, &rc, fmt); }
}
static void fillRect2(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r; HBRUSH b = CreateSolidBrush(c);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    FillRect(dc, &r, b); DeleteObject(b);
}
static void fillEllipse2(HDC dc, int x, int y, int w, int h, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c); HGDIOBJ ob, op;
    if (w < 1) w = 1; if (h < 1) h = 1;
    ob = SelectObject(dc, b); op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y, x + w, y + h);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}

/* ---------------- audio (synthesized WAV + PlaySoundA) ---------------- */
typedef BOOL (WINAPI *PFNPS)(LPCSTR, HMODULE, DWORD);
static PFNPS g_ps = 0;
static unsigned char g_wavJump[20000];
static unsigned char g_wavCoin[20000];
static unsigned char g_wavHit[20000];

static void wavHeader(unsigned char *b, int nSamples, int sr) {
    int dataSize = nSamples * 2, fileSize = 36 + dataSize;
    memcpy(b, "RIFF", 4); memcpy(b + 4, &fileSize, 4); memcpy(b + 8, "WAVE", 4);
    memcpy(b + 12, "fmt ", 4);
    { int v16 = 16; memcpy(b + 16, &v16, 4); }
    { short v = 1; memcpy(b + 20, &v, 2); }
    { short v = 1; memcpy(b + 22, &v, 2); }
    memcpy(b + 24, &sr, 4);
    { int br = sr * 2; memcpy(b + 28, &br, 4); }
    { short v = 2; memcpy(b + 32, &v, 2); }
    { short v = 16; memcpy(b + 34, &v, 2); }
    memcpy(b + 36, "data", 4); memcpy(b + 40, &dataSize, 4);
}
static void putSample(unsigned char *b, int i, double s) {
    int v = (int)(s * 32000.0);
    short sv;
    if (v > 32000) v = 32000; if (v < -32000) v = -32000;
    sv = (short)v;
    memcpy(b + 44 + i * 2, &sv, 2);
}
static void buildSounds(void) {
    int sr = 22050, i, n;
    /* jump: upward chirp */
    n = (int)(sr * 0.16);
    wavHeader(g_wavJump, n, sr);
    {
        double phase = 0.0, dur = 0.16;
        for (i = 0; i < n; i++) {
            double t = (double)i / sr;
            double f = 480.0 + 1500.0 * (t / dur);
            double env = 1.0 - (double)i / n;
            phase += 2 * PI_ * f / sr;
            putSample(g_wavJump, i, sin(phase) * env * env * 0.55);
        }
    }
    /* coin: two bright tones */
    n = (int)(sr * 0.16);
    wavHeader(g_wavCoin, n, sr);
    {
        double phase = 0.0; int half = n / 2;
        for (i = 0; i < n; i++) {
            double f = (i < half) ? 1050.0 : 1560.0;
            double env = (i < half) ? (1.0 - (double)i / half) : (1.0 - (double)(i - half) / (n - half));
            double s;
            phase += 2 * PI_ * f / sr;
            s = sin(phase) * 0.5 + sin(phase * 2.0) * 0.12;
            putSample(g_wavCoin, i, s * env * 0.7);
        }
    }
    /* hit: low falling buzz */
    n = (int)(sr * 0.34);
    wavHeader(g_wavHit, n, sr);
    {
        double phase = 0.0, dur = 0.34;
        for (i = 0; i < n; i++) {
            double t = (double)i / sr;
            double f = 220.0 - 150.0 * (t / dur);
            double env = 1.0 - (double)i / n;
            double s;
            phase += 2 * PI_ * f / sr;
            s = sin(phase) + 0.35 * sin(phase * 0.5);
            if (s > 1) s = 1; if (s < -1) s = -1;
            putSample(g_wavHit, i, s * env * 0.7);
        }
    }
}
static void initSound(void) {
    HMODULE h = LoadLibraryA("winmm.dll");
    if (h) g_ps = (PFNPS)GetProcAddress(h, "PlaySoundA");
    buildSounds();
}
static void playWav(const unsigned char *b) {
    if (g_ps) g_ps((LPCSTR)b, 0, 0x0001 | 0x0002 | 0x0004); /* ASYNC|NODEFAULT|MEMORY */
}

/* ---------------- game state ---------------- */
static HWND g_hwnd = 0;
static HDC  g_mem = 0;
static HBITMAP g_bmp = 0, g_oldbmp = 0;
static int  g_run = 1, g_frame = 0;
static HFONT g_fBig, g_fHud, g_fSmall, g_fTitle;

static int    g_onGround = 1, g_over = 0;
static double g_speed = 0.16, g_dist = 0, g_scroll = 0, g_phase = 0;
static int    g_coins = 0, g_score = 0, g_best = 0;
static double g_obAcc = 0, g_coinAcc = 0;
static int    g_keyL = 0, g_keyR = 0, g_keyJump = 0;

#define MAX_OB 12
#define MAX_COIN 24
#define MAX_BLD 26

typedef struct { int active; double z; int lane; int type; } Ob;
typedef struct { int active; double z; int lane; } Coin;
typedef struct { double z; double x; double w, h, d; COLORREF col; } Bld;

static Ob   g_ob[MAX_OB];
static Coin g_co[MAX_COIN];
static Bld  g_bld[MAX_BLD];

/* ---------------- best score ---------------- */
static void bestPath(char *p) {
    char *q; GetModuleFileNameA(0, p, MAX_PATH);
    q = strrchr(p, '\\'); if (q) strcpy(q + 1, "runner3d_best.txt");
}
static void loadBest(void) {
    char p[MAX_PATH]; FILE *f; bestPath(p);
    f = fopen(p, "r"); if (f) { if (fscanf(f, "%d", &g_best) != 1) g_best = 0; fclose(f); }
}
static void saveBest(void) {
    char p[MAX_PATH]; FILE *f; bestPath(p);
    f = fopen(p, "w"); if (f) { fprintf(f, "%d", g_best); fclose(f); }
}

/* ---------------- scene init ---------------- */
static void initScene(void) {
    int i;
    g_px = 0; g_py = 0; g_vy = 0; g_onGround = 1; g_over = 0;
    g_speed = 0.20; g_dist = 0; g_scroll = 0; g_phase = 0;
    g_coins = 0; g_score = 0; g_obAcc = 0; g_coinAcc = 0;
    for (i = 0; i < MAX_OB; i++) g_ob[i].active = 0;
    for (i = 0; i < MAX_COIN; i++) g_co[i].active = 0;
    for (i = 0; i < MAX_BLD; i++) {
        g_bld[i].z = 6.0 + (double)i * (ZFAR / MAX_BLD) + (rand() % 6);
        g_bld[i].x = ((i % 2) ? 1 : -1) * (TRACK_HALF + 3.0 + (rand() % 3));
        g_bld[i].w = 1.6 + (rand() % 20) / 10.0;
        g_bld[i].h = 4.0 + (rand() % 90) / 10.0;
        g_bld[i].d = 1.6 + (rand() % 16) / 10.0;
        g_bld[i].col = RGB(48 + rand() % 30, 54 + rand() % 26, 82 + rand() % 40);
    }
}

/* ---------------- run-cycle avatar ---------------- */
static void addLimb(V3 pivot, double len1, double len2, double ang1, double bend,
                    double thick, COLORREF c1, COLORREF c2) {
    double ca = cos(ang1), sa = sin(ang1);
    V3 ay = v3(0, ca, sa), az = v3(0, -sa, ca);
    V3 dir = v3(0, -ca, -sa);
    V3 knee = vadd(pivot, vmul(dir, len1));
    V3 c1p = vadd(pivot, vmul(dir, len1 * 0.5));
    double a2 = ang1 + bend, cb = cos(a2), sb = sin(a2);
    V3 ay2 = v3(0, cb, sb), az2 = v3(0, -sb, cb);
    V3 dir2 = v3(0, -cb, -sb);
    V3 c2p = vadd(knee, vmul(dir2, len2 * 0.5));
    addBoxOr(c1p, v3(1, 0, 0), ay, az, thick, len1 * 0.5, thick, c1);
    addBoxOr(c2p, v3(1, 0, 0), ay2, az2, thick, len2 * 0.5, thick, c2);
}

static void drawPlayer(void) {
    COLORREF jacket = RGB(70, 200, 210), jacket2 = RGB(56, 168, 178);
    COLORREF pants = RGB(46, 54, 86), pants2 = RGB(38, 44, 72);
    COLORREF skin = RGB(246, 206, 168), hair = RGB(38, 30, 26);
    COLORREF shoe = RGB(28, 28, 34);
    double bx = g_px, by = g_py;
    double rp = g_phase;
    double bob = g_onGround ? sin(rp * 2.0) * 0.025 : 0.0;
    double la, ra, lb, rb, aa, ab;
    double hipY = by + 0.82 + bob, shY = by + 1.42 + bob, headY = by + 1.62 + bob;

    if (g_onGround) {
        la = sin(rp) * 0.62; ra = sin(rp + PI_) * 0.62;
        lb = 0.15 + 0.55 * (0.5 + 0.5 * sin(rp + PI_));
        rb = 0.15 + 0.55 * (0.5 + 0.5 * sin(rp));
        aa = sin(rp + PI_) * 0.5; ab = sin(rp) * 0.5;
    } else {
        la = 0.55; ra = -0.15; lb = 0.95; rb = 0.45;
        aa = -0.7; ab = 0.5;
    }

    /* legs */
    addLimb(v3(bx - 0.13, hipY, 0), 0.44, 0.4, la, lb, 0.085, pants, pants2);
    addLimb(v3(bx + 0.13, hipY, 0), 0.44, 0.4, ra, rb, 0.085, pants2, pants);
    /* shoes */
    addBox(v3(bx - 0.13, by + 0.06, -sin(la) * 0.72), 0.09, 0.06, 0.14, shoe);
    addBox(v3(bx + 0.13, by + 0.06, -sin(ra) * 0.72), 0.09, 0.06, 0.14, shoe);

    /* torso */
    addBox(v3(bx, by + 1.12 + bob, 0.0), 0.27, 0.36, 0.17, jacket);
    /* chest stripe / backpack */
    addBox(v3(bx, by + 1.16 + bob, -0.20), 0.20, 0.24, 0.07, jacket2);

    /* arms */
    addLimb(v3(bx - 0.33, shY, 0), 0.32, 0.3, aa, 0.5, 0.07, jacket, skin);
    addLimb(v3(bx + 0.33, shY, 0), 0.32, 0.3, ab, 0.5, 0.07, jacket, skin);

    /* head */
    addBox(v3(bx, headY, 0), 0.16, 0.17, 0.16, skin);
    addBox(v3(bx, headY + 0.08, -0.05), 0.17, 0.1, 0.15, hair);
}

/* ---------------- world draw ---------------- */
static void drawGround(HDC dc) {
    int horizon, i, bands = 26;
    double hx, hy, hz;
    if (project(v3(0, 0, 1e6), &hx, &hy, &hz)) horizon = (int)hy; else horizon = g_cy;
    if (horizon < 40) horizon = 40;
    if (horizon > WIN_H - 40) horizon = WIN_H - 40;
    /* sky */
    for (i = 0; i < bands; i++) {
        int y0 = horizon * i / bands, y1 = horizon * (i + 1) / bands + 1;
        COLORREF c = lerpc(RGB(26, 32, 66), RGB(248, 178, 126), (double)i / bands);
        fillRect2(dc, 0, y0, WIN_W, y1 - y0, c);
    }
    /* sun (near horizon-right) */
    fillEllipse2(dc, WIN_W - 340, horizon - 128, 200, 200, RGB(255, 218, 150));
    fillEllipse2(dc, WIN_W - 308, horizon - 100, 136, 136, RGB(255, 242, 195));
    /* ground base */
    for (i = 0; i < bands; i++) {
        int y0 = horizon + (WIN_H - horizon) * i / bands;
        int y1 = horizon + (WIN_H - horizon) * (i + 1) / bands + 1;
        COLORREF c = lerpc(RGB(120, 92, 82), RGB(40, 40, 54), (double)i / bands);
        fillRect2(dc, 0, y0, WIN_W, y1 - y0, c);
    }
}

static void drawTrack(HDC dc) {
    double rem = g_scroll - (double)(long)(g_scroll / SEG) * SEG;
    double start = -rem;
    int count = (int)((ZFAR - start) / SEG);
    int k;
    V3 up = v3(0, 1, 0);
    for (k = count; k >= 0; k--) {
        double z0 = start + k * SEG, z1 = z0 + SEG;
        COLORREF road = ((k & 1) ? RGB(70, 72, 88) : RGB(62, 64, 80));
        if (z1 < -5 || z0 > ZFAR) continue;
        /* road */
        drawQuadNow(dc, v3(-TRACK_HALF, 0, z0), v3(TRACK_HALF, 0, z0),
                        v3(TRACK_HALF, 0, z1), v3(-TRACK_HALF, 0, z1), up, road);
        /* curb edges */
        drawQuadNow(dc, v3(-TRACK_HALF, 0.02, z0), v3(-TRACK_HALF + 0.45, 0.02, z0),
                        v3(-TRACK_HALF + 0.45, 0.02, z1), v3(-TRACK_HALF, 0.02, z1), up,
                    ((k & 1) ? RGB(220, 210, 200) : RGB(200, 80, 70)));
        drawQuadNow(dc, v3(TRACK_HALF - 0.45, 0.02, z0), v3(TRACK_HALF, 0.02, z0),
                        v3(TRACK_HALF, 0.02, z1), v3(TRACK_HALF - 0.45, 0.02, z1), up,
                    ((k & 1) ? RGB(220, 210, 200) : RGB(200, 80, 70)));
        /* dashed lane dividers at x = +/- 1.1 */
        if ((k & 1) == 0) {
            drawQuadNow(dc, v3(-1.15, 0.02, z0 + 0.4), v3(-1.05, 0.02, z0 + 0.4),
                            v3(-1.05, 0.02, z1 - 0.4), v3(-1.15, 0.02, z1 - 0.4), up, RGB(230, 226, 210));
            drawQuadNow(dc, v3(1.05, 0.02, z0 + 0.4), v3(1.15, 0.02, z0 + 0.4),
                            v3(1.15, 0.02, z1 - 0.4), v3(1.05, 0.02, z1 - 0.4), up, RGB(230, 226, 210));
        }
    }
}

static void drawShadow(HDC dc, double x, double z, double rw) {
    double sx, sy, sz;
    if (!project(v3(x, 0.03, z), &sx, &sy, &sz)) return;
    {
        int r = (int)(rw * g_focal / sz);
        if (r < 1) r = 1;
        if (r > 260) r = 260;
        fillEllipse2(dc, (int)sx - r, (int)sy - r / 3, r * 2, r / 3 * 2, RGB(34, 34, 44));
    }
}

static void buildObjects(void) {
    int i;
    /* buildings */
    for (i = 0; i < MAX_BLD; i++) {
        Bld *b = &g_bld[i];
        if (b->z < -6 || b->z > ZFAR + 20) continue;
        addBox(v3(b->x, b->h / 2, b->z), b->w / 2, b->h / 2, b->d / 2, b->col);
        /* lit roof cap for a bit of detail */
        addBox(v3(b->x, b->h + 0.15, b->z), b->w / 2 + 0.05, 0.15, b->d / 2 + 0.05,
               RGB(90, 96, 130));
    }
    /* obstacles */
    for (i = 0; i < MAX_OB; i++) {
        Ob *o = &g_ob[i];
        double lx;
        if (!o->active) continue;
        lx = (o->lane - 1) * LANE_W;
        if (o->type == 0) {
            /* hurdle: jump over */
            addBox(v3(lx, 0.34, o->z), 0.85, 0.34, 0.28, RGB(244, 150, 52));
            addBox(v3(lx, 0.66, o->z), 0.9, 0.06, 0.3, RGB(60, 40, 30));
        } else {
            /* tall wall: dodge */
            addBox(v3(lx, 1.1, o->z), 0.9, 1.1, 0.34, RGB(150, 62, 78));
            addBox(v3(lx, 2.18, o->z), 0.95, 0.08, 0.36, RGB(90, 40, 50));
            addBox(v3(lx, 1.1, o->z + 0.36), 0.62, 0.24, 0.03, RGB(220, 200, 90));
        }
    }
    /* coins */
    for (i = 0; i < MAX_COIN; i++) {
        Coin *c = &g_co[i];
        double lx;
        if (!c->active) continue;
        lx = (c->lane - 1) * LANE_W;
        addBox(v3(lx, 1.05, c->z), 0.22, 0.22, 0.05, RGB(255, 208, 70));
        addBox(v3(lx, 1.05, c->z), 0.10, 0.10, 0.07, RGB(255, 240, 170));
    }
    drawPlayer();
}

/* ---------------- update ---------------- */
static void spawnObstacle(void) {
    int i, lane, type;
    for (i = 0; i < MAX_OB; i++) if (!g_ob[i].active) {
        lane = rand() % 3;
        type = (rand() % 100 < 34) ? 1 : 0;
        g_ob[i].active = 1; g_ob[i].z = ZFAR; g_ob[i].lane = lane; g_ob[i].type = type;
        return;
    }
}
static void spawnCoin(void) {
    int i;
    for (i = 0; i < MAX_COIN; i++) if (!g_co[i].active) {
        g_co[i].active = 1; g_co[i].z = ZFAR; g_co[i].lane = rand() % 3;
        return;
    }
}
static void update(void) {
    int i;
    g_frame++;
    if (g_over) return;

    g_speed = 0.20 + g_dist * 0.00055;
    if (g_speed > 0.48) g_speed = 0.48;
    g_dist += g_speed;
    g_scroll += g_speed;
    g_phase += 0.12 + g_speed * 0.6;

    /* horizontal */
    {
        double dx = 0;
        if (g_keyL) dx -= 1;
        if (g_keyR) dx += 1;
        g_px += dx * 0.14;
        if (g_px < -TRACK_HALF + 0.6) g_px = -TRACK_HALF + 0.6;
        if (g_px >  TRACK_HALF - 0.6) g_px =  TRACK_HALF - 0.6;
    }
    /* jump */
    if (g_keyJump) { if (g_onGround) { g_vy = 0.30; g_onGround = 0; g_keyJump = 0; } }
    if (!g_onGround) {
        g_py += g_vy; g_vy -= 0.028;
        if (g_py <= 0) { g_py = 0; g_vy = 0; g_onGround = 1; }
    }

    /* spawn */
    g_obAcc += g_speed;
    if (g_obAcc > 9.5) { g_obAcc = 0; spawnObstacle(); }
    g_coinAcc += g_speed;
    if (g_coinAcc > 13.0) { g_coinAcc = 0; spawnCoin(); }

    /* move obstacles */
    for (i = 0; i < MAX_OB; i++) if (g_ob[i].active) {
        double lx = (g_ob[i].lane - 1) * LANE_W;
        g_ob[i].z -= g_speed;
        if (g_ob[i].z < -6) { g_ob[i].active = 0; continue; }
        if (g_ob[i].z < 0.7 && g_ob[i].z > -0.7 && fabs(g_px - lx) < 0.95) {
            int hit = 0;
            if (g_ob[i].type == 0) { if (g_py < 0.72) hit = 1; }
            else hit = 1;
            if (hit) {
                g_over = 1;
                if (g_score > g_best) { g_best = g_score; saveBest(); }
                playWav(g_wavHit);
            }
        }
    }
    /* coins */
    for (i = 0; i < MAX_COIN; i++) if (g_co[i].active) {
        double lx = (g_co[i].lane - 1) * LANE_W;
        g_co[i].z -= g_speed;
        if (g_co[i].z < -6) { g_co[i].active = 0; continue; }
        if (g_co[i].z < 0.9 && g_co[i].z > -0.9 && fabs(g_px - lx) < 0.9) {
            g_co[i].active = 0; g_coins++; playWav(g_wavCoin);
        }
    }
    /* buildings recycle */
    for (i = 0; i < MAX_BLD; i++) {
        g_bld[i].z -= g_speed;
        if (g_bld[i].z < -3.0) {
            g_bld[i].z = ZFAR + (rand() % 12);
            g_bld[i].x = ((rand() & 1) ? 1 : -1) * (TRACK_HALF + 3.0 + (rand() % 3));
            g_bld[i].w = 1.6 + (rand() % 20) / 10.0;
            g_bld[i].h = 4.0 + (rand() % 90) / 10.0;
            g_bld[i].d = 1.6 + (rand() % 16) / 10.0;
            g_bld[i].col = RGB(48 + rand() % 30, 54 + rand() % 26, 82 + rand() % 40);
        }
    }

    g_score = (int)(g_dist * 4.0) + g_coins * 50;
    if (g_score > g_best) { g_best = g_score; }
}

/* ---------------- HUD ---------------- */
static void drawHUD(HDC dc) {
    char buf[96];
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fHud);
    SetTextColor(dc, RGB(255, 244, 220));
    sprintf(buf, "得分  %d", g_score);
    textU8(dc, 26, 22, buf);
    SetTextColor(dc, RGB(255, 214, 150));
    sprintf(buf, "最高  %d", g_best);
    textU8(dc, 26, 56, buf);
    SetTextColor(dc, RGB(180, 220, 255));
    sprintf(buf, "金币  %d", g_coins);
    textU8(dc, 26, 90, buf);

    SelectObject(dc, g_fSmall);
    SetTextColor(dc, RGB(230, 226, 236));
    textU8(dc, 26, WIN_H - 34, "A / D 或 ← → 移动      空格 / ↑ 跳跃      R 重新开始      ESC 退出");
}

static void drawOver(HDC dc) {
    char buf[96];
    fillRect2(dc, 0, 0, WIN_W, WIN_H, RGB(16, 18, 30));
    /* keep a hint of the scene */
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fTitle);
    SetTextColor(dc, RGB(255, 214, 120));
    textU8Box(dc, 0, 150, WIN_W, 210, "撞车了！", DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, g_fHud);
    SetTextColor(dc, RGB(238, 240, 255));
    sprintf(buf, "本局得分   %d", g_score);
    textU8Box(dc, 0, 250, WIN_W, 290, buf, DT_CENTER | DT_SINGLELINE);
    SetTextColor(dc, RGB(255, 210, 140));
    sprintf(buf, "最高纪录   %d", g_best);
    textU8Box(dc, 0, 294, WIN_W, 334, buf, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, g_fSmall);
    SetTextColor(dc, RGB(170, 200, 255));
    textU8Box(dc, 0, 380, WIN_W, 410, "按 空格 或 R 重新开始", DT_CENTER | DT_SINGLELINE);
}

/* ---------------- render ---------------- */
static void render(void) {
    HDC dc = g_mem, wdc;
    int i;

    setupCamera();
    drawGround(dc);
    drawTrack(dc);

    /* shadows on the road */
    drawShadow(dc, g_px, 0.0, 0.55 + g_py * 0.12);
    for (i = 0; i < MAX_OB; i++) if (g_ob[i].active)
        drawShadow(dc, (g_ob[i].lane - 1) * LANE_W, g_ob[i].z, 0.8);

    g_pn = 0;
    buildObjects();
    qsort(g_poly, g_pn, sizeof(Poly), cmpPoly);
    for (i = 0; i < g_pn; i++) {
        HBRUSH br = CreateSolidBrush(g_poly[i].col);
        SelectObject(dc, br);
        Polygon(dc, g_poly[i].p, g_poly[i].n);
        DeleteObject(br);
    }

    drawHUD(dc);
    if (g_over) drawOver(dc);

    wdc = GetDC(g_hwnd);
    BitBlt(wdc, 0, 0, WIN_W, WIN_H, dc, 0, 0, SRCCOPY);
    ReleaseDC(g_hwnd, wdc);
}

/* ---------------- window ---------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == 'A' || wp == VK_LEFT) g_keyL = 1;
        else if (wp == 'D' || wp == VK_RIGHT) g_keyR = 1;
        else if (wp == VK_SPACE || wp == VK_UP) {
            if (g_over) { initScene(); playWav(g_wavJump); }
            else g_keyJump = 1;
        } else if (wp == 'R' || wp == 'r') { initScene(); }
        else if (wp == VK_ESCAPE) { g_run = 0; PostQuitMessage(0); }
        return 0;
    case WM_KEYUP:
        if (wp == 'A' || wp == VK_LEFT) g_keyL = 0;
        else if (wp == 'D' || wp == VK_RIGHT) g_keyR = 0;
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC d = BeginPaint(hwnd, &ps);
        if (g_mem) BitBlt(d, 0, 0, WIN_W, WIN_H, g_mem, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY: g_run = 0; PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

#ifndef PREVIEW
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    WNDCLASSA wc; MSG msg; RECT rc; int ww, wh, sx, sy;
    (void)hPrev; (void)cmd;

    srand((unsigned)time(0));
    g_light = vnorm(v3(0.40, 0.86, 0.32));
    loadBest();
    initSound();
    initScene();

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(0, IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = "Runner3DWnd";
    RegisterClassA(&wc);

    rc.left = 0; rc.top = 0; rc.right = WIN_W; rc.bottom = WIN_H;
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    ww = rc.right - rc.left; wh = rc.bottom - rc.top;
    sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    if (sx < 0) sx = 0; if (sy < 0) sy = 0;

    g_hwnd = CreateWindowExA(0, "Runner3DWnd", "Dusk Runner 3D",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        sx, sy, ww, wh, 0, 0, hInst, 0);

    g_fTitle = CreateFontA(56, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fBig   = CreateFontA(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fHud   = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fSmall = CreateFontA(17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");

    {
        HDC wdc = GetDC(g_hwnd);
        g_mem = CreateCompatibleDC(wdc);
        g_bmp = CreateCompatibleBitmap(wdc, WIN_W, WIN_H);
        g_oldbmp = SelectObject(g_mem, g_bmp);
        ReleaseDC(g_hwnd, wdc);
    }

    ShowWindow(g_hwnd, show);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_hwnd);
    UpdateWindow(g_hwnd);

    while (g_run) {
        while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_run = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!g_run) break;
        update();
        render();
        Sleep(16);
    }

    if (g_mem) {
        SelectObject(g_mem, g_oldbmp);
        DeleteObject(g_bmp);
        DeleteDC(g_mem);
    }
    return 0;
}
#endif /* PREVIEW */

/* ---------------- hidden offscreen preview (build with -DPREVIEW) ---------------- */
#ifdef PREVIEW
static void saveBmp(const char *path, HDC dc, int w, int h) {
    int rowSize = ((w * 3 + 3) / 4) * 4;
    int dataSize = rowSize * h;
    unsigned char *buf = (unsigned char *)malloc(dataSize);
    unsigned char hdr[54];
    BITMAPINFO bi;
    FILE *fp;
    int i;
    if (!buf) return;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(dc, (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP), 0, h, buf, &bi, DIB_RGB_COLORS);

    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    { unsigned int v = 54 + dataSize; memcpy(hdr + 2, &v, 4); }
    { unsigned int v = 54; memcpy(hdr + 10, &v, 4); }
    { unsigned int v = 40; memcpy(hdr + 14, &v, 4); }
    { int v = w; memcpy(hdr + 18, &v, 4); }
    { int v = -h; memcpy(hdr + 22, &v, 4); }
    { unsigned short v = 1; memcpy(hdr + 26, &v, 2); }
    { unsigned short v = 24; memcpy(hdr + 28, &v, 2); }
    { unsigned int v = dataSize; memcpy(hdr + 34, &v, 4); }
    { int v = 2835; memcpy(hdr + 38, &v, 4); memcpy(hdr + 42, &v, 4); }

    fp = fopen(path, "wb");
    if (fp) { fwrite(hdr, 1, 54, fp); fwrite(buf, 1, dataSize, fp); fclose(fp); }
    free(buf);
    (void)i;
}

int main(void) {
    HDC screen; int i;
    g_light = vnorm(v3(0.40, 0.86, 0.32));
    initScene();
    /* stage a few objects for the still */
    g_ob[0].active = 1; g_ob[0].z = 6.0;  g_ob[0].lane = 1; g_ob[0].type = 0;
    g_ob[1].active = 1; g_ob[1].z = 17.0; g_ob[1].lane = 0; g_ob[1].type = 1;
    g_co[0].active = 1; g_co[0].z = 9.0;  g_co[0].lane = 2;
    g_co[1].active = 1; g_co[1].z = 22.0; g_co[1].lane = 1;
    g_frame = 4;

    screen = GetDC(0);
    g_mem = CreateCompatibleDC(screen);
    g_bmp = CreateCompatibleBitmap(screen, WIN_W, WIN_H);
    g_oldbmp = SelectObject(g_mem, g_bmp);

    g_fTitle = CreateFontA(56, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fBig   = CreateFontA(44, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fHud   = CreateFontA(24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");
    g_fSmall = CreateFontA(17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Microsoft YaHei");

    setupCamera();
    drawGround(g_mem);
    drawTrack(g_mem);
    drawShadow(g_mem, g_px, 0.0, 0.55);
    for (i = 0; i < MAX_OB; i++) if (g_ob[i].active)
        drawShadow(g_mem, (g_ob[i].lane - 1) * LANE_W, g_ob[i].z, 0.8);

    g_pn = 0;
    buildObjects();
    qsort(g_poly, g_pn, sizeof(Poly), cmpPoly);
    for (i = 0; i < g_pn; i++) {
        HBRUSH br = CreateSolidBrush(g_poly[i].col);
        SelectObject(g_mem, br);
        Polygon(g_mem, g_poly[i].p, g_poly[i].n);
        DeleteObject(br);
    }
    drawHUD(g_mem);
    saveBmp("preview.bmp", g_mem, WIN_W, WIN_H);
    return 0;
}
#endif
