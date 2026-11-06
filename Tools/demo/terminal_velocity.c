// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// terminal_velocity.c — a demoscene production for terminals that can take it.
//
// Nine timed parts, each built on a different piece of the escape-code
// surface, all frame-synchronized with DECSET 2026 and pushed uncapped:
//
//   1  PLASMA        heavy truecolor: every cell cursor-addressed with
//                    colon-form fg+bg+underline SGR — ~1 MB a frame in the
//                    slowest parse lane, at a 120 fps frame-count clock: a
//                    terminal below the demand plays it in slow motion
//   2  ROTOZOOMER    same emitter, texture sampling
//   3  TUNNEL        sixel graphics (DCS payload, 64 registers, RLE)
//   4  METABALLS     sextant mosaics (2x3 sub-cell, two-color quantization)
//   5  STORM         cursor-addressed particle storm, self-scaling: grows
//                    until the terminal's drain rate says stop — the
//                    particle count is the benchmark (short-CSI shape)
//   6  FIRE          half-blocks + big-font sine scroller
//   7  PHANTOM       120 Hz flicker-fusion: the object exists only on odd
//                    frames — translucent at high refresh, a strobe below
//   8  STARFIELD     3D stars + greetings in the big font
//   9  STATS         what just happened, in numbers
//
// The animation clock is wall time, so the CONTENT is terminal-independent;
// the frame rate is whatever the terminal drains. The HUD shows live FPS and
// MB/s.
//
// Keys: 1-9 jump to a part, n/p (or arrow right/left) next/previous,
// a resumes the timed loop, q quits.
//
//   terminal_velocity [--fps N] [--part K] [--bench] [--frames N]
//
// --bench renders every part headless and reports generator throughput (the
// demo must never be the bottleneck it is measuring). --frames N exits after
// N frames (structural testing). q or Ctrl-C exits cleanly.

#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- timing --

static double nowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ------------------------------------------------------------- terminal ---

static struct termios savedTermios;
static int rawActive = 0;

static void terminalRestore(void)
{
    const char* out = "\x1b[?2026l\x1b\\\x1b[0m\x1b[?25h\x1b[?1049l";
    (void)!write(STDOUT_FILENO, out, strlen(out));
    if (rawActive)
        tcsetattr(STDIN_FILENO, TCSANOW, &savedTermios);
}

static void onSignal(int sig)
{
    terminalRestore();
    _exit(sig == SIGINT ? 0 : 1);
}

static void terminalSetup(void)
{
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &savedTermios) == 0) {
        struct termios raw = savedTermios;
        raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        rawActive = 1;
    }
    atexit(terminalRestore);
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    const char* out = "\x1b[?1049h\x1b[?25l\x1b[2J";
    (void)!write(STDOUT_FILENO, out, strlen(out));
}

static volatile sig_atomic_t resized = 1;
static void onWinch(int sig)
{
    (void)sig;
    resized = 1;
}

static int termCols = 100, termRows = 30;
static int cellPxW = 9, cellPxH = 18;

static void querySize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        termCols = ws.ws_col;
        termRows = ws.ws_row;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            cellPxW = ws.ws_xpixel / ws.ws_col;
            cellPxH = ws.ws_ypixel / ws.ws_row;
        }
    }
}

// ---------------------------------------------------------------- tables ---

#define SINN 4096
static float sinTab[SINN];
static char decTab[256][4]; // "0".."255"
static uint8_t decLen[256];

static void tablesInit(void)
{
    for (int i = 0; i < SINN; i++)
        sinTab[i] = sinf((float)i * (2.0f * (float)M_PI / SINN));
    for (int i = 0; i < 256; i++) {
        int n = snprintf(decTab[i], 4, "%d", i);
        decLen[i] = (uint8_t)n;
    }
}
static inline float fsin(float t) { return sinTab[(int)(t * (SINN / (2.0f * (float)M_PI))) & (SINN - 1)]; }
static inline float fcos(float t) { return fsin(t + (float)M_PI / 2.0f); }

// HSV rainbow palette, v in [0,1)
static inline void rainbow(float v, uint8_t* r, uint8_t* g, uint8_t* b)
{
    float a = v * 6.2831853f;
    int rr = (int)(128.0f + 127.0f * fsin(a));
    int gg = (int)(128.0f + 127.0f * fsin(a + 2.0944f));
    int bb = (int)(128.0f + 127.0f * fsin(a + 4.1888f));
    *r = (uint8_t)rr;
    *g = (uint8_t)gg;
    *b = (uint8_t)bb;
}

// ------------------------------------------------------------ out buffer ---

static char* out;
static size_t outLen, outCap;

static inline void emit(const char* s, size_t n)
{
    if (outLen + n > outCap) {
        outCap = (outLen + n) * 2;
        out = realloc(out, outCap);
    }
    memcpy(out + outLen, s, n);
    outLen += n;
}
static inline void emitStr(const char* s) { emit(s, strlen(s)); }
static inline void emitByte(char c) { emit(&c, 1); }
static inline void emitDec(int v)
{
    if (v < 0)
        v = 0;
    if (v > 255) {
        char tmp[12];
        emit(tmp, (size_t)snprintf(tmp, sizeof tmp, "%d", v));
        return;
    }
    emit(decTab[v], decLen[v]);
}

static void flushFrame(void)
{
    size_t off = 0;
    while (off < outLen) {
        ssize_t n = write(STDOUT_FILENO, out + off, outLen - off);
        if (n > 0)
            off += (size_t)n;
        else if (n < 0)
            break;
    }
    outLen = 0;
}

// Shared frame state: parity/phase for the temporal effects, HUD extras.
static uint64_t emittedFrames;
static char extraHud[40];

// ------------------------------------------------------------ pixel buf ----

