// Overclock: CPU clock settings for Onion OS on the Miyoo Mini / Mini v4 / Mini Plus.
// Writes the files Onion's RetroArch reads when a game starts (gfx/drivers/miyoomini/sdl_miyoomini_gfx.c):
//   Saves/CurrentProfile/config/<core library name>/cpuclock.txt   per core ("per system")
//   RetroArch/cpuclock.txt                                         fallback for every core ("All systems")
// Systems that set their own clock in launch.sh ("cpuclock 1500", e.g. Drastic) get that number edited instead.
// Same no-SDL approach as the Bible app: draws to /dev/fb0 rotated 180, reads /dev/input/event0.
// Build (device):  zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 -O2 -static -s overclock.c -o overclock -lm
// Build (preview): zig cc -target x86_64-windows-gnu -DPREVIEW -O2 overclock.c -o overclock_preview.exe
//                  SDROOT=<mock card> overclock_preview.exe <key> ...   -> frames/NNN.png after every key
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

enum { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_A, K_B, K_X, K_Y, K_L1, K_R1, K_L2, K_R2, K_SELECT, K_START, K_MENU, K_QUIT };
static const char *KEYNAMES[] __attribute__((unused)) = { "up", "down", "left", "right", "a", "b", "x", "y", "l1", "r1", "l2", "r2", "select", "start", "menu" };
static int W = 640, H = 480;
static uint32_t *px;                         // back buffer, 0xRRGGBB

// ---------------------------------------------------------------- platform
#ifdef PREVIEW
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define MKDIR(p) mkdir(p)
static char SD[512] = "mock_sd", TMP[512] = "mock_sd/tmp";
static char **pkeys; static int npkeys, ipkey, frame;
static int plat_init(void)
{
    if (getenv("W")) W = atoi(getenv("W"));
    if (getenv("H")) H = atoi(getenv("H"));
    if (getenv("SDROOT")) { snprintf(SD, sizeof SD, "%s", getenv("SDROOT")); snprintf(TMP, sizeof TMP, "%s/tmp", SD); }
    px = malloc(W * H * 4);
    return 0;
}
static void plat_present(void)
{
    char fn[64]; unsigned char *rgb = malloc(W * H * 3);
    for (int i = 0; i < W * H; i++) { rgb[i * 3] = px[i] >> 16; rgb[i * 3 + 1] = px[i] >> 8; rgb[i * 3 + 2] = px[i]; }
    sprintf(fn, "frames/%03d.png", frame++);
    stbi_write_png(fn, W, H, 3, rgb, W * 3); free(rgb);
}
static int plat_key(void)
{
    while (ipkey < npkeys) {
        const char *k = pkeys[ipkey++];
        for (int i = 0; i < K_QUIT; i++) if (!strcmp(k, KEYNAMES[i])) return i;
    }
    return K_QUIT;
}
static void plat_sync(void) {}
#else
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#define MKDIR(p) mkdir(p, 0777)
static const char SD[] = "/mnt/SDCARD", TMP[] = "/tmp";
static int fbfd = -1, infd = -1, stride, bpp;
static uint8_t *fbmem;
static struct fb_var_screeninfo vinfo;
static volatile sig_atomic_t quit_flag;
static void on_signal(int s) { (void)s; quit_flag = 1; }
static int plat_init(void)
{
    struct fb_fix_screeninfo finfo;
    if ((fbfd = open("/dev/fb0", O_RDWR)) < 0) return -1;
    ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    vinfo.yoffset = 0;                          // show page 0 (same as Onion's display_reset)
    ioctl(fbfd, FBIOPUT_VSCREENINFO, &vinfo);
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo);
    W = vinfo.xres; H = vinfo.yres; bpp = vinfo.bits_per_pixel / 8;
    stride = finfo.line_length ? (int)finfo.line_length : (int)vinfo.xres_virtual * bpp;
    fbmem = mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) return -1;
    px = malloc(W * H * 4);
    infd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal);
    return 0;
}
static void plat_present(void)                  // the panel is mounted upside down: rotate 180
{
    uint8_t *base = fbmem + vinfo.yoffset * stride;
    for (int y = 0; y < H; y++) {
        const uint32_t *s = px + y * W;
        if (bpp == 4) {
            uint32_t *d = (uint32_t *)(base + (H - 1 - y) * stride) + (W - 1);
            for (int x = 0; x < W; x++) *d-- = s[x] | 0xFF000000;
        }
        else {
            uint16_t *d = (uint16_t *)(base + (H - 1 - y) * stride) + (W - 1);
            for (int x = 0; x < W; x++) *d-- = ((s[x] >> 8) & 0xF800) | ((s[x] >> 5) & 0x07E0) | ((s[x] >> 3) & 0x001F);
        }
    }
}
static long long now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static int map_key(int code)
{
    switch (code) {             // Onion keymap_hw.h
    case KEY_UP: return K_UP;           case KEY_DOWN: return K_DOWN;
    case KEY_LEFT: return K_LEFT;       case KEY_RIGHT: return K_RIGHT;
    case KEY_SPACE: return K_A;         case KEY_LEFTCTRL: return K_B;
    case KEY_LEFTSHIFT: return K_X;     case KEY_LEFTALT: return K_Y;
    case KEY_E: return K_L1;            case KEY_T: return K_R1;
    case KEY_TAB: return K_L2;          case KEY_BACKSPACE: return K_R2;
    case KEY_RIGHTCTRL: return K_SELECT; case KEY_ENTER: return K_START;
    case KEY_ESC: return K_MENU;
    }
    return -1;
}
// Held D-pad keys repeat. Menu only counts when released without another button in between,
// so Onion's Menu+button shortcuts (brightness etc.) keep working.
static int plat_key(void)
{
    static int queue[32], qn, rep_key = -1, menu_down, menu_combo;
    static long long rep_at;
    for (;;) {
        if (quit_flag) return K_QUIT;
        if (qn) { int k = queue[0]; memmove(queue, queue + 1, --qn * sizeof(int)); return k; }
        int timeout = -1;
        if (rep_key >= 0) { long long d = rep_at - now_ms(); timeout = d < 0 ? 0 : (int)d; }
        struct pollfd p = { infd, POLLIN, 0 };
        int r = poll(&p, 1, timeout);
        if (r < 0) { if (errno == EINTR) continue; return K_QUIT; }
        if (r == 0) {
            if (rep_key >= 0) { rep_at = now_ms() + (rep_key <= K_DOWN ? 70 : 110); return rep_key; }
            continue;
        }
        struct input_event ev[16];
        ssize_t n = read(infd, ev, sizeof ev);
        for (int i = 0; i < n / (ssize_t)sizeof ev[0]; i++) {
            if (ev[i].type != EV_KEY) continue;
            int k = map_key(ev[i].code);
            if (k < 0) continue;
            if (ev[i].value == 1) {
                if (k == K_MENU) { menu_down = 1; menu_combo = 0; continue; }
                if (menu_down) { menu_combo = 1; continue; }
                if (k <= K_RIGHT) { rep_key = k; rep_at = now_ms() + 350; }
                if (qn < 32) queue[qn++] = k;
            }
            else if (ev[i].value == 0) {
                if (k == K_MENU) { if (menu_down && !menu_combo && qn < 32) queue[qn++] = K_MENU; menu_down = 0; }
                if (k == rep_key) rep_key = -1;
            }
        }
    }
}
static void plat_sync(void) { sync(); }
#endif

