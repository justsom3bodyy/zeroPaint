/* zp_ttf.c - optional dlopen(libfreetype.so.6) system ttf/otf text engine
 * keeps compile deps at zero while giving crisp antialiassed russian + latin text */
#include "zp_ttf.h"
#include "zp_raster.h"
#include "zp_font.h"
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* === minimal abi-stable freetype 2 structs === */
/* these layouts have been frozen since freetype 2.0 on both 32-bit and 64-bit linux */
typedef struct { long x; long y; } ZpFtVec;
typedef struct {
    unsigned int rows;
    unsigned int width;
    int pitch;
    unsigned char *buffer;
    unsigned short num_grays;
    unsigned char pixel_mode;
    unsigned char palette_mode;
    void *palette;
} ZpFtBitmap;

typedef struct {
    long width, height;
    long horiBearingX, horiBearingY, horiAdvance;
    long vertBearingX, vertBearingY, vertAdvance;
} ZpFtMetrics;

typedef struct {
    void *library;
    void *face;
    void *next;
    unsigned int glyph_index;
    void *generic_data;
    void *generic_finalizer;
    ZpFtMetrics metrics;
    long linearHoriAdvance;
    long linearVertAdvance;
    ZpFtVec advance;
    unsigned int format;
    ZpFtBitmap bitmap;
    int bitmap_left;
    int bitmap_top;
} ZpFtGlyphSlot;

typedef struct { void *data; void *finalizer; } ZpFtGeneric;
typedef struct { long xMin, yMin, xMax, yMax; } ZpFtBBox;

typedef struct {
    long num_faces;
    long face_index;
    long face_flags;
    long style_flags;
    long num_glyphs;
    const char *family_name;
    const char *style_name;
    int num_fixed_sizes;
    void *available_sizes;
    int num_charmaps;
    void *charmaps;
    ZpFtGeneric generic;
    ZpFtBBox bbox;
    unsigned short units_per_EM;
    short ascender;
    short descender;
    short height;
    short max_advance_width;
    short max_advance_height;
    short underline_position;
    short underline_thickness;
    ZpFtGlyphSlot *glyph;
} ZpFtFace;

typedef int (*pfn_FT_Init)(void **);
typedef int (*pfn_FT_Done)(void *);
typedef int (*pfn_FT_NewFace)(void *, const char *, long, void **);
typedef int (*pfn_FT_DoneFace)(void *);
typedef int (*pfn_FT_SetPx)(void *, unsigned int, unsigned int);
typedef int (*pfn_FT_LoadChar)(void *, unsigned long, int);

#define ZP_FT_LOAD_RENDER       4
#define ZP_FT_LOAD_TARGET_LIGHT (1 << 16)

typedef struct {
    char name[64];
    char regular_path[512];
    char bold_path[512];
    int priority;
} ZpSysFont;

static void *g_dl = NULL;
static void *g_ft = NULL;
static pfn_FT_Init     g_ft_init = NULL;
static pfn_FT_Done     g_ft_done = NULL;
static pfn_FT_NewFace  g_ft_new_face = NULL;
static pfn_FT_DoneFace g_ft_done_face = NULL;
static pfn_FT_SetPx    g_ft_set_px = NULL;
static pfn_FT_LoadChar g_ft_load_char = NULL;

static ZpSysFont g_fonts[ZP_MAX_SYS_FONTS];
static int g_font_count = 0;

/* cached active face so we dont reopen disk files on every frame */
static ZpFtFace *g_cached_face = NULL;
static int g_cached_idx = -1;
static bool g_cached_bold = false;
static int g_cached_px = 0;

/* === utf-8 decoder === */
static uint32_t next_utf8(const char **p) {
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

/* === system font scanner === */
static bool has_font_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 5) return false;
    const char *ext = name + n - 4;
    return (tolower((unsigned char)ext[0]) == '.' &&
            ((tolower((unsigned char)ext[1]) == 't' && tolower((unsigned char)ext[2]) == 't' && tolower((unsigned char)ext[3]) == 'f') ||
             (tolower((unsigned char)ext[1]) == 'o' && tolower((unsigned char)ext[2]) == 't' && tolower((unsigned char)ext[3]) == 'f')));
}