static uint8_t* px; // W*H*3
static int pxW, pxH;
static size_t pxCap;

static void pxResize(int w, int h)
{
    pxW = w;
    pxH = h;
    size_t need = (size_t)w * (size_t)h * 3;
    if (need > pxCap) {
        px = realloc(px, need);
        pxCap = need;
    }
}
static inline uint8_t* pxAt(int x, int y) { return px + ((size_t)y * (size_t)pxW + (size_t)x) * 3; }

// ------------------------------------------------ half-block cell writer ---
// Two pixels per cell (upper = fg on U+2580, lower = bg). Emits an SGR only
// when the color pair changes, a bg-only SGR + space when both halves match,
// and CSI Ps b (REP) when whole cells repeat — flat regions cost 8 bytes a
// row instead of 40 a cell.

// Heavy emitter: full coverage, every cell individually cursor-addressed
// with colon-form truecolor fg+bg+underline SGR — ~60 bytes per cell, zero
// mergeable runs. At a fullscreen grid this is ~1 MB per frame in the
// short-parameter CSI shape, at 120 design-rate frames a second. The demo's
// clock counts FRAMES, so a terminal below the demand does not get choppy —
// it gets slow motion.
static void emitHeavyFrame(void)
{
    for (int row = 0; row < pxH / 2; row++) {
        for (int col = 0; col < pxW; col++) {
            uint8_t* top = pxAt(col, row * 2);
            uint8_t* bot = pxAt(col, row * 2 + 1);
            emitStr("\x1b[");
            emitDec(row + 1);
            emitByte(';');
            emitDec(col + 1);
            emitStr("H\x1b[38:2::");
            emitDec(top[0]);
            emitByte(':');
            emitDec(top[1]);
            emitByte(':');
            emitDec(top[2]);
            emitStr(";48:2::");
            emitDec(bot[0]);
            emitByte(':');
            emitDec(bot[1]);
            emitByte(':');
            emitDec(bot[2]);
            emitStr(";58:2::");
            emitDec((top[0] + bot[0]) / 2);
            emitByte(':');
            emitDec((top[1] + bot[1]) / 2);
            emitByte(':');
            emitDec((top[2] + bot[2]) / 2);
            emitByte('m');
            emitStr("\xe2\x96\x80");
        }
    }
    emitStr("\x1b[0m");
}

static void emitHalfBlockFrame(void)
{
    uint32_t curFg = 0xFFFFFFFF, curBg = 0xFFFFFFFF;
    emitStr("\x1b[H");
    for (int row = 0; row < pxH / 2; row++) {
        if (row)
            emitStr("\r\n");
        int col = 0;
        while (col < pxW) {
            uint8_t* top = pxAt(col, row * 2);
            uint8_t* bot = pxAt(col, row * 2 + 1);
            uint32_t fg = ((uint32_t)top[0] << 16) | ((uint32_t)top[1] << 8) | top[2];
            uint32_t bg = ((uint32_t)bot[0] << 16) | ((uint32_t)bot[1] << 8) | bot[2];
            // run length of identical pixel pairs
            int run = 1;
            while (col + run < pxW) {
                uint8_t* t2 = pxAt(col + run, row * 2);
                uint8_t* b2 = pxAt(col + run, row * 2 + 1);
                if ((((uint32_t)t2[0] << 16) | ((uint32_t)t2[1] << 8) | t2[2]) != fg)
                    break;
                if ((((uint32_t)b2[0] << 16) | ((uint32_t)b2[1] << 8) | b2[2]) != bg)
                    break;
                run++;
            }
            if (fg == bg) {
                if (bg != curBg) {
                    emitStr("\x1b[48;2;");
                    emitDec(top[0]);
                    emitByte(';');
                    emitDec(top[1]);
                    emitByte(';');
                    emitDec(top[2]);
                    emitByte('m');
                    curBg = bg;
                }
                emitByte(' ');
            } else {
                if (fg != curFg && bg != curBg) {
                    emitStr("\x1b[38;2;");
                    emitDec(top[0]);
                    emitByte(';');
                    emitDec(top[1]);
                    emitByte(';');
                    emitDec(top[2]);
                    emitStr(";48;2;");
                    emitDec(bot[0]);
                    emitByte(';');
                    emitDec(bot[1]);
                    emitByte(';');
                    emitDec(bot[2]);
                    emitByte('m');
                } else if (fg != curFg) {
                    emitStr("\x1b[38;2;");
                    emitDec(top[0]);
                    emitByte(';');
                    emitDec(top[1]);
                    emitByte(';');
                    emitDec(top[2]);
                    emitByte('m');
                } else if (bg != curBg) {
                    emitStr("\x1b[48;2;");
                    emitDec(bot[0]);
                    emitByte(';');
                    emitDec(bot[1]);
                    emitByte(';');
                    emitDec(bot[2]);
                    emitByte('m');
                }
                curFg = fg;
                curBg = bg;
                emitStr("\xe2\x96\x80"); // U+2580 upper half block
            }
            if (run > 1) {
                emitStr("\x1b[");
                emitDec(run - 1);
                emitByte('b'); // REP: repeat preceding graphic
            }
            col += run;
        }
    }
}

// ----------------------------------------------------- sextant cell writer --
// 2x3 pixels per cell via U+1FB00.. BLOCK SEXTANT glyphs (plus the half/full
// blocks the sextant range omits). Per cell: split the six pixels around the
// mean luma, foreground = mean of the bright side, background = the dark side.

static void emitUtf8(uint32_t cp)
{
    char b[4];
    if (cp < 0x80) {
        emitByte((char)cp);
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 63));
        emit(b, 2);
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 63));
        b[2] = (char)(0x80 | (cp & 63));
        emit(b, 3);
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 63));
        b[2] = (char)(0x80 | ((cp >> 6) & 63));
        b[3] = (char)(0x80 | (cp & 63));
        emit(b, 4);
    }
}

