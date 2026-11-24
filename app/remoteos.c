// Remote OS — native room remote for the Xiaomi Redmi 7 (onclite); runs as /sbin/recovery in a TWRP-based ramdisk.
// Pages: TV (Nova, NEC 00 7F), FAN (Walton BLDC, NEC D8 7E), SYSTEM.
// Screen off after inactivity; wake by touchscreen double-tap (goodix gesture) or /tmp/wake.
// Build: zig cc -target aarch64-linux-musl -static -O2 -o remoteos remoteos.c -lm
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
#include <time.h>
#include <unistd.h>
#include "fonts.h"

#define VERSION "0.7"
#define IR_DEV "/dev/spidev6.1"
#define BL_PATH "/sys/class/leds/lcd-backlight/brightness"
#define BL_MAX_PATH "/sys/class/leds/lcd-backlight/max_brightness"
#define BAT_CAP "/sys/class/power_supply/battery/capacity"
#define BAT_STATUS "/sys/class/power_supply/battery/status"
#define GESTURE "/sys/android_touch/wakeup_gesture_enable"
#define HEARTBEAT "/tmp/rhb"
#define WAKEFILE "/tmp/wake"
#define LOGF "/tmp/remoteos.log"
#define SCREEN_TIMEOUT_MS 5000       /* idle after waking */
#define SCREEN_TIMEOUT_KEY_MS 5000   /* idle after a button press */
#define TPAD_STEP 80                  /* touchpad: px of travel per arrow key */
#define SLEEP_AFTER_OFF_MS 3000        /* enter suspend this long after the screen goes off */
#define BT_LINGER_MS 300000           /* keep the TV Bluetooth link this long after screen-off */
#define SAFETY_WAKE_S 18000            /* RTC alarm: wake every 5 h (battery check, safety net) */
#define STATE_DIR "/cache/remoteos"
#define STATE_FILE STATE_DIR "/state"

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOGF, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fputc('\n', f); fclose(f);
}
static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static int read_int(const char *p) { FILE *f = fopen(p, "r"); int v = -1; if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); } return v; }
static void read_str(const char *p, char *buf, int n) { FILE *f = fopen(p, "r"); buf[0] = 0; if (f) { if (!fgets(buf, n, f)) buf[0] = 0; fclose(f); } buf[strcspn(buf, "\n")] = 0; }
static void write_str(const char *p, const char *s) { int fd = open(p, O_WRONLY); if (fd >= 0) { if (write(fd, s, strlen(s)) < 0) {} close(fd); } }

/* =========================================================== framebuffer + drawing */
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static uint8_t *fbmem;
static uint32_t *back;
static int fbfd, W, H, STRIDE_PX;

static uint32_t C(uint32_t rgb) { // 0xRRGGBB -> fb pixel
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    return r << vi.red.offset | g << vi.green.offset | b << vi.blue.offset | (vi.transp.length ? 255u << vi.transp.offset : 0);
}
static inline void blend(int x, int y, uint32_t c, float a) {
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    uint32_t *p = &back[y * STRIDE_PX + x];
    if (a >= 1) { *p = c; return; }
    uint32_t d = *p, o = 0;
    for (int s = 0; s < 32; s += 8) {
        float v = (float)(c >> s & 255) * a + (float)(d >> s & 255) * (1 - a);
        o |= (uint32_t)(v + 0.5f) << s;
    }
    *p = o;
}
static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void fill_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++) for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) back[j * STRIDE_PX + i] = c;
}
static void fill_rrect(int x, int y, int w, int h, float r, uint32_t c) {
    if (r > w / 2.f) r = w / 2.f; if (r > h / 2.f) r = h / 2.f;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) {
        float px = i + 0.5f, py = j + 0.5f, cx = px, cy = py;
        if (px < x + r) cx = x + r; else if (px > x + w - r) cx = x + w - r;
        if (py < y + r) cy = y + r; else if (py > y + h - r) cy = y + h - r;
        float d = hypotf(px - cx, py - cy);
        blend(i, j, c, r > 0 ? clampf(r + 0.5f - d) : 1.f);
    }
}
static void fill_rrect_clip(int x, int y, int w, int h, float r, int cy0, int cy1, uint32_t c) {
    for (int j = (y > cy0 ? y : cy0); j < y + h && j < cy1; j++) for (int i = x; i < x + w; i++) {
        float px = i + 0.5f, py = j + 0.5f, qx = px, qy = py;
        if (px < x + r) qx = x + r; else if (px > x + w - r) qx = x + w - r;
        if (py < y + r) qy = y + r; else if (py > y + h - r) qy = y + h - r;
        blend(i, j, c, clampf(r + 0.5f - hypotf(px - qx, py - qy)));
    }
}
static void fill_circle(float cx, float cy, float r, uint32_t c) {
    for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++) for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++)
        blend(i, j, c, clampf(r + 0.5f - hypotf(i + 0.5f - cx, j + 0.5f - cy)));
}
static void ring(float cx, float cy, float r, float wdt, uint32_t c) {
    for (int j = (int)(cy - r - wdt); j <= (int)(cy + r + wdt); j++) for (int i = (int)(cx - r - wdt); i <= (int)(cx + r + wdt); i++)
        blend(i, j, c, clampf(wdt / 2 + 0.5f - fabsf(hypotf(i + 0.5f - cx, j + 0.5f - cy) - r)));
}
static void line(float x0, float y0, float x1, float y1, float wdt, uint32_t c) {
    float minx = fminf(x0, x1) - wdt, maxx = fmaxf(x0, x1) + wdt, miny = fminf(y0, y1) - wdt, maxy = fmaxf(y0, y1) + wdt;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int j = (int)miny; j <= (int)maxy; j++) for (int i = (int)minx; i <= (int)maxx; i++) {
        float px = i + 0.5f, py = j + 0.5f, t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        blend(i, j, c, clampf(wdt / 2 + 0.5f - hypotf(px - (x0 + t * dx), py - (y0 + t * dy))));
    }
}
/* annulus sector (for D-pad highlight): dir 0 up,1 right,2 down,3 left */
static void sector(float cx, float cy, float ri, float ro, int dir, uint32_t c) {
    for (int j = (int)(cy - ro); j <= (int)(cy + ro); j++) for (int i = (int)(cx - ro); i <= (int)(cx + ro); i++) {
        float dx = i + 0.5f - cx, dy = j + 0.5f - cy, d = hypotf(dx, dy);
        int dd = fabsf(dx) > fabsf(dy) ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0);
        if (dd != dir) continue;
        blend(i, j, c, clampf(fminf(ro + 0.5f - d, d - ri + 0.5f)));
    }
}

/* --- text */
static const Glyph *glyph(const Font *f, char ch) {
    const char *p = strchr(f->chars, ch);
    return p && ch ? &f->g[p - f->chars] : NULL;
}
static int text_w(const Font *f, const char *s) { int w = 0; for (; *s; s++) { const Glyph *g = glyph(f, *s); if (g) w += g->adv; } return w; }
static void text_at(const Font *f, int x, int top, const char *s, uint32_t c) {
    for (; *s; s++) {
        const Glyph *g = glyph(f, *s); if (!g) continue;
        const uint8_t *b = f->bits + g->off;
        for (int j = 0; j < g->h; j++) for (int i = 0; i < g->w; i++) if (b[j * g->w + i]) blend(x + g->xoff + i, top + g->yoff + j, c, b[j * g->w + i] / 255.f);
        x += g->adv;
    }
}
/* center text on (cx, cy) using the cap height (~0.71 of ascent-ish) */
static void text_c(const Font *f, int cx, int cy, const char *s, uint32_t c) {
    int cap = (int)(f->asc * 0.73f);
    text_at(f, cx - text_w(f, s) / 2, cy - f->asc + cap / 2 + (int)(f->asc * 0.02f), s, c);
}