// ---------------------------------------------------------------- drawing
enum { C_BG = 0x101318, C_PANEL = 0x181D24, C_SEL = 0x222A35, C_LINE = 0x2A323D, C_FG = 0xE6EDF3, C_DIM = 0x7D8794,
       C_ACCENT = 0x58A6FF, C_GREEN = 0x3FB950, C_YELLOW = 0xE3B341, C_ORANGE = 0xF0883E };

static inline uint32_t mix(uint32_t d, uint32_t c, int a)
{
    int ia = 255 - a;
    return ((((c & 0xFF00FF) * a + (d & 0xFF00FF) * ia) >> 8) & 0xFF00FF) | ((((c & 0xFF00) * a + (d & 0xFF00) * ia) >> 8) & 0xFF00);
}
static void fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px[(y + j) * W + x + i] = c;
}
static void blend_px(int x, int y, uint32_t c, int a)
{
    if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = mix(px[y * W + x], c, a);
}
static void rrect(int x, int y, int w, int h, int r, uint32_t c)      // anti-aliased corners
{
    for (int j = 0; j < h; j++) {
        float dy = j < r ? r - j - 0.5f : j >= h - r ? j - (h - r) + 0.5f : 0;
        float in = dy > 0 ? r - sqrtf(r * r - dy * dy) : 0;
        int ii = (int)in, a = (int)((1 - (in - ii)) * 255);
        fill(x + ii + 1, y + j, w - 2 * (ii + 1), 1, c);
        blend_px(x + ii, y + j, c, a);
        blend_px(x + w - 1 - ii, y + j, c, a);
    }
}
static void darken(int a) { for (int i = 0; i < W * H; i++) px[i] = mix(px[i], 0, a); }

// ---------------------------------------------------------------- fonts
typedef struct { int cp, gi, x0, y0, w, h; float adv; unsigned char *bmp; } Glyph;
typedef struct { int size; float scale; int asc, lineh; Glyph tab[256]; } Face;
static stbtt_fontinfo font;
static Face *faces[12];
static int nfaces;

static Face *face(int size)
{
    for (int i = 0; i < nfaces; i++) if (faces[i]->size == size) return faces[i];
    if (nfaces == 12) return faces[0];
    Face *f = calloc(1, sizeof *f);
    int a, d, g;
    f->size = size; f->scale = stbtt_ScaleForPixelHeight(&font, size);
    stbtt_GetFontVMetrics(&font, &a, &d, &g);
    f->asc = (int)ceilf(a * f->scale);
    f->lineh = (int)ceilf((a - d + g) * f->scale);
    return faces[nfaces++] = f;
}
static Glyph *glyph(Face *f, int cp)
{
    unsigned h = (unsigned)cp * 2654435761u % 256;
    for (int i = 0; i < 256; i++, h = (h + 1) % 256) {
        Glyph *g = &f->tab[h];
        if (g->cp == cp && (g->bmp || g->adv)) return g;
        if (!g->bmp && !g->adv) {
            int adv, lsb;
            g->cp = cp; g->gi = stbtt_FindGlyphIndex(&font, cp);
            stbtt_GetGlyphHMetrics(&font, g->gi, &adv, &lsb);
            g->adv = adv * f->scale + 0.0001f;
            g->bmp = stbtt_GetGlyphBitmap(&font, f->scale, f->scale, g->gi, &g->w, &g->h, &g->x0, &g->y0);
            return g;
        }
    }
    return &f->tab[0];
}
static int utf8(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (s[0] < 0x80) { *p += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) { *p += 3; return ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    *p += 1; return '?';
}
static float text_w(Face *f, const char *s, int n)
{
    const char *e = s + n; float x = 0; int prev = 0;
    while (s < e) {
        Glyph *g = glyph(f, utf8(&s));
        if (prev) x += stbtt_GetGlyphKernAdvance(&font, prev, g->gi) * f->scale;
        x += g->adv; prev = g->gi;
    }
    return x;
}
static float draw_text(Face *f, float x, int base, const char *s, int n, uint32_t col)
{
    const char *e = s + n; int prev = 0;
    while (s < e) {
        Glyph *g = glyph(f, utf8(&s));
        if (prev) x += stbtt_GetGlyphKernAdvance(&font, prev, g->gi) * f->scale;
        int gx = (int)(x + 0.5f) + g->x0, gy = base + g->y0;
        for (int j = 0; j < g->h; j++) {
            int y = gy + j;
            if (y < 0 || y >= H) continue;
            for (int i = 0; i < g->w; i++) {
                int a = g->bmp[j * g->w + i], xx = gx + i;
                if (!a || xx < 0 || xx >= W) continue;
                uint32_t *d = &px[y * W + xx];
                *d = a == 255 ? col : mix(*d, col, a);
            }
        }
        x += g->adv; prev = g->gi;
    }
    return x;
}
static void draw_str(Face *f, int x, int base, const char *s, uint32_t col) { draw_text(f, x, base, s, strlen(s), col); }
static void draw_center(Face *f, int cx, int base, const char *s, uint32_t col) { draw_text(f, cx - text_w(f, s, strlen(s)) / 2, base, s, strlen(s), col); }
static void draw_right(Face *f, int rx, int base, const char *s, uint32_t col) { draw_text(f, rx - text_w(f, s, strlen(s)), base, s, strlen(s), col); }
static void draw_fit(Face *f, int x, int base, const char *s, int maxw, uint32_t col)    // cut with "..." to fit
{
    int n = strlen(s);
    if (text_w(f, s, n) <= maxw) { draw_text(f, x, base, s, n, col); return; }
    float dots = text_w(f, "...", 3);
    while (n > 0 && text_w(f, s, n) + dots > maxw) { n--; while (n > 0 && (s[n] & 0xC0) == 0x80) n--; }
    float e = draw_text(f, x, base, s, n, col);
    draw_text(f, e, base, "...", 3, col);
}

// ---------------------------------------------------------------- files
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    n = fread(b, 1, n, f); b[n] = 0; fclose(f);
    return b;
}
static int write_file(const char *path, const char *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, n, f);
    return fclose(f) || w != n ? -1 : 0;
}
static int read_mhz(const char *path)                  // 0 = no file / unreadable
{
    FILE *f = fopen(path, "r");
    int v = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &v) != 1 || v < 100 || v > 2400) v = 0;
    fclose(f);
    return v;
}
static void write_mhz(const char *path, int mhz)       // same format as Onion's netplay script (echo -n)
{
    char s[16]; int n = snprintf(s, sizeof s, "%d", mhz);
    write_file(path, s, n);
}
static void mkdirs(const char *path)
{
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; MKDIR(tmp); *p = '/'; }
    MKDIR(tmp);
}
static int is_dir(const char *path) { struct stat st; return !stat(path, &st) && S_ISDIR(st.st_mode); }
// "key": "value" from a flat JSON file
static int json_str(const char *json, const char *key, char *out, size_t n)
{
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p || !(p = strchr(p + strlen(pat), ':')) || !(p = strchr(p, '"'))) return 0;
    const char *e = strchr(++p, '"');
    if (!e) return 0;
    snprintf(out, n, "%.*s", (int)(e - p), p);
    return 1;
}