static void emitSextantFrame(void)
{
    uint32_t curFg = 0xFFFFFFFF, curBg = 0xFFFFFFFF;
    emitStr("\x1b[H");
    for (int row = 0; row < pxH / 3; row++) {
        if (row)
            emitStr("\r\n");
        for (int col = 0; col < pxW / 2; col++) {
            uint8_t* p[6];
            int luma[6], sum = 0;
            for (int i = 0; i < 6; i++) {
                p[i] = pxAt(col * 2 + (i & 1), row * 3 + i / 2);
                luma[i] = p[i][0] * 3 + p[i][1] * 6 + p[i][2];
                sum += luma[i];
            }
            int mean = sum / 6;
            int pattern = 0;
            int fr = 0, fgc = 0, fb = 0, fn = 0, br = 0, bgc = 0, bb = 0, bn = 0;
            for (int i = 0; i < 6; i++) {
                if (luma[i] > mean) {
                    pattern |= 1 << i;
                    fr += p[i][0];
                    fgc += p[i][1];
                    fb += p[i][2];
                    fn++;
                } else {
                    br += p[i][0];
                    bgc += p[i][1];
                    bb += p[i][2];
                    bn++;
                }
            }
            if (!fn) { // flat cell
                fr = br / bn;
                fgc = bgc / bn;
                fb = bb / bn;
                fn = 1;
            }
            uint32_t fg = ((uint32_t)(fr / fn) << 16) | ((uint32_t)(fgc / fn) << 8) | (uint32_t)(fb / fn);
            uint32_t bg = bn ? (((uint32_t)(br / bn) << 16) | ((uint32_t)(bgc / bn) << 8) | (uint32_t)(bb / bn)) : fg;
            if (fg != curFg || bg != curBg) {
                emitStr("\x1b[38;2;");
                emitDec((fg >> 16) & 255);
                emitByte(';');
                emitDec((fg >> 8) & 255);
                emitByte(';');
                emitDec(fg & 255);
                emitStr(";48;2;");
                emitDec((bg >> 16) & 255);
                emitByte(';');
                emitDec((bg >> 8) & 255);
                emitByte(';');
                emitDec(bg & 255);
                emitByte('m');
                curFg = fg;
                curBg = bg;
            }
            // Sextant range skips empty, left half, right half, and full.
            if (pattern == 0)
                emitByte(' ');
            else if (pattern == 0x15)
                emitUtf8(0x258C); // left half
            else if (pattern == 0x2A)
                emitUtf8(0x2590); // right half
            else if (pattern == 0x3F)
                emitUtf8(0x2588); // full block
            else
                emitUtf8(0x1FB00 + (uint32_t)pattern - 1 - (pattern > 0x15 ? 1 : 0) - (pattern > 0x2A ? 1 : 0));
        }
    }
}

// -------------------------------------------------------- sixel encoder ----
// DCS q with 64 registers. Pixels arrive as palette indices (quantized by the
// effect), one band = 6 rows, per active register one RLE pass.

#define SIXCOLORS 64
static uint8_t sixPal[SIXCOLORS][3];
static uint8_t* sixIdx; // pxW*pxH palette indices
static size_t sixIdxCap;

static void sixResize(void)
{
    size_t need = (size_t)pxW * (size_t)pxH;
    if (need > sixIdxCap) {
        sixIdx = realloc(sixIdx, need);
        sixIdxCap = need;
    }
}

static void emitSixelFrame(void)
{
    emitStr("\x1b[H\x1bP0;1;0q\"1;1;");
    emitDec(pxW);
    emitByte(';');
    emitDec(pxH);
    for (int c = 0; c < SIXCOLORS; c++) {
        emitByte('#');
        emitDec(c);
        emitStr(";2;");
        emitDec(sixPal[c][0] * 100 / 255);
        emitByte(';');
        emitDec(sixPal[c][1] * 100 / 255);
        emitByte(';');
        emitDec(sixPal[c][2] * 100 / 255);
    }
    static uint8_t bits[SIXCOLORS][4096];
    static uint8_t used[SIXCOLORS];
    for (int band = 0; band < (pxH + 5) / 6; band++) {
        memset(used, 0, sizeof used);
        int rows = pxH - band * 6;
        if (rows > 6)
            rows = 6;
        for (int r = 0; r < rows; r++) {
            const uint8_t* line = sixIdx + (size_t)(band * 6 + r) * (size_t)pxW;
            for (int x = 0; x < pxW; x++) {
                uint8_t c = line[x];
                if (!used[c]) {
                    memset(bits[c], 0, (size_t)pxW);
                    used[c] = 1;
                }
                bits[c][x] |= (uint8_t)(1 << r);
            }
        }
        int first = 1;
        for (int c = 0; c < SIXCOLORS; c++) {
            if (!used[c])
                continue;
            if (!first)
                emitByte('$'); // carriage return within band
            first = 0;
            emitByte('#');
            emitDec(c);
            int x = 0;
            while (x < pxW) {
                uint8_t v = bits[c][x];
                int run = 1;
                while (x + run < pxW && bits[c][x + run] == v)
                    run++;
                char ch = (char)(63 + v);
                if (run > 3) {
                    emitByte('!');
                    emitDec(run);
                    emitByte(ch);
                } else {
                    for (int k = 0; k < run; k++)
                        emitByte(ch);
                }
                x += run;
            }
        }
        emitByte('-'); // next band
    }
    emitStr("\x1b\\");
}

// ------------------------------------------------------------- 5x7 font ----
// Bits are rows, LSB = leftmost of 5 columns.

