/* zp_raster.c - software argb8888 drawing engine, flood fill, lasso and undo ring */
#include "zp_raster.h"
#include "zp_font.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int zp_min(int a, int b) { return a < b ? a : b; }
static int zp_max(int a, int b) { return a > b ? a : b; }
static int zp_clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* === image buffers === */
bool zp_img_init(ZPImage *img, int w, int h, uint32_t fill_color) {
    if (w < ZP_MIN_DIM || h < ZP_MIN_DIM || w > ZP_MAX_DIM || h > ZP_MAX_DIM) return false;
    size_t count = (size_t)w * (size_t)h;
    uint32_t *buf = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!buf) return false;
    img->w = w;
    img->h = h;
    img->pixels = buf;
    zp_img_fill(img, fill_color);
    return true;
}

void zp_img_free(ZPImage *img) {
    if (img->pixels) {
        free(img->pixels);
        img->pixels = NULL;
    }
    img->w = 0;
    img->h = 0;
}

bool zp_img_clone(const ZPImage *src, ZPImage *dst) {
    if (!src->pixels || src->w <= 0 || src->h <= 0) return false;
    size_t bytes = (size_t)src->w * (size_t)src->h * sizeof(uint32_t);
    uint32_t *buf = (uint32_t *)malloc(bytes);
    if (!buf) return false;
    memcpy(buf, src->pixels, bytes);
    dst->w = src->w;
    dst->h = src->h;
    dst->pixels = buf;
    return true;
}

void zp_img_fill(ZPImage *img, uint32_t color) {
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; ++i) {
        img->pixels[i] = color;
    }
}

uint32_t zp_img_get(const ZPImage *img, int x, int y) {
    if (!img->pixels || x < 0 || y < 0 || x >= img->w || y >= img->h) return ZP_COL_WHITE;
    return img->pixels[(size_t)y * (size_t)img->w + (size_t)x];
}

void zp_img_put(ZPImage *img, int x, int y, uint32_t color) {
    if (!img->pixels || x < 0 || y < 0 || x >= img->w || y >= img->h) return;
    img->pixels[(size_t)y * (size_t)img->w + (size_t)x] = color;
}

/* === brushes and strokes === */
void zp_draw_disk(ZPImage *img, int cx, int cy, int diameter, uint32_t color) {
    if (diameter <= 1) {
        zp_img_put(img, cx, cy, color);
        return;
    }
    int r = diameter / 2;
    int r2 = r * r;
    int x0 = zp_max(0, cx - r);
    int x1 = zp_min(img->w - 1, cx + r);
    int y0 = zp_max(0, cy - r);
    int y1 = zp_min(img->h - 1, cy + r);
    for (int y = y0; y <= y1; ++y) {
        int dy = y - cy;
        uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
        for (int x = x0; x <= x1; ++x) {
            int dx = x - cx;
            if (dx * dx + dy * dy <= r2) {
                row[x] = color;
            }
        }
    }
}

void zp_draw_calligraphy(ZPImage *img, int cx, int cy, int size, uint32_t color) {
    int half = zp_max(1, size / 2);
    for (int i = -half; i <= half; ++i) {
        zp_img_put(img, cx + i, cy - i, color);
        zp_img_put(img, cx + i + 1, cy - i, color);
    }
}

static void zp_draw_square_stamp(ZPImage *img, int cx, int cy, int size, uint32_t color) {
    int half = size / 2;
    int x0 = zp_max(0, cx - half);
    int y0 = zp_max(0, cy - half);
    int x1 = zp_min(img->w - 1, cx - half + size - 1);
    int y1 = zp_min(img->h - 1, cy - half + size - 1);
    for (int y = y0; y <= y1; ++y) {
        uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
        for (int x = x0; x <= x1; ++x) {
            row[x] = color;
        }
    }
}

