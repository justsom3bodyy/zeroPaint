/* zp_ui.h - immediate-mode software framebuffer ui and app actions */
#ifndef ZP_UI_H
#define ZP_UI_H

#include "zp_types.h"

/* === layout heights === */
#define ZP_MENUBAR_H   32
#define ZP_RIBBON_H    84
#define ZP_TOP_H       (ZP_MENUBAR_H + ZP_RIBBON_H)
#define ZP_TEXTBAR_H   30
#define ZP_STATUSBAR_H 28
#define ZP_RULER_SIZE  18

/* === rendering and hit testing === */
int  zp_ui_top_h(const ZPApp *app);
int  zp_eraser_size(int stroke_size);
void zp_ui_render(ZPApp *app, ZPImage *fb, uint32_t anim_tick);
bool zp_ui_screen_to_canvas(const ZPApp *app, int sx, int sy, int *cx, int *cy);
int  zp_ui_hit_resize_handle(const ZPApp *app, int sx, int sy);
int  zp_ui_hit_sel_handle(const ZPApp *app, int sx, int sy);
bool zp_ui_handle_click(ZPApp *app, int sx, int sy, int button);

/* === shared commands === */
void zp_app_commit_selection(ZPApp *app);
void zp_app_commit_text(ZPApp *app);
void zp_app_new(ZPApp *app);
void zp_app_open(ZPApp *app);
void zp_app_save(ZPApp *app, bool force_dialog);
void zp_app_copy(ZPApp *app);
void zp_app_cut(ZPApp *app);
void zp_app_paste(ZPApp *app);
void zp_app_crop_to_selection(ZPApp *app);
void zp_app_rotate(ZPApp *app, bool clockwise);
void zp_app_flip(ZPApp *app, bool horizontal);
void zp_app_zoom_step(ZPApp *app, int dir);
void zp_app_zoom_at(ZPApp *app, int dir, int sx, int sy);

#endif