static bool ci_contains(const char *hay, const char *needle) {
    if (!hay || !needle) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

static int font_priority(const char *fam) {
    if (ci_contains(fam, "Liberation Sans") && !ci_contains(fam, "Narrow")) return 100;
    if (ci_contains(fam, "DejaVu Sans") && !ci_contains(fam, "Mono") && !ci_contains(fam, "Condensed")) return 98;
    if (ci_contains(fam, "Noto Sans") && !ci_contains(fam, "CJK") && !ci_contains(fam, "Mono")) return 96;
    if (ci_contains(fam, "Inter") || ci_contains(fam, "Ubuntu") || ci_contains(fam, "Roboto")) return 94;
    if (ci_contains(fam, "Liberation Serif") || ci_contains(fam, "DejaVu Serif")) return 90;
    if (ci_contains(fam, "Liberation Mono") || ci_contains(fam, "DejaVu Sans Mono") || ci_contains(fam, "JetBrains Mono")) return 86;
    if (ci_contains(fam, "Cantarell") || ci_contains(fam, "Fira") || ci_contains(fam, "Droid")) return 80;
    return 40;
}

static void register_font_file(const char *path) {
    if (!g_ft || g_font_count >= ZP_MAX_SYS_FONTS) return;
    void *fraw = NULL;
    if (g_ft_new_face(g_ft, path, 0, &fraw) != 0 || !fraw) return;
    ZpFtFace *f = (ZpFtFace *)fraw;
    if (!f->family_name || !f->family_name[0]) { g_ft_done_face(fraw); return; }

    /* skip emoji, math, symbol-only and italic-only fonts */
    if (ci_contains(f->family_name, "Emoji") || ci_contains(f->family_name, "Symbol") ||
        ci_contains(f->family_name, "Math") || ci_contains(f->family_name, "Dingbat") ||
        ci_contains(f->family_name, "Nerd")) {
        g_ft_done_face(fraw);
        return;
    }

    const char *st = f->style_name ? f->style_name : "";
    bool is_italic = (f->style_flags & 1) || ci_contains(st, "Italic") || ci_contains(st, "Oblique");
    bool is_bold = (f->style_flags & 2) || ci_contains(st, "Bold");
    if (is_italic) { g_ft_done_face(fraw); return; }

    /* only keep regular or bold weights */
    bool is_reg = !is_bold && (!st[0] || ci_contains(st, "Regular") || ci_contains(st, "Book") || ci_contains(st, "Normal") || ci_contains(st, "Medium"));
    if (!is_reg && !is_bold) { g_ft_done_face(fraw); return; }

    /* check if font supports basic latin 'A' (skip script-only fonts with .notdef) + cyrillic 'А' */
    if (g_ft_set_px(fraw, 0, 16) != 0 ||
        g_ft_load_char(fraw, 'A', ZP_FT_LOAD_RENDER) != 0 ||
        !f->glyph || f->glyph->glyph_index == 0) {
        g_ft_done_face(fraw);
        return;
    }
    int has_cyr = (g_ft_load_char(fraw, 0x0410u, ZP_FT_LOAD_RENDER) == 0 && f->glyph && f->glyph->glyph_index != 0);

    int existing = -1;
    for (int i = 0; i < g_font_count; ++i) {
        if (strcmp(g_fonts[i].name, f->family_name) == 0) { existing = i; break; }
    }
    if (existing >= 0) {
        if (is_reg && !g_fonts[existing].regular_path[0]) {
            snprintf(g_fonts[existing].regular_path, sizeof(g_fonts[existing].regular_path), "%s", path);
        } else if (is_bold && !g_fonts[existing].bold_path[0]) {
            snprintf(g_fonts[existing].bold_path, sizeof(g_fonts[existing].bold_path), "%s", path);
        }
    } else if (g_font_count < ZP_MAX_SYS_FONTS) {
        ZpSysFont *sf = &g_fonts[g_font_count++];
        memset(sf, 0, sizeof(*sf));
        snprintf(sf->name, sizeof(sf->name), "%s", f->family_name);
        if (is_reg) snprintf(sf->regular_path, sizeof(sf->regular_path), "%s", path);
        else snprintf(sf->bold_path, sizeof(sf->bold_path), "%s", path);
        sf->priority = font_priority(sf->name) + (has_cyr ? 25 : 0);
    }
    g_ft_done_face(fraw);
}

static void scan_dir(const char *dir, int depth) {
    if (depth > 5 || g_font_count >= ZP_MAX_SYS_FONTS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_font_count < ZP_MAX_SYS_FONTS) {
        if (ent->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, depth + 1);
        } else if (S_ISREG(st.st_mode) && has_font_ext(ent->d_name)) {
            register_font_file(full);
        }
    }
    closedir(d);
}