/* integer bresenham so 1px lines stay sharp without blurry halos */
void zp_draw_line(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t color, ZPBrushVariant variant) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        if (variant == ZP_BRUSH_CALLIGRAPHY) {
            zp_draw_calligraphy(img, x0, y0, size, color);
        } else if (variant == ZP_BRUSH_MARKER) {
            zp_draw_square_stamp(img, x0, y0, size, color);
        } else {
            zp_draw_disk(img, x0, y0, size, color);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static uint32_t zp_xorshift32(uint32_t *state) {
    uint32_t x = *state ? *state : 0xA5A51234u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

void zp_draw_airbrush(ZPImage *img, int cx, int cy, int radius, uint32_t color, uint32_t *rng_state) {
    if (radius < 3) radius = 3;
    int dots = radius * 2;
    for (int i = 0; i < dots; ++i) {
        int rx = (int)(zp_xorshift32(rng_state) % (uint32_t)(radius * 2 + 1)) - radius;
        int ry = (int)(zp_xorshift32(rng_state) % (uint32_t)(radius * 2 + 1)) - radius;
        if (rx * rx + ry * ry <= radius * radius) {
            zp_img_put(img, cx + rx, cy + ry, color);
        }
    }
}

/* === shapes and polygons === */
void zp_draw_rect(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode) {
    int lx = zp_min(x0, x1), rx = zp_max(x0, x1);
    int ty = zp_min(y0, y1), by = zp_max(y0, y1);
    if (mode == ZP_SHAPE_FILL || mode == ZP_SHAPE_BOTH) {
        int fx0 = zp_max(0, lx), fx1 = zp_min(img->w - 1, rx);
        int fy0 = zp_max(0, ty), fy1 = zp_min(img->h - 1, by);
        uint32_t fc = (mode == ZP_SHAPE_FILL) ? outline : fill;
        for (int y = fy0; y <= fy1; ++y) {
            uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
            for (int x = fx0; x <= fx1; ++x) row[x] = fc;
        }
    }
    if (mode == ZP_SHAPE_OUTLINE || mode == ZP_SHAPE_BOTH) {
        zp_draw_line(img, lx, ty, rx, ty, size, outline, ZP_BRUSH_PIXEL);
        zp_draw_line(img, rx, ty, rx, by, size, outline, ZP_BRUSH_PIXEL);
        zp_draw_line(img, rx, by, lx, by, size, outline, ZP_BRUSH_PIXEL);
        zp_draw_line(img, lx, by, lx, ty, size, outline, ZP_BRUSH_PIXEL);
    }
}

void zp_draw_round_rect(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode) {
    int lx = zp_min(x0, x1), rx = zp_max(x0, x1);
    int ty = zp_min(y0, y1), by = zp_max(y0, y1);
    int w = rx - lx, h = by - ty;
    int r = zp_min(zp_min(w, h) / 4, 24);
    if (r < 2) {
        zp_draw_rect(img, lx, ty, rx, by, size, outline, fill, mode);
        return;
    }
    ZPPoint pts[40];
    int n = 0;
    const double pi2 = 1.57079632679;
    int cx[4] = { rx - r, lx + r, lx + r, rx - r };
    int cy[4] = { by - r, by - r, ty + r, ty + r };
    double a0[4] = { 0.0, pi2, pi2 * 2.0, pi2 * 3.0 };
    for (int c = 0; c < 4; ++c) {
        for (int s = 0; s < 10; ++s) {
            double ang = a0[c] + pi2 * ((double)s / 9.0);
            pts[n].x = cx[c] + (int)lround(cos(ang) * r);
            pts[n].y = cy[c] + (int)lround(sin(ang) * r);
            n++;
        }
    }
    zp_draw_polygon(img, pts, n, size, outline, fill, mode);
}

void zp_draw_ellipse(ZPImage *img, int x0, int y0, int x1, int y1, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode) {
    int lx = zp_min(x0, x1), rx = zp_max(x0, x1);
    int ty = zp_min(y0, y1), by = zp_max(y0, y1);
    double cx = (lx + rx) * 0.5;
    double cy = (ty + by) * 0.5;
    double a = (rx - lx) * 0.5;
    double b = (by - ty) * 0.5;
    if (a < 0.5 || b < 0.5) {
        zp_draw_line(img, lx, ty, rx, by, size, outline, ZP_BRUSH_PIXEL);
        return;
    }
    if (mode == ZP_SHAPE_FILL || mode == ZP_SHAPE_BOTH) {
        uint32_t fc = (mode == ZP_SHAPE_FILL) ? outline : fill;
        int fy0 = zp_max(0, ty), fy1 = zp_min(img->h - 1, by);
        int fx0 = zp_max(0, lx), fx1 = zp_min(img->w - 1, rx);
        for (int y = fy0; y <= fy1; ++y) {
            double dy = (y - cy) / b;
            double dy2 = dy * dy;
            if (dy2 > 1.0) continue;
            uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
            for (int x = fx0; x <= fx1; ++x) {
                double dx = (x - cx) / a;
                if (dx * dx + dy2 <= 1.0) row[x] = fc;
            }
        }
    }
    if (mode == ZP_SHAPE_OUTLINE || mode == ZP_SHAPE_BOTH) {
        int steps = zp_max(32, (int)((a + b) * 4.0));
        if (steps > 720) steps = 720;
        int px = (int)lround(cx + a);
        int py = (int)lround(cy);
        for (int i = 1; i <= steps; ++i) {
            double t = (6.28318530718 * i) / steps;
            int nx = (int)lround(cx + cos(t) * a);
            int ny = (int)lround(cy + sin(t) * b);
            zp_draw_line(img, px, py, nx, ny, size, outline, ZP_BRUSH_PIXEL);
            px = nx;
            py = ny;
        }
    }
}

static void zp_fill_polygon_scanline(ZPImage *img, const ZPPoint *pts, int count, uint32_t color) {
    if (count < 3) return;
    int min_y = pts[0].y, max_y = pts[0].y;
    for (int i = 1; i < count; ++i) {
        if (pts[i].y < min_y) min_y = pts[i].y;
        if (pts[i].y > max_y) max_y = pts[i].y;
    }
    min_y = zp_max(0, min_y);
    max_y = zp_min(img->h - 1, max_y);

    int *nodes = (int *)malloc((size_t)count * sizeof(int));
    if (!nodes) return;

    for (int y = min_y; y <= max_y; ++y) {
        int n = 0;
        int j = count - 1;
        for (int i = 0; i < count; ++i) {
            if ((pts[i].y < y && pts[j].y >= y) || (pts[j].y < y && pts[i].y >= y)) {
                double ix = pts[i].x + (double)(y - pts[i].y) / (double)(pts[j].y - pts[i].y) * (pts[j].x - pts[i].x);
                nodes[n++] = (int)lround(ix);
            }
            j = i;
        }
        /* tiny insertion sort is faster than qsort for usual poly counts */
        for (int i = 1; i < n; ++i) {
            int key = nodes[i], k = i - 1;
            while (k >= 0 && nodes[k] > key) { nodes[k + 1] = nodes[k]; k--; }
            nodes[k + 1] = key;
        }
        uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
        for (int i = 0; i + 1 < n; i += 2) {
            int x0 = zp_max(0, nodes[i]);
            int x1 = zp_min(img->w - 1, nodes[i + 1]);
            for (int x = x0; x <= x1; ++x) row[x] = color;
        }
    }
    free(nodes);
}

void zp_draw_polygon(ZPImage *img, const ZPPoint *pts, int count, int size, uint32_t outline, uint32_t fill, ZPShapeMode mode) {
    if (count < 2) return;
    if ((mode == ZP_SHAPE_FILL || mode == ZP_SHAPE_BOTH) && count >= 3) {
        uint32_t fc = (mode == ZP_SHAPE_FILL) ? outline : fill;
        zp_fill_polygon_scanline(img, pts, count, fc);
    }
    if (mode == ZP_SHAPE_OUTLINE || mode == ZP_SHAPE_BOTH) {
        for (int i = 0; i < count; ++i) {
            int next = (i + 1) % count;
            zp_draw_line(img, pts[i].x, pts[i].y, pts[next].x, pts[next].y, size, outline, ZP_BRUSH_PIXEL);
        }
    }
}

void zp_draw_bezier(ZPImage *img, ZPPoint p0, ZPPoint p1, ZPPoint p2, ZPPoint p3, int size, uint32_t color) {
    const int steps = 96;
    int px = p0.x, py = p0.y;
    for (int i = 1; i <= steps; ++i) {
        double t = (double)i / steps;
        double u = 1.0 - t;
        double b0 = u * u * u;
        double b1 = 3.0 * u * u * t;
        double b2 = 3.0 * u * t * t;
        double b3 = t * t * t;
        int nx = (int)lround(b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x);
        int ny = (int)lround(b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y);
        zp_draw_line(img, px, py, nx, ny, size, color, ZP_BRUSH_PIXEL);
        px = nx;
        py = ny;
    }
}

void zp_draw_shape(ZPImage *img, ZPTool tool, int x0, int y0, int x1, int y1, int size, uint32_t c1, uint32_t c2, ZPShapeMode mode) {
    int lx = zp_min(x0, x1), rx = zp_max(x0, x1);
    int ty = zp_min(y0, y1), by = zp_max(y0, y1);
    int mx = (lx + rx) / 2, my = (ty + by) / 2;

    switch (tool) {
        case ZP_TOOL_LINE:
            zp_draw_line(img, x0, y0, x1, y1, size, c1, ZP_BRUSH_PIXEL);
            break;
        case ZP_TOOL_RECT:
            zp_draw_rect(img, lx, ty, rx, by, size, c1, c2, mode);
            break;
        case ZP_TOOL_ROUND_RECT:
            zp_draw_round_rect(img, lx, ty, rx, by, size, c1, c2, mode);
            break;
        case ZP_TOOL_ELLIPSE:
            zp_draw_ellipse(img, lx, ty, rx, by, size, c1, c2, mode);
            break;
        case ZP_TOOL_TRIANGLE: {
            ZPPoint pts[3] = { { mx, ty }, { rx, by }, { lx, by } };
            zp_draw_polygon(img, pts, 3, size, c1, c2, mode);
            break;
        }
        case ZP_TOOL_DIAMOND: {
            ZPPoint pts[4] = { { mx, ty }, { rx, my }, { mx, by }, { lx, my } };
            zp_draw_polygon(img, pts, 4, size, c1, c2, mode);
            break;
        }
        case ZP_TOOL_ARROW_RIGHT: {
            int bw = (rx - lx) / 2;
            int bh = (by - ty) / 4;
            ZPPoint pts[7] = {
                { lx, ty + bh }, { lx + bw, ty + bh }, { lx + bw, ty },
                { rx, my }, { lx + bw, by }, { lx + bw, by - bh }, { lx, by - bh }
            };
            zp_draw_polygon(img, pts, 7, size, c1, c2, mode);
            break;
        }
        case ZP_TOOL_STAR: {
            ZPPoint pts[10];
            double rxr = (rx - lx) * 0.5, ryr = (by - ty) * 0.5;
            for (int i = 0; i < 10; ++i) {
                double ang = -1.57079632679 + (6.28318530718 * i) / 10.0;
                double k = (i % 2 == 0) ? 1.0 : 0.42;
                pts[i].x = mx + (int)lround(cos(ang) * rxr * k);
                pts[i].y = my + (int)lround(sin(ang) * ryr * k);
            }
            zp_draw_polygon(img, pts, 10, size, c1, c2, mode);
            break;
        }
        default:
            break;
    }
}

static void zp_blend_px(ZPImage *img, int x, int y, uint32_t fg, uint8_t alpha) {
    if (alpha == 0 || !img->pixels || x < 0 || y < 0 || x >= img->w || y >= img->h) return;
    if (alpha == 255) {
        img->pixels[(size_t)y * (size_t)img->w + (size_t)x] = fg;
        return;
    }
    uint32_t dst = img->pixels[(size_t)y * (size_t)img->w + (size_t)x];
    uint32_t a = alpha;
    uint32_t inv = 255u - a;
    uint32_t r = (ZP_R(fg) * a + ZP_R(dst) * inv + 127u) / 255u;
    uint32_t g = (ZP_G(fg) * a + ZP_G(dst) * inv + 127u) / 255u;
    uint32_t b = (ZP_B(fg) * a + ZP_B(dst) * inv + 127u) / 255u;
    img->pixels[(size_t)y * (size_t)img->w + (size_t)x] = ZP_RGB(r, g, b);
}

static uint32_t zp_utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)(*p);
    if (!*s) return 0;
    if (s[0] < 0x80) { *p += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *p += 2;
        return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *p += 4;
        return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    }
    *p += 1;
    return '?';
}

