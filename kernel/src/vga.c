/**
 * QuantumOS VGA Text-Mode Console + Boot Splash Implementation
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/vga.h>
#include <kernel/fb.h>

static volatile uint16_t *const VGA_MEM = (uint16_t *)0xB8000;

/* CP437 shade blocks, light -> full */
#define CH_SHADE_LIGHT 0xB0
#define CH_SHADE_MED 0xB1
#define CH_SHADE_DARK 0xB2
#define CH_BLOCK_FULL 0xDB

static int splash_frame = 0;

/* ---- port I/O (hardware cursor) ---- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t cell(char ch, vga_color_t fg, vga_color_t bg) {
    return (uint16_t)((uint8_t)ch) | ((uint16_t)((bg << 4) | fg) << 8);
}

/* ---- integer sine, period 32, amplitude +/-100 ---- */
static const int8_t sine32[32] = {0,   20,  38,   56,  71,  83,  92,  98,  100, 98,  92,
                                  83,  71,  56,   38,  20,  0,   -20, -38, -56, -71, -83,
                                  -92, -98, -100, -98, -92, -83, -71, -56, -38, -20};

/* Coarse busy-wait: interrupts are not enabled during the splash, so
 * animation frames are paced by a calibrated volatile loop. Kept small
 * so total splash time stays well under the boot smoke-test budget. */
static void frame_delay(void) {
    for (volatile uint32_t i = 0; i < 400000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

/* ---- primitives ---- */
void vga_put(int x, int y, char ch, vga_color_t fg, vga_color_t bg) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT) {
        return;
    }
    VGA_MEM[y * VGA_WIDTH + x] = cell(ch, fg, bg);
}

void vga_print(int x, int y, const char *s, vga_color_t fg, vga_color_t bg) {
    for (int i = 0; s[i]; i++) {
        vga_put(x + i, y, s[i], fg, bg);
    }
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

void vga_print_center(int y, const char *s, vga_color_t fg, vga_color_t bg) {
    int x = (VGA_WIDTH - str_len(s)) / 2;
    vga_print(x, y, s, fg, bg);
}

void vga_clear(vga_color_t bg) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEM[i] = cell(' ', VGA_LIGHT_GREY, bg);
    }
}

static void hide_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20); /* bit 5 disables the cursor */
}

void vga_init(void) {
    hide_cursor();
    vga_clear(VGA_BLACK);
}

/* ---- boot splash ---- */

/* One frame of a two-source interference pattern across the wave band.
 * Two waves travel in opposite directions; their sum sets the shade
 * and colour of each column, producing a shimmering standing pattern. */
static void draw_wave(int frame) {
    const int top = 10; /* wave band rows 10..13 */
    const int rows = 4;
    const int mid = top + rows / 2;

    for (int x = 4; x < VGA_WIDTH - 4; x++) {
        int a = sine32[(x + frame) & 31];
        int b = sine32[(x * 2 - frame) & 31];
        int sum = (a + b) / 2;          /* -100..100 */
        int amp = sum < 0 ? -sum : sum; /* 0..100 */

        int level = amp / 26; /* 0..3 shade */
        char ch;
        vga_color_t fg;
        switch (level) {
        case 0:
            ch = CH_SHADE_LIGHT;
            fg = VGA_BLUE;
            break;
        case 1:
            ch = CH_SHADE_MED;
            fg = VGA_LIGHT_BLUE;
            break;
        case 2:
            ch = CH_SHADE_DARK;
            fg = VGA_LIGHT_CYAN;
            break;
        default:
            ch = CH_BLOCK_FULL;
            fg = VGA_WHITE;
            break;
        }

        /* Map the signed sum to a row within the band for a crest line */
        int row = mid - (sum * (rows / 2)) / 100;
        for (int y = top; y < top + rows; y++) {
            if (y == row) {
                vga_put(x, y, ch, fg, VGA_BLACK);
            } else {
                vga_put(x, y, CH_SHADE_LIGHT, (y < row) ? VGA_DARK_GREY : VGA_BLUE, VGA_BLACK);
            }
        }
    }
}

