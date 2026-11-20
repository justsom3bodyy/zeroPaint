/* zp_raster.h - software pixel buffer, drawing ops, lasso and undo ring */
#ifndef ZP_RASTER_H
#define ZP_RASTER_H

#include "zp_types.h"

/* === image lifecycle === */
bool zp_img_init(ZPImage *img, int w, int h, uint32_t fill_color);
void zp_img_free(ZPImage *img);
bool zp_img_clone(const ZPImage *src, ZPImage *dst);
void zp_img_fill(ZPImage *img, uint32_t color);
uint32_t zp_img_get(const ZPImage *img, int x, int y);
void zp_img_put(ZPImage *img, int x, int y, uint32_t color);

/* === drawing primitives === */
void zp_draw_disk(ZPImage *img, int cx, int cy, int diameter, uint32_t color);
void zp_draw_calligraphy(ZPImage *img, int cx, int cy, int size, uint32_t color);
void zp_draw_line(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t color, ZPBrushVariant variant);
void zp_draw_airbrush(ZPImage *img, int cx, int cy, int radius, uint32_t color, uint32_t *rng_state);
void zp_draw_rect(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode);
void zp_draw_round_rect(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode);
void zp_draw_ellipse(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode);
void zp_draw_polygon(ZPImage *img, const ZPPoint *pts, int count, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode);
void zp_draw_bezier(ZPImage *img, ZPPoint p0, ZPPoint p1, ZPPoint p2, ZPPoint p3, int size, uint32_t color);
void zp_draw_shape(ZPImage *img, ZPTool tool, int x0, int y0, int x1, int y1, int size, uint32_t c1, uint32_t c2, ZPShapeMode mode);
void zp_draw_text(ZPImage *img, int x, int y, const char *text, int scale, uint32_t fg);
void zp_draw_text_font(ZPImage *img, int x, int y, const char *text, int font_kind, int scale, uint32_t fg);
int zp_text_width(const char *text, int scale);
int zp_text_width_font(const char *text, int font_kind, int scale);
void zp_flood_fill(ZPImage *img, int sx, int sy, uint32_t fill_color);

/* === transforms === */
bool zp_canvas_resize_borders(ZPImage *img, int new_w, int new_h, uint32_t bg_color);
bool zp_img_scale_nearest(const ZPImage *src, ZPImage *dst, int new_w, int new_h);
bool zp_img_skew(const ZPImage *src, ZPImage *dst, int deg_h, int deg_v, uint32_t bg_color);
bool zp_img_rotate90(const ZPImage *src, ZPImage *dst, bool clockwise);
void zp_img_flip_h(ZPImage *img);
void zp_img_flip_v(ZPImage *img);
void zp_img_invert(ZPImage *img);
void zp_img_grayscale(ZPImage *img);

/* === selection and undo === */
void zp_sel_clear(ZPSelection *sel);
bool zp_sel_extract_rect(ZPImage *canvas, int x0, int y0, int x1, int y1, uint32_t bg_color, ZPSelection *sel, bool transparent_bg);
bool zp_sel_extract_lasso(ZPImage *canvas, const ZPPoint *pts, int count, uint32_t bg_color, ZPSelection *sel, bool transparent_bg);
bool zp_sel_begin_transform(ZPSelection *sel, int handle);
void zp_sel_apply_transform(ZPSelection *sel, int new_x, int new_y, int new_w, int new_h);
void zp_sel_end_transform(ZPSelection *sel);
void zp_sel_blit(ZPImage *dst, const ZPSelection *sel);

void zp_history_init(ZPHistory *h, size_t max_mb);
void zp_history_free(ZPHistory *h);
void zp_history_push(ZPHistory *h, const ZPImage *img);
bool zp_history_undo(ZPHistory *h, ZPImage *canvas);
bool zp_history_redo(ZPHistory *h, ZPImage *canvas);

#endif