typedef struct {
    char c;
    uint8_t rows[7];
} Glyph;
static const Glyph font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'B', { 0x0F, 0x11, 0x11, 0x0F, 0x11, 0x11, 0x0F } },
    { 'C', { 0x0E, 0x11, 0x01, 0x01, 0x01, 0x11, 0x0E } },
    { 'D', { 0x0F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0F } },
    { 'E', { 0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x1F } },
    { 'F', { 0x1F, 0x01, 0x01, 0x0F, 0x01, 0x01, 0x01 } },
    { 'G', { 0x0E, 0x11, 0x01, 0x1D, 0x11, 0x11, 0x0E } },
    { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'J', { 0x1C, 0x08, 0x08, 0x08, 0x08, 0x09, 0x06 } },
    { 'K', { 0x11, 0x09, 0x05, 0x03, 0x05, 0x09, 0x11 } },
    { 'L', { 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x1F } },
    { 'M', { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 } },
    { 'N', { 0x11, 0x13, 0x15, 0x19, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x01 } },
    { 'Q', { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x09, 0x16 } },
    { 'R', { 0x0F, 0x11, 0x11, 0x0F, 0x05, 0x09, 0x11 } },
    { 'S', { 0x1E, 0x01, 0x01, 0x0E, 0x10, 0x10, 0x0F } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'V', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 } },
    { 'W', { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { 'Y', { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 } },
    { 'Z', { 0x1F, 0x10, 0x08, 0x04, 0x02, 0x01, 0x1F } },
    { '0', { 0x0E, 0x19, 0x15, 0x15, 0x15, 0x13, 0x0E } },
    { '1', { 0x04, 0x06, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x10, 0x08, 0x04, 0x02, 0x1F } },
    { '3', { 0x0E, 0x11, 0x10, 0x0C, 0x10, 0x11, 0x0E } },
    { '4', { 0x08, 0x0C, 0x0A, 0x09, 0x1F, 0x08, 0x08 } },
    { '5', { 0x1F, 0x01, 0x0F, 0x10, 0x10, 0x11, 0x0E } },
    { '6', { 0x0C, 0x02, 0x01, 0x0F, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x10, 0x08, 0x04, 0x02, 0x02, 0x02 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x1E, 0x10, 0x08, 0x06 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06 } },
    { '!', { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '/', { 0x10, 0x10, 0x08, 0x04, 0x02, 0x01, 0x01 } },
    { ':', { 0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00 } },
    { ' ', { 0, 0, 0, 0, 0, 0, 0 } },
};

static const uint8_t* glyphRows(char c)
{
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return font[sizeof(font) / sizeof(font[0]) - 1].rows;
}

// Draw text into the pixel buffer at (x,y), scale s, additive color.
static void drawText(const char* s, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t* rows = glyphRows(*s);
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(rows[gy] >> gx & 1))
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int X = x + gx * scale + sx, Y = y + gy * scale + sy;
                        if (X >= 0 && X < pxW && Y >= 0 && Y < pxH) {
                            uint8_t* p = pxAt(X, Y);
                            p[0] = r;
                            p[1] = g;
                            p[2] = b;
                        }
                    }
            }
    }
}

// --------------------------------------------------------------- effects ---

// 4x4 Bayer matrix: ordered dither de-bands the gradients and — the real
// point — makes adjacent cells unequal, so the emitter cannot collapse the
// frame into cheap merged runs. Pretty and heavy.
static const int8_t bayer[4][4] = { { -2, 0, -1, 1 }, { 1, -1, 2, 0 }, { -1, 1, -2, 0 }, { 2, 0, 1, -1 } };

static inline void ditherAt(int x, int y, uint8_t* p)
{
    int d = bayer[y & 3][x & 3];
    for (int i = 0; i < 3; i++) {
        int v = p[i] + d;
        p[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255
                                             : v);
    }
}

static void fxPlasma(double t)
{
    float ft = (float)t;
    for (int y = 0; y < pxH; y++) {
        float fy = (float)y;
        for (int x = 0; x < pxW; x++) {
            float fx = (float)x;
            float v = fsin(fx * 0.06f + ft * 2.4f) + fsin(fy * 0.11f - ft * 3.1f)
                + fsin((fx + fy) * 0.045f + ft * 1.7f)
                + fsin(sqrtf((fx - pxW * 0.5f) * (fx - pxW * 0.5f) + (fy - pxH * 0.5f) * (fy - pxH * 0.5f)) * 0.09f - ft * 4.8f);
            uint8_t* p = pxAt(x, y);
            rainbow(v * 0.125f + 0.5f + (float)t * 0.02f, p, p + 1, p + 2);
            ditherAt(x, y, p);
        }
    }
}

static void fxRotozoom(double t)
{
    float ang = (float)t * 1.4f;
    float zoom = 1.6f + 1.2f * fsin((float)t * 0.5f);
    float ca = fcos(ang) * zoom, sa = fsin(ang) * zoom;
    float cx = pxW * 0.5f, cy = pxH * 0.5f;
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            float dx = (float)x - cx, dy = ((float)y - cy) * 2.0f;
            int u = (int)(dx * ca - dy * sa + (float)t * 40.0f);
            int v = (int)(dx * sa + dy * ca);
            uint8_t* p = pxAt(x, y);
            if (((u >> 4) ^ (v >> 4)) & 1) {
                rainbow((float)((u + v) & 255) / 256.0f, p, p + 1, p + 2);
            } else {
                int shade = 30 + ((u ^ v) & 15) * 4;
                p[0] = p[1] = (uint8_t)(shade / 2);
                p[2] = (uint8_t)shade;
            }
            ditherAt(x, y, p);
        }
}

// Tunnel renders palette INDICES for the sixel encoder.
static float* tunAngle;
static float* tunDepth;
static int tunW, tunH;