static int fb_open(void) {
    fbfd = open("/dev/graphics/fb0", O_RDWR);
    if (fbfd < 0) fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) || ioctl(fbfd, FBIOGET_FSCREENINFO, &fi)) { logf_("fb failed"); return -1; }
    W = vi.xres; H = vi.yres; STRIDE_PX = fi.line_length / 4;
    fbmem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED || vi.bits_per_pixel != 32) { logf_("fb mmap/bpp failed"); return -1; }
    back = calloc((size_t)STRIDE_PX * H, 4);
    logf_("fb %dx%d stride_px %d", W, H, STRIDE_PX);
    return 0;
}
static void fb_flip(void) {
    memcpy(fbmem, back, (size_t)STRIDE_PX * H * 4);
    vi.yoffset = 0; vi.activate = FB_ACTIVATE_VBL;
    if (ioctl(fbfd, FBIOPAN_DISPLAY, &vi) < 0) ioctl(fbfd, FBIOPUT_VSCREENINFO, &vi);
}

/* =========================================================== IR (ir2661 over spidev, ~750 kHz) */
static uint8_t irbuf[16384];
static int ir_write_pattern(const int *pat, int n) {
    static const char chunk[] = "0000000001111111111000000000001111111111";
    memset(irbuf, 0, sizeof irbuf);
    long pos = 0, cap = sizeof irbuf * 8;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) {
            int cycles = pat[i] * 38000 / 1000000;
            long nb = (long)(cycles / 2) * 40 + ((cycles & 1) ? 19 : 0);
            for (long k = 0; k < nb && pos < cap; k++, pos++) if (chunk[k % 40] == '1') irbuf[pos >> 3] |= 0x80 >> (pos & 7);
        } else pos += (long)(pat[i] * 0.8f + 0.5f);
    }
    int len = (int)((pos + 7) / 8), fd = open(IR_DEV, O_RDWR);
    if (fd < 0) { logf_("ir open failed"); return -1; }
    int w = (int)write(fd, irbuf, len); close(fd);
    return w == len ? 0 : -1;
}
static int ir_nec(int a0, int a1, int cmd) {
    int pat[67], n = 0, bytes[4] = {a0, a1, cmd & 255, ~cmd & 255};
    pat[n++] = 9000; pat[n++] = 4500;
    for (int b = 0; b < 4; b++) for (int bit = 0; bit < 8; bit++) { pat[n++] = 562; pat[n++] = (bytes[b] >> bit & 1) ? 1687 : 562; }
    pat[n++] = 562;
    int r = ir_write_pattern(pat, n);
    logf_("ir %02X %02X %02X -> %d", a0, a1, cmd, r);
    return r;
}
static void ir_repeat(void) { int pat[3] = {9000, 2250, 562}; ir_write_pattern(pat, 3); }

/* =========================================================== touch */
static int tfd = -1, tmaxx = 720, tmaxy = 1520;
static int find_touch(void) {
    DIR *d = opendir("/dev/input"); struct dirent *e; char path[64], name[64];
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        name[0] = 0; ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (strstr(name, "goodix") || strstr(name, "touch")) {
            struct input_absinfo a;
            if (!ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &a) && a.maximum > 0) tmaxx = a.maximum + 1;
            if (!ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &a) && a.maximum > 0) tmaxy = a.maximum + 1;
            logf_("touch %s %s", path, name); closedir(d); return fd;
        }
        close(fd);
    }
    if (d) closedir(d);
    return -1;
}

/* =========================================================== UI model */
enum { PG_HOME, PG_TV, PG_FAN, PG_SYS, NPAGES };
enum { S_RR, S_CIRCLE, S_PILL_TOP, S_PILL_BOT, S_DPAD, S_TAB, S_TPAD };
enum { IC_NONE, IC_POWER, IC_UP, IC_DOWN, IC_LEFT, IC_RIGHT, IC_BACK, IC_HOME, IC_X, IC_PLUS, IC_MINUS, IC_PLAY, IC_REW, IC_FF };
enum { A_IR, A_TAB, A_SOON, A_REBOOT, A_RECOVERY, A_BT_PAIR, A_BL_UP, A_BL_DN, A_FAN_SPEED, A_FAN_POWER, A_NONE };

typedef struct {
    int page, shape, x, y, w, h;
    int act, a0, a1, cmd, rep, arg;
    const char *label; int icon; uint32_t bg, fg;
} Wd;

#define TV 0x00, 0x7F
#define FAN 0xD8, 0x7E
static uint32_t COL_BG, COL_CARD, COL_CARD2, COL_PRESS, COL_FG, COL_MUTED, COL_ACC, COL_RED, COL_GREEN, COL_AMBER;
static Wd wd[96]; static int nwd;
static int page = PG_HOME, pressed = -1, dpad_zone = -1;
static char status[64] = "Ready";
static int fan_on = -1, fan_speed = 0, bl_level = 110, bl_max = 255;
static int n_sleeps, n_sleep_fail; static long long slept_ms;
static void state_save(void);
static volatile int rest_mv;
static int est_soc(void);
static char bt_state[24] = "off";
static volatile long long bt_linger_until;
static long long pair_until;
static int bt_connected(void);
static long long boot_ms(void);
static volatile int sleep_allowed, resumed_flag;
static void wake_lock(void);

static void add(int pg, int sh, int x, int y, int w, int h, int act, int a0, int a1, int cmd, int rep, int arg, const char *label, int icon, uint32_t bg) {
    Wd *q = &wd[nwd++];
    *q = (Wd){pg, sh, x, y, w, h, act, a0, a1, cmd, rep, arg, label, icon, bg, 0};
}
#define IR(pg, sh, x, y, w, h, a, cmd, rep, label, icon, bg) add(pg, sh, x, y, w, h, A_IR, a, cmd, rep, 0, label, icon, bg)

