/**
 * QuantumOS Linear Framebuffer Graphics Implementation
 *
 * Integer-only (compiles under -mno-sse). Active only when a bootloader
 * supplies a linear 32-bpp framebuffer; the QEMU -kernel path provides
 * none, so CI and the qBraid watch window keep the VGA text splash.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <kernel/fb.h>

/* Boot paging tables + a spare PD (kernel/src/boot.S) and CR3 helpers
 * (kernel/src/interrupts.S), used to identity-map a high framebuffer. */
extern uint64_t boot_pdpt[];
extern uint64_t fb_map_pd[];
extern uint64_t get_cr3(void);
extern void set_cr3(uint64_t cr3);

/* Identity-map the 1 GB region containing [base, base+len) with 2 MB
 * supervisor pages, via the spare boot PD. The boot page tables only
 * cover the low 1 GB; GRUB's linear framebuffer sits far above that. */
static void fb_identity_map(uint64_t base, uint64_t len) {
    (void)len; /* a 1024x768x32 FB (~3 MB) fits within one 1 GB slot */
    uint64_t region = base & ~0x3FFFFFFFULL; /* 1 GB aligned */
    for (int i = 0; i < 512; i++) {
        fb_map_pd[i] = (region + (uint64_t)i * 0x200000ULL) | 0x83; /* P|RW|PS */
    }
    boot_pdpt[region >> 30] = ((uint64_t)fb_map_pd) | 0x3; /* P|RW, supervisor */
    set_cr3(get_cr3());                                    /* flush TLB */
}

static volatile uint8_t *fb_addr = 0;
static uint32_t fb_pitch = 0;
static int fb_w = 0, fb_h = 0;
static int fb_bpp = 0;
static int fb_ready = 0;
static int fb_text_mode = 0; /* #264: framebuffer given to the console image */
static int splash_frame = 0;

/* This image's Multiboot1 header (kernel/src/boot.S): [0] magic, [1] flags. */
extern const uint32_t mb1_header[];

#define GLYPH_SCALE 3 /* 8x8 font drawn as 24x24 */

/* period-32 integer sine, amplitude +/-100 */
static const int8_t sine32[32] = {0,   20,  38,   56,  71,  83,  92,  98,  100, 98,  92,
                                  83,  71,  56,   38,  20,  0,   -20, -38, -56, -71, -83,
                                  -92, -98, -100, -98, -92, -83, -71, -56, -38, -20};