// ---------------------------------------------------------------- model
// Numbers are saved in settings.txt, so new models go on the end; DEV_ORDER is the order people see.
enum { DEV_AUTO, DEV_MINI, DEV_MINI_V4, DEV_PLUS, DEV_PLUS_V2, DEV_FLIP, NDEV };
static const char *DEVNAMES[] = { "Auto", "Miyoo Mini (v1-v3)", "Miyoo Mini v4", "Miyoo Mini Plus", "Miyoo Mini Plus V2 (RK3562)", "Miyoo Mini Flip" };
static const char *DEVSHORT[] = { "", "Mini", "Mini v4", "Mini Plus", "Mini Plus V2", "Mini Flip" };
static const int DEV_ORDER[NDEV] = { DEV_AUTO, DEV_MINI, DEV_MINI_V4, DEV_FLIP, DEV_PLUS, DEV_PLUS_V2 };
static int dev_pos(int d) { for (int i = 0; i < NDEV; i++) if (DEV_ORDER[i] == d) return i; return 0; }
static int dev_choice, dev_detected;
static int dev(void) { return dev_choice ? dev_choice : dev_detected; }
// Zones follow Onion's FAQ: Mini 1600 recommended / 1700 max stable, Mini Plus 1800 / 1900
// (a Mini v3 here froze Neo Geo at 1700 while 1600 ran everything).
// The Plus V2's Rockchip RK3562 (4x Cortex-A53) is rated up to 2.0 GHz; Onion doesn't run on it yet.
// The Mini Flip (Onion model 285) gets the Plus tier: Onion's Flip build caps its OC hotkey at 1800, like the Plus.
static int plus_tier(void) { return dev() == DEV_PLUS || dev() == DEV_FLIP; }
static int green_max(void) { return dev() == DEV_PLUS_V2 ? 2000 : plus_tier() ? 1800 : 1600; }
static int yellow_max(void) { return dev() == DEV_PLUS_V2 ? 2000 : plus_tier() ? 1900 : 1700; }
static int max_mhz(void) { return plus_tier() ? 2100 : dev() == DEV_PLUS_V2 ? 2000 : 1900; }
#define MIN_MHZ 600
#define STOCK_MHZ 1200

enum { R_RA, R_SCRIPT };
typedef struct {
    char label[96], core[64], script[300];
    int kind, value, orig;          // value 0 = follow "All systems" (R_RA only), else MHz
    int script_off;                 // R_SCRIPT: offset of the number after "cpuclock "
} Row;
#define MAXROWS 96
static Row rows[MAXROWS];
static int nrows, universal, universal_orig;      // universal 0 = stock (no file)
static char cfgdir[400], cfgdir_v4[400], unipath[400], appdir[400];
static char cfgnames[128][64];
static int ncfgnames;