static int cmp_fonts(const void *a, const void *b) {
    const ZpSysFont *fa = (const ZpSysFont *)a;
    const ZpSysFont *fb = (const ZpSysFont *)b;
    if (fb->priority != fa->priority) return fb->priority - fa->priority;
    return strcmp(fa->name, fb->name);
}

void zp_ttf_init(void) {
    g_font_count = 0;
    g_dl = dlopen("libfreetype.so.6", RTLD_LAZY);
    if (!g_dl) g_dl = dlopen("libfreetype.so", RTLD_LAZY);
    if (!g_dl) return;

    g_ft_init      = (pfn_FT_Init)dlsym(g_dl, "FT_Init_FreeType");
    g_ft_done      = (pfn_FT_Done)dlsym(g_dl, "FT_Done_FreeType");
    g_ft_new_face  = (pfn_FT_NewFace)dlsym(g_dl, "FT_New_Face");
    g_ft_done_face = (pfn_FT_DoneFace)dlsym(g_dl, "FT_Done_Face");
    g_ft_set_px    = (pfn_FT_SetPx)dlsym(g_dl, "FT_Set_Pixel_Sizes");
    g_ft_load_char = (pfn_FT_LoadChar)dlsym(g_dl, "FT_Load_Char");

    if (!g_ft_init || !g_ft_done || !g_ft_new_face || !g_ft_done_face || !g_ft_set_px || !g_ft_load_char) {
        dlclose(g_dl);
        g_dl = NULL;
        return;
    }
    if (g_ft_init(&g_ft) != 0 || !g_ft) {
        dlclose(g_dl);
        g_dl = NULL;
        g_ft = NULL;
        return;
    }

    scan_dir("/usr/share/fonts/truetype", 0);
    scan_dir("/usr/share/fonts/TTF", 0);
    scan_dir("/usr/share/fonts/opentype", 0);
    scan_dir("/usr/share/fonts", 0);
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char udir[512];
        snprintf(udir, sizeof(udir), "%s/.local/share/fonts", home);
        scan_dir(udir, 0);
    }

    /* ensure every slot has a regular fallback path if only bold was found */
    for (int i = 0; i < g_font_count; ++i) {
        if (!g_fonts[i].regular_path[0] && g_fonts[i].bold_path[0]) {
            snprintf(g_fonts[i].regular_path, sizeof(g_fonts[i].regular_path), "%s", g_fonts[i].bold_path);
        }
    }
    if (g_font_count > 1) {
        qsort(g_fonts, (size_t)g_font_count, sizeof(ZpSysFont), cmp_fonts);
    }
}

void zp_ttf_shutdown(void) {
    if (g_cached_face && g_ft_done_face) { g_ft_done_face(g_cached_face); g_cached_face = NULL; }
    if (g_ft && g_ft_done) { g_ft_done(g_ft); g_ft = NULL; }
    if (g_dl) { dlclose(g_dl); g_dl = NULL; }
    g_font_count = 0;
}

int zp_ttf_font_count(void) {
    return g_font_count > 0 ? g_font_count : 1;
}

const char *zp_ttf_font_name(int idx) {
    if (g_font_count <= 0) return "Built-in Sans";
    if (idx < 0 || idx >= g_font_count) idx = 0;
    return g_fonts[idx].name;
}

static ZpFtFace *ensure_face(int font_idx, int px_size, bool bold) {
    if (!g_ft || g_font_count <= 0) return NULL;
    if (font_idx < 0 || font_idx >= g_font_count) font_idx = 0;
    if (px_size < 6) px_size = 6;
    if (px_size > 256) px_size = 256;

    if (g_cached_face && g_cached_idx == font_idx && g_cached_bold == bold) {
        if (g_cached_px != px_size) {
            g_ft_set_px(g_cached_face, 0, (unsigned int)px_size);
            g_cached_px = px_size;
        }
        return g_cached_face;
    }

    if (g_cached_face) { g_ft_done_face(g_cached_face); g_cached_face = NULL; }
    const char *path = (bold && g_fonts[font_idx].bold_path[0])
        ? g_fonts[font_idx].bold_path
        : g_fonts[font_idx].regular_path;
    void *fraw = NULL;
    if (g_ft_new_face(g_ft, path, 0, &fraw) != 0 || !fraw) return NULL;
    g_ft_set_px(fraw, 0, (unsigned int)px_size);
    g_cached_face = (ZpFtFace *)fraw;
    g_cached_idx = font_idx;
    g_cached_bold = bold;
    g_cached_px = px_size;
    return g_cached_face;
}