static void build_ui(void) {
    nwd = 0;
    // tabs (all pages)
    for (int p = 0; p < NPAGES; p++) {
        add(p, S_TAB, 24, 40, 136, 72, A_TAB, 0, 0, 0, 0, PG_HOME, "Home", 0, 0);
        add(p, S_TAB, 170, 40, 110, 72, A_TAB, 0, 0, 0, 0, PG_TV, "TV", 0, 0);
        add(p, S_TAB, 290, 40, 120, 72, A_TAB, 0, 0, 0, 0, PG_FAN, "FAN", 0, 0);
        add(p, S_TAB, 420, 40, 96, 72, A_TAB, 0, 0, 0, 0, PG_SYS, "...", 0, 0);
    }
    // ---------------- HOME: essentials only, ordered by reach (rare/dangerous top -> frequent bottom)
    IR(PG_HOME, S_CIRCLE, 100, 136, 104, 104, TV, 0x15, 0, NULL, IC_POWER, COL_RED);                 // TV power
    add(PG_HOME, S_CIRCLE, 308, 136, 104, 104, A_FAN_POWER, FAN, 0x08, 0, 0, NULL, IC_POWER, COL_RED);  // fan power
    add(PG_HOME, S_CIRCLE, 516, 136, 104, 104, A_SOON, 0, 0, 0, 0, 0, NULL, IC_POWER, COL_CARD2);   // AC (later)
    add(PG_HOME, S_RR, 24, 296, 328, 104, A_SOON, 0, 0, 0, 0, 1, NULL, 0, COL_CARD);                 // AC card
    add(PG_HOME, S_RR, 368, 296, 328, 104, A_SOON, 0, 0, 0, 0, 2, NULL, 0, COL_CARD);                // Light card
    {
        static const char *sp[7] = {"1", "2", "3", "4", "5", "6", "7"};
        static const int spc[7] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x15, 0x13};
        for (int i = 0; i < 8; i++) {
            int x = 24 + (i % 4) * 172, y = 448 + (i / 4) * 116;
            if (i < 7) add(PG_HOME, S_RR, x, y, 156, 100, A_FAN_SPEED, FAN, spc[i], 0, i + 1, sp[i], 0, COL_CARD);
            else IR(PG_HOME, S_RR, x, y, 156, 100, FAN, 0x14, 0, "Eco", 0, COL_CARD2);
        }
    }
    add(PG_HOME, S_TPAD, 24, 696, 520, 560, A_NONE, TV, 0, 0, 0, NULL, 0, COL_CARD);
    IR(PG_HOME, S_PILL_TOP, 568, 696, 128, 280, TV, 0x1B, 1, "VOL", IC_PLUS, COL_CARD2);
    IR(PG_HOME, S_PILL_BOT, 568, 976, 128, 280, TV, 0x1A, 1, NULL, IC_MINUS, COL_CARD2);
    IR(PG_HOME, S_RR, 24, 1272, 208, 144, TV, 0x42, 0, NULL, IC_HOME, COL_CARD2);
    IR(PG_HOME, S_RR, 248, 1272, 224, 144, TV, 0x13, 0, NULL, IC_BACK, COL_CARD2);
    IR(PG_HOME, S_RR, 488, 1272, 208, 144, TV, 0x16, 0, "Mute", 0, COL_CARD2);

    // ---------------- TV
    IR(PG_TV, S_CIRCLE, 36, 150, 112, 112, TV, 0x15, 0, NULL, IC_POWER, COL_RED);
    IR(PG_TV, S_RR, 172, 158, 160, 96, TV, 0x12, 0, "Input", 0, COL_CARD);
    IR(PG_TV, S_RR, 352, 158, 160, 96, TV, 0x16, 0, "Mute", 0, COL_CARD);
    IR(PG_TV, S_RR, 532, 158, 152, 96, TV, 0x4F, 0, "NETFLIX", 0, COL_CARD);
    add(PG_TV, S_DPAD, 120, 300, 480, 480, A_IR, TV, 0, 1, 0, NULL, 0, COL_CARD);       // sub-keys resolved in hit()
    IR(PG_TV, S_CIRCLE, 120, 812, 108, 108, TV, 0x13, 0, NULL, IC_BACK, COL_CARD2);
    IR(PG_TV, S_CIRCLE, 306, 812, 108, 108, TV, 0x42, 0, NULL, IC_HOME, COL_CARD2);
    IR(PG_TV, S_CIRCLE, 492, 812, 108, 108, TV, 0x11, 0, NULL, IC_X, COL_CARD2);
    IR(PG_TV, S_PILL_TOP, 36, 960, 140, 170, TV, 0x1B, 1, "VOL", IC_PLUS, COL_CARD2);
    IR(PG_TV, S_PILL_BOT, 36, 1130, 140, 170, TV, 0x1A, 1, NULL, IC_MINUS, COL_CARD2);
    IR(PG_TV, S_PILL_TOP, 544, 960, 140, 170, TV, 0x19, 1, "CH", IC_PLUS, COL_CARD2);
    IR(PG_TV, S_PILL_BOT, 544, 1130, 140, 170, TV, 0x18, 1, NULL, IC_MINUS, COL_CARD2);
    IR(PG_TV, S_RR, 210, 980, 300, 96, TV, 0x17, 0, "Settings", 0, COL_CARD);
    IR(PG_TV, S_RR, 210, 1096, 300, 96, TV, 0x14, 0, "Info", 0, COL_CARD);
    IR(PG_TV, S_RR, 210, 1212, 300, 88, TV, 0x4C, 0, "Media", 0, COL_CARD);
    IR(PG_TV, S_RR, 36, 1340, 200, 104, TV, 0x47, 1, NULL, IC_REW, COL_CARD);
    IR(PG_TV, S_RR, 260, 1340, 200, 104, TV, 0x43, 0, NULL, IC_PLAY, COL_CARD);
    IR(PG_TV, S_RR, 484, 1340, 200, 104, TV, 0x46, 1, NULL, IC_FF, COL_CARD);
    // ---------------- FAN
    add(PG_FAN, S_CIRCLE, 260, 560, 200, 200, A_FAN_POWER, FAN, 0x08, 0, 0, NULL, IC_POWER, COL_RED);
    static const char *sp[7] = {"1", "2", "3", "4", "5", "6", "7"};
    static const int spc[7] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x15, 0x13};
    for (int i = 0; i < 7; i++) {
        int row = i < 4 ? 0 : 1, col = i < 4 ? i : i - 4;
        int bw = 146, gap = 18, x0 = row == 0 ? 36 : 36 + (bw + gap) / 2;
        add(PG_FAN, S_RR, x0 + col * (bw + gap), 812 + row * 140, bw, 120, A_FAN_SPEED, FAN, spc[i], 0, i + 1, sp[i], 0, COL_CARD);
    }
    IR(PG_FAN, S_RR, 36, 1110, 200, 100, FAN, 0x07, 0, "Light", 0, COL_CARD2);
    IR(PG_FAN, S_RR, 260, 1110, 200, 100, FAN, 0x12, 0, "Reverse", 0, COL_CARD2);
    IR(PG_FAN, S_RR, 484, 1110, 200, 100, FAN, 0x14, 0, "Eco", 0, COL_CARD2);
    IR(PG_FAN, S_RR, 36, 1234, 312, 100, FAN, 0x09, 0, "Timer 2H", 0, COL_CARD2);
    IR(PG_FAN, S_RR, 372, 1234, 312, 100, FAN, 0x10, 0, "Timer 4H", 0, COL_CARD2);
    // ---------------- SYSTEM
    add(PG_SYS, S_RR, 36, 760, 312, 120, A_REBOOT, 0, 0, 0, 0, 0, "Restart", 0, COL_RED);
    add(PG_SYS, S_RR, 372, 760, 312, 120, A_RECOVERY, 0, 0, 0, 0, 0, "Recovery", 0, COL_RED);
    add(PG_SYS, S_RR, 36, 900, 312, 120, A_BL_DN, 0, 0, 0, 0, 0, "Dimmer", 0, COL_CARD);
    add(PG_SYS, S_RR, 372, 900, 312, 120, A_BL_UP, 0, 0, 0, 0, 0, "Brighter", 0, COL_CARD);
    add(PG_SYS, S_RR, 36, 1040, 648, 120, A_BT_PAIR, 0, 0, 0, 0, 0, "Pair TV (Bluetooth)", 0, COL_ACC);
}