static void detect_device(void)
{
    char p[600]; int model = 0; char res[32] = "";
    snprintf(p, sizeof p, "%s/deviceModel", TMP);
    FILE *f = fopen(p, "r");
    if (f) { if (fscanf(f, "%d", &model) != 1) model = 0; fclose(f); }
    snprintf(p, sizeof p, "%s/screen_resolution", TMP);
    if ((f = fopen(p, "r"))) { if (fscanf(f, "%31s", res) != 1) res[0] = 0; fclose(f); }
    // the Flip shares the v4's 752x560 screen, so check its model number first
    dev_detected = model == 354 ? DEV_PLUS : model == 285 ? DEV_FLIP : !strcmp(res, "752x560") ? DEV_MINI_V4 : model == 283 ? DEV_MINI : 0;
    if (!dev_detected) {                        // no Onion files: look for the V2's Rockchip SoC
#ifdef PREVIEW
        snprintf(p, sizeof p, "%s/compatible", TMP);
#else
        snprintf(p, sizeof p, "/proc/device-tree/compatible");
#endif
        if ((f = fopen(p, "rb"))) {
            char buf[256]; size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';            // the entries are NUL-separated
            buf[n] = 0;
            if (strstr(buf, "rk3562")) dev_detected = DEV_PLUS_V2;
        }
    }
}
// RetroArch names the folder after the core's library_name; the .info corename matches it,
// but prefer a folder that already exists (Onion ships "lutro" in lower case).
static void core_folder(const char *so, char *out, size_t n)
{
    char p[600], name[64] = "";
    snprintf(p, sizeof p, "%s/RetroArch/.retroarch/cores/%s_libretro.info", SD, so);
    char *info = read_file(p);
    if (info) {
        char *c = strstr(info, "\ncorename");
        if (!c && !strncmp(info, "corename", 8)) c = info - 1;
        if (c && (c = strchr(c, '"'))) { char *e = strchr(c + 1, '"'); if (e) snprintf(name, sizeof name, "%.*s", (int)(e - c - 1), c + 1); }
        free(info);
    }
    if (!name[0]) snprintf(name, sizeof name, "%s", so);
    for (int i = 0; i < ncfgnames; i++) if (!strcasecmp(cfgnames[i], name)) { snprintf(out, n, "%s", cfgnames[i]); return; }
    snprintf(out, n, "%s", name);
}
// Cores a RetroArch launch.sh can start: literal "<core>_libretro.so" names, plus every value assigned to a
// variable used as "${var}_libretro.so" or "$var" (e.g. Arcade switching MAME 2003-Plus / FinalBurn Neo per game).
#define MAXCORES 6
static void add_core(char cores[][48], int *nc, const char *name, int len)
{
    char c[48]; snprintf(c, sizeof c, "%.*s", len, name);
    char *suffix = strstr(c, "_libretro"); if (suffix) *suffix = 0;
    if (!c[0]) return;
    for (int i = 0; i < *nc; i++) if (!strcmp(cores[i], c)) return;
    if (*nc < MAXCORES) snprintf(cores[(*nc)++], 48, "%s", c);
}
static int find_cores(const char *sh, char cores[][48])
{
    int nc = 0;
    for (const char *p = sh; (p = strstr(p, "_libretro.so")); p += 12) {
        const char *s = p;
        while (s > sh && (isalnum((unsigned char)s[-1]) || strchr("_${}", s[-1]))) s--;
        char tok[64]; snprintf(tok, sizeof tok, "%.*s", (int)(p - s), s);
        if (!strchr(tok, '$')) { add_core(cores, &nc, tok, strlen(tok)); continue; }
        char var[48] = "", *v = tok;
        while (*v == '$' || *v == '{') v++;
        snprintf(var, sizeof var, "%s", v);
        char *b = strchr(var, '}'); if (b) *b = 0;
        char pat[64]; snprintf(pat, sizeof pat, "%s=", var);
        for (const char *q = sh; (q = strstr(q, pat)); q++) {
            if (q > sh && !isspace((unsigned char)q[-1])) continue;
            const char *val = q + strlen(pat);
            if (*val == '"' || *val == '\'') val++;
            int k = 0;
            while (k < 47 && (isalnum((unsigned char)val[k]) || val[k] == '_')) k++;
            add_core(cores, &nc, val, k);
        }
    }
    // "-L .../$core" with core=<name>_libretro.so assignments
    for (const char *p = sh; (p = strstr(p, "_libretro.so")); p += 12) {
        const char *s = p;
        while (s > sh && (isalnum((unsigned char)s[-1]) || s[-1] == '_')) s--;
        if (s > sh && s[-1] == '=') add_core(cores, &nc, s, (int)(p - s));
    }
    return nc;
}
static int has_games(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d))) {
        const char *n = e->d_name, *dot = strrchr(n, '.');
        if (n[0] == '.' || !strcasecmp(n, "Imgs") || !strcasecmp(n, "miyoogamelist.xml") || (dot && !strcasecmp(dot, ".db"))) continue;
        found = 1;
    }
    closedir(d);
    return found;
}
static int cmp_rows(const void *a, const void *b) { return strcasecmp(((const Row *)a)->label, ((const Row *)b)->label); }

static void scan(void)
{
    char p[600];
    snprintf(cfgdir, sizeof cfgdir, "%s/Saves/CurrentProfile/config", SD);
    snprintf(cfgdir_v4, sizeof cfgdir_v4, "%s/Saves/CurrentProfile/config_v4", SD);
    snprintf(unipath, sizeof unipath, "%s/RetroArch/cpuclock.txt", SD);
    universal = universal_orig = read_mhz(unipath);
    DIR *d = opendir(cfgdir);
    struct dirent *e;
    if (d) { while ((e = readdir(d)) && ncfgnames < 128) if (e->d_name[0] != '.') snprintf(cfgnames[ncfgnames++], 64, "%s", e->d_name); closedir(d); }

    snprintf(p, sizeof p, "%s/Emu", SD);
    if (!(d = opendir(p))) return;
    while ((e = readdir(d)) && nrows < MAXROWS) {
        if (e->d_name[0] == '.') continue;
        char sys[300], json_path[600], sh_path[600], label[96], rompath[300], romdir[700];
        snprintf(sys, sizeof sys, "%s/Emu/%s", SD, e->d_name);
        snprintf(json_path, sizeof json_path, "%s/config.json", sys);
        snprintf(sh_path, sizeof sh_path, "%s/launch.sh", sys);
        char *json = read_file(json_path), *sh = read_file(sh_path);
        if (!json || !sh) { free(json); free(sh); continue; }
        if (!json_str(json, "label", label, sizeof label)) snprintf(label, sizeof label, "%s", e->d_name);
        if (!json_str(json, "rompath", rompath, sizeof rompath)) rompath[0] = 0;
        if (!strncmp(rompath, "/mnt/SDCARD", 11)) snprintf(romdir, sizeof romdir, "%s%s", SD, rompath + 11);
        else snprintf(romdir, sizeof romdir, "%s/%s", sys, rompath);
        char cores[MAXCORES][48];
        int nc = 0;
        if (rompath[0] && has_games(romdir)) {
            if (strstr(sh, "retroarch") && (nc = find_cores(sh, cores))) {
                for (int c = 0; c < nc && nrows < MAXROWS; c++) {
                    char core[64], name[96]; core_folder(cores[c], core, sizeof core);
                    if (nc > 1) snprintf(name, sizeof name, "%s (%s)", label, core);   // e.g. Arcade (FinalBurn Neo)
                    else snprintf(name, sizeof name, "%s", label);
                    Row *r = NULL;
                    for (int i = 0; i < nrows; i++) if (rows[i].kind == R_RA && !strcmp(rows[i].core, core)) r = &rows[i];
                    if (r) {
                        size_t l = strlen(r->label);
                        snprintf(r->label + l, sizeof r->label - l, " / %s", name);
                    }
                    else {
                        r = &rows[nrows++]; memset(r, 0, sizeof *r);
                        r->kind = R_RA;
                        snprintf(r->label, sizeof r->label, "%s", name);
                        snprintf(r->core, sizeof r->core, "%s", core);
                        snprintf(p, sizeof p, "%s/%s/cpuclock.txt", cfgdir, core);
                        r->value = r->orig = read_mhz(p);
                    }
                }
            }
            else {
                const char *c = sh;
                while ((c = strstr(c, "cpuclock ")) && !isdigit((unsigned char)c[9])) c += 9;
                if (c) {
                    Row *r = &rows[nrows++]; memset(r, 0, sizeof *r);
                    r->kind = R_SCRIPT;
                    snprintf(r->label, sizeof r->label, "%s", label);
                    snprintf(r->core, sizeof r->core, "own setting in launch.sh");
                    snprintf(r->script, sizeof r->script, "%s", sh_path);
                    r->script_off = (int)(c + 9 - sh);
                    r->value = r->orig = atoi(c + 9);
                }
            }
        }
        free(json); free(sh);
    }
    closedir(d);
    qsort(rows, nrows, sizeof rows[0], cmp_rows);
}