/* === utf-8 measurement and anti-aliased rendering === */
void zp_ttf_measure(const char *utf8, int font_idx, int px_size, bool bold, int *out_w, int *out_h) {
    int mw = 0, mh = px_size > 8 ? px_size : 12;
    if (!utf8 || !utf8[0]) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = mh + 4;
        return;
    }
    ZpFtFace *f = ensure_face(font_idx, px_size, bold);
    if (!f) {
        int sc = px_size >= 18 ? (px_size + 6) / 12 : 1;
        mw = zp_text_width_font(utf8, bold ? ZP_FONT_BOLD : ZP_FONT_REG, sc);
        int lines = 1;
        for (const char *p = utf8; *p; ++p) if (*p == '\n') lines++;
        if (out_w) *out_w = mw;
        if (out_h) *out_h = lines * 15 * sc;
        return;
    }

    int line_h = (int)(px_size * 1.25f);
    if (line_h < px_size + 2) line_h = px_size + 2;
    int cur = 0, lines = 1;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (cp == '\n') {
            if (cur > mw) mw = cur;
            cur = 0;
            lines++;
            continue;
        }
        if (g_ft_load_char(f, cp, ZP_FT_LOAD_RENDER | ZP_FT_LOAD_TARGET_LIGHT) == 0 && f->glyph) {
            cur += (int)((f->glyph->advance.x + 32) >> 6);
        } else {
            cur += px_size / 2;
        }
    }
    if (cur > mw) mw = cur;
    if (out_w) *out_w = mw;
    if (out_h) *out_h = lines * line_h;
}

static void blend_clip(ZPImage *img, int x, int y, uint32_t fg, uint8_t a, int cx0, int cy0, int cx1, int cy1) {
    if (!a || x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
    if (x < 0 || y < 0 || x >= img->w || y >= img->h) return;
    uint32_t *dstp = &img->pixels[(size_t)y * (size_t)img->w + (size_t)x];
    if (a == 255) { *dstp = fg; return; }
    uint32_t dst = *dstp;
    uint32_t inv = 255u - a;
    uint32_t r = (ZP_R(fg) * a + ZP_R(dst) * inv + 127u) / 255u;
    uint32_t g = (ZP_G(fg) * a + ZP_G(dst) * inv + 127u) / 255u;
    uint32_t b = (ZP_B(fg) * a + ZP_B(dst) * inv + 127u) / 255u;
    *dstp = ZP_RGB(r, g, b);
}

void zp_ttf_draw_clipped(ZPImage *img, int x, int y, const char *utf8, int font_idx, int px_size, bool bold, uint32_t fg,
                         int cx0, int cy0, int cx1, int cy1) {
    if (!img || !img->pixels || !utf8 || !utf8[0]) return;
    ZpFtFace *f = ensure_face(font_idx, px_size, bold);
    if (!f) {
        int sc = px_size >= 18 ? (px_size + 6) / 12 : 1;
        zp_draw_text_font(img, x, y, utf8, bold ? ZP_FONT_BOLD : ZP_FONT_REG, sc, fg);
        return;
    }

    int ascent = (int)(px_size * 0.85f);
    int line_h = (int)(px_size * 1.25f);
    if (line_h < px_size + 2) line_h = px_size + 2;

    int pen_x = x;
    int pen_y = y + ascent;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = next_utf8(&p);
        if (cp == '\n') {
            pen_x = x;
            pen_y += line_h;
            continue;
        }
        if (g_ft_load_char(f, cp, ZP_FT_LOAD_RENDER | ZP_FT_LOAD_TARGET_LIGHT) != 0 || !f->glyph) {
            pen_x += px_size / 2;
            continue;
        }
        ZpFtGlyphSlot *g = f->glyph;
        ZpFtBitmap *bm = &g->bitmap;
        int gx = pen_x + g->bitmap_left;
        int gy = pen_y - g->bitmap_top;
        for (unsigned int r = 0; r < bm->rows; ++r) {
            const unsigned char *srow = bm->buffer + (int)r * bm->pitch;
            for (unsigned int c = 0; c < bm->width; ++c) {
                blend_clip(img, gx + (int)c, gy + (int)r, fg, srow[c], cx0, cy0, cx1, cy1);
            }
        }
        pen_x += (int)((g->advance.x + 32) >> 6);
    }
}

void zp_ttf_draw(ZPImage *img, int x, int y, const char *utf8, int font_idx, int px_size, bool bold, uint32_t fg) {
    if (!img) return;
    zp_ttf_draw_clipped(img, x, y, utf8, font_idx, px_size, bold, fg, 0, 0, img->w, img->h);
}