static void frame_delay(void) {
    for (volatile uint32_t i = 0; i < 800000; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

/* --- 8x8 font: every printable ASCII byte (32..126), rows MSB-left. The
 * screen console (#264) draws arbitrary shell text here, so a partial
 * splash-only font is no longer enough. Derived from the public-domain IBM
 * VGA 8x8 font (Marcel Sondaar; Daniel Hepper's font8x8_basic), bit-reversed
 * to MSB-left. Bytes outside the range render blank. Coverage is checked by
 * scripts/check_fb_font.py. --- */
#define FONT_FIRST 32
#define FONT_COUNT 95
static const uint8_t font_ascii[FONT_COUNT][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* ' ' */
    {0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00}, /* '!' */
    {0x6C, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '"' */
    {0x6C, 0x6C, 0xFE, 0x6C, 0xFE, 0x6C, 0x6C, 0x00}, /* '#' */
    {0x30, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x30, 0x00}, /* '$' */
    {0x00, 0xC6, 0xCC, 0x18, 0x30, 0x66, 0xC6, 0x00}, /* '%' */
    {0x38, 0x6C, 0x38, 0x76, 0xDC, 0xCC, 0x76, 0x00}, /* '&' */
    {0x60, 0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '\'' */
    {0x18, 0x30, 0x60, 0x60, 0x60, 0x30, 0x18, 0x00}, /* '(' */
    {0x60, 0x30, 0x18, 0x18, 0x18, 0x30, 0x60, 0x00}, /* ')' */
    {0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00}, /* '*' */
    {0x00, 0x30, 0x30, 0xFC, 0x30, 0x30, 0x00, 0x00}, /* '+' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x60}, /* ',' */
    {0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x00}, /* '-' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00}, /* '.' */
    {0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00}, /* '/' */
    {0x7C, 0xC6, 0xCE, 0xDE, 0xF6, 0xE6, 0x7C, 0x00}, /* '0' */
    {0x30, 0x70, 0x30, 0x30, 0x30, 0x30, 0xFC, 0x00}, /* '1' */
    {0x78, 0xCC, 0x0C, 0x38, 0x60, 0xCC, 0xFC, 0x00}, /* '2' */
    {0x78, 0xCC, 0x0C, 0x38, 0x0C, 0xCC, 0x78, 0x00}, /* '3' */
    {0x1C, 0x3C, 0x6C, 0xCC, 0xFE, 0x0C, 0x1E, 0x00}, /* '4' */
    {0xFC, 0xC0, 0xF8, 0x0C, 0x0C, 0xCC, 0x78, 0x00}, /* '5' */
    {0x38, 0x60, 0xC0, 0xF8, 0xCC, 0xCC, 0x78, 0x00}, /* '6' */
    {0xFC, 0xCC, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}, /* '7' */
    {0x78, 0xCC, 0xCC, 0x78, 0xCC, 0xCC, 0x78, 0x00}, /* '8' */
    {0x78, 0xCC, 0xCC, 0x7C, 0x0C, 0x18, 0x70, 0x00}, /* '9' */
    {0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00}, /* ':' */
    {0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x60}, /* ';' */
    {0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00}, /* '<' */
    {0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC, 0x00, 0x00}, /* '=' */
    {0x60, 0x30, 0x18, 0x0C, 0x18, 0x30, 0x60, 0x00}, /* '>' */
    {0x78, 0xCC, 0x0C, 0x18, 0x30, 0x00, 0x30, 0x00}, /* '?' */
    {0x7C, 0xC6, 0xDE, 0xDE, 0xDE, 0xC0, 0x78, 0x00}, /* '@' */
    {0x30, 0x78, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0x00}, /* 'A' */
    {0xFC, 0x66, 0x66, 0x7C, 0x66, 0x66, 0xFC, 0x00}, /* 'B' */
    {0x3C, 0x66, 0xC0, 0xC0, 0xC0, 0x66, 0x3C, 0x00}, /* 'C' */
    {0xF8, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0xF8, 0x00}, /* 'D' */
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x62, 0xFE, 0x00}, /* 'E' */
    {0xFE, 0x62, 0x68, 0x78, 0x68, 0x60, 0xF0, 0x00}, /* 'F' */
    {0x3C, 0x66, 0xC0, 0xC0, 0xCE, 0x66, 0x3E, 0x00}, /* 'G' */
    {0xCC, 0xCC, 0xCC, 0xFC, 0xCC, 0xCC, 0xCC, 0x00}, /* 'H' */
    {0x78, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, /* 'I' */
    {0x1E, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78, 0x00}, /* 'J' */
    {0xE6, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0xE6, 0x00}, /* 'K' */
    {0xF0, 0x60, 0x60, 0x60, 0x62, 0x66, 0xFE, 0x00}, /* 'L' */
    {0xC6, 0xEE, 0xFE, 0xFE, 0xD6, 0xC6, 0xC6, 0x00}, /* 'M' */
    {0xC6, 0xE6, 0xF6, 0xDE, 0xCE, 0xC6, 0xC6, 0x00}, /* 'N' */
    {0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0x6C, 0x38, 0x00}, /* 'O' */
    {0xFC, 0x66, 0x66, 0x7C, 0x60, 0x60, 0xF0, 0x00}, /* 'P' */
    {0x78, 0xCC, 0xCC, 0xCC, 0xDC, 0x78, 0x1C, 0x00}, /* 'Q' */
    {0xFC, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0xE6, 0x00}, /* 'R' */
    {0x78, 0xCC, 0xE0, 0x70, 0x1C, 0xCC, 0x78, 0x00}, /* 'S' */
    {0xFC, 0xB4, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, /* 'T' */
    {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xFC, 0x00}, /* 'U' */
    {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00}, /* 'V' */
    {0xC6, 0xC6, 0xC6, 0xD6, 0xFE, 0xEE, 0xC6, 0x00}, /* 'W' */
    {0xC6, 0xC6, 0x6C, 0x38, 0x38, 0x6C, 0xC6, 0x00}, /* 'X' */
    {0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x30, 0x78, 0x00}, /* 'Y' */
    {0xFE, 0xC6, 0x8C, 0x18, 0x32, 0x66, 0xFE, 0x00}, /* 'Z' */
    {0x78, 0x60, 0x60, 0x60, 0x60, 0x60, 0x78, 0x00}, /* '[' */
    {0xC0, 0x60, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00}, /* '\\' */
    {0x78, 0x18, 0x18, 0x18, 0x18, 0x18, 0x78, 0x00}, /* ']' */
    {0x10, 0x38, 0x6C, 0xC6, 0x00, 0x00, 0x00, 0x00}, /* '^' */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF}, /* '_' */
    {0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '`' */
    {0x00, 0x00, 0x78, 0x0C, 0x7C, 0xCC, 0x76, 0x00}, /* 'a' */
    {0xE0, 0x60, 0x60, 0x7C, 0x66, 0x66, 0xDC, 0x00}, /* 'b' */
    {0x00, 0x00, 0x78, 0xCC, 0xC0, 0xCC, 0x78, 0x00}, /* 'c' */
    {0x1C, 0x0C, 0x0C, 0x7C, 0xCC, 0xCC, 0x76, 0x00}, /* 'd' */
    {0x00, 0x00, 0x78, 0xCC, 0xFC, 0xC0, 0x78, 0x00}, /* 'e' */
    {0x38, 0x6C, 0x60, 0xF0, 0x60, 0x60, 0xF0, 0x00}, /* 'f' */
    {0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8}, /* 'g' */
    {0xE0, 0x60, 0x6C, 0x76, 0x66, 0x66, 0xE6, 0x00}, /* 'h' */
    {0x30, 0x00, 0x70, 0x30, 0x30, 0x30, 0x78, 0x00}, /* 'i' */
    {0x0C, 0x00, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78}, /* 'j' */
    {0xE0, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0xE6, 0x00}, /* 'k' */
    {0x70, 0x30, 0x30, 0x30, 0x30, 0x30, 0x78, 0x00}, /* 'l' */
    {0x00, 0x00, 0xCC, 0xFE, 0xFE, 0xD6, 0xC6, 0x00}, /* 'm' */
    {0x00, 0x00, 0xF8, 0xCC, 0xCC, 0xCC, 0xCC, 0x00}, /* 'n' */
    {0x00, 0x00, 0x78, 0xCC, 0xCC, 0xCC, 0x78, 0x00}, /* 'o' */
    {0x00, 0x00, 0xDC, 0x66, 0x66, 0x7C, 0x60, 0xF0}, /* 'p' */
    {0x00, 0x00, 0x76, 0xCC, 0xCC, 0x7C, 0x0C, 0x1E}, /* 'q' */
    {0x00, 0x00, 0xDC, 0x76, 0x66, 0x60, 0xF0, 0x00}, /* 'r' */
    {0x00, 0x00, 0x7C, 0xC0, 0x78, 0x0C, 0xF8, 0x00}, /* 's' */
    {0x10, 0x30, 0x7C, 0x30, 0x30, 0x34, 0x18, 0x00}, /* 't' */
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0xCC, 0x76, 0x00}, /* 'u' */
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x78, 0x30, 0x00}, /* 'v' */
    {0x00, 0x00, 0xC6, 0xD6, 0xFE, 0xFE, 0x6C, 0x00}, /* 'w' */
    {0x00, 0x00, 0xC6, 0x6C, 0x38, 0x6C, 0xC6, 0x00}, /* 'x' */
    {0x00, 0x00, 0xCC, 0xCC, 0xCC, 0x7C, 0x0C, 0xF8}, /* 'y' */
    {0x00, 0x00, 0xFC, 0x98, 0x30, 0x64, 0xFC, 0x00}, /* 'z' */
    {0x1C, 0x30, 0x30, 0xE0, 0x30, 0x30, 0x1C, 0x00}, /* '{' */
    {0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00}, /* '|' */
    {0xE0, 0x30, 0x30, 0x1C, 0x30, 0x30, 0xE0, 0x00}, /* '}' */
    {0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* '~' */
};

static const uint8_t *glyph_for(char c) {
    uint8_t u = (uint8_t)c;
    if (u < FONT_FIRST || u >= FONT_FIRST + FONT_COUNT) {
        return font_ascii[0]; /* blank */
    }
    return font_ascii[u - FONT_FIRST];
}

/* --- multiboot framebuffer info --- */
bool fb_init_from_multiboot(uint32_t mb_info_addr) {
    uint32_t *info = (uint32_t *)(uintptr_t)mb_info_addr;
    if (!(info[0] & (1u << 12))) {
        return false; /* no framebuffer field */
    }
    uint64_t addr = *(uint64_t *)(uintptr_t)(mb_info_addr + 88);
    uint32_t pitch = info[24];  /* offset 96 */
    uint32_t width = info[25];  /* offset 100 */
    uint32_t height = info[26]; /* offset 104 */
    uint8_t bpp = *(uint8_t *)(uintptr_t)(mb_info_addr + 108);
    uint8_t type = *(uint8_t *)(uintptr_t)(mb_info_addr + 109);

    /* Only direct-RGB (type 1) 32-bpp is handled; else fall back */
    if (type != 1 || bpp != 32 || addr == 0 || width == 0 || height == 0) {
        return false;
    }
    /* The framebuffer usually lives well above the identity-mapped low
     * 1 GB, so map it before any access. */
    if (addr >= 0x40000000ULL) {
        fb_identity_map(addr, (uint64_t)pitch * height);
    }

    fb_addr = (volatile uint8_t *)(uintptr_t)addr;
    fb_pitch = pitch;
    fb_w = (int)width;
    fb_h = (int)height;
    fb_bpp = bpp;
    fb_ready = 1;
    /* #264: did THIS image ask for a video mode (MB1 header flag bit 2)? The
     * console image does not; a framebuffer arriving anyway means the loader
     * had no text mode to leave us in (UEFI/GOP), so the screen console must
     * render here — 0xB8000 is plain RAM on such a machine. */
    fb_text_mode = (mb1_header[1] & 0x4u) ? 0 : 1;
    return true;
}

bool fb_available(void) {
    return fb_ready;
}

bool fb_text_console_mode(void) {
    return fb_ready && fb_text_mode;
}

/* --- framebuffer text-console cell backend (#264) ------------------------ *
 * vga.c keeps the whole line discipline (wrap console, SGR colours, pen);
 * these are the cells it draws into when the display is a GOP framebuffer.
 * A cell is the 8x8 glyph with every row doubled (8x16, the text-mode
 * aspect), scaled up once more on panels 1600 px and wider so a 1080p
 * laptop is still readable. The cell grid is capped at FBCONS_MAX_COLS x
 * FBCONS_MAX_ROWS so the shadow store (needed to lift the cursor without
 * disturbing the glyph under it) stays a bounded static array. Pixels are
 * written straight through the row pointer — no per-pixel bounds call —
 * because the console writes with interrupts off and a full-row clear is
 * 128 cells. */
#define FBCONS_MAX_COLS 160
#define FBCONS_MAX_ROWS 64
static int fbcons_scale = 1;
static int fbcons_cols, fbcons_rows;
static char fbcons_shadow_ch[FBCONS_MAX_ROWS][FBCONS_MAX_COLS];
static uint8_t fbcons_shadow_attr[FBCONS_MAX_ROWS][FBCONS_MAX_COLS];
static int fbcons_cur_x = -1, fbcons_cur_y = -1;

/* Standard VGA 16-colour palette, as 0x00RRGGBB */
static const uint32_t vga_palette[16] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                         0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                         0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

static void fbcons_geometry(void) {
    if (fbcons_cols) {
        return;
    }
    fbcons_scale = fb_w >= 1600 ? 2 : 1;
    fbcons_cols = fb_w / (8 * fbcons_scale);
    fbcons_rows = fb_h / (16 * fbcons_scale);
    if (fbcons_cols > FBCONS_MAX_COLS)
        fbcons_cols = FBCONS_MAX_COLS;
    if (fbcons_rows > FBCONS_MAX_ROWS)
        fbcons_rows = FBCONS_MAX_ROWS;
}

int fb_cons_cols(void) {
    if (!fb_ready)
        return 0;
    fbcons_geometry();
    return fbcons_cols;
}

int fb_cons_rows(void) {
    if (!fb_ready)
        return 0;
    fbcons_geometry();
    return fbcons_rows;
}

static void fbcons_paint(int cx, int cy, char ch, uint8_t attr, int cursor) {
    const uint8_t *g = glyph_for(ch);
    uint32_t fg = vga_palette[attr & 15], bg = vga_palette[(attr >> 4) & 15];
    int s = fbcons_scale;
    uint32_t px = (uint32_t)cx * 8u * (uint32_t)s;
    uint32_t py = (uint32_t)cy * 16u * (uint32_t)s;
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        if (cursor && row == 7) {
            bits = 0xFF; /* underline cursor on the glyph's last row */
        }
        for (int sub = 0; sub < 2 * s; sub++) {
            volatile uint32_t *line =
                (volatile uint32_t *)(fb_addr + (py + (uint32_t)(row * 2 * s + sub)) * fb_pitch +
                                      px * 4u);
            for (int col = 0; col < 8; col++) {
                uint32_t c = (bits & (0x80 >> col)) ? fg : bg;
                for (int k = 0; k < s; k++) {
                    line[col * s + k] = c;
                }
            }
        }
    }
}

void fb_cons_cell(int cx, int cy, char ch, uint8_t fg, uint8_t bg) {
    if (!fb_ready)
        return;
    fbcons_geometry();
    if (cx < 0 || cy < 0 || cx >= fbcons_cols || cy >= fbcons_rows)
        return;
    uint8_t attr = (uint8_t)((bg << 4) | (fg & 15));
    fbcons_shadow_ch[cy][cx] = ch;
    fbcons_shadow_attr[cy][cx] = attr;
    fbcons_paint(cx, cy, ch, attr, cx == fbcons_cur_x && cy == fbcons_cur_y);
}

void fb_cons_cursor(int cx, int cy) {
    if (!fb_ready)
        return;
    fbcons_geometry();
    if (fbcons_cur_x == cx && fbcons_cur_y == cy)
        return;
    if (fbcons_cur_x >= 0 && fbcons_cur_x < fbcons_cols && fbcons_cur_y >= 0 &&
        fbcons_cur_y < fbcons_rows) {
        fbcons_paint(fbcons_cur_x, fbcons_cur_y, fbcons_shadow_ch[fbcons_cur_y][fbcons_cur_x],
                     fbcons_shadow_attr[fbcons_cur_y][fbcons_cur_x], 0);
    }
    fbcons_cur_x = cx;
    fbcons_cur_y = cy;
    if (cx >= 0 && cx < fbcons_cols && cy >= 0 && cy < fbcons_rows) {
        fbcons_paint(cx, cy, fbcons_shadow_ch[cy][cx], fbcons_shadow_attr[cy][cx], 1);
    }
}
int fb_width(void) {
    return fb_w;
}
int fb_height(void) {
    return fb_h;
}

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void fb_put_pixel(int x, int y, uint32_t color) {
    if (!fb_ready || x < 0 || y < 0 || x >= fb_w || y >= fb_h) {
        return;
    }
    *(volatile uint32_t *)(fb_addr + (uint32_t)y * fb_pitch + (uint32_t)x * 4) = color;
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            fb_put_pixel(x + i, y + j, color);
        }
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, fb_w, fb_h, color);
}

void fb_draw_char(int x, int y, char c, uint32_t fg) {
    const uint8_t *g = glyph_for(c);
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (g[row] & (0x80 >> col)) {
                fb_fill_rect(x + col * GLYPH_SCALE, y + row * GLYPH_SCALE, GLYPH_SCALE, GLYPH_SCALE,
                             fg);
            }
        }
    }
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n])
        n++;
    return n;
}