int zp_text_width_font(const char *text, int font_kind, int scale) {
    if (!text || scale < 1) return 0;
    if (font_kind < 0 || font_kind > 2) font_kind = 0;
    const ZPFont *f = &ZP_FONTS[font_kind];
    int max_w = 0, cur = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = zp_utf8_next(&p);
        if (cp == '\n') {
            if (cur > max_w) max_w = cur;
            cur = 0;
            continue;
        }
        int idx = zp_font_glyph_idx(cp);
        if (idx < 0) cur += 5 * scale;
        else cur += (int)f->glyphs[idx].adv * scale;
    }
    return cur > max_w ? cur : max_w;
}

int zp_text_width(const char *text, int scale) {
    return zp_text_width_font(text, ZP_FONT_REG, scale);
}

void zp_draw_text_font(ZPImage *img, int x, int y, const char *text, int font_kind, int scale, uint32_t fg) {
    if (!text || scale < 1) return;
    if (font_kind < 0 || font_kind > 2) font_kind = 0;
    const ZPFont *f = &ZP_FONTS[font_kind];
    int cx = x, cy = y;
    const char *p = text;
    while (*p) {
        uint32_t cp = zp_utf8_next(&p);
        if (cp == '\n') {
            cx = x;
            cy += (int)f->line_h * scale;
            continue;
        }
        int idx = zp_font_glyph_idx(cp);
        if (idx < 0) {
            cx += 5 * scale;
            continue;
        }
        const ZPGlyph *g = &f->glyphs[idx];
        const uint8_t *src = f->alpha + g->off;
        for (int row = 0; row < g->h; ++row) {
            for (int col = 0; col < g->w; ++col) {
                uint8_t a = src[row * g->w + col];
                if (!a) continue;
                int bx = cx + (g->xoff + col) * scale;
                int by = cy + (g->yoff + row) * scale;
                if (scale == 1) {
                    zp_blend_px(img, bx, by, fg, a);
                } else {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            zp_blend_px(img, bx + sx, by + sy, fg, a);
                        }
                    }
                }
            }
        }
        cx += (int)g->adv * scale;
    }
}