void vga_boot_splash(void) {
    vga_init();

    /* Frame border */
    for (int x = 0; x < VGA_WIDTH; x++) {
        vga_put(x, 0, CH_BLOCK_FULL, VGA_BLUE, VGA_BLACK);
        vga_put(x, VGA_HEIGHT - 1, CH_BLOCK_FULL, VGA_BLUE, VGA_BLACK);
    }

    /* Title */
    vga_print_center(3, "Q U A N T U M   O S", VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print_center(5, "wave-interference microkernel", VGA_WHITE, VGA_BLACK);
    vga_print_center(7, "// ring-3 isolation // capability security // qubits //", VGA_DARK_GREY,
                     VGA_BLACK);

    /* Progress bar frame at row 18 */
    vga_print(14, 18, "[", VGA_LIGHT_GREY, VGA_BLACK);
    vga_print(14 + 52 + 1, 18, "]", VGA_LIGHT_GREY, VGA_BLACK);

    vga_print_center(22, "seeding from the quantum lab  ~  qBraid boot target", VGA_DARK_GREY,
                     VGA_BLACK);
}

void vga_boot_stage(const char *label, int percent) {
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    /* Animate a couple of wave frames so the pattern travels between
     * stages */
    for (int f = 0; f < 2; f++) {
        draw_wave(splash_frame++);
        frame_delay();
    }

    /* Progress bar: 52 cells wide starting at column 15 */
    int filled = (percent * 52) / 100;
    for (int i = 0; i < 52; i++) {
        vga_put(15 + i, 18, (i < filled) ? CH_BLOCK_FULL : CH_SHADE_LIGHT,
                (i < filled) ? VGA_LIGHT_GREEN : VGA_DARK_GREY, VGA_BLACK);
    }

    /* Percent readout + current stage label (clear the line first) */
    char pct[5];
    pct[0] = (char)('0' + (percent / 100) % 10);
    pct[1] = (char)('0' + (percent / 10) % 10);
    pct[2] = (char)('0' + percent % 10);
    pct[3] = '%';
    pct[4] = '\0';
    const char *p = pct;
    if (percent < 100)
        p = pct + 1; /* drop leading zero */
    vga_print_center(19, "                                        ", VGA_BLACK, VGA_BLACK);
    for (int i = 0; i < VGA_WIDTH; i++)
        vga_put(i, 20, ' ', VGA_BLACK, VGA_BLACK);
    vga_print(15, 20, label, VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print(15 + 52 - str_len(p), 20, p, VGA_YELLOW, VGA_BLACK);
}

void vga_boot_ready(void) {
    for (int f = 0; f < 6; f++) {
        draw_wave(splash_frame++);
        frame_delay();
    }
    for (int i = 0; i < 52; i++) {
        vga_put(15 + i, 18, CH_BLOCK_FULL, VGA_LIGHT_GREEN, VGA_BLACK);
    }
    for (int i = 0; i < VGA_WIDTH; i++)
        vga_put(i, 20, ' ', VGA_BLACK, VGA_BLACK);
    vga_print_center(20, ">>  Q U A N T U M   O S   R E A D Y  <<", VGA_YELLOW, VGA_BLACK);
}

/* ---- scrolling text console (epic #101) ----
 *
 * Callers already serialize against interrupts (console_write holds an
 * IRQ-save guard; boot_log runs its serial half with IF managed by
 * early_console_write), so these routines do plain memory writes and
 * keep no locks of their own. Worst case under a race is a torn line on
 * screen, never corruption beyond the 0xB8000 cell array.
 *
 * The console WRAPS instead of scrolling: past the bottom row the pen
 * returns to row 0, clearing the row it enters plus the row below as a
 * moving blank separator, teletype-style. Two cheaper-looking options
 * were tried and rejected with evidence:
 *   - memmove scroll: ~4000 0xB8000 accesses per line, each an MMIO
 *     callback under QEMU TCG, all with interrupts disabled — it stole
 *     enough CPU to starve the ring-3 services and break the
 *     paradoxd/ghostd coupling CI gate.
 *   - CRTC start-address panning: QEMU's text renderer places the
 *     origin at 2x the programmed start value while real VGA uses 1x
 *     (character units), so no single value renders correctly on both.
 * Wrapping costs a flat 160 cell writes per line and needs no CRTC
 * state, so it behaves identically on QEMU and real hardware. */

static int cons_active;
static int cons_x, cons_y;
/* Cell grid and backend (#264): 80x25 at 0xB8000 on a text-mode boot, or
 * the framebuffer cell store (fb.c) when the loader handed the console image
 * a GOP framebuffer because the firmware had no text mode — UEFI. The line
 * discipline below is shared; only cons_cell/cons_clear_row and the cursor
 * know which display they are on. */
static int cons_cols = VGA_WIDTH, cons_rows = VGA_HEIGHT;
static int cons_on_fb;

/* ---- minimal ANSI SGR colour for the screen console ----
 * The serial console passes escape bytes straight through, so a real
 * terminal (xterm in the browser demo) colours itself; the VGA screen has
 * no terminal, so we interpret the handful of SGR codes qsh uses and set
 * the text attribute directly. Anything we don't recognise is swallowed,
 * never printed — so a colour code can never leave `[31m` litter on screen.
 * ANSI colour order differs from the VGA palette, hence the map. */
static uint8_t cons_fg = VGA_LIGHT_GREY;
static uint8_t cons_bg = VGA_BLACK;
static uint8_t cons_bold;
static const uint8_t ansi2vga[8] = {VGA_BLACK, VGA_RED,     VGA_GREEN, VGA_BROWN,
                                    VGA_BLUE,  VGA_MAGENTA, VGA_CYAN,  VGA_LIGHT_GREY};
static enum { ESC_NONE, ESC_SEEN, ESC_CSI } cons_esc;
#define CONS_MAXPARAM 8
static int cons_param[CONS_MAXPARAM];
static int cons_nparam;
static int cons_curparam;

static void cons_apply_sgr(void) {
    for (int i = 0; i < cons_nparam; i++) {
        int p = cons_param[i];
        if (p == 0) {
            cons_fg = VGA_LIGHT_GREY;
            cons_bg = VGA_BLACK;
            cons_bold = 0;
        } else if (p == 1) {
            cons_bold = 1;
        } else if (p == 22) {
            cons_bold = 0;
        } else if (p >= 30 && p <= 37) {
            cons_fg = (uint8_t)(ansi2vga[p - 30] | (cons_bold ? 8 : 0));
        } else if (p == 39) {
            cons_fg = VGA_LIGHT_GREY;
        } else if (p >= 40 && p <= 47) {
            cons_bg = ansi2vga[p - 40];
        } else if (p == 49) {
            cons_bg = VGA_BLACK;
        } else if (p >= 90 && p <= 97) {
            cons_fg = (uint8_t)(ansi2vga[p - 90] | 8);
        } else if (p >= 100 && p <= 107) {
            cons_bg = (uint8_t)(ansi2vga[p - 100] | 8);
        }
    }
}

static void cursor_show(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0E); /* cursor start scanline 14, enable (bit 5 clear) */
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F); /* cursor end scanline 15 — thin underline */
}