static void fxTunnelTables(void)
{
    if (tunW == pxW && tunH == pxH)
        return;
    tunW = pxW;
    tunH = pxH;
    tunAngle = realloc(tunAngle, (size_t)pxW * pxH * sizeof(float));
    tunDepth = realloc(tunDepth, (size_t)pxW * pxH * sizeof(float));
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            float dx = (float)x - pxW * 0.5f, dy = ((float)y - pxH * 0.5f) * 1.2f;
            float d = sqrtf(dx * dx + dy * dy);
            tunAngle[(size_t)y * pxW + x] = atan2f(dy, dx) / (2.0f * (float)M_PI) + 0.5f;
            tunDepth[(size_t)y * pxW + x] = d > 1.0f ? 96.0f / d : 96.0f;
        }
}

static void fxTunnel(double t)
{
    fxTunnelTables();
    for (int c = 0; c < SIXCOLORS; c++) {
        float shade = (float)(c & 15) / 15.0f;
        uint8_t r, g, b;
        rainbow((float)(c >> 4) * 0.25f + (float)t * 0.01f, &r, &g, &b);
        sixPal[c][0] = (uint8_t)(r * shade);
        sixPal[c][1] = (uint8_t)(g * shade);
        sixPal[c][2] = (uint8_t)(b * shade);
    }
    float ft = (float)t;
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            size_t i = (size_t)y * pxW + x;
            int u = (int)((tunAngle[i] * 16.0f + ft * 0.5f) * 2.0f);
            int v = (int)(tunDepth[i] + ft * 18.0f);
            int hue = ((u ^ (v >> 3)) & 3);
            float depth = tunDepth[i] / 96.0f;
            int shade = (int)(depth * 14.0f * (0.75f + 0.25f * fsin(ft * 3.0f)));
            if (((u + (v >> 3)) & 1))
                shade = shade * 2 / 3;
            if (shade > 15)
                shade = 15;
            sixIdx[i] = (uint8_t)((hue << 4) | shade);
        }
}

static void fxMetaballs(double t)
{
    float bx[5], by[5];
    for (int i = 0; i < 5; i++) {
        bx[i] = pxW * (0.5f + 0.38f * fsin((float)t * (0.6f + 0.13f * i) + i * 2.1f));
        by[i] = pxH * (0.5f + 0.38f * fcos((float)t * (0.5f + 0.17f * i) + i * 1.3f));
    }
    float r2 = (float)(pxW * pxH) * 0.01f;
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            float f = 0;
            for (int i = 0; i < 5; i++) {
                float dx = (float)x - bx[i], dy = (float)y - by[i];
                f += r2 / (dx * dx + dy * dy + 1.0f);
            }
            uint8_t* p = pxAt(x, y);
            if (f > 1.0f) {
                float glow = f - 1.0f;
                if (glow > 1.0f)
                    glow = 1.0f;
                p[0] = (uint8_t)(60 + 195 * glow);
                p[1] = (uint8_t)(20 + 80 * glow);
                p[2] = (uint8_t)(120 + 135 * glow);
            } else {
                int v = (int)(f * 40.0f);
                p[0] = (uint8_t)(v / 3);
                p[1] = 0;
                p[2] = (uint8_t)v;
            }
        }
}

static uint8_t* fireBuf;
static size_t fireCap;
static uint32_t rngState = 0x12345;
static inline uint32_t rnd(void) { return rngState = rngState * 1103515245u + 12345u; }

static const char* scrollText = "   UBIQUITTY PRESENTS: TERMINAL VELOCITY   ...   MEGABYTES A SECOND OF PURE ESCAPE CODES   ...   NO GPU WAS HARMED: EVERY PIXEL YOU SEE IS AN SGR   ...   THE PTY IS A 1024 BYTE STRAW AND WE ARE DRINKING THE OCEAN THROUGH IT   ...   ";

static void fxFire(double t)
{
    size_t need = (size_t)pxW * (size_t)(pxH + 2);
    if (need > fireCap) {
        fireBuf = realloc(fireBuf, need);
        memset(fireBuf, 0, need);
        fireCap = need;
    }
    for (int x = 0; x < pxW; x++) {
        uint8_t v = (uint8_t)(160 + (rnd() >> 24 & 95));
        fireBuf[(size_t)pxH * pxW + x] = v;
        fireBuf[(size_t)(pxH + 1) * pxW + x] = v;
    }
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            int below = (y + 1) * pxW + x;
            int v = (fireBuf[below] + fireBuf[below + (x > 0 ? -1 : 0)] + fireBuf[below + (x < pxW - 1 ? 1 : 0)] + fireBuf[(y + 2 <= pxH + 1 ? y + 2 : y + 1) * pxW + x]) / 4;
            v -= (int)(rnd() >> 28 & 3);
            fireBuf[(size_t)y * pxW + x] = (uint8_t)(v < 0 ? 0 : v);
        }
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            int v = fireBuf[(size_t)y * pxW + x];
            uint8_t* p = pxAt(x, y);
            p[0] = (uint8_t)(v > 64 ? 255 : v * 4);
            p[1] = (uint8_t)(v > 128 ? (v - 128) * 2 + 60 : v / 2);
            p[2] = (uint8_t)(v > 192 ? (v - 192) * 3 : 0);
        }
    // sine scroller across the middle, big font
    int scale = pxH >= 100 ? 3 : 2;
    int stepX = 6 * scale;
    int offset = (int)(t * 90.0) % ((int)strlen(scrollText) * stepX);
    for (int i = 0; i < (int)strlen(scrollText); i++) {
        int x = pxW + i * stepX - offset;
        if (x < -stepX || x > pxW)
            continue;
        int y = pxH / 3 + (int)(fsin((float)t * 2.0f + (float)x * 0.02f) * (float)pxH * 0.12f);
        char s[2] = { scrollText[i], 0 };
        drawText(s, x, y, scale, 255, 255, 255);
    }
}