void zp_draw_text(ZPImage *img, int x, int y, const char *text, int scale, uint32_t fg) {
    zp_draw_text_font(img, x, y, text, ZP_FONT_REG, scale, fg);
}

/* === scanline flood fill === */
/* iterative heap stack so huge canvases never blow the call stack */
void zp_flood_fill(ZPImage *img, int sx, int sy, uint32_t fill_color) {
    if (!img->pixels || sx < 0 || sy < 0 || sx >= img->w || sy >= img->h) return;
    uint32_t target = zp_img_get(img, sx, sy);
    if (target == fill_color) return;

    size_t cap = (size_t)img->w * 4 + 1024;
    int *stack_x = (int *)malloc(cap * sizeof(int));
    int *stack_y = (int *)malloc(cap * sizeof(int));
    if (!stack_x || !stack_y) {
        free(stack_x);
        free(stack_y);
        return;
    }

    size_t sp = 0;
    stack_x[sp] = sx;
    stack_y[sp] = sy;
    sp++;

    while (sp > 0) {
        sp--;
        int x = stack_x[sp];
        int y = stack_y[sp];
        uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
        if (row[x] != target) continue;

        int lx = x;
        while (lx > 0 && row[lx - 1] == target) lx--;
        int rx = x;
        while (rx + 1 < img->w && row[rx + 1] == target) rx++;

        for (int i = lx; i <= rx; ++i) row[i] = fill_color;

        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = y + dir;
            if (ny < 0 || ny >= img->h) continue;
            uint32_t *nrow = img->pixels + (size_t)ny * (size_t)img->w;
            bool in_span = false;
            for (int i = lx; i <= rx; ++i) {
                if (nrow[i] == target) {
                    if (!in_span) {
                        if (sp >= cap) {
                            size_t ncap = cap * 2;
                            int *nx = (int *)realloc(stack_x, ncap * sizeof(int));
                            int *ny_buf = (int *)realloc(stack_y, ncap * sizeof(int));
                            if (!nx || !ny_buf) {
                                free(nx ? nx : stack_x);
                                free(ny_buf ? ny_buf : stack_y);
                                return;
                            }
                            stack_x = nx;
                            stack_y = ny_buf;
                            cap = ncap;
                        }
                        stack_x[sp] = i;
                        stack_y[sp] = ny;
                        sp++;
                        in_span = true;
                    }
                } else {
                    in_span = false;
                }
            }
        }
    }
    free(stack_x);
    free(stack_y);
}

