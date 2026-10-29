/**
 * QuantumOS Linear Framebuffer Graphics
 *
 * Pixel graphics for the boot splash when a bootloader provides a
 * linear 32-bpp framebuffer (GRUB gfxpayload / real hardware VBE). The
 * QEMU `-kernel` multiboot loader does NOT provide one, so that path —
 * used by CI and the qBraid `/qos` watch window — stays in VGA text
 * mode untouched. fb_available() reports which mode is active.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef FB_H
#define FB_H

#include <kernel/types.h>

/* Try to initialise from the multiboot info block. Returns true iff a
 * usable linear 32-bpp RGB framebuffer was provided. */
bool fb_init_from_multiboot(uint32_t mb_info_addr);

/* Is the linear framebuffer active (vs. VGA text fallback)? */
bool fb_available(void);

/* #264: true when a framebuffer is active but THIS image never asked for a
 * video mode (the console image, boot.S MB1_TEXT_ONLY). That only happens
 * when the loader had no text mode to leave us in — UEFI firmware exposes a
 * GOP framebuffer and nothing at 0xB8000 — so the screen console must render
 * on the framebuffer. The video-requesting image keeps the live field view. */
bool fb_text_console_mode(void);

/* Framebuffer cell backend for the screen console (vga.c owns the line
 * discipline; these are the cell store it draws into). Cells are an 8x8
 * glyph with doubled rows (8x16, the text-mode aspect), scaled up once more
 * on wide panels; geometry is capped so the shadow store stays bounded.
 * fg/bg are VGA palette indices. fb_cons_cursor redraws the previous cursor
 * cell from the shadow and underlines the new one. */
int fb_cons_cols(void);
int fb_cons_rows(void);
void fb_cons_cell(int cx, int cy, char ch, uint8_t fg, uint8_t bg);
void fb_cons_cursor(int cx, int cy);

/* Packed 0x00RRGGBB colour */
uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b);

void fb_clear(uint32_t color);
void fb_put_pixel(int x, int y, uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_draw_char(int x, int y, char c, uint32_t fg);
void fb_draw_string(int x, int y, const char *s, uint32_t fg);
void fb_draw_string_center(int y, const char *s, uint32_t fg);

int fb_width(void);
int fb_height(void);

/* Graphical boot splash (mirrors the VGA text splash stages) */
void fb_boot_splash(void);
void fb_boot_stage(const char *label, int percent);
void fb_boot_ready(void);

/* Render a live memory-field snapshot: `snap` is `n` signed bytes (one
 * cos-projection per field oscillator, as published by ghostd over
 * SYS_FIELD_SNAPSHOT). Draws the field as a colour grid so the picture
 * ripples with REMEMBER/RECALL activity. Framebuffer path only — a no-op
 * when the VGA text fallback is active. */
void fb_render_field(const int8_t *snap, int n);

#endif /* FB_H */