// First run: remember what the card had, so "Restore original clocks" can put it back.
static void backup_originals(void)
{
    char p[600];
    snprintf(p, sizeof p, "%s/backup", appdir);
    mkdirs(p);
    snprintf(p, sizeof p, "%s/backup/originals.txt", appdir);
    FILE *f = fopen(p, "r");
    if (f) { fclose(f); return; }
    if (!(f = fopen(p, "w"))) return;
    fprintf(f, "U\t-\t%d\n", universal);
    for (int i = 0; i < nrows; i++) fprintf(f, "%c\t%s\t%d\n", rows[i].kind == R_RA ? 'C' : 'S', rows[i].kind == R_RA ? rows[i].core : rows[i].script, rows[i].orig);
    fclose(f);
}
static int restore_originals(void)
{
    char p[600];
    snprintf(p, sizeof p, "%s/backup/originals.txt", appdir);
    FILE *f = fopen(p, "r");
    char line[512];
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        *t1 = *t2 = 0;
        int v = atoi(t2 + 1);
        if (line[0] == 'U') universal = v;
        for (int i = 0; i < nrows; i++) {
            Row *r = &rows[i];
            if ((line[0] == 'C' && r->kind == R_RA && !strcmp(r->core, t1 + 1)) || (line[0] == 'S' && r->kind == R_SCRIPT && !strcmp(r->script, t1 + 1))) r->value = v;
        }
    }
    fclose(f);
    return 1;
}
static int warned;                          // risk warning accepted once
static void load_settings(void)
{
    char p[600]; snprintf(p, sizeof p, "%s/settings.txt", appdir);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fscanf(f, "device %d warned %d", &dev_choice, &warned) < 1 || dev_choice < 0 || dev_choice >= NDEV) dev_choice = 0;
        fclose(f);
    }
}
static void save_settings(void)
{
    char p[600]; snprintf(p, sizeof p, "%s/settings.txt", appdir);
    FILE *f = fopen(p, "w");
    if (f) { fprintf(f, "device %d\nwarned %d\n", dev_choice, warned); fclose(f); }
}
static int apply(void)                      // writes only what changed; returns number of files touched
{
    char p[700];
    int n = 0;
    save_settings();
    if (universal != universal_orig) {
        if (universal) write_mhz(unipath, universal); else remove(unipath);
        universal_orig = universal; n++;
    }
    for (int i = 0; i < nrows; i++) {
        Row *r = &rows[i];
        if (r->value == r->orig) continue;
        if (r->kind == R_RA) {
            const char *dirs[2] = { cfgdir, cfgdir_v4 };
            for (int k = 0; k < 2; k++) {
                char d[600]; snprintf(d, sizeof d, "%s/%s", dirs[k], r->core);
                if (k == 1 && !is_dir(d)) continue;          // config_v4 only where the card already uses it
                snprintf(p, sizeof p, "%s/cpuclock.txt", d);
                if (r->value) { mkdirs(d); write_mhz(p, r->value); } else remove(p);
            }
        }
        else {
            char *sh = read_file(r->script);
            if (!sh) continue;
            char bak[700]; const char *sys = strstr(r->script, "/Emu/");
            snprintf(bak, sizeof bak, "%s/backup/%s", appdir, sys ? sys + 5 : "launch.sh");
            for (char *c = bak + strlen(appdir) + 8; *c; c++) if (*c == '/') *c = '_';
            FILE *t = fopen(bak, "r");
            if (t) fclose(t); else write_file(bak, sh, strlen(sh));          // keep the very first version
            int end = r->script_off;
            while (isdigit((unsigned char)sh[end])) end++;
            char num[16]; int nl = snprintf(num, sizeof num, "%d", r->value);
            size_t len = strlen(sh), out_len = len - (end - r->script_off) + nl;
            char *out = malloc(out_len + 1);
            memcpy(out, sh, r->script_off);
            memcpy(out + r->script_off, num, nl);
            memcpy(out + r->script_off + nl, sh + end, len - end + 1);
            write_file(r->script, out, out_len);
            free(out); free(sh);
        }
        r->orig = r->value; n++;
    }
    plat_sync();
    return n;
}

// ---------------------------------------------------------------- ui
enum { I_DEVICE, I_ALL, I_HEADER, I_ROW, I_RESTORE };
typedef struct { int kind, row; } Item;
static Item items[MAXROWS + 4];
static int nitems, sel = 1, scroll, show_help, confirm_restore;
static char toast[64];

static void build_items(void)
{
    nitems = 0;
    items[nitems++] = (Item){ I_DEVICE, -1 };
    items[nitems++] = (Item){ I_ALL, -1 };
    items[nitems++] = (Item){ I_HEADER, -1 };
    for (int i = 0; i < nrows; i++) items[nitems++] = (Item){ I_ROW, i };
    items[nitems++] = (Item){ I_RESTORE, -1 };
}
static uint32_t lerp(uint32_t a, uint32_t b, float t)
{
    int k = (int)(t * 255 + 0.5f);
    return mix(a, b, k < 0 ? 0 : k > 255 ? 255 : k);
}
// Smooth shift green -> yellow -> orange across the zone edges.
static uint32_t mhz_color(int mhz)
{
    int g = green_max(), y = yellow_max();
    if (mhz <= g) return C_GREEN;
    if (mhz <= g + 50) return lerp(C_GREEN, C_YELLOW, (mhz - g) / 50.0f);
    if (mhz <= y) return C_YELLOW;
    if (mhz <= y + 50) return lerp(C_YELLOW, C_ORANGE, (mhz - y) / 50.0f);
    return C_ORANGE;
}
static const char *mhz_rating(int mhz)
{
    if (mhz < STOCK_MHZ) return "Underclock - saves battery";
    if (mhz == STOCK_MHZ) return "Stock speed";
    if (mhz <= green_max()) return "Safe overclock";
    if (mhz <= yellow_max()) return "Mild overclock - most units fine";
    return "Likely to crash or freeze";
}
static int effective(const Item *it)        // MHz the item really runs at (0 = stock)
{
    if (it->kind == I_ALL) return universal;
    if (it->kind == I_ROW) { Row *r = &rows[it->row]; return r->value ? r->value : r->kind == R_RA ? universal : 0; }
    return 0;
}
static void value_text(const Item *it, char *s, size_t n, uint32_t *col)
{
    *col = C_FG;
    if (it->kind == I_DEVICE) { snprintf(s, n, "%s", dev_choice ? DEVSHORT[dev_choice] : dev_detected ? "Auto" : "Choose"); *col = C_ACCENT; return; }
    if (it->kind == I_RESTORE) { snprintf(s, n, "A"); *col = C_DIM; return; }
    int m = effective(it);
    if (it->kind == I_ROW && rows[it->row].kind == R_RA && !rows[it->row].value) {
        if (universal) snprintf(s, n, "All (%d)", universal); else snprintf(s, n, "All (stock)");
        *col = C_DIM; return;
    }
    if (!m) { snprintf(s, n, "Stock"); *col = C_GREEN; return; }
    snprintf(s, n, "%d MHz", m);
    *col = mhz_color(m);
}