static void icon(int ic, float cx, float cy, float s, uint32_t c) {
    float w = 6 * s;
    switch (ic) {
    case IC_POWER: {
        float r = 24 * s;
        for (int j = (int)(cy - r - w); j <= (int)(cy + r + w); j++) for (int i = (int)(cx - r - w); i <= (int)(cx + r + w); i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            if (dy < 0 && fabsf(dx) < -dy * 0.75f) continue;           // gap at top
            blend(i, j, c, clampf(w / 2 + 0.5f - fabsf(hypotf(dx, dy) - r)));
        }
        line(cx, cy - r - 4 * s, cx, cy - 4 * s, w, c); break; }
    case IC_UP: line(cx - 18 * s, cy + 9 * s, cx, cy - 9 * s, w, c); line(cx, cy - 9 * s, cx + 18 * s, cy + 9 * s, w, c); break;
    case IC_DOWN: line(cx - 18 * s, cy - 9 * s, cx, cy + 9 * s, w, c); line(cx, cy + 9 * s, cx + 18 * s, cy - 9 * s, w, c); break;
    case IC_LEFT: line(cx + 9 * s, cy - 18 * s, cx - 9 * s, cy, w, c); line(cx - 9 * s, cy, cx + 9 * s, cy + 18 * s, w, c); break;
    case IC_RIGHT: line(cx - 9 * s, cy - 18 * s, cx + 9 * s, cy, w, c); line(cx + 9 * s, cy, cx - 9 * s, cy + 18 * s, w, c); break;
    case IC_BACK: line(cx + 20 * s, cy, cx - 18 * s, cy, w, c); line(cx - 18 * s, cy, cx - 3 * s, cy - 15 * s, w, c); line(cx - 18 * s, cy, cx - 3 * s, cy + 15 * s, w, c); break;
    case IC_HOME: line(cx - 24 * s, cy - 2 * s, cx, cy - 24 * s, w, c); line(cx, cy - 24 * s, cx + 24 * s, cy - 2 * s, w, c);
        line(cx - 16 * s, cy - 6 * s, cx - 16 * s, cy + 20 * s, w, c); line(cx + 16 * s, cy - 6 * s, cx + 16 * s, cy + 20 * s, w, c);
        line(cx - 16 * s, cy + 20 * s, cx + 16 * s, cy + 20 * s, w, c); break;
    case IC_X: line(cx - 16 * s, cy - 16 * s, cx + 16 * s, cy + 16 * s, w, c); line(cx + 16 * s, cy - 16 * s, cx - 16 * s, cy + 16 * s, w, c); break;
    case IC_PLUS: line(cx - 18 * s, cy, cx + 18 * s, cy, w, c); line(cx, cy - 18 * s, cx, cy + 18 * s, w, c); break;
    case IC_MINUS: line(cx - 18 * s, cy, cx + 18 * s, cy, w, c); break;
    case IC_PLAY: line(cx - 22 * s, cy - 16 * s, cx - 22 * s, cy + 16 * s, w, c); line(cx - 22 * s, cy - 16 * s, cx - 2 * s, cy, w, c);
        line(cx - 22 * s, cy + 16 * s, cx - 2 * s, cy, w, c); line(cx + 10 * s, cy - 16 * s, cx + 10 * s, cy + 16 * s, w, c);
        line(cx + 22 * s, cy - 16 * s, cx + 22 * s, cy + 16 * s, w, c); break;
    case IC_REW: for (int k = 0; k < 2; k++) { float o = (k ? 4 : -20) * s; line(cx + o + 16 * s, cy - 14 * s, cx + o, cy, w, c); line(cx + o, cy, cx + o + 16 * s, cy + 14 * s, w, c); } break;
    case IC_FF: for (int k = 0; k < 2; k++) { float o = (k ? 4 : -20) * s; line(cx + o, cy - 14 * s, cx + o + 16 * s, cy, w, c); line(cx + o + 16 * s, cy, cx + o, cy + 14 * s, w, c); } break;
    }
}

static int bat_cap = -1; static char bat_status[24];
static int dis_cap0 = -1; static long long dis_t0;     // capacity/boottime when unplugged (for average drain)

static long long last_draw_us;
static uint32_t *page_cache[NPAGES]; static int cache_ok[NPAGES];
static void invalidate(int pg) { if (pg < 0) { for (int k = 0; k < NPAGES; k++) cache_ok[k] = 0; } else cache_ok[pg] = 0; }

static void draw_widget(int i) {
        Wd *q = &wd[i];
        int dn = i == pressed;
        uint32_t bg = q->bg;
        if (q->shape == S_TAB) {
            int sel = q->arg == page;
            fill_rrect(q->x, q->y, q->w, q->h, q->h / 2.f, sel ? COL_ACC : (dn ? COL_PRESS : COL_CARD));
            text_c(&f_label, q->x + q->w / 2, q->y + q->h / 2, q->label, COL_FG);
            return;
        }
        if (q->shape == S_TPAD) {
            float cx = q->x + q->w / 2.f, cy = q->y + q->h / 2.f;
            fill_rrect(q->x, q->y, q->w, q->h, 44, dn ? COL_PRESS : COL_CARD);
            uint32_t m = COL_MUTED;
            icon(IC_UP, cx, q->y + 34, 0.8f, m); icon(IC_DOWN, cx, q->y + q->h - 34, 0.8f, m);
            icon(IC_LEFT, q->x + 32, cy, 0.8f, m); icon(IC_RIGHT, q->x + q->w - 32, cy, 0.8f, m);
            text_c(&f_label, (int)cx, (int)cy - 22, "Swipe", m);
            text_c(&f_small, (int)cx, (int)cy + 24, "tap = OK", m);
            return;
        }
        if (q->shape == S_DPAD) {
            float cx = q->x + q->w / 2.f, cy = q->y + q->h / 2.f, ro = q->w / 2.f, ri = ro * 0.38f;
            fill_circle(cx, cy, ro, COL_CARD);
            ring(cx, cy, ro - 1, 2, COL_CARD2);
            if (dn && dpad_zone >= 0 && dpad_zone < 4) sector(cx, cy, ri, ro, dpad_zone, COL_PRESS);
            float dd = (ro + ri) / 2;
            icon(IC_UP, cx, cy - dd, 1.3f, COL_FG); icon(IC_DOWN, cx, cy + dd, 1.3f, COL_FG);
            icon(IC_LEFT, cx - dd, cy, 1.3f, COL_FG); icon(IC_RIGHT, cx + dd, cy, 1.3f, COL_FG);
            fill_circle(cx, cy, ri - 6, dn && dpad_zone == 4 ? 0 : COL_ACC);
            if (dn && dpad_zone == 4) fill_circle(cx, cy, ri - 6, C(0x4865E0));
            text_c(&f_title, (int)cx, (int)cy, "OK", COL_FG);
            return;
        }
        if (dn) bg = (bg == COL_RED) ? C(0xB8363B) : COL_PRESS;
        if (q->act == A_FAN_SPEED && fan_on == 1 && fan_speed == q->arg && !dn) bg = COL_ACC;
        float cx = q->x + q->w / 2.f, cy = q->y + q->h / 2.f;
        switch (q->shape) {
        case S_CIRCLE: fill_circle(cx, cy, q->w / 2.f, bg); break;
        case S_RR: fill_rrect(q->x, q->y, q->w, q->h, 26, bg); break;
        case S_PILL_TOP: break;                                     // drawn together with its bottom half
        case S_PILL_BOT: {
            Wd *t = &wd[i - 1]; int top = t->y, tot = t->h + q->h, mid = q->y;
            fill_rrect(q->x, top, q->w, tot, q->w / 2.f, q->bg);
            if (i - 1 == pressed) fill_rrect_clip(q->x, top, q->w, tot, q->w / 2.f, top, mid, COL_PRESS);
            if (i == pressed) fill_rrect_clip(q->x, top, q->w, tot, q->w / 2.f, mid, top + tot, COL_PRESS);
            icon(t->icon, t->x + t->w / 2.f, t->y + t->h * 0.38f, 1.1f, COL_FG);
            text_c(&f_small, t->x + t->w / 2, mid, t->label, COL_MUTED);
            break; }
        }
        if (q->shape == S_PILL_TOP) return;
        if (q->shape == S_PILL_BOT) { icon(q->icon, cx, q->y + q->h * 0.62f, 1.1f, COL_FG); return; }
        if (q->icon) icon(q->icon, cx, cy, q->w >= 180 && q->shape == S_CIRCLE ? 2.0f : 1.15f, COL_FG);
        if (q->label) {
            const Font *f = (q->act == A_FAN_SPEED) ? &f_title : &f_label;
            uint32_t tc = !strcmp(q->label, "NETFLIX") ? C(0xE50914) : COL_FG;
            text_c(f, (int)cx, (int)cy, q->label, tc);
        }
    }

