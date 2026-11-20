/* zp_ttf.h - runtime system font loader via dlopen(libfreetype) with built-in fallback */
#ifndef ZP_TTF_H
#define ZP_TTF_H

#include "zp_types.h"

#define ZP_MAX_SYS_FONTS 24

/* === lifecycle and font list === */
void zp_ttf_init(void);
void zp_ttf_shutdown(void);
int  zp_ttf_font_count(void);
const char *zp_ttf_font_name(int idx);

/* === utf-8 text rendering and measurement === */
void zp_ttf_measure(const char *utf8, int font_idx, int px_size, bool bold, int *out_w, int *out_h);
void zp_ttf_draw(ZPImage *img, int x, int y, const char *utf8, int font_idx, int px_size, bool bold, uint32_t fg);
void zp_ttf_draw_clipped(ZPImage *img, int x, int y, const char *utf8, int font_idx, int px_size, bool bold, uint32_t fg,
                         int cx0, int cy0, int cx1, int cy1);

#endif