// STORM — mode 3, raw escape emission. Every particle is its own short
// sequence pair (CUP + 256-color SGR + one ASCII glyph), so the stream is
// almost all per-sequence parse cost. The count self-scales on real
// backpressure: write() blocks when the terminal cannot drain, frame time
// rises, the storm thins — the on-screen count is the terminal's score.
#define STORMMAX 262144
static float stormX[STORMMAX], stormY[STORMMAX];
static int stormN = 6000;
static double lastFrameSeconds;

static void fxStorm(double t)
{
    static int seeded = 0;
    if (!seeded) {
        seeded = 1;
        for (int i = 0; i < STORMMAX; i++) {
            stormX[i] = (float)(rnd() % 10000) / 10000.0f;
            stormY[i] = (float)(rnd() % 10000) / 10000.0f;
        }
    }
    // proportional controller on real frame time (gen + drain): 8 ms target
    int densityCap = termRows * termCols * 2 / 5; // legibility: <= 40% fill
    if (densityCap > STORMMAX)
        densityCap = STORMMAX;
    if (lastFrameSeconds > 0) {
        if (lastFrameSeconds < 0.007 && stormN < densityCap)
            stormN += stormN / 32 + 16;
        else if (lastFrameSeconds > 0.009 && stormN > 2000)
            stormN -= stormN / 16;
        if (stormN > densityCap)
            stormN = densityCap;
    }
    snprintf(extraHud, sizeof extraHud, "%d PARTICLES", stormN);
    emitStr("\x1b[2J");
    float ft = (float)t;
    static const char glyphs[] = ".:+*#@";
    for (int i = 0; i < stormN; i++) {
        // swirl field with per-particle phase
        float px_ = stormX[i], py_ = stormY[i];
        float a = fsin(px_ * 5.1f + ft * 0.9f) + fcos(py_ * 4.3f - ft * 0.7f) + fsin((px_ + py_) * 3.7f + ft * 0.31f);
        float speed = 0.0016f + 0.0022f * fsin((float)i * 0.37f + ft);
        stormX[i] += fcos(a * 3.1416f) * speed;
        stormY[i] += fsin(a * 3.1416f) * speed * 0.55f;
        if (stormX[i] < 0.0f)
            stormX[i] += 1.0f;
        if (stormX[i] >= 1.0f)
            stormX[i] -= 1.0f;
        if (stormY[i] < 0.0f)
            stormY[i] += 1.0f;
        if (stormY[i] >= 1.0f)
            stormY[i] -= 1.0f;
        int row = 1 + (int)(stormY[i] * (float)(termRows - 1));
        int col = 1 + (int)(stormX[i] * (float)termCols);
        int c = 16 + ((i + (int)(t * 30.0)) % 216);
        int g = (int)(fabsf(speed) * 2200.0f);
        emitStr("\x1b[");
        emitDec(row);
        emitByte(';');
        emitDec(col);
        emitStr("H\x1b[38;5;");
        emitDec(c);
        emitByte('m');
        emitByte(glyphs[g > 5 ? 5 : g]);
    }
    emitStr("\x1b[0m");
}

// PHANTOM — 120 Hz flicker fusion. The torus knot is drawn only on odd
// frames: at 120 Hz presentation it fuses into a translucent hologram over
// the backdrop; a terminal that cannot present ~100+ fps shows a strobe.
// Frame parity also offsets the dither, temporally averaging extra color
// depth the palette does not have.
static void fxPhantom(double t)
{
    float ft = (float)t;
    for (int y = 0; y < pxH; y++)
        for (int x = 0; x < pxW; x++) {
            float v = fsin((float)x * 0.045f + ft * 0.4f) + fsin((float)y * 0.07f - ft * 0.3f);
            uint8_t* p = pxAt(x, y);
            rainbow(v * 0.1f + 0.62f, p, p + 1, p + 2);
            p[0] /= 3;
            p[1] /= 3;
            p[2] /= 3;
            int d = bayer[(y + (int)(emittedFrames & 1) * 2) & 3][x & 3];
            for (int i = 0; i < 3; i++) {
                int q = p[i] + d;
                p[i] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255
                                                     : q);
            }
        }
    if (emittedFrames & 1) {
        // (2,3) torus knot point cloud, projected
        for (int i = 0; i < 2600; i++) {
            float u = (float)i * 0.0024166f * 6.2832f + ft * 0.5f;
            float r = fcos(3.0f * u) + 2.0f;
            float X = r * fcos(2.0f * u), Y = r * fsin(2.0f * u), Z = fsin(3.0f * u);
            float ca = fcos(ft * 0.8f), sa = fsin(ft * 0.8f);
            float Xr = X * ca - Z * sa, Zr = X * sa + Z * ca + 4.5f;
            int sx = (int)(pxW * 0.5f + Xr / Zr * (float)pxW * 0.62f);
            int sy = (int)(pxH * 0.5f + Y / Zr * (float)pxH * 0.62f);
            if (sx < 1 || sx >= pxW - 1 || sy < 1 || sy >= pxH - 1)
                continue;
            int lum = (int)(255.0f * (1.2f - Zr * 0.13f));
            if (lum > 255)
                lum = 255;
            if (lum < 60)
                lum = 60;
            for (int dy = 0; dy <= 1; dy++)
                for (int dx = 0; dx <= 1; dx++) {
                    uint8_t* p = pxAt(sx + dx, sy + dy);
                    p[0] = (uint8_t)lum;
                    p[1] = (uint8_t)(lum * 0.9f);
                    p[2] = 255;
                }
        }
    }
    snprintf(extraHud, sizeof extraHud, "ODD-FRAME PHANTOM");
}