/* === image transforms === */
bool zp_canvas_resize_borders(ZPImage *img, int new_w, int new_h, uint32_t bg_color) {
    ZPImage next;
    if (!zp_img_init(&next, new_w, new_h, bg_color)) return false;
    int cw = zp_min(img->w, new_w);
    int ch = zp_min(img->h, new_h);
    for (int y = 0; y < ch; ++y) {
        memcpy(next.pixels + (size_t)y * (size_t)new_w,
               img->pixels + (size_t)y * (size_t)img->w,
               (size_t)cw * sizeof(uint32_t));
    }
    zp_img_free(img);
    *img = next;
    return true;
}

bool zp_img_scale_nearest(const ZPImage *src, ZPImage *dst, int new_w, int new_h) {
    if (!zp_img_init(dst, new_w, new_h, ZP_COL_WHITE)) return false;
    for (int y = 0; y < new_h; ++y) {
        int sy = (int)((int64_t)y * src->h / new_h);
        const uint32_t *srow = src->pixels + (size_t)sy * (size_t)src->w;
        uint32_t *drow = dst->pixels + (size_t)y * (size_t)new_w;
        for (int x = 0; x < new_w; ++x) {
            int sx = (int)((int64_t)x * src->w / new_w);
            drow[x] = srow[sx];
        }
    }
    return true;
}