/* everything that only changes on page/state changes (cached for TV and FAN pages) */
static void draw_static(void) {
    fill_rect(0, 0, W, H, COL_BG);
    for (int i = 0; i < nwd; i++) if (wd[i].page == page) draw_widget(i);
    // page headers
    if (page == PG_HOME) {
        text_c(&f_small, 152, 262, "TV", COL_MUTED); text_c(&f_small, 360, 262, "Fan", COL_MUTED);
        text_c(&f_small, 568, 262, "AC", COL_MUTED);
        text_c(&f_label, 188, 330, "AC", COL_MUTED); text_c(&f_small, 188, 368, "coming soon", COL_MUTED);
        text_c(&f_label, 532, 330, "Light", COL_MUTED); text_c(&f_small, 532, 368, "coming soon", COL_MUTED);
        text_c(&f_small, 66, 424, "FAN", COL_MUTED);
    }
    if (page == PG_FAN) {
        fill_rrect(36, 150, 648, 360, 36, COL_CARD);
        text_c(&f_small, 360, 196, "WALTON BLDC FAN  (last sent)", COL_MUTED);
        if (fan_on == 0) text_c(&f_title, 360, 330, "OFF", COL_MUTED);
        else if (fan_speed > 0) { char s[4]; snprintf(s, sizeof s, "%d", fan_speed); text_c(&f_huge, 360, 340, s, COL_FG); }
        else text_c(&f_title, 360, 330, fan_on == 1 ? "ON" : "--", COL_FG);
        text_c(&f_small, 360, 470, fan_on == 1 ? "speed" : "tap a speed to start", COL_MUTED);
    }
    if (page == PG_SYS) {
        char s[96]; int up = read_int("/proc/uptime");
        fill_rrect(36, 150, 648, 580, 36, COL_CARD);
        text_c(&f_title, 360, 230, "Remote OS " VERSION, COL_FG);
        int mvn = read_int("/sys/class/power_supply/battery/voltage_now") / 1000;
        if (!strcmp(bat_status, "Charging") || !strcmp(bat_status, "Full")) snprintf(s, sizeof s, "Battery %s  (%.2f V)", bat_status, mvn / 1000.0);
        else snprintf(s, sizeof s, "Battery now ~%d%%  (%.2f V)", est_soc(), (rest_mv > 0 ? rest_mv : mvn) / 1000.0);
        text_c(&f_label, 360, 330, s, COL_FG);
        if (!strcmp(bat_status, "Charging") || !strcmp(bat_status, "Full"))
            snprintf(s, sizeof s, "Gauge at boot %d%%  (live %% after unplug)", bat_cap);
        else snprintf(s, sizeof s, "Gauge at boot %d%%  ·  estimate ~%d%%", bat_cap, est_soc());
        text_c(&f_label, 360, 400, s, COL_MUTED);
        snprintf(s, sizeof s, "Uptime %d min", up / 60); text_c(&f_label, 360, 470, s, COL_MUTED);
        snprintf(s, sizeof s, "Brightness %d / %d", bl_level, bl_max); text_c(&f_label, 360, 540, s, COL_MUTED);
        snprintf(s, sizeof s, "Slept %d times, %lld min (fail %d)", n_sleeps, slept_ms / 60000, n_sleep_fail); text_c(&f_small, 360, 590, s, COL_MUTED);
        if (dis_cap0 >= 0) {
            double h = (boot_ms() - dis_t0) / 3600000.0; int used = dis_cap0 - bat_cap;
            if (h > 0.05) snprintf(s, sizeof s, "Unplugged %.1f h: -%d%% (~%.0f mA avg)", h, used, used * 39.45 / h);
            else snprintf(s, sizeof s, "Unplugged just now at %d%%", dis_cap0);
        } else snprintf(s, sizeof s, "Now %d mA (charging)", -read_int("/sys/class/power_supply/battery/current_now") / 1000);
        text_c(&f_small, 360, 625, s, COL_MUTED);
        snprintf(s, sizeof s, "Bluetooth: %s", bt_state); text_c(&f_small, 360, 680, s, bt_connected() ? COL_GREEN : COL_MUTED);
        text_c(&f_small, 360, 715, "Double-tap the screen to wake", COL_MUTED);
    }
}

static void draw(void) {
    struct timespec d0, d1; clock_gettime(CLOCK_MONOTONIC, &d0);
    size_t bytes = (size_t)STRIDE_PX * H * 4;
    if (page == PG_SYS || !cache_ok[page]) {
        int sp = pressed; pressed = -1; draw_static(); pressed = sp;
        if (page != PG_SYS) {
            if (!page_cache[page]) page_cache[page] = malloc(bytes);
            if (page_cache[page]) { memcpy(page_cache[page], back, bytes); cache_ok[page] = 1; }
        }
    } else memcpy(back, page_cache[page], bytes);
    if (pressed >= 0 && wd[pressed].page == page) draw_widget(wd[pressed].shape == S_PILL_TOP ? pressed + 1 : pressed);
    // dynamic overlays: battery + BT (top right), status line
    fill_rect(530, 36, W - 530, 84, COL_BG);   // right of the tab row (tabs end at x=516)
    if (strcmp(bt_state, "off")) text_c(&f_label, 640, 76, "BT", bt_connected() ? COL_GREEN : (!strcmp(bt_state, "error") ? COL_RED : COL_AMBER));
    if (page != PG_SYS && strcmp(bat_status, "Charging") && est_soc() < 15) text_c(&f_small, 600, 104, "Battery low", COL_RED);
    fill_rect(0, H - 76, W, 76, COL_BG);
    text_c(&f_small, 360, H - 40, status, COL_MUTED);
    fb_flip();
    clock_gettime(CLOCK_MONOTONIC, &d1);
    last_draw_us = (d1.tv_sec - d0.tv_sec) * 1000000LL + (d1.tv_nsec - d0.tv_nsec) / 1000;
}

static int hit(int x, int y) {
    for (int i = 0; i < nwd; i++) {
        Wd *q = &wd[i];
        if (q->page != page) continue;
        if (q->shape == S_DPAD) {
            float cx = q->x + q->w / 2.f, cy = q->y + q->h / 2.f, dx = x - cx, dy = y - cy, d = hypotf(dx, dy), ro = q->w / 2.f;
            if (d > ro) continue;
            dpad_zone = d < ro * 0.38f ? 4 : fabsf(dx) > fabsf(dy) ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0);
            return i;
        }
        if (x >= q->x && x < q->x + q->w && y >= q->y && y < q->y + q->h) return i;
    }
    return -1;
}
static const int DPAD_CMD[5] = {0x0C, 0x0E, 0x0D, 0x0F, 0x10};
static const char *DPAD_NAME[5] = {"Up", "Right", "Down", "Left", "OK"};