static const char* greets[] = { "GREETINGS TO:", "KITTY", "WEZTERM", "ALACRITTY", "AND EVERY VT", "SINCE 1978" };

static void fxStarfield(double t)
{
    memset(px, 0, (size_t)pxW * pxH * 3);
    uint32_t seed = 1;
    for (int i = 0; i < 400; i++) {
        seed = seed * 1103515245u + 12345u;
        float sx = (float)(seed >> 16 & 1023) / 1023.0f - 0.5f;
        seed = seed * 1103515245u + 12345u;
        float sy = (float)(seed >> 16 & 1023) / 1023.0f - 0.5f;
        seed = seed * 1103515245u + 12345u;
        float z = fmodf((float)(seed >> 16 & 1023) / 1023.0f - (float)t * 0.13f, 1.0f);
        if (z <= 0.02f)
            z += 1.0f;
        int X = (int)(pxW * 0.5f + sx / z * pxW * 0.9f);
        int Y = (int)(pxH * 0.5f + sy / z * pxH * 0.9f);
        if (X < 0 || X >= pxW || Y < 0 || Y >= pxH)
            continue;
        int lum = (int)((1.0f - z) * 255.0f);
        uint8_t* p = pxAt(X, Y);
        p[0] = p[1] = p[2] = (uint8_t)lum;
        if (z < 0.3f && X + 1 < pxW) {
            p = pxAt(X + 1, Y);
            p[0] = p[1] = p[2] = (uint8_t)(lum / 2);
        }
    }
    int line = (int)(t / 2.2) % (int)(sizeof(greets) / sizeof(greets[0]));
    const char* s = greets[line];
    int scale = pxH >= 100 ? 3 : 2;
    int w = (int)strlen(s) * 6 * scale;
    float pulse = 0.75f + 0.25f * fsin((float)t * 6.0f);
    uint8_t r, g, b;
    rainbow((float)line * 0.14f, &r, &g, &b);
    drawText(s, (pxW - w) / 2, (pxH - 7 * scale) / 2, scale, (uint8_t)(r * pulse), (uint8_t)(g * pulse), (uint8_t)(b * pulse));
}

static uint64_t totalBytes;
static uint64_t totalFrames;
static double demoStart;

static void fxStats(double t)
{
    fxPlasma(t * 0.3);
    for (size_t i = 0; i < (size_t)pxW * pxH * 3; i++)
        px[i] /= 4; // dim backdrop
    double elapsed = nowSeconds() - demoStart;
    char l1[64], l2[64], l3[64];
    snprintf(l1, sizeof l1, "%llu FRAMES", (unsigned long long)totalFrames);
    snprintf(l2, sizeof l2, "%.1f GB OF ESCAPES", (double)totalBytes / 1e9);
    snprintf(l3, sizeof l3, "%.0f MB/S SUSTAINED", (double)totalBytes / elapsed / 1e6);
    int scale = pxH >= 100 ? 3 : 2;
    const char* lines[] = { "TERMINAL VELOCITY", l1, l2, l3, "UBIQUITTY 2026" };
    for (int i = 0; i < 5; i++) {
        int w = (int)strlen(lines[i]) * 6 * scale;
        drawText(lines[i], (pxW - w) / 2, pxH / 2 + (i - 2) * 10 * scale, scale, 255, 255, 255);
    }
}

// ------------------------------------------------------------------ HUD ----
#define NPARTS 9

static double fpsWindowStart;
static int fpsWindowFrames;
static uint64_t fpsWindowBytes;
static double shownFps, shownMBs;

static void emitHud(int part, const char* name)
{
    char hud[160];
    int n = snprintf(hud, sizeof hud, " %d/%d %s %s %4.0f FPS  %5.1f MB/S ", part, NPARTS, name, extraHud, shownFps, shownMBs);
    emitStr("\x1b[1;");
    emitDec(termCols - n + 1 > 1 ? termCols - n + 1 : 1);
    emitStr("H\x1b[0;30;47m");
    emit(hud, (size_t)n);
    emitStr("\x1b[0m");
}

// ------------------------------------------------------------------ main ---

typedef void (*EffectFn)(double);
typedef struct {
    const char* name;
    EffectFn fn;
    int mode; // 0 halfblock, 1 sixel, 2 sextant, 3 raw emit
    double seconds;
    double fpsTarget; // 0 = uncapped; PHANTOM needs paced parity
} Part;

static const Part parts[] = {
    { "PLASMA", fxPlasma, 4, 14, 0 },
    { "ROTOZOOM", fxRotozoom, 4, 12, 0 },
    { "TUNNEL", fxTunnel, 1, 14, 0 },
    { "METABALLS", fxMetaballs, 2, 12, 0 },
    { "STORM", fxStorm, 3, 18, 0 },
    { "FIRE", fxFire, 0, 16, 0 },
    { "PHANTOM", fxPhantom, 0, 16, 0 },
    { "STARFIELD", fxStarfield, 2, 16, 0 },
    { "STATS", fxStats, 0, 10, 0 },
};
_Static_assert(sizeof(parts) / sizeof(parts[0]) == NPARTS, "part roster and NPARTS agree");

static void sizeForMode(int mode)
{
    if (mode == 0)
        pxResize(termCols, (termRows - 0) * 2);
    else if (mode == 2)
        pxResize(termCols * 2, termRows * 3);
    else {
        int w = termCols * cellPxW, h = termRows * cellPxH;
        if (w > 1024)
            w = 1024;
        if (h > 640)
            h = 640;
        pxResize(w & ~1, h - h % 6);
        sixResize();
    }
}

