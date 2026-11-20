/* zp_types.h - core types, constants, palette and main app state struct */
#ifndef ZP_TYPES_H
#define ZP_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* === limits and defaults === */
#define ZP_VERSION          "1.0.0"
#define ZP_DEFAULT_WIDTH    1024
#define ZP_DEFAULT_HEIGHT   640
#define ZP_MAX_DIM          8192
#define ZP_MIN_DIM          1
#define ZP_MAX_HISTORY      40
#define ZP_MIN_HISTORY      4
#define ZP_DEFAULT_UNDO_MB  32
#define ZP_PALETTE_COUNT    28
#define ZP_CUSTOM_COLORS    10
#define ZP_MAX_POLY_PTS     2048
#define ZP_PATH_MAX         512

/* === argb8888 packing === */
#define ZP_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define ZP_RGB(r, g, b) ZP_ARGB(255, (r), (g), (b))
#define ZP_A(c) (((c) >> 24) & 0xFFu)
#define ZP_R(c) (((c) >> 16) & 0xFFu)
#define ZP_G(c) (((c) >> 8)  & 0xFFu)
#define ZP_B(c) ((c) & 0xFFu)

/* === light ui theme colors === */
#define ZP_COL_WELL         ZP_RGB(207, 214, 225)
#define ZP_COL_CHROME       ZP_RGB(244, 245, 247)
#define ZP_COL_SURFACE      ZP_RGB(233, 236, 239)
#define ZP_COL_BORDER       ZP_RGB(198, 205, 214)
#define ZP_COL_BORDER_DARK  ZP_RGB(160, 170, 184)
#define ZP_COL_INK          ZP_RGB(22, 26, 31)
#define ZP_COL_INK_MUTED    ZP_RGB(80, 88, 99)
#define ZP_COL_ACCENT_BG    ZP_RGB(216, 232, 250)
#define ZP_COL_ACCENT       ZP_RGB(0, 95, 184)
#define ZP_COL_WHITE        ZP_RGB(255, 255, 255)
#define ZP_COL_BLACK        ZP_RGB(0, 0, 0)

/* === enums and structs === */
typedef struct {
    int x;
    int y;
} ZPPoint;

typedef struct {
    int x, y, w, h;
} ZPRect;

typedef enum {
    ZP_TOOL_SELECT_RECT = 0,
    ZP_TOOL_SELECT_LASSO,
    ZP_TOOL_PENCIL,
    ZP_TOOL_FILL,
    ZP_TOOL_TEXT,
    ZP_TOOL_ERASER,
    ZP_TOOL_EYEDROPPER,
    ZP_TOOL_MAGNIFIER,
    ZP_TOOL_BRUSH,
    ZP_TOOL_AIRBRUSH,
    ZP_TOOL_LINE,
    ZP_TOOL_CURVE,
    ZP_TOOL_RECT,
    ZP_TOOL_ROUND_RECT,
    ZP_TOOL_ELLIPSE,
    ZP_TOOL_POLYGON,
    ZP_TOOL_TRIANGLE,
    ZP_TOOL_DIAMOND,
    ZP_TOOL_ARROW_RIGHT,
    ZP_TOOL_STAR,
    ZP_TOOL_COUNT
} ZPTool;

typedef enum {
    ZP_BRUSH_PIXEL = 0,
    ZP_BRUSH_ROUND,
    ZP_BRUSH_CALLIGRAPHY,
    ZP_BRUSH_MARKER
} ZPBrushVariant;

typedef enum {
    ZP_SHAPE_OUTLINE = 0,
    ZP_SHAPE_FILL,
    ZP_SHAPE_BOTH
} ZPShapeMode;

typedef struct {
    int w;
    int h;
    uint32_t *pixels;
} ZPImage;

typedef struct {
    bool active;
    int x;
    int y;
    ZPImage buf;
    uint8_t *mask; /* non-null only for free-form lasso selctions */
    bool transparent_bg;
    uint32_t key_color;
    bool dragging;
    int drag_start_mx;
    int drag_start_my;
    int drag_orig_x;
    int drag_orig_y;
    /* active scale/stretch transform (0..7 handles, -1 = none) */
    int transform_handle;
    ZPImage orig_buf;
    uint8_t *orig_mask;
    int orig_x;
    int orig_y;
    int orig_w;
    int orig_h;
} ZPSelection;