static void crtc_word(uint8_t idx_hi, uint8_t idx_lo, uint16_t value) {
    outb(0x3D4, idx_hi);
    outb(0x3D5, (uint8_t)(value >> 8));
    outb(0x3D4, idx_lo);
    outb(0x3D5, (uint8_t)(value & 0xFF));
}

static void cons_cell(int x, int y, char ch) {
    if (cons_on_fb) {
        fb_cons_cell(x, y, ch, cons_fg, cons_bg);
        return;
    }
    VGA_MEM[y * VGA_WIDTH + x] = cell(ch, (vga_color_t)cons_fg, (vga_color_t)cons_bg);
}

static void cons_clear_row(int y) {
    if (cons_on_fb) {
        for (int i = 0; i < cons_cols; i++) {
            fb_cons_cell(i, y, ' ', VGA_LIGHT_GREY, VGA_BLACK);
        }
        return;
    }
    int row = y * VGA_WIDTH;
    for (int i = 0; i < VGA_WIDTH; i++) {
        VGA_MEM[row + i] = cell(' ', VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* Advance the pen one row, wrapping to the top past the bottom. The
 * entered row is cleared, and so is the one below it — a moving blank
 * separator that marks where the newest line sits. */
static void cons_next_row(void) {
    cons_y = (cons_y + 1) % cons_rows;
    cons_clear_row(cons_y);
    cons_clear_row((cons_y + 1) % cons_rows);
}

void vga_console_enable(void) {
    cons_on_fb = fb_text_console_mode() ? 1 : 0;
    if (cons_on_fb) {
        cons_cols = fb_cons_cols();
        cons_rows = fb_cons_rows();
        fb_clear(0);
    } else {
        cons_cols = VGA_WIDTH;
        cons_rows = VGA_HEIGHT;
        vga_clear(VGA_BLACK);
        cursor_show();
    }
    cons_x = 0;
    cons_y = 0;
    cons_active = 1;
    vga_console_sync();
}

int vga_console_active(void) {
    return cons_active;
}

void vga_console_putc(char c) {
    if (!cons_active) {
        return;
    }
    /* ANSI escape handling (SGR colour only; everything else is swallowed). */
    if (cons_esc == ESC_SEEN) {
        if (c == '[') {
            cons_esc = ESC_CSI;
            cons_nparam = 0;
            cons_curparam = 0;
        } else {
            cons_esc = ESC_NONE; /* not a CSI — ignore the escape */
        }
        return;
    }
    if (cons_esc == ESC_CSI) {
        if (c >= '0' && c <= '9') {
            cons_curparam = cons_curparam * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (cons_nparam < CONS_MAXPARAM) {
                cons_param[cons_nparam++] = cons_curparam;
            }
            cons_curparam = 0;
            return;
        }
        if (c == 'm') { /* SGR — the only sequence we act on */
            if (cons_nparam < CONS_MAXPARAM) {
                cons_param[cons_nparam++] = cons_curparam;
            }
            cons_apply_sgr();
        }
        cons_esc = ESC_NONE; /* any letter terminates the CSI */
        return;
    }
    if (c == 0x1b) {
        cons_esc = ESC_SEEN;
        return;
    }
    switch (c) {
    case '\r':
        cons_x = 0;
        break;
    case '\n':
        cons_x = 0;
        cons_next_row();
        break;
    case '\b':
        if (cons_x > 0) {
            cons_x--;
            cons_cell(cons_x, cons_y, ' ');
        }
        break;
    case '\t':
        do {
            cons_cell(cons_x, cons_y, ' ');
            cons_x++;
        } while ((cons_x & 3) && cons_x < cons_cols);
        break;
    default:
        if ((uint8_t)c < 32 || (uint8_t)c > 126) {
            return; /* other control bytes: not a screen concern */
        }
        cons_cell(cons_x, cons_y, c);
        cons_x++;
        break;
    }
    if (cons_x >= cons_cols) {
        cons_x = 0;
        cons_next_row();
    }
    /* NOTE: the hardware cursor is NOT moved here. Each move is 4 port
     * writes, and under TCG every port write is a trap — per-character
     * that slowed console output enough to break timing-sensitive CI
     * gates. Callers batch it: vga_console_sync() once per write. */
}

void vga_console_sync(void) {
    if (!cons_active) {
        return;
    }
    if (cons_on_fb) {
        fb_cons_cursor(cons_x, cons_y); /* one cell repainted, no port I/O */
        return;
    }
    crtc_word(0x0E, 0x0F, (uint16_t)(cons_y * VGA_WIDTH + cons_x));
}

/* Whole-string write under an IRQ guard: boot_log tees from both thread
 * and interrupt context, and an unguarded interleave tears lines on
 * screen (the shared pen position advances mid-line). console_write
 * holds its own guard already — nesting is safe (flags restore). */
void vga_console_puts(const char *s) {
    if (!s) {
        return;
    }
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    while (*s) {
        vga_console_putc(*s++);
    }
    vga_console_sync();
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory");
}

static void panic_row(int y, const char *s) {
    int on_fb = fb_text_console_mode();
    int width = on_fb ? fb_cons_cols() : VGA_WIDTH;
    int len = str_len(s);
    int start = (width - len) / 2;
    for (int x = 0; x < width; x++) {
        char ch = ' ';
        if (x >= start && x - start < len && start >= 0) {
            ch = s[x - start];
        }
        if (on_fb) {
            fb_cons_cell(x, y, ch, VGA_WHITE, VGA_RED);
        } else {
            VGA_MEM[y * VGA_WIDTH + x] = cell(ch, VGA_WHITE, VGA_RED);
        }
    }
}

void vga_panic_banner(const char *msg) {
    if (fb_available() && !fb_text_console_mode()) {
        return; /* graphics mode (live field view): 0xB8000 is not the display */
    }
    panic_row(0, "*** KERNEL PANIC ***");
    panic_row(1, msg ? msg : "(no message)");
    panic_row(2, "system halted");
}
