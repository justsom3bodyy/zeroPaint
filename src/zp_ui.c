/* zp_ui.c - software framebuffer renderer for menus, ribbon, canvas and status bar */
#include "zp_ui.h"
#include "zp_raster.h"
#include "zp_ttf.h"
#include "zp_font.h"
#include "zp_icons.h"
#include "zp_logo.h"
#include "zp_codec.h"
#include "zp_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* === framebuffer draw helpers === */
static void fb_fill(ZPImage *fb, int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w > fb->w) ? fb->w : x + w;
    int y1 = (y + h > fb->h) ? fb->h : y + h;
    for (int py = y0; py < y1; ++py) {
        uint32_t *row = fb->pixels + (size_t)py * (size_t)fb->w;
        for (int px = x0; px < x1; ++px) row[px] = col;
    }
}

static void fb_blend(ZPImage *fb, int x, int y, uint32_t col, uint8_t a) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || a == 0) return;
    if (a == 255) { fb->pixels[(size_t)y * (size_t)fb->w + (size_t)x] = col; return; }
    uint32_t dst = fb->pixels[(size_t)y * (size_t)fb->w + (size_t)x];
    uint32_t inv = 255u - a;
    uint32_t r = (ZP_R(col) * a + ZP_R(dst) * inv + 127u) / 255u;
    uint32_t g = (ZP_G(col) * a + ZP_G(dst) * inv + 127u) / 255u;
    uint32_t b = (ZP_B(col) * a + ZP_B(dst) * inv + 127u) / 255u;
    fb->pixels[(size_t)y * (size_t)fb->w + (size_t)x] = ZP_RGB(r, g, b);
}

static void fb_rect_outline(ZPImage *fb, int x, int y, int w, int h, uint32_t col) {
    fb_fill(fb, x, y, w, 1, col);
    fb_fill(fb, x, y + h - 1, w, 1, col);
    fb_fill(fb, x, y, 1, h, col);
    fb_fill(fb, x + w - 1, y, 1, h, col);
}

/* subtle 2px rounded box matching win10 ribbon buttons */
static void fb_rbox(ZPImage *fb, int x, int y, int w, int h, uint32_t bg, uint32_t bd) {
    if (w < 4 || h < 4) return;
    fb_fill(fb, x + 1, y + 1, w - 2, h - 2, bg);
    fb_fill(fb, x + 1, y, w - 2, 1, bd);
    fb_fill(fb, x + 1, y + h - 1, w - 2, 1, bd);
    fb_fill(fb, x, y + 1, 1, h - 2, bd);
    fb_fill(fb, x + w - 1, y + 1, 1, h - 2, bd);
    fb_blend(fb, x, y, bd, 90);
    fb_blend(fb, x + w - 1, y, bd, 90);
    fb_blend(fb, x, y + h - 1, bd, 90);
    fb_blend(fb, x + w - 1, y + h - 1, bd, 90);
}

static void fb_circle_aa(ZPImage *fb, int cx, int cy, int r, uint32_t col) {
    for (int dy = -r - 1; dy <= r + 1; ++dy) {
        for (int dx = -r - 1; dx <= r + 1; ++dx) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= (r - 1) * (r - 1)) {
                zp_img_put(fb, cx + dx, cy + dy, col);
            } else if (d2 <= r * r) {
                fb_blend(fb, cx + dx, cy + dy, col, 220);
            } else if (d2 <= (r + 1) * (r + 1)) {
                fb_blend(fb, cx + dx, cy + dy, col, 70);
            }
        }
    }
}

static void fb_text(ZPImage *fb, int x, int y, const char *s, uint32_t col) {
    zp_draw_text_font(fb, x, y, s, ZP_FONT_REG, 1, col);
}

static void fb_text_f(ZPImage *fb, int x, int y, const char *s, int fk, uint32_t col) {
    zp_draw_text_font(fb, x, y, s, fk, 1, col);
}

static void fb_text_centered(ZPImage *fb, int x, int w, int y, const char *s, int fk, uint32_t col) {
    int tw = zp_text_width_font(s, fk, 1);
    int tx = x + (w - tw) / 2;
    zp_draw_text_font(fb, tx < x + 1 ? x + 1 : tx, y, s, fk, 1, col);
}

static bool pt_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static bool is_hov(const ZPApp *app, int x, int y, int w, int h) {
    return app->hover_x >= 0 && pt_in(app->hover_x, app->hover_y, x, y, w, h);
}

/* === canvas geometry and handles === */
int zp_ui_top_h(const ZPApp *app) {
    return ZP_TOP_H + ((app->tool == ZP_TOOL_TEXT || app->text_active) ? ZP_TEXTBAR_H : 0);
}

int zp_eraser_size(int stroke_size) {
    if (stroke_size <= 1) return 4;
    if (stroke_size <= 3) return 8;
    if (stroke_size <= 6) return 14;
    return 24;
}

static void zp_canvas_screen_rect(const ZPApp *app, int *ox, int *oy, int *sw, int *sh) {
    int r = app->show_rulers ? ZP_RULER_SIZE : 0;
    int top_h = zp_ui_top_h(app);
    *ox = r + 16 - app->scroll_x;
    *oy = top_h + r + 16 - app->scroll_y;
    *sw = (int)((float)app->canvas.w * app->zoom);
    *sh = (int)((float)app->canvas.h * app->zoom);
    if (*sw < 1) *sw = 1;
    if (*sh < 1) *sh = 1;
}

bool zp_ui_screen_to_canvas(const ZPApp *app, int sx, int sy, int *cx, int *cy) {
    int r = app->show_rulers ? ZP_RULER_SIZE : 0;
    int top_h = zp_ui_top_h(app);
    int ox, oy, sw, sh;
    zp_canvas_screen_rect(app, &ox, &oy, &sw, &sh);
    *cx = (int)((float)(sx - ox) / app->zoom);
    *cy = (int)((float)(sy - oy) / app->zoom);
    if (sy < top_h + r || sy >= app->win_h - ZP_STATUSBAR_H || sx < r) return false;
    return true;
}

static void get_resize_handles(const ZPApp *app, ZPPoint out[8]) {
    int ox, oy, sw, sh;
    zp_canvas_screen_rect(app, &ox, &oy, &sw, &sh);
    out[0] = (ZPPoint){ ox + sw,      oy + sh / 2 };
    out[1] = (ZPPoint){ ox + sw / 2,  oy + sh     };
    out[2] = (ZPPoint){ ox + sw,      oy + sh     };
    out[3] = (ZPPoint){ ox,           oy + sh / 2 };
    out[4] = (ZPPoint){ ox + sw / 2,  oy          };
    out[5] = (ZPPoint){ ox,           oy          };
    out[6] = (ZPPoint){ ox + sw,      oy          };
    out[7] = (ZPPoint){ ox,           oy + sh     };
}

int zp_ui_hit_resize_handle(const ZPApp *app, int sx, int sy) {
    ZPPoint h[8];
    get_resize_handles(app, h);
    /* only right, bottom and corner handles are draggable  like classic paint */
    for (int i = 0; i < 3; ++i) {
        if (pt_in(sx, sy, h[i].x - 4, h[i].y - 4, 9, 9)) return i;
    }
    return -1;
}

static void get_sel_handles(const ZPApp *app, ZPPoint out[8]) {
    int ox, oy, sw, sh;
    zp_canvas_screen_rect(app, &ox, &oy, &sw, &sh);
    int sx0 = ox + (int)(app->sel.x * app->zoom);
    int sy0 = oy + (int)(app->sel.y * app->zoom);
    int sx1 = ox + (int)((app->sel.x + app->sel.buf.w) * app->zoom);
    int sy1 = oy + (int)((app->sel.y + app->sel.buf.h) * app->zoom);
    int mx = (sx0 + sx1) / 2;
    int my = (sy0 + sy1) / 2;
    /* 0:NW 1:N 2:NE 3:E 4:SE 5:S 6:SW 7:W */
    out[0] = (ZPPoint){ sx0, sy0 };
    out[1] = (ZPPoint){ mx,  sy0 };
    out[2] = (ZPPoint){ sx1, sy0 };
    out[3] = (ZPPoint){ sx1, my  };
    out[4] = (ZPPoint){ sx1, sy1 };
    out[5] = (ZPPoint){ mx,  sy1 };
    out[6] = (ZPPoint){ sx0, sy1 };
    out[7] = (ZPPoint){ sx0, my  };
}

int zp_ui_hit_sel_handle(const ZPApp *app, int sx, int sy) {
    if (!app->sel.active || !app->sel.buf.pixels) return -1;
    int top_h = zp_ui_top_h(app);
    if (sy < top_h || sy >= app->win_h - ZP_STATUSBAR_H) return -1;
    ZPPoint h[8];
    get_sel_handles(app, h);
    for (int i = 0; i < 8; ++i) {
        if (pt_in(sx, sy, h[i].x - 5, h[i].y - 5, 11, 11)) return i;
    }
    return -1;
}