bool zp_img_skew(const ZPImage *src, ZPImage *dst, int deg_h, int deg_v, uint32_t bg_color) {
    double th = tan((double)zp_clamp(deg_h, -60, 60) * 3.1415926535 / 180.0);
    double tv = tan((double)zp_clamp(deg_v, -60, 60) * 3.1415926535 / 180.0);
    int extra_w = (int)ceil(fabs(th) * src->h);
    int extra_h = (int)ceil(fabs(tv) * src->w);
    int nw = zp_clamp(src->w + extra_w, 1, ZP_MAX_DIM);
    int nh = zp_clamp(src->h + extra_h, 1, ZP_MAX_DIM);
    if (!zp_img_init(dst, nw, nh, bg_color)) return false;
    int ox = th < 0 ? extra_w : 0;
    int oy = tv < 0 ? extra_h : 0;
    for (int y = 0; y < src->h; ++y) {
        for (int x = 0; x < src->w; ++x) {
            int dx = ox + x + (int)lround(th * y);
            int dy = oy + y + (int)lround(tv * x);
            zp_img_put(dst, dx, dy, zp_img_get(src, x, y));
        }
    }
    return true;
}

bool zp_img_rotate90(const ZPImage *src, ZPImage *dst, bool clockwise) {
    if (!zp_img_init(dst, src->h, src->w, ZP_COL_WHITE)) return false;
    for (int y = 0; y < src->h; ++y) {
        for (int x = 0; x < src->w; ++x) {
            uint32_t c = zp_img_get(src, x, y);
            if (clockwise) {
                zp_img_put(dst, src->h - 1 - y, x, c);
            } else {
                zp_img_put(dst, y, src->w - 1 - x, c);
            }
        }
    }
    return true;
}

void zp_img_flip_h(ZPImage *img) {
    for (int y = 0; y < img->h; ++y) {
        uint32_t *row = img->pixels + (size_t)y * (size_t)img->w;
        for (int x = 0; x < img->w / 2; ++x) {
            uint32_t tmp = row[x];
            row[x] = row[img->w - 1 - x];
            row[img->w - 1 - x] = tmp;
        }
    }
}

void zp_img_flip_v(ZPImage *img) {
    size_t row_bytes = (size_t)img->w * sizeof(uint32_t);
    uint32_t *tmp = (uint32_t *)malloc(row_bytes);
    if (!tmp) return;
    for (int y = 0; y < img->h / 2; ++y) {
        uint32_t *top = img->pixels + (size_t)y * (size_t)img->w;
        uint32_t *bot = img->pixels + (size_t)(img->h - 1 - y) * (size_t)img->w;
        memcpy(tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, tmp, row_bytes);
    }
    free(tmp);
}

void zp_img_invert(ZPImage *img) {
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = img->pixels[i];
        img->pixels[i] = ZP_ARGB(ZP_A(c), 255u - ZP_R(c), 255u - ZP_G(c), 255u - ZP_B(c));
    }
}

void zp_img_grayscale(ZPImage *img) {
    size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = img->pixels[i];
        uint32_t l = (ZP_R(c) * 77u + ZP_G(c) * 150u + ZP_B(c) * 29u) >> 8;
        img->pixels[i] = ZP_ARGB(ZP_A(c), l, l, l);
    }
}

/* === selection and lasso === */
void zp_sel_end_transform(ZPSelection *sel) {
    zp_img_free(&sel->orig_buf);
    if (sel->orig_mask) {
        free(sel->orig_mask);
        sel->orig_mask = NULL;
    }
    sel->transform_handle = -1;
}

void zp_sel_clear(ZPSelection *sel) {
    zp_sel_end_transform(sel);
    zp_img_free(&sel->buf);
    if (sel->mask) {
        free(sel->mask);
        sel->mask = NULL;
    }
    sel->active = false;
    sel->dragging = false;
    sel->transform_handle = -1;
}