static int BAR, ROWH, LIST_Y, LIST_H, PANEL_H, HINT_H, MARGIN;
static void layout(void)
{
    BAR = H / 13; ROWH = H / 14; MARGIN = W / 28;
    HINT_H = H / 16; PANEL_H = H * 23 / 100;
    LIST_Y = BAR; LIST_H = H - BAR - PANEL_H - HINT_H;
}
static void draw_gauge(int x, int y, int w, int h, int mhz)
{
    int lo = MIN_MHZ, hi = max_mhz();
    rrect(x, y, w, h, h / 2, C_LINE);
    int fw = mhz ? (int)((float)(mhz - lo) / (hi - lo) * w) : (int)((float)(STOCK_MHZ - lo) / (hi - lo) * w);
    for (int i = 0; i < fw; i++) {                            // gradient fill: each column takes the colour of its MHz
        int m = lo + (int)((float)i / w * (hi - lo));
        uint32_t c = mhz_color(m);
        int top = y, bot = y + h;
        if (i < h / 2 || i > fw - h / 2) {                    // rounded ends
            int dx = i < h / 2 ? h / 2 - i : i - (fw - h / 2);
            int dy = (int)sqrtf((float)((h / 2) * (h / 2) - dx * dx));
            top = y + h / 2 - dy; bot = y + h / 2 + dy;
        }
        fill(x + i, top, 1, bot - top, c);
    }
    Face *sm = face(H / 34);
    int marks[] = { STOCK_MHZ, green_max(), yellow_max() };
    const char *names[] = { "stock", "", "" };
    for (int k = 0; k < 3; k++) {
        int mx = x + (int)((float)(marks[k] - lo) / (hi - lo) * w);
        fill(mx, y - 3, 1, h + 6, C_DIM);
        char s[24]; snprintf(s, sizeof s, "%d%s%s", marks[k], names[k][0] ? " " : "", names[k]);
        draw_center(sm, mx, y + h + 3 + sm->asc, s, C_DIM);
    }
}
static void draw_panel(void)
{
    Item *it = &items[sel];
    Face *ui = face(H / 22), *big = face(H / 13), *sm = face(H / 30);
    int y = H - HINT_H - PANEL_H;
    fill(0, y, W, PANEL_H, C_PANEL);
    fill(0, y, W, 1, C_LINE);
    int ty = y + H / 60 + sm->asc;
    if (it->kind == I_DEVICE) {
        char s[96];
        draw_str(ui, MARGIN, ty + ui->asc - sm->asc, dev() ? DEVNAMES[dev()] : "Unknown model", C_FG);
        snprintf(s, sizeof s, "%s", dev_detected ? (dev_choice ? "Set by hand. A to pick another, X for auto." : "Detected automatically. A to pick a model by hand.")
                                                : "Could not detect the model. Press A to pick yours.");
        draw_str(sm, MARGIN, ty + ui->lineh + sm->asc / 2, s, C_DIM);
        if (dev() == DEV_PLUS_V2) snprintf(s, sizeof s, "Onion OS doesn't run on the V2 yet, so these clocks won't apply.");
        else snprintf(s, sizeof s, "Color zones:  green up to %d   yellow up to %d   orange above", green_max(), yellow_max());
        draw_str(sm, MARGIN, ty + ui->lineh + sm->lineh + sm->asc / 2 + H / 80, s, dev() == DEV_PLUS_V2 ? C_ORANGE : C_DIM);
        return;
    }
    if (it->kind == I_RESTORE) {
        draw_str(ui, MARGIN, ty + ui->asc - sm->asc, "Restore original clocks", C_FG);
        draw_str(sm, MARGIN, ty + ui->lineh + sm->asc / 2, "Puts back the clocks this card had before Overclock first ran.", C_DIM);
        return;
    }
    char sub[160];
    if (it->kind == I_ALL) snprintf(sub, sizeof sub, "All systems  -  every RetroArch system without its own setting");
    else snprintf(sub, sizeof sub, "%s  -  %s", rows[it->row].label, rows[it->row].core);
    draw_fit(sm, MARGIN, ty, sub, W - 2 * MARGIN, C_DIM);
    int m = effective(it);
    char v[32];
    if (m) snprintf(v, sizeof v, "%d MHz", m); else snprintf(v, sizeof v, "Stock");
    uint32_t col = m ? mhz_color(m) : C_GREEN;
    int vy = ty + sm->lineh / 2 + big->asc;
    float vx = draw_text(big, MARGIN, vy, v, strlen(v), col);
    draw_str(ui, (int)vx + W / 30, vy, m ? mhz_rating(m) : "Onion default (about 1200 MHz)", col);
    draw_gauge(MARGIN, vy + H / 40, W - 2 * MARGIN, H / 40, m);
}
static void draw_help(void)
{
    static const char *rows_[][2] = {
        { "Up / Down", "Choose a setting" }, { "Left / Right", "Change by 50 MHz" }, { "L1 / R1", "Change by 200 MHz" },
        { "X", "Back to default (All systems / stock)" }, { "A", "Pick model / restore (on those rows)" },
        { "B or Menu", "Save and quit" }, { "Select", "Quit without saving" }, { "Start", "This help" },
    };
    int n = sizeof rows_ / sizeof rows_[0];
    Face *ui = face(H / 22), *sm = face(H / 28);
    int rowh = sm->lineh + H / 100, pw = W * 88 / 100, ph = ui->lineh * 2 + rowh * n + sm->lineh * 3 + H / 12;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    darken(170);
    rrect(x, y, pw, ph, H / 40, C_PANEL);
    draw_center(ui, W / 2, y + H / 40 + ui->asc, "Controls", C_ACCENT);
    int ry = y + H / 40 + ui->lineh * 3 / 2, cx = x + pw * 36 / 100;
    for (int i = 0; i < n; i++, ry += rowh) {
        draw_right(sm, cx - H / 40, ry + sm->asc, rows_[i][0], C_ACCENT);
        draw_str(sm, cx + H / 40, ry + sm->asc, rows_[i][1], C_FG);
    }
    ry += H / 60;
    draw_center(sm, W / 2, ry + sm->asc, "Clocks apply the next time a game starts.", C_DIM);
    draw_center(sm, W / 2, ry + sm->lineh + sm->asc, "If a game freezes: hold Power to restart,", C_DIM);
    draw_center(sm, W / 2, ry + sm->lineh * 2 + sm->asc, "then lower that system's clock.", C_DIM);
}
static void draw_warning(void)
{
    static const char *lines[] = {
        "Overclocking runs the CPU faster than it was",
        "designed for. It can cause crashes and freezes,",
        "corrupt a save if a game locks up while saving,",
        "add heat, shorten battery life, and could",
        "damage your device.",
        "",
        "Yellow and orange settings carry more risk.",
        "You use this app at your own risk.",
    };
    Face *big = face(H / 16), *sm = face(H / 26);
    fill(0, 0, W, H, C_BG);
    int n = sizeof lines / sizeof lines[0], y = H / 12;
    rrect(W / 2 - H / 22, y, H / 11, H / 11, H / 22, C_ORANGE);                 // warning badge
    draw_center(big, W / 2, y + (H / 11 - big->lineh) / 2 + big->asc, "!", C_BG);
    y += H / 11 + H / 30;
    draw_center(big, W / 2, y + big->asc, "Before you overclock", C_ORANGE);
    y += big->lineh + H / 40;
    for (int i = 0; i < n; i++, y += sm->lineh) draw_center(sm, W / 2, y + sm->asc, lines[i], i >= 6 ? C_FG : C_DIM);
    int by = H - H / 7, bw = W * 30 / 100, bh = H / 13;
    rrect(W / 2 - bw - W / 40, by, bw, bh, bh / 2, C_ORANGE);
    draw_center(sm, W / 2 - bw / 2 - W / 40, by + (bh - sm->lineh) / 2 + sm->asc, "A  I understand", C_BG);
    rrect(W / 2 + W / 40, by, bw, bh, bh / 2, C_LINE);
    draw_center(sm, W / 2 + bw / 2 + W / 40, by + (bh - sm->lineh) / 2 + sm->asc, "B  Quit", C_FG);
}
// Device picker: A on the Device row lists every model.
static int pick_device, pick_sel;
static void draw_picker(void)
{
    Face *ui = face(H / 22), *sm = face(H / 28);
    int rowh = ui->lineh + H / 60, pw = W * 74 / 100, ph = ui->lineh * 2 + rowh * NDEV + sm->lineh + H / 14;
    int x = (W - pw) / 2, y = (H - ph) / 2;
    darken(170);
    rrect(x, y, pw, ph, H / 40, C_PANEL);
    draw_center(ui, W / 2, y + H / 40 + ui->asc, "Choose your model", C_FG);
    int ry = y + H / 40 + ui->lineh * 3 / 2;
    for (int i = 0; i < NDEV; i++, ry += rowh) {
        int d = DEV_ORDER[i];
        char s[96];
        if (d == DEV_AUTO) snprintf(s, sizeof s, "Auto  (%s)", dev_detected ? DEVNAMES[dev_detected] : "not detected");
        else snprintf(s, sizeof s, "%s", DEVNAMES[d]);
        if (i == pick_sel) rrect(x + H / 40, ry, pw - H / 20, rowh, H / 70, C_SEL);
        if (d == dev_choice) { int r = H / 90; rrect(x + H / 40 + H / 60, ry + rowh / 2 - r, r * 2, r * 2, r, C_ACCENT); }   // current choice
        draw_str(ui, x + H / 40 + H / 60 + H / 30, ry + (rowh - ui->lineh) / 2 + ui->asc, s, d == DEV_AUTO && !dev_detected ? C_DIM : C_FG);
    }
    draw_center(sm, W / 2, y + ph - H / 40 - sm->lineh + sm->asc, "A  Choose        B  Cancel", C_ACCENT);
}
static void draw_confirm(void)
{
    Face *ui = face(H / 22), *sm = face(H / 28);
    int pw = W * 70 / 100, ph = ui->lineh + sm->lineh * 3 + H / 10, x = (W - pw) / 2, y = (H - ph) / 2;
    darken(170);
    rrect(x, y, pw, ph, H / 40, C_PANEL);
    draw_center(ui, W / 2, y + H / 30 + ui->asc, "Restore original clocks?", C_FG);
    draw_center(sm, W / 2, y + H / 30 + ui->lineh + sm->asc, "Every setting goes back to how the card", C_DIM);
    draw_center(sm, W / 2, y + H / 30 + ui->lineh + sm->lineh + sm->asc, "was before Overclock first ran.", C_DIM);
    draw_center(sm, W / 2, y + ph - H / 30 - sm->lineh + sm->asc, "A  Restore        B  Cancel", C_ACCENT);
}
static void draw(void)
{
    Face *ui = face(H / 22), *sm = face(H / 30);
    if (!warned) { draw_warning(); plat_present(); return; }
    fill(0, 0, W, H, C_BG);
    // list
    int content = 0;
    for (int i = 0; i < nitems; i++) content += items[i].kind == I_HEADER ? ROWH * 4 / 5 : ROWH;
    int sy = 0;
    for (int i = 0; i < sel; i++) sy += items[i].kind == I_HEADER ? ROWH * 4 / 5 : ROWH;
    if (sy < scroll) scroll = sy - (sel == 3 ? ROWH * 4 / 5 : 0);
    if (sy + ROWH > scroll + LIST_H) scroll = sy + ROWH - LIST_H;
    if (scroll > content - LIST_H) scroll = content - LIST_H;
    if (scroll < 0) scroll = 0;
    int y = LIST_Y - scroll;
    for (int i = 0; i < nitems; i++) {
        Item *it = &items[i];
        int h = it->kind == I_HEADER ? ROWH * 4 / 5 : ROWH;
        if (y + h > LIST_Y && y < LIST_Y + LIST_H) {
            int base = y + (h - ui->lineh) / 2 + ui->asc;
            if (it->kind == I_HEADER) {
                draw_str(sm, MARGIN, y + h - H / 80 - (sm->lineh - sm->asc), nrows ? "PER SYSTEM" : "PER SYSTEM  (no RetroArch systems with games found)", C_DIM);
            }
            else {
                if (i == sel) rrect(MARGIN / 2, y + 2, W - MARGIN, h - 4, H / 70, C_SEL);
                const char *label = it->kind == I_DEVICE ? "Device" : it->kind == I_ALL ? "All systems" : it->kind == I_RESTORE ? "Restore original clocks" : rows[it->row].label;
                char v[48]; uint32_t vc;
                value_text(it, v, sizeof v, &vc);
                int vw = (int)text_w(ui, v, strlen(v));
                draw_fit(ui, MARGIN, base, label, W - 3 * MARGIN - vw - W / 20, C_FG);
                if (it->kind == I_ROW || it->kind == I_ALL) {
                    int m = effective(it);
                    if (m && !(it->kind == I_ROW && rows[it->row].kind == R_RA && !rows[it->row].value)) {
                        int r = H / 90;
                        rrect(W - MARGIN - vw - r * 4, y + h / 2 - r, r * 2, r * 2, r, vc);      // colour dot
                    }
                }
                draw_str(ui, W - MARGIN - vw, base, v, vc);
            }
        }
        y += h;
    }
    // title bar
    fill(0, 0, W, BAR, C_PANEL);
    fill(0, BAR - 1, W, 1, C_LINE);
    int tb = (BAR - ui->lineh) / 2 + ui->asc;
    draw_str(ui, MARGIN, tb, "Overclock", C_FG);
    char right[48];
    snprintf(right, sizeof right, "%s%s", dev() ? DEVSHORT[dev()] : "Unknown model", dev_choice ? "" : dev_detected ? "  (auto)" : "");
    draw_right(sm, W - MARGIN, tb, right, C_DIM);
    draw_panel();
    // hints
    int hy = H - HINT_H;
    fill(0, hy, W, HINT_H, C_PANEL);
    fill(0, hy, W, 1, C_LINE);
    const char *hint = toast[0] ? toast : dev() == DEV_PLUS_V2 ? "Mini Plus V2: no Onion support yet - clocks won't apply"
                                                            : "Left/Right 50   L1/R1 200   X default   B save & quit   Start help";
    draw_center(sm, W / 2, hy + (HINT_H - sm->lineh) / 2 + sm->asc, hint, toast[0] ? C_ACCENT : dev() == DEV_PLUS_V2 ? C_ORANGE : C_DIM);
    if (show_help) draw_help();
    if (confirm_restore) draw_confirm();
    if (pick_device) draw_picker();
    plat_present();
}