/* =========================================================== Bluetooth (btremote daemon) */
static void bt_ctl(const char *line) {
    int fd = open("/tmp/btctl", O_WRONLY | O_NONBLOCK); if (fd < 0) return;
    char b[64]; int n = snprintf(b, sizeof b, "%s\n", line); if (write(fd, b, n) < 0) {} close(fd);
}
static void bt_poll(void) { read_str("/tmp/btstate", bt_state, sizeof bt_state); if (!bt_state[0]) strcpy(bt_state, "off"); }
static int bt_connected(void) { return !strcmp(bt_state, "connected"); }
/* TV IR command -> BT usage: kb = keyboard usage, cc = consumer usage */
static int bt_map(int cmd, int *is_kb) {
    static const int m[][3] = {{0x15,0,0x30},{0x12,0,0x89},{0x16,0,0xE2},{0x0C,1,0x52},{0x0D,1,0x51},{0x0F,1,0x50},{0x0E,1,0x4F},
        {0x10,0,0x41},{0x13,0,0x224},{0x42,0,0x223},{0x1B,0,0xE9},{0x1A,0,0xEA},{0x19,0,0x9C},{0x18,0,0x9D},{0x14,0,0x60},
        {0x43,0,0xCD},{0x47,0,0xB4},{0x46,0,0xB3}};
    for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i][0] == cmd) { *is_kb = m[i][1]; return m[i][2]; }
    return -1;
}
static int bt_send_key(int cmd, int down) {
    int kb, u = bt_map(cmd, &kb); if (u < 0) return 0;
    char l[32]; snprintf(l, sizeof l, "%s %x %d", kb ? "kb" : "cc", u, down); bt_ctl(l); return 1;
}

static void tv_key(int cmd) {                    // one complete key press: Bluetooth if connected, else IR
    if (bt_connected() && bt_send_key(cmd, 1)) { usleep(30000); bt_send_key(cmd, 0); }
    else ir_nec(0x00, 0x7F, cmd);
}

/* =========================================================== screen power */
static int screen_on = 1;
static void vibrate(int ms) {
    char d[16]; snprintf(d, sizeof d, "%d", ms);
    write_str("/sys/class/leds/vibrator/duration", d); write_str("/sys/class/leds/vibrator/activate", "1");
}
static int screen_timeout = SCREEN_TIMEOUT_MS;
static void backlight(int v) { char s[16]; snprintf(s, sizeof s, "%d", v); write_str(BL_PATH, s); }
static void screen_set(int on) {
    if (on == screen_on) return;
    screen_on = on;
    if (on) { bt_ctl("on"); bt_linger_until = 0; screen_timeout = SCREEN_TIMEOUT_MS; sleep_allowed = 0; wake_lock();
              ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK); draw(); backlight(bl_level); }
    else { backlight(0); ioctl(fbfd, FBIOBLANK, FB_BLANK_POWERDOWN); bt_ctl("standby"); bt_linger_until = boot_ms() + BT_LINGER_MS; }
    logf_("screen %s", on ? "on" : "off");
}

static void do_action(int i, int is_repeat) {
    Wd *q = &wd[i];
    switch (q->act) {
    case A_IR: {
        int cmd = q->shape == S_DPAD ? DPAD_CMD[dpad_zone] : q->cmd;
        if (is_repeat) { ir_repeat(); return; }
        int r = ir_nec(q->a0, q->a1, cmd);
        const char *nm = q->shape == S_DPAD ? DPAD_NAME[dpad_zone] : q->label ? q->label :
            q->icon == IC_POWER ? "Power" : q->icon == IC_BACK ? "Back" : q->icon == IC_HOME ? "Home" : q->icon == IC_X ? "Exit" :
            q->icon == IC_PLUS ? "+" : q->icon == IC_MINUS ? "-" : q->icon == IC_PLAY ? "Play/Pause" : q->icon == IC_REW ? "Rewind" : "Fast fwd";
        snprintf(status, sizeof status, r ? "IR error" : "Sent: %s%s", page == PG_TV ? "TV " : "", nm);
        break; }
    case A_FAN_POWER: if (!ir_nec(q->a0, q->a1, q->cmd)) { fan_on = fan_on == 1 ? 0 : 1; snprintf(status, sizeof status, "Sent: Fan power"); state_save(); invalidate(-1); } break;
    case A_FAN_SPEED: if (!ir_nec(q->a0, q->a1, q->cmd)) { fan_on = 1; fan_speed = q->arg; snprintf(status, sizeof status, "Sent: Fan speed %d", q->arg); state_save(); invalidate(-1); } break;
    case A_TAB: page = q->arg; break;
    case A_SOON: snprintf(status, sizeof status, q->arg == 2 ? "Light: not set up yet" : "AC: not set up yet (needs brand/model)"); break;
    case A_RECOVERY: snprintf(status, sizeof status, "Rebooting to recovery..."); draw(); logf_("reboot recovery"); sync(); usleep(300000);
        syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "recovery"); break;
    case A_REBOOT: snprintf(status, sizeof status, "Restarting..."); draw(); logf_("reboot"); sync(); usleep(300000); reboot(RB_AUTOBOOT); break;
    case A_BT_PAIR: bt_ctl("pair"); pair_until = now_ms() + 120000; snprintf(status, sizeof status, "BT pairing: TV > Settings > Remotes > Pair, or confirm on TV"); break;
    case A_BL_UP: bl_level = bl_level + 30 > bl_max ? bl_max : bl_level + 30; backlight(bl_level); break;
    case A_BL_DN: bl_level = bl_level - 30 < 10 ? 10 : bl_level - 30; backlight(bl_level); break;
    }
}

/* =========================================================== persistence (/cache) */
static int state_ok;
static void state_init(void) {
    mkdir("/cache", 0755);
    if (mount("/dev/block/by-name/cache", "/cache", "ext4", MS_NOATIME, NULL) && errno != EBUSY) { logf_("cache mount failed %d", errno); return; }
    mkdir(STATE_DIR, 0755);
    state_ok = 1;
    FILE *f = fopen(STATE_FILE, "r");
    if (f) { int a, b, c, d; if (fscanf(f, "%d %d %d %d", &a, &b, &c, &d) == 4) { fan_on = a; fan_speed = b; if (c > 0) bl_level = c; (void)d; page = PG_HOME; } fclose(f); }
}
static void state_save(void) {
    if (!state_ok) return;
    FILE *f = fopen(STATE_FILE ".tmp", "w"); if (!f) return;
    fprintf(f, "%d %d %d %d\n", fan_on, fan_speed, bl_level, PG_HOME);
    fflush(f); fsync(fileno(f)); fclose(f); rename(STATE_FILE ".tmp", STATE_FILE);
}

/* =========================================================== battery estimate + log
   The QG gauge only computes SOC at power-on here (runtime updates come from an Android-side algorithm via
   /dev/qg), so the live % is estimated from the resting voltage sampled right after each resume. */