bool zp_sel_begin_transform(ZPSelection *sel, int handle) {
    if (!sel->active || !sel->buf.pixels) return false;
    zp_sel_end_transform(sel);
    if (!zp_img_clone(&sel->buf, &sel->orig_buf)) return false;
    if (sel->mask) {
        size_t len = (size_t)sel->buf.w * (size_t)sel->buf.h;
        sel->orig_mask = (uint8_t *)malloc(len);
        if (sel->orig_mask) memcpy(sel->orig_mask, sel->mask, len);
    }
    sel->orig_x = sel->x;
    sel->orig_y = sel->y;
    sel->orig_w = sel->buf.w;
    sel->orig_h = sel->buf.h;
    sel->transform_handle = handle;
    return true;
}

void zp_sel_apply_transform(ZPSelection *sel, int new_x, int new_y, int new_w, int new_h) {
    if (!sel->active || !sel->orig_buf.pixels) return;
    new_w = zp_clamp(new_w, 1, ZP_MAX_DIM);
    new_h = zp_clamp(new_h, 1, ZP_MAX_DIM);
    ZPImage scaled;
    if (!zp_img_scale_nearest(&sel->orig_buf, &scaled, new_w, new_h)) return;
    zp_img_free(&sel->buf);
    sel->buf = scaled;
    sel->x = new_x;
    sel->y = new_y;

    if (sel->orig_mask) {
        uint8_t *sm = (uint8_t *)malloc((size_t)new_w * (size_t)new_h);
        if (sm) {
            for (int y = 0; y < new_h; ++y) {
                int sy = (int)((int64_t)y * sel->orig_h / new_h);
                const uint8_t *srow = sel->orig_mask + (size_t)sy * (size_t)sel->orig_w;
                uint8_t *drow = sm + (size_t)y * (size_t)new_w;
                for (int x = 0; x < new_w; ++x) {
                    int sx = (int)((int64_t)x * sel->orig_w / new_w);
                    drow[x] = srow[sx];
                }
            }
            free(sel->mask);
            sel->mask = sm;
        }
    }
}

bool zp_sel_extract_rect(ZPImage *canvas, int x0, int y0, int x1, int y1, uint32_t bg_color, ZPSelection *sel, bool transparent_bg) {
    zp_sel_clear(sel);
    int lx = zp_clamp(zp_min(x0, x1), 0, canvas->w - 1);
    int rx = zp_clamp(zp_max(x0, x1), 0, canvas->w - 1);
    int ty = zp_clamp(zp_min(y0, y1), 0, canvas->h - 1);
    int by = zp_clamp(zp_max(y0, y1), 0, canvas->h - 1);
    int w = rx - lx + 1;
    int h = by - ty + 1;
    if (w < 1 || h < 1) return false;
    if (!zp_img_init(&sel->buf, w, h, bg_color)) return false;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint32_t c = zp_img_get(canvas, lx + x, ty + y);
            zp_img_put(&sel->buf, x, y, c);
            zp_img_put(canvas, lx + x, ty + y, bg_color);
        }
    }
    sel->active = true;
    sel->x = lx;
    sel->y = ty;
    sel->mask = NULL;
    sel->transparent_bg = transparent_bg;
    sel->key_color = bg_color;
    return true;
}

/* rasterize free-form lasso polygon into a byte mask and lift pixels out */
bool zp_sel_extract_lasso(ZPImage *canvas, const ZPPoint *pts, int count, uint32_t bg_color, ZPSelection *sel, bool transparent_bg) {
    zp_sel_clear(sel);
    if (count < 3) return false;
    int lx = pts[0].x, rx = pts[0].x, ty = pts[0].y, by = pts[0].y;
    for (int i = 1; i < count; ++i) {
        if (pts[i].x < lx) lx = pts[i].x;
        if (pts[i].x > rx) rx = pts[i].x;
        if (pts[i].y < ty) ty = pts[i].y;
        if (pts[i].y > by) by = pts[i].y;
    }
    lx = zp_clamp(lx, 0, canvas->w - 1);
    rx = zp_clamp(rx, 0, canvas->w - 1);
    ty = zp_clamp(ty, 0, canvas->h - 1);
    by = zp_clamp(by, 0, canvas->h - 1);
    int w = rx - lx + 1;
    int h = by - ty + 1;
    if (w < 2 || h < 2) return false;

    uint8_t *mask = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
    if (!mask) return false;
    if (!zp_img_init(&sel->buf, w, h, bg_color)) {
        free(mask);
        return false;
    }

    int *nodes = (int *)malloc((size_t)count * sizeof(int));
    if (!nodes) {
        free(mask);
        zp_img_free(&sel->buf);
        return false;
    }
    bool any_px = false;
    for (int y = ty; y <= by; ++y) {
        int n = 0, j = count - 1;
        for (int i = 0; i < count; ++i) {
            if ((pts[i].y < y && pts[j].y >= y) || (pts[j].y < y && pts[i].y >= y)) {
                double ix = pts[i].x + (double)(y - pts[i].y) / (double)(pts[j].y - pts[i].y) * (pts[j].x - pts[i].x);
                nodes[n++] = (int)lround(ix);
            }
            j = i;
        }
        for (int i = 1; i < n; ++i) {
            int key = nodes[i], k = i - 1;
            while (k >= 0 && nodes[k] > key) { nodes[k + 1] = nodes[k]; k--; }
            nodes[k + 1] = key;
        }
        for (int i = 0; i + 1 < n; i += 2) {
            int sx0 = zp_max(lx, nodes[i]);
            int sx1 = zp_min(rx, nodes[i + 1]);
            for (int x = sx0; x <= sx1; ++x) {
                int mx = x - lx, my = y - ty;
                mask[(size_t)my * (size_t)w + (size_t)mx] = 1;
                zp_img_put(&sel->buf, mx, my, zp_img_get(canvas, x, y));
                zp_img_put(canvas, x, y, bg_color);
                any_px = true;
            }
        }
    }
    free(nodes);

    if (!any_px) {
        free(mask);
        zp_img_free(&sel->buf);
        return false;
    }

    sel->active = true;
    sel->x = lx;
    sel->y = ty;
    sel->mask = mask;
    sel->transparent_bg = transparent_bg;
    sel->key_color = bg_color;
    return true;
}