typedef struct {
    ZPImage frames[ZP_MAX_HISTORY];
    size_t bytes[ZP_MAX_HISTORY];
    int count;
    int index;
    size_t total_bytes;
    size_t max_bytes;
} ZPHistory;

typedef enum {
    ZP_MODAL_NONE = 0,
    ZP_MODAL_RESIZE_SKEW,
    ZP_MODAL_PROPERTIES,
    ZP_MODAL_PATH_OPEN,
    ZP_MODAL_PATH_SAVE
} ZPModalKind;

typedef struct {
    ZPImage canvas;
    ZPHistory history;
    char file_path[ZP_PATH_MAX];
    bool dirty;

    int win_w;
    int win_h;
    float zoom;
    int scroll_x;
    int scroll_y;
    bool show_grid;
    bool show_rulers;

    ZPTool tool;
    ZPBrushVariant brush_variant;
    int stroke_size;
    ZPShapeMode shape_mode;
    uint32_t color1;
    uint32_t color2;
    int active_slot;
    uint32_t palette[ZP_PALETTE_COUNT];
    uint32_t custom_colors[ZP_CUSTOM_COLORS];
    bool transparent_selection;

    bool drawing;
    int active_button;
    ZPPoint start_pt;
    ZPPoint prev_pt;
    ZPPoint curr_pt;
    ZPPoint cursor_px;
    bool cursor_in_canvas;

    /* curve needs 3 clicks/drags: 1=line, 2=bend1, 3=bend2 */
    int curve_stage;
    ZPPoint curve_pts[4];
    ZPPoint poly_pts[ZP_MAX_POLY_PTS];
    int poly_count;

    ZPSelection sel;
    ZPImage clipboard;
    uint8_t *clipboard_mask;

    bool text_active;
    int text_x;
    int text_y;
    int text_scale;
    int text_font_idx;
    int text_size_px;
    bool text_bold;
    bool font_menu_open;
    char text_buf[512];
    int text_len;

    bool panning;
    bool space_down;
    int pan_start_sx;
    int pan_start_sy;
    int pan_orig_x;
    int pan_orig_y;

    int resize_handle;
    int resize_preview_w;
    int resize_preview_h;

    int open_menu;
    bool rotate_menu_open;
    int hover_x;
    int hover_y;
    ZPModalKind modal;
    char modal_input[256];
    int modal_w;
    int modal_h;
    int modal_skew_h;
    int modal_skew_v;
    int modal_field;
    char status_msg[128];
} ZPApp;

/* === default 28-color palette === */
static const uint32_t ZP_CLASSIC_PALETTE[ZP_PALETTE_COUNT] = {
    ZP_RGB(0, 0, 0),       ZP_RGB(127, 127, 127), ZP_RGB(136, 0, 21),    ZP_RGB(237, 28, 36),
    ZP_RGB(255, 127, 39),  ZP_RGB(255, 242, 0),   ZP_RGB(34, 177, 76),   ZP_RGB(0, 162, 232),
    ZP_RGB(63, 72, 204),   ZP_RGB(163, 73, 164),  ZP_RGB(128, 64, 0),    ZP_RGB(0, 128, 128),
    ZP_RGB(64, 128, 128),  ZP_RGB(0, 64, 128),
    ZP_RGB(255, 255, 255), ZP_RGB(195, 195, 195), ZP_RGB(185, 122, 87),  ZP_RGB(255, 174, 201),
    ZP_RGB(255, 201, 14),  ZP_RGB(239, 228, 176), ZP_RGB(181, 230, 29),  ZP_RGB(153, 217, 234),
    ZP_RGB(112, 146, 190), ZP_RGB(200, 191, 231), ZP_RGB(255, 128, 128), ZP_RGB(128, 255, 128),
    ZP_RGB(128, 255, 255), ZP_RGB(224, 224, 224)
};

static const float ZP_ZOOM_LEVELS[] = {
    0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 12.0f, 16.0f
};
#define ZP_ZOOM_COUNT 8

#endif