/* === dashed overlay helpers === */
static void fb_dashed_line_clip(ZPImage *fb, int x0, int y0, int x1, int y1,
                                int cx0, int cy0, int cx1, int cy1, uint32_t tick, int dash_len) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int step = 0;
    for (;;) {
        if (x0 >= cx0 && x0 < cx1 && y0 >= cy0 && y0 < cy1) {
            uint32_t c = (((step + (int)tick) / dash_len) & 1) ? ZP_COL_BLACK : ZP_COL_WHITE;
            zp_img_put(fb, x0, y0, c);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}

static void fb_dashed_rect_clip(ZPImage *fb, int x0, int y0, int x1, int y1,
                                int cx0, int cy0, int cx1, int cy1, uint32_t tick, uint32_t c_a, uint32_t c_b) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int x = x0; x <= x1; ++x) {
        uint32_t c = (((x + (int)tick) / 4) & 1) ? c_a : c_b;
        if (x >= cx0 && x < cx1) {
            if (y0 >= cy0 && y0 < cy1) zp_img_put(fb, x, y0, c);
            if (y1 >= cy0 && y1 < cy1) zp_img_put(fb, x, y1, c);
        }
    }
    for (int y = y0; y <= y1; ++y) {
        uint32_t c = (((y + (int)tick) / 4) & 1) ? c_a : c_b;
        if (y >= cy0 && y < cy1) {
            if (x0 >= cx0 && x0 < cx1) zp_img_put(fb, x0, y, c);
            if (x1 >= cx0 && x1 < cx1) zp_img_put(fb, x1, y, c);
        }
    }
}

/* === shared app actions === */
void zp_app_commit_selection(ZPApp *app) {
    if (!app->sel.active) return;
    zp_sel_blit(&app->canvas, &app->sel);
    zp_sel_clear(&app->sel);
    zp_history_push(&app->history, &app->canvas);
    app->dirty = true;
}

void zp_app_commit_text(ZPApp *app) {
    if (!app->text_active) return;
    if (app->text_len > 0) {
        zp_ttf_draw(&app->canvas, app->text_x, app->text_y, app->text_buf,
                    app->text_font_idx, app->text_size_px, app->text_bold, app->color1);
        zp_history_push(&app->history, &app->canvas);
        app->dirty = true;
    }
    app->text_active = false;
    app->text_len = 0;
    app->text_buf[0] = '\0';
}

void zp_app_new(ZPApp *app) {
    zp_sel_clear(&app->sel);
    app->text_active = false;
    zp_img_free(&app->canvas);
    zp_img_init(&app->canvas, ZP_DEFAULT_WIDTH, ZP_DEFAULT_HEIGHT, ZP_COL_WHITE);
    app->file_path[0] = '\0';
    app->dirty = false;
    app->scroll_x = 0;
    app->scroll_y = 0;
    zp_history_free(&app->history);
    zp_history_init(&app->history, ZP_DEFAULT_UNDO_MB);
    zp_history_push(&app->history, &app->canvas);
    snprintf(app->status_msg, sizeof(app->status_msg), "New 1024x640 canvas");
}

void zp_app_open(ZPApp *app) {
    zp_app_commit_selection(app);
    zp_app_commit_text(app);
    char path[ZP_PATH_MAX];
    if (zp_platform_file_dialog(false, path, sizeof(path))) {
        ZPImage loaded;
        if (zp_load_image(path, &loaded)) {
            zp_img_free(&app->canvas);
            app->canvas = loaded;
            snprintf(app->file_path, sizeof(app->file_path), "%s", path);
            app->dirty = false;
            zp_history_push(&app->history, &app->canvas);
            snprintf(app->status_msg, sizeof(app->status_msg), "Opened %s", path);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Unsupported or unreadable image");
        }
    } else {
        app->modal = ZP_MODAL_PATH_OPEN;
        snprintf(app->modal_input, sizeof(app->modal_input), "%s", app->file_path[0] ? app->file_path : "image.bmp");
    }
}

void zp_app_save(ZPApp *app, bool force_dialog) {
    zp_app_commit_selection(app);
    zp_app_commit_text(app);
    char path[ZP_PATH_MAX];
    if (!force_dialog && app->file_path[0]) {
        snprintf(path, sizeof(path), "%s", app->file_path);
    } else if (zp_platform_file_dialog(true, path, sizeof(path))) {
        snprintf(app->file_path, sizeof(app->file_path), "%s", path);
    } else {
        app->modal = ZP_MODAL_PATH_SAVE;
        snprintf(app->modal_input, sizeof(app->modal_input), "%s", app->file_path[0] ? app->file_path : "untitled.png");
        return;
    }
    if (zp_save_image(path, &app->canvas)) {
        app->dirty = false;
        snprintf(app->status_msg, sizeof(app->status_msg), "Saved %s", path);
    } else {
        snprintf(app->status_msg, sizeof(app->status_msg), "Save failed: %s", path);
    }
}

void zp_app_copy(ZPApp *app) {
    const ZPImage *src = app->sel.active ? &app->sel.buf : &app->canvas;
    zp_img_free(&app->clipboard);
    if (app->clipboard_mask) { free(app->clipboard_mask); app->clipboard_mask = NULL; }
    if (zp_img_clone(src, &app->clipboard)) {
        if (app->sel.active && app->sel.mask) {
            size_t mlen = (size_t)src->w * (size_t)src->h;
            app->clipboard_mask = (uint8_t *)malloc(mlen);
            if (app->clipboard_mask) memcpy(app->clipboard_mask, app->sel.mask, mlen);
        }
        zp_platform_clipboard_copy(src);
        snprintf(app->status_msg, sizeof(app->status_msg), "Copied %dx%d to clipboard", src->w, src->h);
    }
}

void zp_app_cut(ZPApp *app) {
    if (!app->sel.active) return;
    zp_app_copy(app);
    zp_sel_clear(&app->sel);
    zp_history_push(&app->history, &app->canvas);
    app->dirty = true;
    snprintf(app->status_msg, sizeof(app->status_msg), "Cut selection");
}

void zp_app_paste(ZPApp *app) {
    zp_app_commit_selection(app);
    zp_app_commit_text(app);
    ZPImage pasted;
    bool from_sys = zp_platform_clipboard_paste(&pasted);
    if (!from_sys) {
        if (!app->clipboard.pixels) return;
        if (!zp_img_clone(&app->clipboard, &pasted)) return;
    }
    /* grow canvas automaticaly if pasted screenshot is bigger */
    if (pasted.w > app->canvas.w || pasted.h > app->canvas.h) {
        int nw = pasted.w > app->canvas.w ? pasted.w : app->canvas.w;
        int nh = pasted.h > app->canvas.h ? pasted.h : app->canvas.h;
        zp_canvas_resize_borders(&app->canvas, nw, nh, app->color2);
    }
    zp_sel_clear(&app->sel);
    app->sel.active = true;
    app->sel.x = 0;
    app->sel.y = 0;
    app->sel.buf = pasted;
    app->sel.transparent_bg = app->transparent_selection;
    app->sel.key_color = app->color2;
    if (!from_sys && app->clipboard_mask) {
        size_t mlen = (size_t)pasted.w * (size_t)pasted.h;
        app->sel.mask = (uint8_t *)malloc(mlen);
        if (app->sel.mask) memcpy(app->sel.mask, app->clipboard_mask, mlen);
    }
    app->tool = ZP_TOOL_SELECT_RECT;
    snprintf(app->status_msg, sizeof(app->status_msg), "Pasted %dx%d", pasted.w, pasted.h);
}

void zp_app_crop_to_selection(ZPApp *app) {
    if (!app->sel.active || !app->sel.buf.pixels) return;
    ZPImage cropped;
    if (!zp_img_init(&cropped, app->sel.buf.w, app->sel.buf.h, app->color2)) return;
    ZPSelection tmp = app->sel;
    tmp.x = 0; tmp.y = 0;
    zp_sel_blit(&cropped, &tmp);
    zp_sel_clear(&app->sel);
    zp_img_free(&app->canvas);
    app->canvas = cropped;
    zp_history_push(&app->history, &app->canvas);
    app->dirty = true;
    snprintf(app->status_msg, sizeof(app->status_msg), "Cropped to %dx%d", app->canvas.w, app->canvas.h);
}

void zp_app_rotate(ZPApp *app, bool clockwise) {
    ZPImage rot;
    if (app->sel.active) {
        if (zp_img_rotate90(&app->sel.buf, &rot, clockwise)) {
            zp_img_free(&app->sel.buf);
            app->sel.buf = rot;
            if (app->sel.mask) { free(app->sel.mask); app->sel.mask = NULL; }
        }
    } else if (zp_img_rotate90(&app->canvas, &rot, clockwise)) {
        zp_img_free(&app->canvas);
        app->canvas = rot;
        zp_history_push(&app->history, &app->canvas);
        app->dirty = true;
    }
}

void zp_app_flip(ZPApp *app, bool horizontal) {
    ZPImage *target = app->sel.active ? &app->sel.buf : &app->canvas;
    if (horizontal) zp_img_flip_h(target);
    else zp_img_flip_v(target);
    if (!app->sel.active) {
        zp_history_push(&app->history, &app->canvas);
        app->dirty = true;
    }
}

static int zoom_index(float z) {
    int best = 2;
    for (int i = 0; i < ZP_ZOOM_COUNT; ++i) {
        if (z >= ZP_ZOOM_LEVELS[i] - 0.01f) best = i;
    }
    return best;
}