void zp_sel_blit(ZPImage *dst, const ZPSelection *sel) {
    if (!sel->active || !sel->buf.pixels) return;
    for (int y = 0; y < sel->buf.h; ++y) {
        for (int x = 0; x < sel->buf.w; ++x) {
            if (sel->mask && !sel->mask[(size_t)y * (size_t)sel->buf.w + (size_t)x]) continue;
            uint32_t c = zp_img_get(&sel->buf, x, y);
            if (sel->transparent_bg && c == sel->key_color) continue;
            zp_img_put(dst, sel->x + x, sel->y + y, c);
        }
    }
}

/* === ram-capped undo history === */
void zp_history_init(ZPHistory *h, size_t max_mb) {
    memset(h, 0, sizeof(*h));
    h->index = -1;
    h->max_bytes = (max_mb < 8 ? 8 : max_mb) * 1024u * 1024u;
}

void zp_history_free(ZPHistory *h) {
    for (int i = 0; i < h->count; ++i) {
        zp_img_free(&h->frames[i]);
    }
    h->count = 0;
    h->index = -1;
    h->total_bytes = 0;
}

void zp_history_push(ZPHistory *h, const ZPImage *img) {
    for (int i = h->index + 1; i < h->count; ++i) {
        zp_img_free(&h->frames[i]);
        h->total_bytes -= h->bytes[i];
        h->bytes[i] = 0;
    }
    h->count = h->index + 1;

    ZPImage snap;
    if (!zp_img_clone(img, &snap)) return;
    size_t sz = (size_t)img->w * (size_t)img->h * sizeof(uint32_t);

    /* drop oldest snaps when over ram cap so low-mem machines dont swap */
    while (h->count >= ZP_MAX_HISTORY || (h->count >= ZP_MIN_HISTORY && h->total_bytes + sz > h->max_bytes)) {
        zp_img_free(&h->frames[0]);
        h->total_bytes -= h->bytes[0];
        memmove(&h->frames[0], &h->frames[1], (size_t)(h->count - 1) * sizeof(ZPImage));
        memmove(&h->bytes[0], &h->bytes[1], (size_t)(h->count - 1) * sizeof(size_t));
        h->count--;
        if (h->index > 0) h->index--;
    }

    h->frames[h->count] = snap;
    h->bytes[h->count] = sz;
    h->total_bytes += sz;
    h->index = h->count;
    h->count++;
}

bool zp_history_undo(ZPHistory *h, ZPImage *canvas) {
    if (h->index <= 0) return false;
    h->index--;
    ZPImage restored;
    if (!zp_img_clone(&h->frames[h->index], &restored)) return false;
    zp_img_free(canvas);
    *canvas = restored;
    return true;
}

bool zp_history_redo(ZPHistory *h, ZPImage *canvas) {
    if (h->index + 1 >= h->count) return false;
    h->index++;
    ZPImage restored;
    if (!zp_img_clone(&h->frames[h->index], &restored)) return false;
    zp_img_free(canvas);
    *canvas = restored;
    return true;
}