void fb_draw_string(int x, int y, const char *s, uint32_t fg) {
    int cw = 8 * GLYPH_SCALE + GLYPH_SCALE; /* glyph + 1-scale gap */
    for (int i = 0; s[i]; i++) {
        fb_draw_char(x + i * cw, y, s[i], fg);
    }
}

void fb_draw_string_center(int y, const char *s, uint32_t fg) {
    int cw = 8 * GLYPH_SCALE + GLYPH_SCALE;
    int x = (fb_w - str_len(s) * cw) / 2;
    fb_draw_string(x, y, s, fg);
}

/* Centered text on a dark plate, so it stays legible over the bright
 * interference field. */
static void fb_banner(int y, const char *s, uint32_t fg) {
    int cw = 8 * GLYPH_SCALE + GLYPH_SCALE;
    int tw = str_len(s) * cw;
    int x = (fb_w - tw) / 2;
    fb_fill_rect(x - 16, y - 8, tw + 32, 8 * GLYPH_SCALE + 16, fb_rgb(4, 8, 16));
    fb_draw_string(x, y, s, fg);
}

/* --- boot splash --- */

static int isqrt(int v) {
    if (v <= 0)
        return 0;
    int x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* Two-source radial interference across the whole screen, drawn at 4x4
 * block resolution so a frame is cheap enough for the busy-loop pacing.
 * (FB path runs only under an ISO/real-HW boot, never the CI -kernel
 * path, so this does not affect the smoke-test budget.) */
static void draw_interference(int frame) {
    int s1x = fb_w * 3 / 10, s1y = fb_h / 2;
    int s2x = fb_w * 7 / 10, s2y = fb_h / 2;
    const int blk = 4;

    for (int y = 0; y < fb_h; y += blk) {
        for (int x = 0; x < fb_w; x += blk) {
            int d1 = isqrt((x - s1x) * (x - s1x) + (y - s1y) * (y - s1y));
            int d2 = isqrt((x - s2x) * (x - s2x) + (y - s2y) * (y - s2y));
            int a = sine32[((d1 / 12) - frame) & 31];
            int b = sine32[((d2 / 12) - frame) & 31];
            int sum = (a + b) / 2;          /* -100..100 */
            int mag = sum < 0 ? -sum : sum; /* 0..100 */

            /* deep blue trough -> bright cyan/white crest */
            uint8_t bl = (uint8_t)(40 + mag * 2); /* up to ~240 */
            uint8_t gr = (uint8_t)(mag * 2);
            uint8_t rd = (uint8_t)(mag > 70 ? (mag - 70) * 6 : 0);
            fb_fill_rect(x, y, blk, blk, fb_rgb(rd, gr, bl));
        }
    }
}

static void draw_progress(int percent) {
    int bw = fb_w * 6 / 10;
    int bx = (fb_w - bw) / 2;
    int by = fb_h * 3 / 4;
    int bh = 28;
    /* frame */
    fb_fill_rect(bx - 3, by - 3, bw + 6, bh + 6, fb_rgb(20, 40, 60));
    fb_fill_rect(bx, by, bw, bh, fb_rgb(8, 12, 20));
    int filled = bw * percent / 100;
    fb_fill_rect(bx, by, filled, bh, fb_rgb(40, 230, 120));

    /* percent readout, centred under the bar */
    char pct[5];
    int n = 0;
    if (percent >= 100) {
        pct[n++] = '1';
        pct[n++] = '0';
        pct[n++] = '0';
    } else if (percent >= 10) {
        pct[n++] = (char)('0' + percent / 10);
        pct[n++] = (char)('0' + percent % 10);
    } else {
        pct[n++] = (char)('0' + percent);
    }
    pct[n++] = '%';
    pct[n] = 0;
    fb_draw_string_center(by + bh + 16, pct, fb_rgb(230, 230, 120));
}

void fb_boot_splash(void) {
    fb_clear(fb_rgb(4, 6, 12));
    fb_banner(fb_h / 6, "QUANTUM OS", fb_rgb(120, 230, 255));
}

void fb_boot_stage(const char *label, int percent) {
    (void)label; /* detailed labels go to the serial console */
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    if (fb_text_mode) {
        /* Console boot on a GOP framebuffer (#264): the screen is about to
         * become the text console, so keep the splash to the banner and the
         * bar — the whole-screen interference frames cost ~200 ms each under
         * emulation and would only be cleared a moment later. */
        draw_progress(percent);
        return;
    }
    draw_interference(splash_frame++);
    fb_banner(fb_h / 6, "QUANTUM OS", fb_rgb(120, 230, 255));
    draw_progress(percent);
    frame_delay();
}

void fb_boot_ready(void) {
    if (fb_text_mode) {
        draw_progress(100);
        fb_banner(fb_h / 6 + 44, "READY", fb_rgb(230, 230, 120));
        return;
    }
    for (int f = 0; f < 6; f++) {
        draw_interference(splash_frame++);
        fb_banner(fb_h / 6, "QUANTUM OS", fb_rgb(120, 230, 255));
        draw_progress(100);
        frame_delay();
    }
    fb_banner(fb_h / 6 + 44, "READY", fb_rgb(230, 230, 120));
}

/* --- live memory-field view (post-boot) ---------------------------------- *
 * Replaces the canned splash once the system is up: the `n` signed snapshot
 * bytes are one cos-projection per field oscillator (published by ghostd over
 * SYS_FIELD_SNAPSHOT), laid out as the nearest square grid and drawn as filled
 * blocks. Deep blue = phase trough (−), bright cyan/white = crest (+); the
 * picture ripples as REMEMBER/RECALL relax the field. Integer-only, and only
 * reached when a real framebuffer is present. */
void fb_render_field(const int8_t *snap, int n) {
    if (!fb_ready || n <= 0) {
        return;
    }
    /* square-ish grid side = ceil(sqrt(n)); for ghostd's N=256 this is 16 */
    int side = isqrt(n);
    while (side * side < n)
        side++;
    if (side <= 0)
        return;

    /* Field occupies a centred square, leaving room for the banners. */
    int margin = fb_h / 8;
    int area = fb_h - 2 * margin;
    if (area > fb_w)
        area = fb_w;
    int cell = area / side;
    if (cell < 1)
        cell = 1;
    int grid = cell * side;
    int ox = (fb_w - grid) / 2;
    int oy = (fb_h - grid) / 2;

    for (int idx = 0; idx < side * side; idx++) {
        int v = (idx < n) ? snap[idx] : 0;             /* −128..127 */
        int mag = v < 0 ? -v : v;                      /* 0..128 */
        uint8_t bl = (uint8_t)(30 + mag);              /* trough floor -> bright */
        uint8_t gr = (uint8_t)(v > 0 ? mag : mag / 3); /* crests greener */
        uint8_t rd = (uint8_t)(mag > 96 ? (mag - 96) * 4 : 0);
        int gx = idx % side, gy = idx / side;
        fb_fill_rect(ox + gx * cell, oy + gy * cell, cell - 1, cell - 1, fb_rgb(rd, gr, bl));
    }

    fb_banner(margin / 2, "QUANTUM OS", fb_rgb(120, 230, 255));
    fb_banner(fb_h - margin, "GHOST FIELD LIVE", fb_rgb(230, 230, 120));
}