void zp_app_zoom_step(ZPApp *app, int dir) {
    int next = zoom_index(app->zoom) + dir;
    if (next < 0) next = 0;
    if (next >= ZP_ZOOM_COUNT) next = ZP_ZOOM_COUNT - 1;
    app->zoom = ZP_ZOOM_LEVELS[next];
}

void zp_app_zoom_at(ZPApp *app, int dir, int sx, int sy) {
    float old_z = app->zoom;
    int next = zoom_index(old_z) + dir;
    if (next < 0) next = 0;
    if (next >= ZP_ZOOM_COUNT) next = ZP_ZOOM_COUNT - 1;
    float new_z = ZP_ZOOM_LEVELS[next];
    if (new_z == old_z) return;

    int r = app->show_rulers ? ZP_RULER_SIZE : 0;
    int top_h = zp_ui_top_h(app);
    if (sx < r || sx >= app->win_w || sy < top_h + r || sy >= app->win_h - ZP_STATUSBAR_H) {
        sx = r + (app->win_w - r) / 2;
        sy = top_h + r + (app->win_h - top_h - r - ZP_STATUSBAR_H) / 2;
    }
    int ox, oy, sw, sh;
    zp_canvas_screen_rect(app, &ox, &oy, &sw, &sh);
    float cxf = (float)(sx - ox) / old_z;
    float cyf = (float)(sy - oy) / old_z;
    app->zoom = new_z;
    app->scroll_x = (int)(r + 16 + cxf * new_z - (float)sx + 0.5f);
    app->scroll_y = (int)(top_h + r + 16 + cyf * new_z - (float)sy + 0.5f);
}

/* === flat ribbon button helpers === */
static void draw_flat_bg(const ZPApp *app, ZPImage *fb, int x, int y, int w, int h, bool active, bool disabled) {
    if (active) {
        fb_rbox(fb, x, y, w, h, ZP_COL_ACCENT_BG, ZP_COL_ACCENT);
    } else if (!disabled && is_hov(app, x, y, w, h)) {
        fb_rbox(fb, x, y, w, h, ZP_RGB(229, 234, 239), ZP_RGB(191, 199, 209));
    }
}

static void draw_flat_icon(const ZPApp *app, ZPImage *fb, int x, int y, int w, int h, const uint8_t *ico, bool active, bool disabled) {
    draw_flat_bg(app, fb, x, y, w, h, active, disabled);
    uint32_t ink = disabled ? ZP_RGB(154, 163, 175) : ZP_COL_INK;
    zp_draw_icon_alpha(fb, x + (w - 16) / 2, y + (h - 16) / 2, 16, ico, ink);
}

static void draw_flat_icon_label(const ZPApp *app, ZPImage *fb, int x, int y, int w, int h, const uint8_t *ico, const char *label, bool active, bool disabled) {
    draw_flat_bg(app, fb, x, y, w, h, active, disabled);
    uint32_t ink = disabled ? ZP_RGB(154, 163, 175) : ZP_COL_INK;
    zp_draw_icon_alpha(fb, x + 5, y + (h - 16) / 2, 16, ico, ink);
    fb_text_f(fb, x + 23, y + (h - 10) / 2, label, ZP_FONT_REG, ink);
}

static void draw_brand_mark(ZPImage *fb, int x, int y) {
    for (int py = 0; py < ZP_LOGO_MENU_SIZE; ++py) {
        for (int px = 0; px < ZP_LOGO_MENU_SIZE; ++px) {
            uint32_t c = ZP_LOGO_20[py * ZP_LOGO_MENU_SIZE + px];
            uint8_t a = (uint8_t)(c >> 24);
            if (a) fb_blend(fb, x + px, y + py, c | 0xFF000000u, a);
        }
    }
}