static void renderFrame(int partIndex, double t)
{
    const Part* p = &parts[partIndex];
    extraHud[0] = 0;
    emitStr("\x1b[?2026h"); // begin synchronized update
    if (p->mode == 3) {
        p->fn(t); // raw mode: the effect emits its own escape stream
    } else {
        sizeForMode(p->mode == 4 ? 0 : p->mode);
        p->fn(t);
        if (p->mode == 1) {
            emitStr("\x1b[2J");
            emitSixelFrame();
        } else if (p->mode == 2)
            emitSextantFrame();
        else if (p->mode == 4)
            emitHeavyFrame();
        else
            emitHalfBlockFrame();
    }
    emitHud(partIndex + 1, p->name);
    emitStr("\x1b[?2026l"); // end synchronized update: one complete frame
    emittedFrames++;
}

static int runBench(void)
{
    tablesInit();
    termCols = 200;
    termRows = 60;
    cellPxW = 8;
    cellPxH = 16;
    printf("terminal_velocity --bench (%dx%d cells)\n", termCols, termRows);
    printf("%-10s %8s %10s %9s\n", "part", "frames", "MB", "gen MB/s");
    for (int i = 0; i < NPARTS; i++) {
        outLen = 0;
        uint64_t bytes = 0;
        const int frames = 120;
        double t0 = nowSeconds();
        for (int f = 0; f < frames; f++) {
            renderFrame(i, (double)f / 60.0);
            bytes += outLen;
            outLen = 0;
        }
        double dt = nowSeconds() - t0;
        printf("%-10s %8d %10.1f %9.0f\n", parts[i].name, frames, (double)bytes / 1e6, (double)bytes / dt / 1e6);
    }
    return 0;
}

int main(int argc, char** argv)
{
    double fpsCap = 120; // the design rate; --fps 0 unlocks
    int wallClock = 0;
    int onlyPart = -1;
    long maxFrames = 0;
    int bench = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench"))
            bench = 1;
        else if (!strcmp(argv[i], "--wall"))
            wallClock = 1;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            fpsCap = atof(argv[++i]);
        else if (!strcmp(argv[i], "--part") && i + 1 < argc)
            onlyPart = atoi(argv[++i]) - 1;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            maxFrames = atol(argv[++i]);
        else {
            fprintf(stderr, "usage: terminal_velocity [--fps N] [--part K] [--frames N] [--bench]\n");
            return 2;
        }
    }
    outCap = 8 << 20;
    out = malloc(outCap);
    if (bench)
        return runBench();

    tablesInit();
    signal(SIGWINCH, onWinch);
    terminalSetup();
    demoStart = nowSeconds();
    fpsWindowStart = demoStart;

    for (;;) {
        if (resized) {
            resized = 0;
            querySize();
        }
        // The production clock counts frames at the 120 fps design rate:
        // a terminal that cannot drain the design rate plays the demo in
        // slow motion — pacing (below) keeps a faster terminal from
        // playing it fast-forward. --wall restores the wall clock.
        double t = wallClock ? nowSeconds() - demoStart : (double)totalFrames / 120.0;
        int part;
        if (onlyPart >= 0)
            part = onlyPart % NPARTS;
        else {
            double cycle = 0;
            for (int i = 0; i < NPARTS; i++)
                cycle += parts[i].seconds;
            double m = fmod(t, cycle);
            part = 0;
            while (m >= parts[part].seconds) {
                m -= parts[part].seconds;
                part++;
            }
        }
        const double frameStart = nowSeconds();
        renderFrame(part, t);
        totalBytes += outLen;
        fpsWindowBytes += outLen;
        flushFrame();
        totalFrames++;
        fpsWindowFrames++;

        double now = nowSeconds();
        if (now - fpsWindowStart >= 0.5) {
            shownFps = fpsWindowFrames / (now - fpsWindowStart);
            shownMBs = (double)fpsWindowBytes / (now - fpsWindowStart) / 1e6;
            fpsWindowStart = now;
            fpsWindowFrames = 0;
            fpsWindowBytes = 0;
        }
        if (maxFrames && totalFrames >= (uint64_t)maxFrames)
            break;
        char c;
        int quit = 0;
        while (rawActive && read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 3) {
                quit = 1;
                break;
            }
            if (c == 0x1b) { // arrow keys arrive as ESC [ C / ESC [ D
                char seq[2];
                if (read(STDIN_FILENO, seq, 2) == 2 && seq[0] == '[')
                    c = seq[1] == 'C' ? 'n' : seq[1] == 'D' ? 'p'
                                                            : 0;
                else
                    c = 0;
            }
            if (c >= '1' && c <= '0' + NPARTS)
                onlyPart = c - '1';
            else if (c == 'n')
                onlyPart = (onlyPart < 0 ? part : onlyPart) + 1;
            else if (c == 'p')
                onlyPart = (onlyPart < 0 ? part : onlyPart) + NPARTS - 1;
            else if (c == 'a')
                onlyPart = -1; // resume the timed loop
            if (onlyPart >= 0)
                onlyPart %= NPARTS;
        }
        if (quit)
            break;
        // Frame time = from the render-clock sample to post-drain: the
        // write() backpressure the storm controller feeds on lives here.
        lastFrameSeconds = now - frameStart;
        double cap = fpsCap > 0 ? fpsCap : parts[part].fpsTarget;
        if (cap > 0) {
            double frameBudget = 1.0 / cap;
            double spent = nowSeconds() - now;
            if (spent < frameBudget)
                usleep((useconds_t)((frameBudget - spent) * 1e6));
        }
    }
    terminalRestore();
    printf("terminal velocity: %llu frames, %.2f GB, %.0f MB/s sustained\n",
        (unsigned long long)totalFrames, (double)totalBytes / 1e9,
        (double)totalBytes / (nowSeconds() - demoStart) / 1e6);
    return 0;
}