// rest_mv: voltage sampled just after resume (near-OCV, tiny load)
static int soc_from_mv(int mv) {                   // ATL 4000 mAh, 4.4 V Li-ion, approximate OCV curve
    static const int t[][2] = {{4380,100},{4310,95},{4250,90},{4200,85},{4150,80},{4100,75},{4060,70},{4020,65},
        {3980,60},{3940,55},{3900,50},{3860,45},{3830,40},{3810,35},{3790,30},{3770,25},{3740,20},{3710,15},
        {3690,10},{3620,5},{3400,0}};
    if (mv >= t[0][0]) return 100;
    for (unsigned i = 1; i < sizeof t / sizeof t[0]; i++)
        if (mv >= t[i][0]) return t[i][1] + (mv - t[i][0]) * (t[i - 1][1] - t[i][1]) / (t[i - 1][0] - t[i][0]);
    return 0;
}
static int est_soc(void) {
    int mv = rest_mv > 0 ? rest_mv : read_int("/sys/class/power_supply/battery/voltage_now") / 1000;
    return soc_from_mv(mv);
}
static void batlog(const char *ev) {
    if (!state_ok) return;
    struct stat st;
    if (!stat(STATE_DIR "/batlog.csv", &st) && st.st_size > 200000) rename(STATE_DIR "/batlog.csv", STATE_DIR "/batlog.1.csv");
    FILE *f = fopen(STATE_DIR "/batlog.csv", "a"); if (!f) return;
    char stt[24]; read_str(BAT_STATUS, stt, sizeof stt);
    fprintf(f, "%.3f,%s,%d,%d,%d,%s,%d\n", boot_ms() / 3600000.0, ev, est_soc(), rest_mv,
            read_int("/sys/class/power_supply/battery/voltage_now") / 1000, stt, read_int(BAT_CAP));
    fflush(f); fsync(fileno(f)); fclose(f);
}