static const char *file_basename(const char *path) {
    if (!path || !path[0]) return "untitled.png";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* === frame rendering === */
void zp_ui_render(ZPApp *app, ZPImage *fb, uint32_t anim_tick) {
    int top_h = zp_ui_top_h(app);
    fb_fill(fb, 0, top_h, fb->w, fb->h - top_h - ZP_STATUSBAR_H, ZP_COL_WELL);

    int ox, oy, sw, sh;
    zp_canvas_screen_rect(app, &ox, &oy, &sw, &sh);
    int r = app->show_rulers ? ZP_RULER_SIZE : 0;
    int clip_x0 = r, clip_y0 = top_h + r;
    int clip_x1 = fb->w, clip_y1 = fb->h - ZP_STATUSBAR_H;

    /* soft canvas drop shadow + nearest-neighbor blit */
    int shx0 = (ox + 2 < clip_x0) ? clip_x0 : ox + 2;
    int shy0 = (oy + 2 < clip_y0) ? clip_y0 : oy + 2;
    int shx1 = (ox + 2 + sw > clip_x1) ? clip_x1 : ox + 2 + sw;
    int shy1 = (oy + 2 + sh > clip_y1) ? clip_y1 : oy + 2 + sh;
    fb_fill(fb, shx0, shy0, shx1 - shx0, shy1 - shy0, ZP_RGB(178, 186, 199));

    int dx0 = ox < clip_x0 ? clip_x0 : ox;
    int dy0 = oy < clip_y0 ? clip_y0 : oy;
    int dx1 = (ox + sw > clip_x1) ? clip_x1 : ox + sw;
    int dy1 = (oy + sh > clip_y1) ? clip_y1 : oy + sh;

    for (int py = dy0; py < dy1; ++py) {
        int cy = (int)((float)(py - oy) / app->zoom);
        if (cy < 0) cy = 0;
        if (cy >= app->canvas.h) cy = app->canvas.h - 1;
        const uint32_t *srow = app->canvas.pixels + (size_t)cy * (size_t)app->canvas.w;
        uint32_t *drow = fb->pixels + (size_t)py * (size_t)fb->w;
        for (int px = dx0; px < dx1; ++px) {
            int cx = (int)((float)(px - ox) / app->zoom);
            if (cx < 0) cx = 0;
            if (cx >= app->canvas.w) cx = app->canvas.w - 1;
            drow[px] = srow[cx];
        }
    }

    if (app->show_grid && app->zoom >= 4.0f) {
        int step = (int)app->zoom;
        uint32_t gc = ZP_RGB(205, 212, 222);
        for (int gx = 0; gx <= app->canvas.w; ++gx) {
            int sx = ox + gx * step;
            if (sx >= clip_x0 && sx < clip_x1) fb_fill(fb, sx, dy0, 1, dy1 - dy0, gc);
        }
        for (int gy = 0; gy <= app->canvas.h; ++gy) {
            int sy = oy + gy * step;
            if (sy >= clip_y0 && sy < clip_y1) fb_fill(fb, dx0, sy, dx1 - dx0, 1, gc);
        }
    }

    /* canvas border + 8 canvas resize handles (clipped to viewport) */
    fb_dashed_rect_clip(fb, ox - 1, oy - 1, ox + sw, oy + sh, clip_x0, clip_y0, clip_x1, clip_y1, 0,
                        ZP_RGB(169, 178, 192), ZP_RGB(169, 178, 192));
    ZPPoint handles[8];
    get_resize_handles(app, handles);
    for (int i = 0; i < 8; ++i) {
        if (handles[i].x < clip_x0 - 4 || handles[i].x > clip_x1 + 4 ||
            handles[i].y < clip_y0 - 4 || handles[i].y > clip_y1 + 4) continue;
        uint32_t hc = (i < 3) ? ZP_COL_ACCENT : ZP_RGB(140, 150, 164);
        fb_fill(fb, handles[i].x - 3, handles[i].y - 3, 6, 6, ZP_COL_WHITE);
        fb_rect_outline(fb, handles[i].x - 3, handles[i].y - 3, 6, 6, hc);
    }

    /* active lifted selection + free-form lasso contour + 8 transform handles */
    if (app->sel.active && app->sel.buf.pixels) {
        int sx0 = ox + (int)(app->sel.x * app->zoom);
        int sy0 = oy + (int)(app->sel.y * app->zoom);
        int sx1 = ox + (int)((app->sel.x + app->sel.buf.w) * app->zoom);
        int sy1 = oy + (int)((app->sel.y + app->sel.buf.h) * app->zoom);
        int vx0 = sx0 < clip_x0 ? clip_x0 : sx0;
        int vy0 = sy0 < clip_y0 ? clip_y0 : sy0;
        int vx1 = sx1 > clip_x1 ? clip_x1 : sx1;
        int vy1 = sy1 > clip_y1 ? clip_y1 : sy1;

        for (int py = vy0; py < vy1; ++py) {
            int cy = (int)((float)(py - oy) / app->zoom);
            int sy = cy - app->sel.y;
            if (sy < 0 || sy >= app->sel.buf.h) continue;
            uint32_t *drow = fb->pixels + (size_t)py * (size_t)fb->w;
            for (int px = vx0; px < vx1; ++px) {
                int cx = (int)((float)(px - ox) / app->zoom);
                int sx = cx - app->sel.x;
                if (sx < 0 || sx >= app->sel.buf.w) continue;
                if (app->sel.mask && !app->sel.mask[(size_t)sy * (size_t)app->sel.buf.w + (size_t)sx]) continue;
                uint32_t c = zp_img_get(&app->sel.buf, sx, sy);
                if (app->sel.transparent_bg && c == app->sel.key_color) continue;
                drow[px] = c;
            }
        }

        /* draw free-form marching ants along actual lasso mask boundary */
        if (app->sel.mask) {
            int mw = app->sel.buf.w, mh = app->sel.buf.h;
            for (int py = vy0; py < vy1; ++py) {
                int cy = (int)((float)(py - oy) / app->zoom);
                int sy = cy - app->sel.y;
                if (sy < 0 || sy >= mh) continue;
                for (int px = vx0; px < vx1; ++px) {
                    int cx = (int)((float)(px - ox) / app->zoom);
                    int sx = cx - app->sel.x;
                    if (sx < 0 || sx >= mw) continue;
                    if (!app->sel.mask[(size_t)sy * (size_t)mw + (size_t)sx]) continue;
                    bool edge = (sx == 0 || sx == mw - 1 || sy == 0 || sy == mh - 1 ||
                                 !app->sel.mask[(size_t)sy * (size_t)mw + (size_t)(sx - 1)] ||
                                 !app->sel.mask[(size_t)sy * (size_t)mw + (size_t)(sx + 1)] ||
                                 !app->sel.mask[(size_t)(sy - 1) * (size_t)mw + (size_t)sx] ||
                                 !app->sel.mask[(size_t)(sy + 1) * (size_t)mw + (size_t)sx]);
                    if (edge) {
                        uint32_t mc = (((px + py + (int)anim_tick) / 4) & 1) ? ZP_COL_BLACK : ZP_COL_WHITE;
                        zp_img_put(fb, px, py, mc);
                    }
                }
            }
        }

        /* dashed selection bounding box + 8 transform resize handles */
        fb_dashed_rect_clip(fb, sx0, sy0, sx1, sy1, clip_x0, clip_y0, clip_x1, clip_y1,
                            anim_tick, app->sel.mask ? ZP_COL_ACCENT : ZP_COL_BLACK, ZP_COL_WHITE);

        ZPPoint shandles[8];
        get_sel_handles(app, shandles);
        for (int i = 0; i < 8; ++i) {
            int hx = shandles[i].x, hy = shandles[i].y;
            if (hx < clip_x0 - 4 || hx > clip_x1 + 4 || hy < clip_y0 - 4 || hy > clip_y1 + 4) continue;
            bool hot = (app->sel.transform_handle == i) || is_hov(app, hx - 5, hy - 5, 11, 11);
            fb_fill(fb, hx - 3, hy - 3, 7, 7, hot ? ZP_COL_ACCENT : ZP_COL_WHITE);
            fb_rect_outline(fb, hx - 3, hy - 3, 7, 7, ZP_COL_ACCENT);
        }
    }

    /* live rect select drag box */
    if (app->drawing && app->tool == ZP_TOOL_SELECT_RECT) {
        int rx0 = ox + (int)(app->start_pt.x * app->zoom);
        int ry0 = oy + (int)(app->start_pt.y * app->zoom);
        int rx1 = ox + (int)(app->curr_pt.x * app->zoom);
        int ry1 = oy + (int)(app->curr_pt.y * app->zoom);
        fb_dashed_rect_clip(fb, rx0, ry0, rx1, ry1, clip_x0, clip_y0, clip_x1, clip_y1,
                            anim_tick, ZP_COL_BLACK, ZP_COL_WHITE);
    }

    /* live free-form lasso path while dragging */
    if (app->drawing && app->tool == ZP_TOOL_SELECT_LASSO && app->poly_count > 1) {
        for (int i = 0; i + 1 < app->poly_count; ++i) {
            int x0 = ox + (int)(app->poly_pts[i].x * app->zoom);
            int y0 = oy + (int)(app->poly_pts[i].y * app->zoom);
            int x1 = ox + (int)(app->poly_pts[i + 1].x * app->zoom);
            int y1 = oy + (int)(app->poly_pts[i + 1].y * app->zoom);
            fb_dashed_line_clip(fb, x0, y0, x1, y1, clip_x0, clip_y0, clip_x1, clip_y1, anim_tick, 4);
        }
        /* closing guide line back to start */
        int fx = ox + (int)(app->poly_pts[0].x * app->zoom);
        int fy = oy + (int)(app->poly_pts[0].y * app->zoom);
        int lx = ox + (int)(app->poly_pts[app->poly_count - 1].x * app->zoom);
        int ly = oy + (int)(app->poly_pts[app->poly_count - 1].y * app->zoom);
        fb_dashed_line_clip(fb, lx, ly, fx, fy, clip_x0, clip_y0, clip_x1, clip_y1, anim_tick, 2);
    }

    /* crisp eraser footprint outline under mouse */
    if (app->tool == ZP_TOOL_ERASER && app->cursor_in_canvas && !app->panning) {
        int es = zp_eraser_size(app->stroke_size);
        int half = es / 2;
        int ex0 = ox + (int)((float)(app->cursor_px.x - half) * app->zoom);
        int ey0 = oy + (int)((float)(app->cursor_px.y - half) * app->zoom);
        int ew = (int)((float)es * app->zoom);
        if (ew < 3) ew = 3;
        fb_dashed_rect_clip(fb, ex0, ey0, ex0 + ew, ey0 + ew, clip_x0, clip_y0, clip_x1, clip_y1, 0,
                            ZP_COL_BLACK, ZP_COL_BLACK);
        if (ew >= 5) {
            fb_dashed_rect_clip(fb, ex0 + 1, ey0 + 1, ex0 + ew - 1, ey0 + ew - 1, clip_x0, clip_y0, clip_x1, clip_y1, 0,
                                ZP_COL_WHITE, ZP_COL_WHITE);
        }
    }

    /* crisp freetype live text preview + caret */
    if (app->text_active) {
        int tx = ox + (int)(app->text_x * app->zoom);
        int ty = oy + (int)(app->text_y * app->zoom);
        int px_scr = (int)((float)app->text_size_px * app->zoom + 0.5f);
        if (px_scr < 8) px_scr = 8;
        if (px_scr > 256) px_scr = 256;
        int tw = 0, th = 0;
        zp_ttf_measure(app->text_buf, app->text_font_idx, px_scr, app->text_bold, &tw, &th);
        int bw = tw + 16;
        if (bw < 64) bw = 64;
        int bh = th + 8;
        if (bh < px_scr + 10) bh = px_scr + 10;

        fb_dashed_rect_clip(fb, tx - 4, ty - 4, tx + bw, ty + bh, clip_x0, clip_y0, clip_x1, clip_y1,
                            0, ZP_COL_ACCENT, ZP_COL_WHITE);
        zp_ttf_draw_clipped(fb, tx, ty, app->text_buf, app->text_font_idx, px_scr, app->text_bold, app->color1,
                            clip_x0, clip_y0, clip_x1, clip_y1);

        /* draw vertical text caret at end of last line */
        const char *last_line = app->text_buf;
        int line_idx = 0;
        for (const char *p = app->text_buf; *p; ++p) {
            if (*p == '\n') { line_idx++; last_line = p + 1; }
        }
        int lw = 0;
        zp_ttf_measure(last_line, app->text_font_idx, px_scr, app->text_bold, &lw, NULL);
        int line_h = (int)(px_scr * 1.25f);
        if (line_h < px_scr + 2) line_h = px_scr + 2;
        int cx_bar = tx + lw + 1;
        int cy_bar = ty + line_idx * line_h + 2;
        for (int y = cy_bar; y < cy_bar + px_scr; ++y) {
            if (cx_bar >= clip_x0 && cx_bar < clip_x1 && y >= clip_y0 && y < clip_y1) {
                zp_img_put(fb, cx_bar, y, ZP_COL_ACCENT);
            }
        }
    }

    if (app->show_rulers) {
        fb_fill(fb, 0, top_h, fb->w, ZP_RULER_SIZE, ZP_COL_CHROME);
        fb_fill(fb, 0, top_h, ZP_RULER_SIZE, fb->h - top_h - ZP_STATUSBAR_H, ZP_COL_CHROME);
        fb_fill(fb, 0, top_h + ZP_RULER_SIZE - 1, fb->w, 1, ZP_COL_BORDER);
        fb_fill(fb, ZP_RULER_SIZE - 1, top_h, 1, fb->h - top_h - ZP_STATUSBAR_H, ZP_COL_BORDER);
        int tick_px = (app->zoom >= 4.0f) ? 10 : 50;
        for (int p = 0; p <= app->canvas.w; p += tick_px) {
            int sx = ox + (int)(p * app->zoom);
            if (sx > ZP_RULER_SIZE && sx < fb->w) fb_fill(fb, sx, top_h + 10, 1, 7, ZP_COL_INK_MUTED);
        }
        for (int p = 0; p <= app->canvas.h; p += tick_px) {
            int sy = oy + (int)(p * app->zoom);
            if (sy > top_h + ZP_RULER_SIZE && sy < fb->h - ZP_STATUSBAR_H) {
                fb_fill(fb, 10, sy, 7, 1, ZP_COL_INK_MUTED);
            }
        }
    }

    /* === top menu bar (32px, clean white) === */
    fb_fill(fb, 0, 0, fb->w, ZP_MENUBAR_H, ZP_COL_WHITE);
    fb_fill(fb, 0, ZP_MENUBAR_H - 1, fb->w, 1, ZP_RGB(198, 205, 214));

    draw_brand_mark(fb, 8, 6);
    fb_text_f(fb, 32, 11, "0Paint", ZP_FONT_BOLD, ZP_COL_INK);
    fb_fill(fb, 79, 8, 1, 16, ZP_RGB(213, 219, 227));

    static const char *MENUS[4] = { "File", "Edit", "View", "Image" };
    static const int MX[4] = { 86, 126, 166, 210 };
    static const int MW[4] = { 38, 38, 42, 48 };
    for (int i = 0; i < 4; ++i) {
        bool open = (app->open_menu == i);
        if (open) {
            fb_rbox(fb, MX[i], 5, MW[i], 22, ZP_COL_ACCENT_BG, ZP_COL_ACCENT);
        } else if (is_hov(app, MX[i], 5, MW[i], 22)) {
            fb_rbox(fb, MX[i], 5, MW[i], 22, ZP_RGB(233, 236, 239), ZP_RGB(198, 205, 214));
        }
        fb_text_centered(fb, MX[i], MW[i], 11, MENUS[i], ZP_FONT_REG, ZP_COL_INK);
    }

    fb_fill(fb, 266, 8, 1, 16, ZP_RGB(213, 219, 227));
    bool can_undo = app->history.index > 0;
    bool can_redo = app->history.index + 1 < app->history.count;
    draw_flat_icon(app, fb, 274, 5, 24, 22, ZP_ICO_UNDO, false, !can_undo);
    draw_flat_icon(app, fb, 300, 5, 24, 22, ZP_ICO_REDO, false, !can_redo);
    draw_flat_icon(app, fb, 326, 5, 24, 22, ZP_ICO_SAVE, false, false);

    char title_buf[180];
    snprintf(title_buf, sizeof(title_buf), "%s%s - zeroPaint", file_basename(app->file_path), app->dirty ? "*" : "");
    fb_text(fb, 362, 11, title_buf, ZP_COL_INK_MUTED);

    /* === ribbon toolbar (84px, flat win10 light chrome) === */
    fb_fill(fb, 0, ZP_MENUBAR_H, fb->w, ZP_RIBBON_H, ZP_COL_CHROME);
    fb_fill(fb, 0, ZP_TOP_H - 1, fb->w, 1, ZP_RGB(175, 184, 196));

    /* group 1: clipboard */
    draw_flat_bg(app, fb, 8, 38, 44, 54, false, false);
    zp_draw_icon_alpha(fb, 20, 44, 20, ZP_ICO_PASTE20, ZP_COL_INK);
    fb_text_centered(fb, 8, 44, 72, "Paste", ZP_FONT_REG, ZP_COL_INK);
    draw_flat_icon_label(app, fb, 56, 38, 52, 26, ZP_ICO_CUT,  "Cut",  false, !app->sel.active);
    draw_flat_icon_label(app, fb, 56, 66, 52, 26, ZP_ICO_COPY, "Copy", false, false);
    fb_text_centered(fb, 8, 100, 99, "Clipboard", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    fb_fill(fb, 114, 38, 1, 70, ZP_RGB(205, 212, 221));

    /* group 2: image & selection */
    draw_flat_icon_label(app, fb, 121, 38, 62, 26, ZP_ICO_SELECT, "Select", app->tool == ZP_TOOL_SELECT_RECT, false);
    draw_flat_icon_label(app, fb, 121, 66, 62, 26, ZP_ICO_LASSO,  "Lasso",  app->tool == ZP_TOOL_SELECT_LASSO, false);
    draw_flat_icon_label(app, fb, 187, 38, 56, 26, ZP_ICO_CROP,   "Crop",   false, !app->sel.active);
    draw_flat_icon_label(app, fb, 245, 38, 66, 26, ZP_ICO_RESIZE, "Resize", false, false);
    draw_flat_bg(app, fb, 187, 66, 66, 26, app->transparent_selection, false);
    fb_text_centered(fb, 187, 66, 75, "Transparent", ZP_FONT_SMALL, ZP_COL_INK);
    draw_flat_icon_label(app, fb, 255, 66, 62, 26, ZP_ICO_ROTATE, "Rotate", app->rotate_menu_open, false);
    fb_text_centered(fb, 121, 196, 99, "Image", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    fb_fill(fb, 323, 38, 1, 70, ZP_RGB(205, 212, 221));

    /* group 3: classic 3x2 tools */
    static const struct { ZPTool t; const uint8_t *ico; } CORE_TOOLS[6] = {
        { ZP_TOOL_PENCIL, ZP_ICO_PENCIL }, { ZP_TOOL_FILL, ZP_ICO_FILL }, { ZP_TOOL_TEXT, ZP_ICO_TEXT },
        { ZP_TOOL_ERASER, ZP_ICO_ERASER }, { ZP_TOOL_EYEDROPPER, ZP_ICO_PICKER }, { ZP_TOOL_MAGNIFIER, ZP_ICO_ZOOM }
    };
    for (int i = 0; i < 6; ++i) {
        int col = i % 3, row = i / 3;
        draw_flat_icon(app, fb, 330 + col * 30, 38 + row * 28, 28, 26, CORE_TOOLS[i].ico, app->tool == CORE_TOOLS[i].t, false);
    }
    fb_text_centered(fb, 330, 88, 99, "Tools", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    fb_fill(fb, 424, 38, 1, 70, ZP_RGB(205, 212, 221));

    /* group 4: brushes & size */
    draw_flat_icon_label(app, fb, 431, 38, 60, 26, ZP_ICO_BRUSH, "Brush", app->tool == ZP_TOOL_BRUSH, false);
    draw_flat_icon_label(app, fb, 431, 66, 60, 26, ZP_ICO_SPRAY, "Spray", app->tool == ZP_TOOL_AIRBRUSH, false);

    static const char *NIB_NAMES[4] = { "Pixel Nib", "Smooth Round", "Calligraphy", "Square Marker" };
    uint32_t nib_bd = is_hov(app, 495, 38, 102, 25) ? ZP_COL_ACCENT : ZP_RGB(191, 199, 209);
    fb_rbox(fb, 495, 38, 102, 25, ZP_COL_WHITE, nib_bd);
    fb_text_f(fb, 502, 46, NIB_NAMES[app->brush_variant & 3], ZP_FONT_SMALL, ZP_COL_INK);
    zp_draw_icon_alpha(fb, 579, 43, 16, ZP_ICO_CHEVRON, ZP_COL_INK_MUTED);

    static const int SIZES[4] = { 1, 3, 6, 12 };
    static const char *SLBL[4] = { "1", "3", "6", "12" };
    for (int i = 0; i < 4; ++i) {
        int bx = 495 + i * 26;
        bool act = (app->stroke_size == SIZES[i]);
        draw_flat_bg(app, fb, bx, 67, 24, 24, act, false);
        fb_text_centered(fb, bx, 24, 75, SLBL[i], ZP_FONT_SMALL, ZP_COL_INK);
    }
    fb_text_centered(fb, 431, 166, 99, "Brushes & Size", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    fb_fill(fb, 603, 38, 1, 70, ZP_RGB(205, 212, 221));

    /* group 5: shapes gallery + fill mode */
    fb_rbox(fb, 610, 38, 134, 54, ZP_COL_WHITE, ZP_RGB(205, 212, 221));
    static const struct { ZPTool t; const uint8_t *ico; } SHAPES[10] = {
        { ZP_TOOL_LINE, ZP_ICO_LINE }, { ZP_TOOL_CURVE, ZP_ICO_CURVE },
        { ZP_TOOL_RECT, ZP_ICO_RECT }, { ZP_TOOL_ROUND_RECT, ZP_ICO_RRECT },
        { ZP_TOOL_ELLIPSE, ZP_ICO_ELLIPSE }, { ZP_TOOL_POLYGON, ZP_ICO_POLY },
        { ZP_TOOL_TRIANGLE, ZP_ICO_TRI }, { ZP_TOOL_DIAMOND, ZP_ICO_DIAMOND },
        { ZP_TOOL_ARROW_RIGHT, ZP_ICO_ARROW }, { ZP_TOOL_STAR, ZP_ICO_STAR }
    };
    for (int i = 0; i < 10; ++i) {
        int col = i % 5, row = i / 5;
        draw_flat_icon(app, fb, 613 + col * 26, 41 + row * 25, 24, 23, SHAPES[i].ico, app->tool == SHAPES[i].t, false);
    }

    static const char *MODES[3] = { "Outline", "Fill", "Out+Fill" };
    for (int i = 0; i < 3; ++i) {
        int my = 38 + i * 18;
        draw_flat_bg(app, fb, 748, my, 48, 17, (int)app->shape_mode == i, false);
        fb_text_centered(fb, 748, 48, my + 4, MODES[i], ZP_FONT_SMALL, ZP_COL_INK);
    }
    fb_text_centered(fb, 610, 186, 99, "Shapes", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    fb_fill(fb, 802, 38, 1, 70, ZP_RGB(205, 212, 221));

    /* group 6: color 1 / color 2 + swap + 28 swatches */
    bool c1_act = (app->active_slot == 1);
    fb_rbox(fb, 809, 38, 42, 54,
            c1_act ? ZP_COL_ACCENT_BG : (is_hov(app, 809, 38, 42, 54) ? ZP_RGB(225, 230, 236) : ZP_COL_SURFACE),
            c1_act ? ZP_COL_ACCENT : ZP_COL_BORDER);
    fb_fill(fb, 818, 44, 24, 24, app->color1);
    fb_rect_outline(fb, 818, 44, 24, 24, ZP_RGB(124, 135, 150));
    fb_text_centered(fb, 809, 42, 75, "Color 1", ZP_FONT_SMALL, ZP_COL_INK);

    bool c2_act = (app->active_slot == 2);
    fb_rbox(fb, 855, 38, 42, 54,
            c2_act ? ZP_COL_ACCENT_BG : (is_hov(app, 855, 38, 42, 54) ? ZP_RGB(225, 230, 236) : ZP_COL_SURFACE),
            c2_act ? ZP_COL_ACCENT : ZP_COL_BORDER);
    fb_fill(fb, 864, 44, 24, 24, app->color2);
    fb_rect_outline(fb, 864, 44, 24, 24, ZP_RGB(124, 135, 150));
    fb_text_centered(fb, 855, 42, 75, "Color 2", ZP_FONT_SMALL, ZP_COL_INK);

    draw_flat_bg(app, fb, 900, 38, 22, 54, false, false);
    zp_draw_icon_alpha(fb, 903, 57, 16, ZP_ICO_SWAP, ZP_COL_INK_MUTED);

    for (int i = 0; i < ZP_PALETTE_COUNT; ++i) {
        int col = i % 14, row = i / 14;
        int sx = 926 + col * 19;
        int sy = 42 + row * 21;
        bool hov = is_hov(app, sx, sy, 17, 17);
        fb_fill(fb, sx, sy, 17, 17, app->palette[i]);
        fb_rect_outline(fb, sx, sy, 17, 17, hov ? ZP_COL_ACCENT : ZP_RGB(140, 150, 164));
    }
    fb_text_centered(fb, 809, 380, 99, "Colors (LMB = Color 1, RMB = Color 2)", ZP_FONT_SMALL, ZP_COL_INK_MUTED);

    /* === contextual text bar (30px, visible when text tool is active) === */
    if (app->tool == ZP_TOOL_TEXT || app->text_active) {
        int ty = ZP_TOP_H;
        fb_fill(fb, 0, ty, fb->w, ZP_TEXTBAR_H, ZP_RGB(248, 250, 252));
        fb_fill(fb, 0, ty + ZP_TEXTBAR_H - 1, fb->w, 1, ZP_RGB(175, 184, 196));

        fb_text_f(fb, 10, ty + 10, "Font:", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
        uint32_t fbd = (app->font_menu_open || is_hov(app, 44, ty + 3, 180, 23)) ? ZP_COL_ACCENT : ZP_RGB(186, 194, 204);
        fb_rbox(fb, 44, ty + 3, 180, 23, ZP_COL_WHITE, fbd);
        char fname[28];
        snprintf(fname, sizeof(fname), "%.24s", zp_ttf_font_name(app->text_font_idx));
        fb_text_f(fb, 51, ty + 10, fname, ZP_FONT_REG, ZP_COL_INK);
        zp_draw_icon_alpha(fb, 205, ty + 7, 16, ZP_ICO_CHEVRON, ZP_COL_INK_MUTED);

        fb_text_f(fb, 234, ty + 10, "Size:", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
        draw_flat_bg(app, fb, 266, ty + 4, 22, 21, false, false);
        fb_text_centered(fb, 266, 22, ty + 10, "-", ZP_FONT_BOLD, ZP_COL_INK);

        char szbuf[16];
        snprintf(szbuf, sizeof(szbuf), "%dpx", app->text_size_px);
        fb_rbox(fb, 290, ty + 4, 42, 21, ZP_COL_WHITE, ZP_RGB(191, 199, 209));
        fb_text_centered(fb, 290, 42, ty + 10, szbuf, ZP_FONT_SMALL, ZP_COL_INK);

        draw_flat_bg(app, fb, 334, ty + 4, 22, 21, false, false);
        fb_text_centered(fb, 334, 22, ty + 10, "+", ZP_FONT_BOLD, ZP_COL_INK);

        static const int TSIZES[7] = { 12, 16, 20, 28, 36, 48, 64 };
        static const char *TSLBL[7] = { "12", "16", "20", "28", "36", "48", "64" };
        for (int i = 0; i < 7; ++i) {
            int bx = 364 + i * 28;
            draw_flat_bg(app, fb, bx, ty + 4, 26, 21, app->text_size_px == TSIZES[i], false);
            fb_text_centered(fb, bx, 26, ty + 10, TSLBL[i], ZP_FONT_SMALL, ZP_COL_INK);
        }

        fb_fill(fb, 566, ty + 6, 1, 17, ZP_RGB(205, 212, 221));
        draw_flat_bg(app, fb, 574, ty + 4, 46, 21, app->text_bold, false);
        fb_text_centered(fb, 574, 46, ty + 10, "Bold", ZP_FONT_BOLD, ZP_COL_INK);

        fb_text_f(fb, 632, ty + 10, "Enter: newline | Ctrl+Enter or click outside: apply | Esc: cancel", ZP_FONT_SMALL, ZP_COL_INK_MUTED);
    }

    /* === status bar (28px, with interactive zoom slider) === */
    int sby = fb->h - ZP_STATUSBAR_H;
    fb_fill(fb, 0, sby, fb->w, ZP_STATUSBAR_H, ZP_COL_CHROME);
    fb_fill(fb, 0, sby, fb->w, 1, ZP_RGB(175, 184, 196));

    char buf[160];
    if (app->cursor_in_canvas) {
        snprintf(buf, sizeof(buf), "%d, %d px", app->cursor_px.x, app->cursor_px.y);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    fb_text(fb, 10, sby + 9, buf, ZP_COL_INK);
    fb_fill(fb, 114, sby + 6, 1, 16, ZP_RGB(205, 212, 221));

    if (app->sel.active) {
        snprintf(buf, sizeof(buf), "Sel %dx%d px", app->sel.buf.w, app->sel.buf.h);
        fb_text(fb, 124, sby + 9, buf, ZP_COL_ACCENT);
    }
    fb_fill(fb, 240, sby + 6, 1, 16, ZP_RGB(205, 212, 221));

    snprintf(buf, sizeof(buf), "%d x %d px", app->canvas.w, app->canvas.h);
    fb_text(fb, 250, sby + 9, buf, ZP_COL_INK);
    fb_fill(fb, 358, sby + 6, 1, 16, ZP_RGB(205, 212, 221));
    fb_text(fb, 368, sby + 9, app->status_msg, ZP_COL_INK_MUTED);

    /* right controls: grid toggle + zoom % + slider */
    draw_flat_icon_label(app, fb, fb->w - 246, sby + 4, 56, 20, ZP_ICO_GRID, "Grid", app->show_grid, false);
    fb_fill(fb, fb->w - 183, sby + 6, 1, 16, ZP_RGB(205, 212, 221));

    snprintf(buf, sizeof(buf), "%d%%", (int)(app->zoom * 100.0f + 0.5f));
    int zw = zp_text_width(buf, 1);
    fb_text(fb, fb->w - 139 - zw, sby + 9, buf, ZP_COL_INK);

    fb_rbox(fb, fb->w - 132, sby + 4, 20, 20,
            is_hov(app, fb->w - 132, sby + 4, 20, 20) ? ZP_RGB(229, 234, 239) : ZP_COL_WHITE,
            ZP_COL_BORDER);
    zp_draw_icon_alpha(fb, fb->w - 130, sby + 6, 16, ZP_ICO_MINUS, ZP_COL_INK);

    int track_x = fb->w - 106;
    int track_w = 70;
    int track_y = sby + 12;
    int zidx = zoom_index(app->zoom);
    int thumb_x = track_x + (zidx * track_w) / (ZP_ZOOM_COUNT - 1);
    fb_fill(fb, track_x, track_y, track_w, 4, ZP_RGB(203, 213, 225));
    fb_fill(fb, track_x, track_y, thumb_x - track_x, 4, ZP_COL_ACCENT);
    fb_circle_aa(fb, thumb_x, track_y + 2, 5, ZP_COL_ACCENT);

    fb_rbox(fb, fb->w - 28, sby + 4, 20, 20,
            is_hov(app, fb->w - 28, sby + 4, 20, 20) ? ZP_RGB(229, 234, 239) : ZP_COL_WHITE,
            ZP_COL_BORDER);
    zp_draw_icon_alpha(fb, fb->w - 26, sby + 6, 16, ZP_ICO_PLUS, ZP_COL_INK);

    /* === rotate popover === */
    if (app->rotate_menu_open) {
        static const char *RITEMS[4] = { "Rotate right 90 deg", "Rotate left 90 deg", "Flip horizontal", "Flip vertical" };
        int rx = 255, ry = 93, rw = 148, rh = 4 * 24 + 8;
        fb_fill(fb, rx + 2, ry + 2, rw, rh, ZP_RGB(178, 186, 198));
        fb_fill(fb, rx, ry, rw, rh, ZP_COL_WHITE);
        fb_rect_outline(fb, rx, ry, rw, rh, ZP_RGB(175, 184, 196));
        for (int i = 0; i < 4; ++i) {
            int iy = ry + 4 + i * 24;
            if (is_hov(app, rx + 1, iy, rw - 2, 24)) {
                fb_fill(fb, rx + 1, iy, rw - 2, 24, ZP_COL_ACCENT_BG);
            }
            fb_text(fb, rx + 12, iy + 7, RITEMS[i], ZP_COL_INK);
        }
    }

    /* === system font picker dropdown === */
    if (app->font_menu_open && (app->tool == ZP_TOOL_TEXT || app->text_active)) {
        int cnt = zp_ttf_font_count();
        if (cnt > 14) cnt = 14;
        int fx = 44, fy = ZP_TOP_H + ZP_TEXTBAR_H - 1, fw = 212, fh = cnt * 22 + 8;
        fb_fill(fb, fx + 2, fy + 2, fw, fh, ZP_RGB(178, 186, 198));
        fb_fill(fb, fx, fy, fw, fh, ZP_COL_WHITE);
        fb_rect_outline(fb, fx, fy, fw, fh, ZP_RGB(175, 184, 196));
        for (int i = 0; i < cnt; ++i) {
            int iy = fy + 4 + i * 22;
            bool act = (app->text_font_idx == i);
            if (act || is_hov(app, fx + 1, iy, fw - 2, 22)) {
                fb_fill(fb, fx + 1, iy, fw - 2, 22, act ? ZP_COL_ACCENT_BG : ZP_RGB(235, 240, 246));
            }
            char item_name[32];
            snprintf(item_name, sizeof(item_name), "%.28s", zp_ttf_font_name(i));
            fb_text_f(fb, fx + 10, iy + 6, item_name, act ? ZP_FONT_BOLD : ZP_FONT_REG, ZP_COL_INK);
        }
    }

    /* === dropdown menus === */
    if (app->open_menu >= 0 && app->open_menu < 4) {
        static const char *ITEMS[4][6] = {
            { "New", "Open...", "Save", "Save As...", "Properties", NULL },
            { "Undo", "Redo", "Cut", "Copy", "Paste", "Invert Colors" },
            { "Zoom In", "Zoom Out", "Actual Size (100%)", "Pixel Gridlines", "Rulers", NULL },
            { "Resize / Skew...", "Rotate 90 CW", "Rotate 90 CCW", "Flip Horizontal", "Flip Vertical", "Grayscale" }
        };
        static const char *KEYS[4][6] = {
            { "Ctrl+N", "Ctrl+O", "Ctrl+S", "Ctrl+Shift+S", "Ctrl+E", NULL },
            { "Ctrl+Z", "Ctrl+Y", "Ctrl+X", "Ctrl+C", "Ctrl+V", "Ctrl+Shift+I" },
            { "Ctrl++", "Ctrl+-", "Ctrl+0", "Ctrl+G", "Ctrl+R", NULL },
            { "Ctrl+W", "", "", "", "", "" }
        };
        int dx = MX[app->open_menu];
        int dy = ZP_MENUBAR_H;
        int cnt = 0;
        while (cnt < 6 && ITEMS[app->open_menu][cnt]) cnt++;
        int mw = 212, mh = cnt * 24 + 8;
        fb_fill(fb, dx + 2, dy + 2, mw, mh, ZP_RGB(178, 186, 198));
        fb_fill(fb, dx, dy, mw, mh, ZP_COL_WHITE);
        fb_rect_outline(fb, dx, dy, mw, mh, ZP_RGB(175, 184, 196));
        for (int i = 0; i < cnt; ++i) {
            int iy = dy + 4 + i * 24;
            if (is_hov(app, dx + 1, iy, mw - 2, 24)) {
                fb_fill(fb, dx + 1, iy, mw - 2, 24, ZP_COL_ACCENT_BG);
            }
            bool checked = (app->open_menu == 2 && i == 3 && app->show_grid) ||
                           (app->open_menu == 2 && i == 4 && app->show_rulers);
            if (checked) fb_text_f(fb, dx + 8, iy + 7, "*", ZP_FONT_BOLD, ZP_COL_ACCENT);
            fb_text(fb, dx + 20, iy + 7, ITEMS[app->open_menu][i], ZP_COL_INK);
            if (KEYS[app->open_menu][i] && KEYS[app->open_menu][i][0]) {
                int kw = zp_text_width_font(KEYS[app->open_menu][i], ZP_FONT_SMALL, 1);
                fb_text_f(fb, dx + mw - kw - 12, iy + 8, KEYS[app->open_menu][i], ZP_FONT_SMALL, ZP_COL_INK_MUTED);
            }
        }
    }

    if (app->modal != ZP_MODAL_NONE) {
        int mw = 368, mh = 136;
        int mx = (fb->w - mw) / 2, my = (fb->h - mh) / 2;
        fb_fill(fb, mx + 3, my + 3, mw, mh, ZP_RGB(155, 165, 180));
        fb_fill(fb, mx, my, mw, mh, ZP_COL_WHITE);
        fb_rect_outline(fb, mx, my, mw, mh, ZP_COL_ACCENT);
        fb_fill(fb, mx + 1, my + 1, mw - 2, 28, ZP_COL_CHROME);
        fb_fill(fb, mx + 1, my + 29, mw - 2, 1, ZP_COL_BORDER);
        const char *title = (app->modal == ZP_MODAL_PATH_OPEN) ? "Open Image Path"
            : (app->modal == ZP_MODAL_PATH_SAVE) ? "Save Image Path (.png/.bmp/.ppm/.tga)"
            : (app->modal == ZP_MODAL_RESIZE_SKEW) ? "Resize Canvas (WxH)"
            : "Canvas Properties";
        fb_text_f(fb, mx + 12, my + 9, title, ZP_FONT_BOLD, ZP_COL_INK);
        fb_rbox(fb, mx + 14, my + 46, mw - 28, 26, ZP_COL_WHITE, ZP_COL_ACCENT);
        fb_text(fb, mx + 22, my + 54, app->modal_input, ZP_COL_INK);
        fb_text(fb, mx + 14, my + 86, "Type value & press Enter (Esc to cancel)", ZP_COL_INK_MUTED);
    }
}

/* === ui click routing === */
bool zp_ui_handle_click(ZPApp *app, int sx, int sy, int button) {
    static const int MX[4] = { 86, 126, 166, 210 };
    static const int MW[4] = { 38, 38, 42, 48 };

    if (app->font_menu_open) {
        int cnt = zp_ttf_font_count();
        if (cnt > 14) cnt = 14;
        int fx = 44, fy = ZP_TOP_H + ZP_TEXTBAR_H - 1, fw = 212, fh = cnt * 22 + 8;
        app->font_menu_open = false;
        if (pt_in(sx, sy, fx, fy, fw, fh)) {
            int idx = (sy - fy - 4) / 22;
            if (idx >= 0 && idx < cnt) {
                app->text_font_idx = idx;
                snprintf(app->status_msg, sizeof(app->status_msg), "Font: %s", zp_ttf_font_name(idx));
            }
            return true;
        }
        return true;
    }

    if (app->rotate_menu_open) {
        int rx = 255, ry = 93, rw = 148, rh = 4 * 24 + 8;
        app->rotate_menu_open = false;
        if (pt_in(sx, sy, rx, ry, rw, rh)) {
            int idx = (sy - ry - 4) / 24;
            if (idx == 0) zp_app_rotate(app, true);
            else if (idx == 1) zp_app_rotate(app, false);
            else if (idx == 2) zp_app_flip(app, true);
            else if (idx == 3) zp_app_flip(app, false);
            return true;
        }
        return true;
    }

    if (app->open_menu >= 0) {
        int dx = MX[app->open_menu];
        int dy = ZP_MENUBAR_H;
        if (pt_in(sx, sy, dx, dy, 212, 152)) {
            int idx = (sy - dy - 4) / 24;
            int m = app->open_menu;
            app->open_menu = -1;
            if (m == 0) {
                if (idx == 0) zp_app_new(app);
                else if (idx == 1) zp_app_open(app);
                else if (idx == 2) zp_app_save(app, false);
                else if (idx == 3) zp_app_save(app, true);
                else if (idx == 4) {
                    app->modal = ZP_MODAL_PROPERTIES;
                    snprintf(app->modal_input, sizeof(app->modal_input), "%dx%d", app->canvas.w, app->canvas.h);
                }
            } else if (m == 1) {
                if (idx == 0) { zp_app_commit_selection(app); zp_history_undo(&app->history, &app->canvas); }
                else if (idx == 1) { zp_app_commit_selection(app); zp_history_redo(&app->history, &app->canvas); }
                else if (idx == 2) zp_app_cut(app);
                else if (idx == 3) zp_app_copy(app);
                else if (idx == 4) zp_app_paste(app);
                else if (idx == 5) { zp_img_invert(&app->canvas); zp_history_push(&app->history, &app->canvas); }
            } else if (m == 2) {
                if (idx == 0) zp_app_zoom_step(app, 1);
                else if (idx == 1) zp_app_zoom_step(app, -1);
                else if (idx == 2) app->zoom = 1.0f;
                else if (idx == 3) app->show_grid = !app->show_grid;
                else if (idx == 4) app->show_rulers = !app->show_rulers;
            } else if (m == 3) {
                if (idx == 0) {
                    app->modal = ZP_MODAL_RESIZE_SKEW;
                    snprintf(app->modal_input, sizeof(app->modal_input), "%dx%d", app->canvas.w, app->canvas.h);
                } else if (idx == 1) zp_app_rotate(app, true);
                else if (idx == 2) zp_app_rotate(app, false);
                else if (idx == 3) zp_app_flip(app, true);
                else if (idx == 4) zp_app_flip(app, false);
                else if (idx == 5) { zp_img_grayscale(&app->canvas); zp_history_push(&app->history, &app->canvas); }
            }
            return true;
        }
        app->open_menu = -1;
        return true;
    }

    if (sy < ZP_MENUBAR_H) {
        for (int i = 0; i < 4; ++i) {
            if (pt_in(sx, sy, MX[i], 0, MW[i], ZP_MENUBAR_H)) {
                app->open_menu = i;
                return true;
            }
        }
        if (pt_in(sx, sy, 274, 5, 24, 22)) { zp_app_commit_selection(app); zp_history_undo(&app->history, &app->canvas); return true; }
        if (pt_in(sx, sy, 300, 5, 24, 22)) { zp_app_commit_selection(app); zp_history_redo(&app->history, &app->canvas); return true; }
        if (pt_in(sx, sy, 326, 5, 24, 22)) { zp_app_save(app, false); return true; }
        return true;
    }

    if (sy < ZP_TOP_H) {
        /* clipboard */
        if (pt_in(sx, sy, 8,  38, 44, 54)) { zp_app_paste(app); return true; }
        if (pt_in(sx, sy, 56, 38, 52, 26)) { zp_app_cut(app); return true; }
        if (pt_in(sx, sy, 56, 66, 52, 26)) { zp_app_copy(app); return true; }

        /* image & selection */
        if (pt_in(sx, sy, 121, 38, 62, 26)) { app->tool = ZP_TOOL_SELECT_RECT; return true; }
        if (pt_in(sx, sy, 121, 66, 62, 26)) { app->tool = ZP_TOOL_SELECT_LASSO; return true; }
        if (pt_in(sx, sy, 187, 38, 56, 26)) { zp_app_crop_to_selection(app); return true; }
        if (pt_in(sx, sy, 245, 38, 66, 26)) {
            app->modal = ZP_MODAL_RESIZE_SKEW;
            snprintf(app->modal_input, sizeof(app->modal_input), "%dx%d", app->canvas.w, app->canvas.h);
            return true;
        }
        if (pt_in(sx, sy, 187, 66, 66, 26)) {
            app->transparent_selection = !app->transparent_selection;
            app->sel.transparent_bg = app->transparent_selection;
            return true;
        }
        if (pt_in(sx, sy, 255, 66, 62, 26)) {
            app->rotate_menu_open = !app->rotate_menu_open;
            return true;
        }

        /* 3x2 core tools */
        static const ZPTool TMAP[6] = {
            ZP_TOOL_PENCIL, ZP_TOOL_FILL, ZP_TOOL_TEXT,
            ZP_TOOL_ERASER, ZP_TOOL_EYEDROPPER, ZP_TOOL_MAGNIFIER
        };
        for (int i = 0; i < 6; ++i) {
            if (pt_in(sx, sy, 330 + (i % 3) * 30, 38 + (i / 3) * 28, 28, 26)) {
                zp_app_commit_selection(app);
                zp_app_commit_text(app);
                app->tool = TMAP[i];
                return true;
            }
        }

        /* brushes & size */
        if (pt_in(sx, sy, 431, 38, 60, 26)) { zp_app_commit_selection(app); zp_app_commit_text(app); app->tool = ZP_TOOL_BRUSH; return true; }
        if (pt_in(sx, sy, 431, 66, 60, 26)) { zp_app_commit_selection(app); zp_app_commit_text(app); app->tool = ZP_TOOL_AIRBRUSH; return true; }
        if (pt_in(sx, sy, 495, 38, 102, 25)) {
            app->brush_variant = (ZPBrushVariant)(((int)app->brush_variant + 1) % 4);
            if (app->tool != ZP_TOOL_BRUSH && app->tool != ZP_TOOL_PENCIL) app->tool = ZP_TOOL_BRUSH;
            return true;
        }
        static const int SIZES[4] = { 1, 3, 6, 12 };
        for (int i = 0; i < 4; ++i) {
            if (pt_in(sx, sy, 495 + i * 26, 67, 24, 24)) {
                app->stroke_size = SIZES[i];
                return true;
            }
        }

        /* shapes & fill mode */
        static const ZPTool SMAP[10] = {
            ZP_TOOL_LINE, ZP_TOOL_CURVE, ZP_TOOL_RECT, ZP_TOOL_ROUND_RECT, ZP_TOOL_ELLIPSE,
            ZP_TOOL_POLYGON, ZP_TOOL_TRIANGLE, ZP_TOOL_DIAMOND, ZP_TOOL_ARROW_RIGHT, ZP_TOOL_STAR
        };
        for (int i = 0; i < 10; ++i) {
            if (pt_in(sx, sy, 613 + (i % 5) * 26, 41 + (i / 5) * 25, 24, 23)) {
                zp_app_commit_selection(app);
                zp_app_commit_text(app);
                app->tool = SMAP[i];
                return true;
            }
        }
        for (int i = 0; i < 3; ++i) {
            if (pt_in(sx, sy, 748, 38 + i * 18, 48, 17)) {
                app->shape_mode = (ZPShapeMode)i;
                return true;
            }
        }

        /* colors */
        if (pt_in(sx, sy, 809, 38, 42, 54)) { app->active_slot = 1; return true; }
        if (pt_in(sx, sy, 855, 38, 42, 54)) { app->active_slot = 2; return true; }
        if (pt_in(sx, sy, 900, 38, 22, 54)) {
            uint32_t tmp = app->color1;
            app->color1 = app->color2;
            app->color2 = tmp;
            return true;
        }
        for (int i = 0; i < ZP_PALETTE_COUNT; ++i) {
            if (pt_in(sx, sy, 926 + (i % 14) * 19, 42 + (i / 14) * 21, 17, 17)) {
                if (button == 3 || app->active_slot == 2) app->color2 = app->palette[i];
                else app->color1 = app->palette[i];
                return true;
            }
        }
        return true;
    }

    /* contextual text bar clicks */
    if ((app->tool == ZP_TOOL_TEXT || app->text_active) && sy < zp_ui_top_h(app)) {
        int ty = ZP_TOP_H;
        if (pt_in(sx, sy, 44, ty + 3, 180, 23)) {
            app->font_menu_open = !app->font_menu_open;
            return true;
        }
        if (pt_in(sx, sy, 266, ty + 4, 22, 21)) {
            app->text_size_px -= 2;
            if (app->text_size_px < 8) app->text_size_px = 8;
            return true;
        }
        if (pt_in(sx, sy, 334, ty + 4, 22, 21)) {
            app->text_size_px += 2;
            if (app->text_size_px > 144) app->text_size_px = 144;
            return true;
        }
        static const int TSIZES[7] = { 12, 16, 20, 28, 36, 48, 64 };
        for (int i = 0; i < 7; ++i) {
            if (pt_in(sx, sy, 364 + i * 28, ty + 4, 26, 21)) {
                app->text_size_px = TSIZES[i];
                return true;
            }
        }
        if (pt_in(sx, sy, 574, ty + 4, 46, 21)) {
            app->text_bold = !app->text_bold;
            return true;
        }
        return true;
    }

    int sby = app->win_h - ZP_STATUSBAR_H;
    if (sy >= sby) {
        if (pt_in(sx, sy, app->win_w - 246, sby + 4, 56, 20)) { app->show_grid = !app->show_grid; return true; }
        if (pt_in(sx, sy, app->win_w - 132, sby + 4, 20, 20)) { zp_app_zoom_step(app, -1); return true; }
        if (pt_in(sx, sy, app->win_w - 28,  sby + 4, 20, 20)) { zp_app_zoom_step(app, 1); return true; }
        int track_x = app->win_w - 106;
        int track_w = 70;
        if (pt_in(sx, sy, track_x - 4, sby + 4, track_w + 8, 20)) {
            int rel = sx - track_x;
            if (rel < 0) rel = 0;
            if (rel > track_w) rel = track_w;
            int idx = (rel * (ZP_ZOOM_COUNT - 1) + track_w / 2) / track_w;
            if (idx < 0) idx = 0;
            if (idx >= ZP_ZOOM_COUNT) idx = ZP_ZOOM_COUNT - 1;
            app->zoom = ZP_ZOOM_LEVELS[idx];
            return true;
        }
        return true;
    }

    return false;
}