static void change(int delta)
{
    Item *it = &items[sel];
    if (it->kind == I_DEVICE) {
        int i = (dev_pos(dev_choice) + (delta > 0 ? 1 : NDEV - 1)) % NDEV;
        if (i == 0 && !dev_detected) i = delta > 0 ? 1 : NDEV - 1;         // no "Auto" without a detected model
        dev_choice = DEV_ORDER[i];
        return;
    }
    int *v = it->kind == I_ALL ? &universal : it->kind == I_ROW ? &rows[it->row].value : NULL;
    if (!v) return;
    int cur = *v ? *v : (it->kind == I_ROW && rows[it->row].kind == R_RA && universal ? universal : STOCK_MHZ);
    int nv = cur + delta;
    if (nv < MIN_MHZ) nv = MIN_MHZ;
    if (nv > max_mhz()) nv = max_mhz();
    nv = nv / 50 * 50;
    *v = nv;
}
static void set_default(void)
{
    Item *it = &items[sel];
    if (it->kind == I_DEVICE) dev_choice = dev_detected ? 0 : dev_choice;
    else if (it->kind == I_ALL) universal = 0;
    else if (it->kind == I_ROW) rows[it->row].value = rows[it->row].kind == R_RA ? 0 : rows[it->row].orig;
}
static int handle(int k)                     // 0 = quit
{
    toast[0] = 0;
    if (k == K_QUIT) return warned ? 0 : -1;
    if (!warned) {
        if (k == K_A) { warned = 1; save_settings(); return 1; }
        return k == K_B || k == K_MENU || k == K_SELECT ? -1 : 1;      // quit without touching any clock
    }
    if (show_help) { show_help = 0; return 1; }
    if (confirm_restore) {
        if (k == K_A) { restore_originals(); snprintf(toast, sizeof toast, "Originals restored - B to save"); }
        confirm_restore = 0;
        return 1;
    }
    if (pick_device) {
        if (k == K_UP) pick_sel = (pick_sel + NDEV - 1) % NDEV;
        else if (k == K_DOWN) pick_sel = (pick_sel + 1) % NDEV;
        else if (k == K_A) { if (pick_sel || dev_detected) dev_choice = DEV_ORDER[pick_sel]; pick_device = 0; }
        else if (k == K_B || k == K_MENU) pick_device = 0;
        return 1;
    }
    switch (k) {
    case K_UP: do sel = sel > 0 ? sel - 1 : nitems - 1; while (items[sel].kind == I_HEADER); break;
    case K_DOWN: do sel = sel < nitems - 1 ? sel + 1 : 0; while (items[sel].kind == I_HEADER); break;
    case K_LEFT: change(-50); break;
    case K_RIGHT: change(50); break;
    case K_L1: change(-200); break;
    case K_R1: change(200); break;
    case K_X: set_default(); break;
    case K_A:
        if (items[sel].kind == I_RESTORE) confirm_restore = 1;
        else if (items[sel].kind == I_DEVICE) { pick_device = 1; pick_sel = dev_pos(dev_choice); }
        break;
    case K_START: show_help = 1; break;
    case K_SELECT: return -1;
    case K_B: case K_MENU: return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
#ifdef PREVIEW
    pkeys = argv + 1; npkeys = argc - 1;
    snprintf(appdir, sizeof appdir, ".");
#else
    (void)argc;
    snprintf(appdir, sizeof appdir, "%s", argv[0]);
    char *slash = strrchr(appdir, '/');
    if (slash) *slash = 0; else snprintf(appdir, sizeof appdir, ".");
    if (chdir(appdir)) return 1;
#endif
    unsigned char *ttf = (unsigned char *)read_file("Inter.ttf");
    if (!ttf || !stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) return 1;
    if (plat_init()) return 1;
    layout();
    detect_device();
    load_settings();
    scan();
    backup_originals();
    build_items();
    if (!dev()) sel = 0;                       // unknown model: start on the device row
    show_help = 0;
    draw();
    int r;
    while ((r = handle(plat_key())) == 1) draw();
    if (r == 0) {
        int n = apply();
        snprintf(toast, sizeof toast, n ? "Saved" : "No changes");
        draw();
    }
    return 0;
}