/* =========================================================== suspend (no autosleep in this kernel) */
static long long boot_ms(void) { struct timespec t; clock_gettime(CLOCK_BOOTTIME, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static void wake_lock(void) { write_str("/sys/power/wake_lock", "remoteos"); }
static void wake_unlock(void) { write_str("/sys/power/wake_unlock", "remoteos"); }
/* Sleeper thread: reading wakeup_count blocks while any wakeup source (our wake lock, USB, pending
   input via EPOLLWAKEUP) is active - exactly the Android suspend pattern, but off the UI thread. */
static void *sleeper(void *arg) {
    (void)arg;
    for (;;) {
        if (!sleep_allowed) { usleep(300000); continue; }
        char wc[32]; read_str("/sys/power/wakeup_count", wc, sizeof wc);    // may block
        if (!sleep_allowed || !wc[0]) continue;
        int fd = open("/sys/power/wakeup_count", O_WRONLY);
        int ok = fd >= 0 && write(fd, wc, strlen(wc)) > 0;
        if (fd >= 0) close(fd);
        if (!ok) { n_sleep_fail++; usleep(500000); continue; }              // new wake events: try again
        write_str("/sys/class/rtc/rtc0/wakealarm", "0");
        // wake for the Bluetooth linger deadline if one is pending, else the 5 h battery check
        long long lg = bt_linger_until ? (bt_linger_until - boot_ms()) / 1000 : 0;
        int wake_s = bt_linger_until ? (lg < 5 ? 5 : (int)lg) : SAFETY_WAKE_S;
        char al[16]; snprintf(al, sizeof al, "+%d", wake_s); write_str("/sys/class/rtc/rtc0/wakealarm", al);
        long long b0 = boot_ms(), m0 = now_ms();
        fd = open("/sys/power/state", O_WRONLY);
        ok = fd >= 0 && write(fd, "mem", 3) == 3;                            // returns after resume
        if (fd >= 0) close(fd);
        long long asleep = (boot_ms() - b0) - (now_ms() - m0);
        if (ok && asleep > 100) {
            rest_mv = read_int("/sys/class/power_supply/battery/voltage_now") / 1000;   // before the UI wakes up
            n_sleeps++; slept_ms += asleep; resumed_flag = 1; logf_("slept %lld ms, rest %d mV", asleep, rest_mv);
        }
        else n_sleep_fail++;
        usleep(2000000);                                                     // give the UI time to grab the wake lock
    }
    return NULL;
}

int main(void) {
    logf_("remoteos " VERSION " start");
    if (fb_open() < 0) return 1;
    COL_BG = C(0x0E1015); COL_CARD = C(0x1A1D25); COL_CARD2 = C(0x232733); COL_PRESS = C(0x3A4152); COL_FG = C(0xE8EAF0);
    COL_MUTED = C(0x8A90A2); COL_ACC = C(0x5B7CFA); COL_RED = C(0xE5484D); COL_GREEN = C(0x46C37B); COL_AMBER = C(0xF5A524);
    int m = read_int(BL_MAX_PATH); if (m > 0) bl_max = m;
    bl_level = bl_max * 45 / 100;
    write_str(GESTURE, "1");
    write_str("/sys/module/lpm_levels/parameters/sleep_disabled", "0");
    // Android's power HAL normally does this; default here is "performance" on all 8 cores (~34 mA wasted awake)
    for (int c = 4; c < 8; c++) { char p[64]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/online", c); write_str(p, "0"); }
    write_str("/sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "schedutil");
    // a remote needs very little CPU: page caching keeps redraws ~15 ms even at the lowest step
    write_str("/sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq", "614400");   // Android does this at boot; needed for deep sleep
    state_init();
    batlog("boot " VERSION);
    wake_lock();
    { pthread_t th; pthread_create(&th, NULL, sleeper, NULL); }
    ioctl(fbfd, FBIOBLANK, FB_BLANK_UNBLANK); backlight(bl_level);
    tfd = find_touch();
    build_ui();
    bat_cap = read_int(BAT_CAP); read_str(BAT_STATUS, bat_status, sizeof bat_status);
    draw();

    int tx = 0, ty = 0, finger = 0, swallow = 0, bt_down = -1;
    int tp_x0 = 0, tp_y0 = 0, tp_ax = 0, tp_ay = 0, tp_steps = 0; long long tp_t0 = 0;
    bt_ctl("on");
    long long last_act = now_ms(), press_t = 0, last_rep = 0, last_bat = 0, low_since = 0;
    for (;;) {
        { static unsigned long beat; FILE *hf = fopen(HEARTBEAT, "w"); if (hf) { fprintf(hf, "%lu\n", ++beat); fclose(hf); } }
        int timeout = (pressed >= 0 && wd[pressed].rep) ? 30 : 1000;
        static int ep = -1;
        if (ep < 0 && tfd >= 0) {
            ep = epoll_create1(0);
            struct epoll_event ee = {.events = EPOLLIN | EPOLLWAKEUP, .data.fd = tfd};
            if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ee)) logf_("epoll wakeup add failed %d", errno);
        }
        struct epoll_event got;
        int r = ep >= 0 ? epoll_wait(ep, &got, 1, timeout) : (usleep(timeout * 1000), 0);
        long long t = now_ms();
        { char old[24]; strcpy(old, bt_state); bt_poll(); if (strcmp(old, bt_state) && screen_on) draw(); }
        if (r > 0) {
            struct input_event ev[64];
            ssize_t n = read(tfd, ev, sizeof ev);
            for (int k = 0; k < (int)(n / (ssize_t)sizeof ev[0]); k++) {
                struct input_event *e = &ev[k];
                if (!screen_on) {                      // any event while off = wake (double-tap gesture key or touch)
                    if (e->type == EV_KEY) logf_("wake key code %d val %d", e->code, e->value);
                    if (e->type == EV_KEY || e->type == EV_ABS) { screen_set(1); swallow = 1; last_act = t; }
                    continue;
                }
                if (e->type == EV_ABS && e->code == ABS_MT_POSITION_X) tx = e->value * W / tmaxx;
                else if (e->type == EV_ABS && e->code == ABS_MT_POSITION_Y) ty = e->value * H / tmaxy;
                else if (e->type == EV_KEY && e->code == BTN_TOUCH) {
                    last_act = t;
                    if (e->value) { finger = 1; }
                    else {
                        finger = 0;
                        if (swallow) { swallow = 0; continue; }
                        if (pressed >= 0 && wd[pressed].shape == S_TPAD) {
                            int moved = abs(tx - tp_x0) > 24 || abs(ty - tp_y0) > 24;
                            if (!tp_steps && !moved && t - tp_t0 < 500) { tv_key(0x10); snprintf(status, sizeof status, "Touchpad: OK"); }
                            pressed = -1; screen_timeout = SCREEN_TIMEOUT_KEY_MS; draw(); continue;
                        }
                        if (pressed >= 0) {
                            int i = pressed; pressed = -1;
                            if (wd[i].act == A_IR || wd[i].act == A_FAN_SPEED || wd[i].act == A_FAN_POWER) screen_timeout = SCREEN_TIMEOUT_KEY_MS;
                            if (bt_down >= 0) { bt_send_key(bt_down, 0); snprintf(status, sizeof status, "Sent via Bluetooth"); bt_down = -1; }
                            else if (!wd[i].rep || press_t == 0) do_action(i, 0);   // non-repeat keys fire on release
                            press_t = 0; draw();
                        }
                    }
                } else if (e->type == EV_SYN && finger && !swallow && pressed >= 0 && wd[pressed].shape == S_TPAD) {
                    int dx = tx - tp_ax, dy = ty - tp_ay;                // swipe: one arrow per TPAD_STEP of travel
                    if (abs(dx) >= TPAD_STEP || abs(dy) >= TPAD_STEP) {
                        int cmd;
                        if (abs(dx) > abs(dy)) { cmd = dx > 0 ? 0x0E : 0x0F; tp_ax += dx > 0 ? TPAD_STEP : -TPAD_STEP; tp_ay = ty; }
                        else { cmd = dy > 0 ? 0x0D : 0x0C; tp_ay += dy > 0 ? TPAD_STEP : -TPAD_STEP; tp_ax = tx; }
                        tv_key(cmd); vibrate(12); tp_steps++; last_act = t;
                        snprintf(status, sizeof status, "Touchpad: %s", cmd == 0x0E ? "Right" : cmd == 0x0F ? "Left" : cmd == 0x0D ? "Down" : "Up");
                    }
                } else if (e->type == EV_SYN && finger && !swallow && pressed < 0) {
                    int i = hit(tx, ty);
                    if (i >= 0) {
                        pressed = i; press_t = 0;
                        if (wd[i].shape == S_TPAD) { tp_x0 = tp_ax = tx; tp_y0 = tp_ay = ty; tp_steps = 0; tp_t0 = t; draw(); continue; }
                        if (wd[i].act != A_TAB) vibrate(25);
                        int bc = wd[i].shape == S_DPAD ? DPAD_CMD[dpad_zone] : wd[i].cmd;
                        if (wd[i].act == A_IR && wd[i].a0 == 0x00 && wd[i].a1 == 0x7F && bt_connected() && bt_send_key(bc, 1)) bt_down = bc;
                        else if (wd[i].rep) { do_action(i, 0); press_t = t; last_rep = t; }   // repeat keys fire on press
                        draw();
                    }
                }
            }
        }
        if (pressed >= 0 && wd[pressed].rep && press_t && t - press_t > 400 && t - last_rep >= 108) { do_action(pressed, 1); last_rep = t; }
        if (access(WAKEFILE, F_OK) == 0) { unlink(WAKEFILE); screen_set(1); last_act = t; }
        if (access("/tmp/cmd", F_OK) == 0) {               // maintenance/debug over adb: "page N" | "tap X Y"
            char c[64]; read_str("/tmp/cmd", c, sizeof c); unlink("/tmp/cmd");
            int a, b2;
            if (!strcmp(c, "shot")) {                     // screenshot of what the app last drew (RGBA, stride STRIDE_PX)
                FILE *sf = fopen("/tmp/shot.raw", "w"); if (sf) { fwrite(back, 4, (size_t)STRIDE_PX * H, sf); fclose(sf); }
            } else
            if (sscanf(c, "page %d", &a) == 1 && a >= 0 && a < NPAGES) { page = a; screen_set(1); draw(); logf_("draw page %d: %lld ms", a, last_draw_us / 1000); }
            else if (sscanf(c, "tap %d %d", &a, &b2) == 2) {
                int i = hit(a, b2);
                if (i >= 0) {
                    int bc = wd[i].shape == S_DPAD ? DPAD_CMD[dpad_zone] : wd[i].cmd;
                    if (wd[i].act == A_IR && wd[i].a0 == 0x00 && wd[i].a1 == 0x7F && bt_connected() && bt_send_key(bc, 1)) {
                        usleep(80000); bt_send_key(bc, 0); snprintf(status, sizeof status, "Sent via Bluetooth");
                    } else do_action(i, 0);
                    draw();
                }
            }
            last_act = t;
        }
        int bt_busy = t < pair_until && !bt_connected();   // keep the screen (and BT) on while pairing
        if (screen_on && pressed < 0 && !bt_busy && t - last_act > screen_timeout) { screen_set(0); state_save(); }
        if (!screen_on && !sleep_allowed && t - last_act > screen_timeout + SLEEP_AFTER_OFF_MS) { sleep_allowed = 1; wake_unlock(); }
        static long long last_log_h = -1;
        if (resumed_flag) { resumed_flag = 0; last_bat = 0; batlog("resume"); last_log_h = boot_ms() / 3600000; }
        if (boot_ms() / 3600000 != last_log_h) { last_log_h = boot_ms() / 3600000; batlog("hourly"); }
        if (!screen_on && bt_linger_until && boot_ms() >= bt_linger_until) { bt_ctl("off"); bt_linger_until = 0; logf_("bt linger over -> off"); }
        if (t - last_bat > 30000) {
            last_bat = t; bat_cap = read_int(BAT_CAP); read_str(BAT_STATUS, bat_status, sizeof bat_status);
            int discharging = strcmp(bat_status, "Charging") && strcmp(bat_status, "Full");
            if (discharging && dis_cap0 < 0 && bat_cap >= 0) { batlog("unplugged"); dis_cap0 = bat_cap; dis_t0 = boot_ms(); logf_("unplugged at %d%%", bat_cap); }
            if (!discharging && dis_cap0 >= 0) { batlog("plugged"); logf_("plugged in; drain %d%% in %lld min", dis_cap0 - bat_cap, (boot_ms() - dis_t0) / 60000); dis_cap0 = -1; }
            // The QG gauge restores its last shutdown SOC at boot and has no coulomb counter here, so % can be stale:
            // shut down on voltage too (sustained < 3.45 V, or < 3.35 V at once), while discharging.
            int mv = read_int("/sys/class/power_supply/battery/voltage_now") / 1000;
            int low = discharging && ((bat_cap >= 0 && bat_cap <= 2) || (mv > 0 && mv < 3450));
            if (low) { if (!low_since) low_since = t; } else low_since = 0;
            if ((low_since && t - low_since > 60000) || (discharging && mv > 0 && mv < 3350)) {
                state_save(); logf_("low battery %d%% %d mV - power off", bat_cap, mv); sync(); reboot(RB_POWER_OFF);
            }
            if (screen_on && pressed < 0) draw();
        }
    }
}
