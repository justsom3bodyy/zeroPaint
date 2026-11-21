/* main.c - 0paint sdl2 entry point, blocking event loop and input routing */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zp_types.h"
#include "zp_raster.h"
#include "zp_ttf.h"
#include "zp_codec.h"
#include "zp_ui.h"
#include "zp_logo.h"

/* pop one full utf-8 codepoint from end of string so cyrillic backspace works cleanly */
static void zp_utf8_pop(char *buf, int *len) {
    int n = *len;
    if (n <= 0) return;
    n--;
    while (n > 0 && (((unsigned char)buf[n] & 0xC0) == 0x80)) n--;
    buf[n] = '\0';
    *len = n;
}

/* === modal commit === */
static void zp_apply_modal(ZPApp *app) {
    if (app->modal == ZP_MODAL_PATH_OPEN) {
        ZPImage loaded;
        if (zp_load_image(app->modal_input, &loaded)) {
            zp_img_free(&app->canvas);
            app->canvas = loaded;
            snprintf(app->file_path, sizeof(app->file_path), "%s", app->modal_input);
            zp_history_push(&app->history, &app->canvas);
            snprintf(app->status_msg, sizeof(app->status_msg), "Opened %s", app->modal_input);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Failed to open %s", app->modal_input);
        }
    } else if (app->modal == ZP_MODAL_PATH_SAVE) {
        if (zp_save_image(app->modal_input, &app->canvas)) {
            snprintf(app->file_path, sizeof(app->file_path), "%s", app->modal_input);
            app->dirty = false;
            snprintf(app->status_msg, sizeof(app->status_msg), "Saved %s", app->modal_input);
        } else {
            snprintf(app->status_msg, sizeof(app->status_msg), "Failed to save %s", app->modal_input);
        }
    } else if (app->modal == ZP_MODAL_RESIZE_SKEW || app->modal == ZP_MODAL_PROPERTIES) {
        int nw = 0, nh = 0;
        if (sscanf(app->modal_input, "%dx%d", &nw, &nh) == 2 && nw >= 1 && nh >= 1 && nw <= ZP_MAX_DIM && nh <= ZP_MAX_DIM) {
            if (app->modal == ZP_MODAL_RESIZE_SKEW) {
                ZPImage scaled;
                if (zp_img_scale_nearest(&app->canvas, &scaled, nw, nh)) {
                    zp_img_free(&app->canvas);
                    app->canvas = scaled;
                    zp_history_push(&app->history, &app->canvas);
                }
            } else {
                zp_canvas_resize_borders(&app->canvas, nw, nh, app->color2);
                zp_history_push(&app->history, &app->canvas);
            }
            app->dirty = true;
            snprintf(app->status_msg, sizeof(app->status_msg), "Canvas %dx%d", nw, nh);
        }
    }
    app->modal = ZP_MODAL_NONE;
}

/* === entry point === */
int main(int argc, char **argv) {
    size_t max_undo_mb = ZP_DEFAULT_UNDO_MB;
    const char *startup_file = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--max-undo-mb=", 14) == 0) {
            max_undo_mb = (size_t)atoi(argv[i] + 14);
        } else if (argv[i][0] != '-') {
            startup_file = argv[i];
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "0Paint: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = SDL_CreateWindow(
        "0Paint (zeroPaint)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1200, 780,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "0Paint: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Surface *win_ico = SDL_CreateRGBSurfaceFrom(
        (void *)ZP_LOGO_32, ZP_LOGO_WIN_SIZE, ZP_LOGO_WIN_SIZE, 32, ZP_LOGO_WIN_SIZE * 4,
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u
    );
    if (win_ico) {
        SDL_SetWindowIcon(win, win_ico);
        SDL_FreeSurface(win_ico);
    }

    /* try hardware first, fall back to software renderer on potato gpus */
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        fprintf(stderr, "0Paint: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    ZPApp app;
    memset(&app, 0, sizeof(app));
    app.win_w = 1200;
    app.win_h = 780;
    app.zoom = 1.0f;
    app.tool = ZP_TOOL_PENCIL;
    app.brush_variant = ZP_BRUSH_PIXEL;
    app.stroke_size = 1;
    app.shape_mode = ZP_SHAPE_OUTLINE;
    app.color1 = ZP_COL_BLACK;
    app.color2 = ZP_COL_WHITE;
    app.active_slot = 1;
    app.text_scale = 2;
    app.text_size_px = 24;
    app.text_font_idx = 0;
    app.text_bold = false;
    app.open_menu = -1;
    app.hover_x = -1;
    app.hover_y = -1;
    app.resize_handle = -1;
    app.sel.transform_handle = -1;
    memcpy(app.palette, ZP_CLASSIC_PALETTE, sizeof(ZP_CLASSIC_PALETTE));

    zp_ttf_init();
    zp_history_init(&app.history, max_undo_mb);
    if (startup_file && zp_load_image(startup_file, &app.canvas)) {
        snprintf(app.file_path, sizeof(app.file_path), "%s", startup_file);
        snprintf(app.status_msg, sizeof(app.status_msg), "Loaded %s", startup_file);
    } else {
        zp_img_init(&app.canvas, ZP_DEFAULT_WIDTH, ZP_DEFAULT_HEIGHT, ZP_COL_WHITE);
        snprintf(app.status_msg, sizeof(app.status_msg), "Ready (Space+drag or MMB to pan)");
    }
    zp_history_push(&app.history, &app.canvas);

    ZPImage fb;
    zp_img_init(&fb, app.win_w, app.win_h, ZP_COL_WELL);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, app.win_w, app.win_h);

    ZPImage shape_backup = { 0, 0, NULL };
    uint32_t rng_state = 0x1337C0DEu;
    uint32_t anim_tick = 0;
    bool running = true;
    bool needs_redraw = true;

    SDL_StartTextInput();

    /* === main event loop === */
    while (running) {
        if (needs_redraw) {
            zp_ui_render(&app, &fb, anim_tick);
            SDL_UpdateTexture(tex, NULL, fb.pixels, fb.w * (int)sizeof(uint32_t));
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
            needs_redraw = false;
        }

        /* block completely when idle so cpu stays at 0% */
        SDL_Event ev;
        int has_ev = app.sel.active ? SDL_WaitEventTimeout(&ev, 200) : SDL_WaitEvent(&ev);
        if (!has_ev) {
            if (app.sel.active) { anim_tick++; needs_redraw = true; }
            continue;
        }

        do {
            switch (ev.type) {
                case SDL_QUIT:
                    running = false;
                    break;

                case SDL_WINDOWEVENT:
                    if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        app.win_w = ev.window.data1 > 320 ? ev.window.data1 : 320;
                        app.win_h = ev.window.data2 > 240 ? ev.window.data2 : 240;
                        zp_img_free(&fb);
                        zp_img_init(&fb, app.win_w, app.win_h, ZP_COL_WELL);
                        SDL_DestroyTexture(tex);
                        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, app.win_w, app.win_h);
                        needs_redraw = true;
                    } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                        needs_redraw = true;
                    } else if (ev.window.event == SDL_WINDOWEVENT_LEAVE) {
                        app.hover_x = -1;
                        app.hover_y = -1;
                        needs_redraw = true;
                    }
                    break;

                case SDL_DROPFILE: {
                    ZPImage dropped;
                    if (ev.drop.file && zp_load_image(ev.drop.file, &dropped)) {
                        zp_sel_clear(&app.sel);
                        zp_img_free(&app.canvas);
                        app.canvas = dropped;
                        snprintf(app.file_path, sizeof(app.file_path), "%s", ev.drop.file);
                        zp_history_push(&app.history, &app.canvas);
                        snprintf(app.status_msg, sizeof(app.status_msg), "Opened dropped image");
                    }
                    SDL_free(ev.drop.file);
                    needs_redraw = true;
                    break;
                }

                case SDL_TEXTINPUT:
                    if (app.modal != ZP_MODAL_NONE) {
                        size_t cur = strlen(app.modal_input);
                        if (cur + strlen(ev.text.text) < sizeof(app.modal_input) - 1) {
                            strcat(app.modal_input, ev.text.text);
                            needs_redraw = true;
                        }
                    } else if (app.text_active) {
                        size_t add = strlen(ev.text.text);
                        if ((size_t)app.text_len + add < sizeof(app.text_buf) - 1) {
                            strcat(app.text_buf, ev.text.text);
                            app.text_len += (int)add;
                            needs_redraw = true;
                        }
                    }
                    break;

                /* === layout-independent hotkeys (scancodes) === */
                case SDL_KEYDOWN: {
                    /* use physical scancodes so ctrl+z / ctrl+c work on russian and other layouts too */
                    SDL_Scancode sc = ev.key.keysym.scancode;
                    Uint16 mod = ev.key.keysym.mod;
                    bool ctrl = (mod & KMOD_CTRL) != 0;
                    bool shift = (mod & KMOD_SHIFT) != 0;

                    if (app.modal != ZP_MODAL_NONE) {
                        if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) zp_apply_modal(&app);
                        else if (sc == SDL_SCANCODE_ESCAPE) app.modal = ZP_MODAL_NONE;
                        else if (sc == SDL_SCANCODE_BACKSPACE && app.modal_input[0]) {
                            int mlen = (int)strlen(app.modal_input);
                            zp_utf8_pop(app.modal_input, &mlen);
                        }
                        needs_redraw = true;
                        break;
                    }

                    if (app.text_active) {
                        if (sc == SDL_SCANCODE_ESCAPE) {
                            app.text_active = false;
                            app.text_len = 0;
                            app.text_buf[0] = '\0';
                        } else if ((sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) && ctrl) {
                            zp_app_commit_text(&app);
                        } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                            if ((size_t)app.text_len + 1 < sizeof(app.text_buf) - 1) {
                                app.text_buf[app.text_len++] = '\n';
                                app.text_buf[app.text_len] = '\0';
                            }
                        } else if (sc == SDL_SCANCODE_BACKSPACE && app.text_len > 0) {
                            zp_utf8_pop(app.text_buf, &app.text_len);
                        } else if (ctrl && sc == SDL_SCANCODE_V) {
                            char *clip = SDL_GetClipboardText();
                            if (clip) {
                                size_t add = strlen(clip);
                                if ((size_t)app.text_len + add < sizeof(app.text_buf) - 1) {
                                    strcat(app.text_buf, clip);
                                    app.text_len += (int)add;
                                }
                                SDL_free(clip);
                            }
                        }
                        needs_redraw = true;
                        break;
                    }

                    if (sc == SDL_SCANCODE_SPACE && !ctrl) {
                        app.space_down = true;
                        break;
                    }

                    if (ctrl) {
                        if (sc == SDL_SCANCODE_Z && !shift) { zp_app_commit_selection(&app); zp_history_undo(&app.history, &app.canvas); }
                        else if (sc == SDL_SCANCODE_Y || (sc == SDL_SCANCODE_Z && shift)) { zp_app_commit_selection(&app); zp_history_redo(&app.history, &app.canvas); }
                        else if (sc == SDL_SCANCODE_N) zp_app_new(&app);
                        else if (sc == SDL_SCANCODE_O) zp_app_open(&app);
                        else if (sc == SDL_SCANCODE_S) zp_app_save(&app, shift);
                        else if (sc == SDL_SCANCODE_C) zp_app_copy(&app);
                        else if (sc == SDL_SCANCODE_X && !shift) zp_app_cut(&app);
                        else if (sc == SDL_SCANCODE_X && shift) zp_app_crop_to_selection(&app);
                        else if (sc == SDL_SCANCODE_V) zp_app_paste(&app);
                        else if (sc == SDL_SCANCODE_A) {
                            zp_app_commit_selection(&app);
                            zp_sel_extract_rect(&app.canvas, 0, 0, app.canvas.w - 1, app.canvas.h - 1, app.color2, &app.sel, app.transparent_selection);
                            app.tool = ZP_TOOL_SELECT_RECT;
                        }
                        else if (sc == SDL_SCANCODE_G) app.show_grid = !app.show_grid;
                        else if (sc == SDL_SCANCODE_R) app.show_rulers = !app.show_rulers;
                        else if (sc == SDL_SCANCODE_W) {
                            app.modal = ZP_MODAL_RESIZE_SKEW;
                            snprintf(app.modal_input, sizeof(app.modal_input), "%dx%d", app.canvas.w, app.canvas.h);
                        }
                        else if (sc == SDL_SCANCODE_E) {
                            app.modal = ZP_MODAL_PROPERTIES;
                            snprintf(app.modal_input, sizeof(app.modal_input), "%dx%d", app.canvas.w, app.canvas.h);
                        }
                        else if (sc == SDL_SCANCODE_I && shift) { zp_img_invert(&app.canvas); zp_history_push(&app.history, &app.canvas); }
                        else if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) zp_app_zoom_step(&app, 1);
                        else if (sc == SDL_SCANCODE_MINUS || sc == SDL_SCANCODE_KP_MINUS) zp_app_zoom_step(&app, -1);
                        else if (sc == SDL_SCANCODE_0 || sc == SDL_SCANCODE_KP_0) app.zoom = 1.0f;
                    } else if (sc == SDL_SCANCODE_DELETE && app.sel.active) {
                        zp_sel_clear(&app.sel);
                        zp_history_push(&app.history, &app.canvas);
                    } else if (sc == SDL_SCANCODE_ESCAPE) {
                        zp_app_commit_selection(&app);
                        app.poly_count = 0;
                        app.curve_stage = 0;
                    } else if (sc == SDL_SCANCODE_P) app.tool = ZP_TOOL_PENCIL;
                    else if (sc == SDL_SCANCODE_B) app.tool = ZP_TOOL_BRUSH;
                    else if (sc == SDL_SCANCODE_E) app.tool = ZP_TOOL_ERASER;
                    else if (sc == SDL_SCANCODE_F) app.tool = ZP_TOOL_FILL;
                    else if (sc == SDL_SCANCODE_T) app.tool = ZP_TOOL_TEXT;
                    else if (sc == SDL_SCANCODE_I) app.tool = ZP_TOOL_EYEDROPPER;
                    else if (sc == SDL_SCANCODE_Z) app.tool = ZP_TOOL_MAGNIFIER;
                    else if (sc == SDL_SCANCODE_S) app.tool = ZP_TOOL_SELECT_RECT;
                    else if (sc == SDL_SCANCODE_L) app.tool = ZP_TOOL_SELECT_LASSO;
                    else if (sc == SDL_SCANCODE_X) {
                        uint32_t tmp = app.color1;
                        app.color1 = app.color2;
                        app.color2 = tmp;
                    }
                    needs_redraw = true;
                    break;
                }

                case SDL_KEYUP:
                    if (ev.key.keysym.scancode == SDL_SCANCODE_SPACE) {
                        app.space_down = false;
                        if (app.panning) {
                            app.panning = false;
                            needs_redraw = true;
                        }
                    }
                    break;

                /* === mouse and drawing input === */
                case SDL_MOUSEWHEEL: {
                    SDL_Keymod km = SDL_GetModState();
                    if (km & KMOD_CTRL) {
                        zp_app_zoom_at(&app, ev.wheel.y > 0 ? 1 : -1, app.hover_x, app.hover_y);
                    } else if (km & KMOD_SHIFT) {
                        app.scroll_x -= ev.wheel.y * 40;
                    } else {
                        app.scroll_y -= ev.wheel.y * 40;
                        app.scroll_x -= ev.wheel.x * 40;
                    }
                    needs_redraw = true;
                    break;
                }

                case SDL_MOUSEBUTTONDOWN: {
                    int sx = ev.button.x, sy = ev.button.y;
                    int btn = ev.button.button;
                    if (app.modal != ZP_MODAL_NONE) break;

                    /* middle mouse button or space + left click pans the canvas viewport */
                    if ((btn == SDL_BUTTON_MIDDLE || (app.space_down && btn == SDL_BUTTON_LEFT)) &&
                        sy >= zp_ui_top_h(&app) && sy < app.win_h - ZP_STATUSBAR_H) {
                        app.panning = true;
                        app.pan_start_sx = sx;
                        app.pan_start_sy = sy;
                        app.pan_orig_x = app.scroll_x;
                        app.pan_orig_y = app.scroll_y;
                        snprintf(app.status_msg, sizeof(app.status_msg), "Panning viewport");
                        needs_redraw = true;
                        break;
                    }

                    if (zp_ui_handle_click(&app, sx, sy, btn)) { needs_redraw = true; break; }

                    /* check active selection transform handles first */
                    int sh = zp_ui_hit_sel_handle(&app, sx, sy);
                    if (sh >= 0 && btn == SDL_BUTTON_LEFT) {
                        zp_sel_begin_transform(&app.sel, sh);
                        needs_redraw = true;
                        break;
                    }

                    int h = zp_ui_hit_resize_handle(&app, sx, sy);
                    if (h >= 0) {
                        zp_app_commit_selection(&app);
                        app.resize_handle = h;
                        app.resize_preview_w = app.canvas.w;
                        app.resize_preview_h = app.canvas.h;
                        needs_redraw = true;
                        break;
                    }

                    int cx, cy;
                    if (!zp_ui_screen_to_canvas(&app, sx, sy, &cx, &cy)) break;
                    uint32_t fg = (btn == SDL_BUTTON_RIGHT) ? app.color2 : app.color1;
                    uint32_t bg = (btn == SDL_BUTTON_RIGHT) ? app.color1 : app.color2;

                    if (app.sel.active && cx >= app.sel.x && cx < app.sel.x + app.sel.buf.w &&
                        cy >= app.sel.y && cy < app.sel.y + app.sel.buf.h) {
                        app.sel.dragging = true;
                        app.sel.drag_start_mx = cx;
                        app.sel.drag_start_my = cy;
                        app.sel.drag_orig_x = app.sel.x;
                        app.sel.drag_orig_y = app.sel.y;
                        needs_redraw = true;
                        break;
                    } else if (app.sel.active) {
                        zp_app_commit_selection(&app);
                    }

                    if (app.text_active) zp_app_commit_text(&app);

                    if (app.tool == ZP_TOOL_EYEDROPPER) {
                        uint32_t picked = zp_img_get(&app.canvas, cx, cy);
                        if (btn == SDL_BUTTON_RIGHT) app.color2 = picked;
                        else app.color1 = picked;
                        app.tool = ZP_TOOL_PENCIL;
                    } else if (app.tool == ZP_TOOL_MAGNIFIER) {
                        zp_app_zoom_at(&app, btn == SDL_BUTTON_RIGHT ? -1 : 1, sx, sy);
                    } else if (app.tool == ZP_TOOL_FILL) {
                        zp_flood_fill(&app.canvas, cx, cy, fg);
                        zp_history_push(&app.history, &app.canvas);
                        app.dirty = true;
                    } else if (app.tool == ZP_TOOL_TEXT) {
                        app.text_active = true;
                        app.text_x = cx;
                        app.text_y = cy;
                        app.text_len = 0;
                        app.text_buf[0] = '\0';
                    } else if (app.tool == ZP_TOOL_PENCIL || app.tool == ZP_TOOL_BRUSH ||
                               app.tool == ZP_TOOL_ERASER || app.tool == ZP_TOOL_AIRBRUSH) {
                        app.drawing = true;
                        app.active_button = btn;
                        app.prev_pt = (ZPPoint){ cx, cy };
                        if (app.tool == ZP_TOOL_ERASER) {
                            zp_draw_line(&app.canvas, cx, cy, cx, cy, zp_eraser_size(app.stroke_size), app.color2, ZP_BRUSH_MARKER);
                        } else if (app.tool == ZP_TOOL_AIRBRUSH) {
                            zp_draw_airbrush(&app.canvas, cx, cy, app.stroke_size * 4 + 6, fg, &rng_state);
                        } else {
                            int sz = (app.tool == ZP_TOOL_PENCIL) ? app.stroke_size : app.stroke_size * 2 + 1;
                            zp_draw_line(&app.canvas, cx, cy, cx, cy, sz, fg, app.brush_variant);
                        }
                    } else if (app.tool == ZP_TOOL_SELECT_LASSO) {
                        app.drawing = true;
                        app.active_button = btn;
                        app.poly_count = 1;
                        app.poly_pts[0] = (ZPPoint){ cx, cy };
                        app.curr_pt = (ZPPoint){ cx, cy };
                    } else if (app.tool == ZP_TOOL_POLYGON) {
                        if (app.poly_count == 0) {
                            zp_img_free(&shape_backup);
                            zp_img_clone(&app.canvas, &shape_backup);
                            app.poly_pts[app.poly_count++] = (ZPPoint){ cx, cy };
                            app.active_button = btn;
                            snprintf(app.status_msg, sizeof(app.status_msg), "Polygon: click vertices, click start or right-click to finish");
                        } else if (btn == SDL_BUTTON_RIGHT || (app.poly_count >= 3 && abs(cx - app.poly_pts[0].x) <= 6 && abs(cy - app.poly_pts[0].y) <= 6)) {
                            if (shape_backup.pixels) {
                                memcpy(app.canvas.pixels, shape_backup.pixels, (size_t)app.canvas.w * (size_t)app.canvas.h * sizeof(uint32_t));
                            }
                            uint32_t pfg = (app.active_button == SDL_BUTTON_RIGHT) ? app.color2 : app.color1;
                            uint32_t pbg = (app.active_button == SDL_BUTTON_RIGHT) ? app.color1 : app.color2;
                            zp_draw_polygon(&app.canvas, app.poly_pts, app.poly_count, app.stroke_size, pfg, pbg, app.shape_mode);
                            app.poly_count = 0;
                            zp_img_free(&shape_backup);
                            zp_history_push(&app.history, &app.canvas);
                            app.dirty = true;
                            snprintf(app.status_msg, sizeof(app.status_msg), "Polygon stamped");
                        } else if (app.poly_count < ZP_MAX_POLY_PTS) {
                            app.poly_pts[app.poly_count++] = (ZPPoint){ cx, cy };
                            if (shape_backup.pixels) {
                                memcpy(app.canvas.pixels, shape_backup.pixels, (size_t)app.canvas.w * (size_t)app.canvas.h * sizeof(uint32_t));
                            }
                            for (int pi = 0; pi + 1 < app.poly_count; ++pi) {
                                zp_draw_line(&app.canvas, app.poly_pts[pi].x, app.poly_pts[pi].y, app.poly_pts[pi + 1].x, app.poly_pts[pi + 1].y, app.stroke_size, fg, ZP_BRUSH_PIXEL);
                            }
                        }
                    } else if (app.tool == ZP_TOOL_CURVE) {
                        app.drawing = true;
                        app.active_button = btn;
                        if (app.curve_stage == 0) {
                            zp_img_free(&shape_backup);
                            zp_img_clone(&app.canvas, &shape_backup);
                            app.curve_stage = 1;
                            for (int ci = 0; ci < 4; ++ci) app.curve_pts[ci] = (ZPPoint){ cx, cy };
                            snprintf(app.status_msg, sizeof(app.status_msg), "Curve 1/3: drag line");
                        } else if (app.curve_stage == 2) {
                            app.curve_pts[1] = (ZPPoint){ cx, cy };
                            app.curve_pts[2] = (ZPPoint){ cx, cy };
                        } else if (app.curve_stage == 3) {
                            app.curve_pts[2] = (ZPPoint){ cx, cy };
                        }
                        if (shape_backup.pixels) {
                            memcpy(app.canvas.pixels, shape_backup.pixels, (size_t)app.canvas.w * (size_t)app.canvas.h * sizeof(uint32_t));
                            zp_draw_bezier(&app.canvas, app.curve_pts[0], app.curve_pts[1], app.curve_pts[2], app.curve_pts[3], app.stroke_size, fg);
                        }
                    } else {
                        app.drawing = true;
                        app.active_button = btn;
                        app.start_pt = (ZPPoint){ cx, cy };
                        app.curr_pt = (ZPPoint){ cx, cy };
                        zp_img_free(&shape_backup);
                        zp_img_clone(&app.canvas, &shape_backup);
                        (void)bg;
                    }
                    needs_redraw = true;
                    break;
                }

                case SDL_MOUSEMOTION: {
                    int sx = ev.motion.x, sy = ev.motion.y;
                    int top_h = zp_ui_top_h(&app);
                    bool was_ui = (app.hover_y >= 0 && app.hover_y < top_h) || (app.hover_y >= app.win_h - ZP_STATUSBAR_H) || app.open_menu >= 0 || app.rotate_menu_open || app.font_menu_open;
                    app.hover_x = sx;
                    app.hover_y = sy;
                    bool now_ui = (sy < top_h) || (sy >= app.win_h - ZP_STATUSBAR_H) || app.open_menu >= 0 || app.rotate_menu_open || app.font_menu_open;
                    if (was_ui || now_ui) needs_redraw = true;

                    if (app.panning) {
                        app.scroll_x = app.pan_orig_x - (sx - app.pan_start_sx);
                        app.scroll_y = app.pan_orig_y - (sy - app.pan_start_sy);
                        needs_redraw = true;
                        break;
                    }

                    int cx, cy;
                    app.cursor_in_canvas = zp_ui_screen_to_canvas(&app, sx, sy, &cx, &cy);
                    if (app.cursor_in_canvas) {
                        app.cursor_px = (ZPPoint){ cx, cy };
                    }

                    if (app.sel.transform_handle >= 0) {
                        int h = app.sel.transform_handle;
                        int x0 = app.sel.orig_x, y0 = app.sel.orig_y;
                        int x1 = app.sel.orig_x + app.sel.orig_w;
                        int y1 = app.sel.orig_y + app.sel.orig_h;
                        /* 0:NW 1:N 2:NE 3:E 4:SE 5:S 6:SW 7:W */
                        if (h == 0 || h == 6 || h == 7) x0 = cx;
                        if (h == 2 || h == 3 || h == 4) x1 = cx;
                        if (h == 0 || h == 1 || h == 2) y0 = cy;
                        if (h == 4 || h == 5 || h == 6) y1 = cy;
                        if (x1 <= x0) { int t = x0; x0 = x1; x1 = t; if (x1 == x0) x1 = x0 + 1; }
                        if (y1 <= y0) { int t = y0; y0 = y1; y1 = t; if (y1 == y0) y1 = y0 + 1; }
                        zp_sel_apply_transform(&app.sel, x0, y0, x1 - x0, y1 - y0);
                        snprintf(app.status_msg, sizeof(app.status_msg), "Transform %dx%d px", app.sel.buf.w, app.sel.buf.h);
                        needs_redraw = true;
                        break;
                    }

                    if (app.resize_handle >= 0) {
                        if (app.resize_handle == 0 || app.resize_handle == 2) app.resize_preview_w = cx > 1 ? cx : 1;
                        if (app.resize_handle == 1 || app.resize_handle == 2) app.resize_preview_h = cy > 1 ? cy : 1;
                        snprintf(app.status_msg, sizeof(app.status_msg), "Resize -> %dx%d", app.resize_preview_w, app.resize_preview_h);
                        needs_redraw = true;
                        break;
                    }

                    if (app.sel.dragging) {
                        app.sel.x = app.sel.drag_orig_x + (cx - app.sel.drag_start_mx);
                        app.sel.y = app.sel.drag_orig_y + (cy - app.sel.drag_start_my);
                        needs_redraw = true;
                        break;
                    }

                    if (app.drawing) {
                        uint32_t fg = (app.active_button == SDL_BUTTON_RIGHT) ? app.color2 : app.color1;
                        uint32_t bg = (app.active_button == SDL_BUTTON_RIGHT) ? app.color1 : app.color2;
                        if (app.tool == ZP_TOOL_PENCIL || app.tool == ZP_TOOL_BRUSH) {
                            int sz = (app.tool == ZP_TOOL_PENCIL) ? app.stroke_size : app.stroke_size * 2 + 1;
                            zp_draw_line(&app.canvas, app.prev_pt.x, app.prev_pt.y, cx, cy, sz, fg, app.brush_variant);
                            app.prev_pt = (ZPPoint){ cx, cy };
                        } else if (app.tool == ZP_TOOL_ERASER) {
                            zp_draw_line(&app.canvas, app.prev_pt.x, app.prev_pt.y, cx, cy, zp_eraser_size(app.stroke_size), app.color2, ZP_BRUSH_MARKER);
                            app.prev_pt = (ZPPoint){ cx, cy };
                        } else if (app.tool == ZP_TOOL_AIRBRUSH) {
                            zp_draw_airbrush(&app.canvas, cx, cy, app.stroke_size * 4 + 6, fg, &rng_state);
                        } else if (app.tool == ZP_TOOL_SELECT_LASSO) {
                            if (app.poly_count < ZP_MAX_POLY_PTS) {
                                ZPPoint last = app.poly_pts[app.poly_count - 1];
                                if (abs(cx - last.x) + abs(cy - last.y) >= 2) {
                                    app.poly_pts[app.poly_count++] = (ZPPoint){ cx, cy };
                                }
                            }
                        } else if (app.tool == ZP_TOOL_CURVE && shape_backup.pixels) {
                            if (app.curve_stage == 1) {
                                app.curve_pts[3] = (ZPPoint){ cx, cy };
                                app.curve_pts[1] = (ZPPoint){ (app.curve_pts[0].x + cx) / 2, (app.curve_pts[0].y + cy) / 2 };
                                app.curve_pts[2] = app.curve_pts[1];
                            } else if (app.curve_stage == 2) {
                                app.curve_pts[1] = (ZPPoint){ cx, cy };
                                app.curve_pts[2] = (ZPPoint){ cx, cy };
                            } else if (app.curve_stage == 3) {
                                app.curve_pts[2] = (ZPPoint){ cx, cy };
                            }
                            memcpy(app.canvas.pixels, shape_backup.pixels, (size_t)app.canvas.w * (size_t)app.canvas.h * sizeof(uint32_t));
                            zp_draw_bezier(&app.canvas, app.curve_pts[0], app.curve_pts[1], app.curve_pts[2], app.curve_pts[3], app.stroke_size, fg);
                        } else if (app.tool >= ZP_TOOL_LINE && shape_backup.pixels) {
                            memcpy(app.canvas.pixels, shape_backup.pixels, (size_t)app.canvas.w * (size_t)app.canvas.h * sizeof(uint32_t));
                            zp_draw_shape(&app.canvas, app.tool, app.start_pt.x, app.start_pt.y, cx, cy, app.stroke_size, fg, bg, app.shape_mode);
                        }
                        app.curr_pt = (ZPPoint){ cx, cy };
                    }
                    needs_redraw = true;
                    break;
                }

                case SDL_MOUSEBUTTONUP: {
                    if (app.panning) {
                        app.panning = false;
                        snprintf(app.status_msg, sizeof(app.status_msg), "Ready");
                        needs_redraw = true;
                        break;
                    }
                    if (app.sel.transform_handle >= 0) {
                        zp_sel_end_transform(&app.sel);
                        snprintf(app.status_msg, sizeof(app.status_msg), "Selection %dx%d px", app.sel.buf.w, app.sel.buf.h);
                        needs_redraw = true;
                        break;
                    }
                    if (app.resize_handle >= 0) {
                        zp_canvas_resize_borders(&app.canvas, app.resize_preview_w, app.resize_preview_h, app.color2);
                        zp_history_push(&app.history, &app.canvas);
                        app.resize_handle = -1;
                        app.dirty = true;
                        needs_redraw = true;
                        break;
                    }
                    if (app.sel.dragging) {
                        app.sel.dragging = false;
                        needs_redraw = true;
                        break;
                    }
                    if (app.drawing) {
                        app.drawing = false;
                        if (app.tool == ZP_TOOL_CURVE) {
                            if (app.curve_stage == 1) {
                                app.curve_stage = 2;
                                snprintf(app.status_msg, sizeof(app.status_msg), "Curve 2/3: drag bend 1");
                            } else if (app.curve_stage == 2) {
                                app.curve_stage = 3;
                                snprintf(app.status_msg, sizeof(app.status_msg), "Curve 3/3: drag bend 2");
                            } else if (app.curve_stage == 3) {
                                app.curve_stage = 0;
                                zp_img_free(&shape_backup);
                                zp_history_push(&app.history, &app.canvas);
                                app.dirty = true;
                                snprintf(app.status_msg, sizeof(app.status_msg), "Curve stamped");
                            }
                        } else if (app.tool == ZP_TOOL_SELECT_RECT) {
                            zp_sel_extract_rect(&app.canvas, app.start_pt.x, app.start_pt.y, app.curr_pt.x, app.curr_pt.y, app.color2, &app.sel, app.transparent_selection);
                            zp_img_free(&shape_backup);
                        } else if (app.tool == ZP_TOOL_SELECT_LASSO) {
                            zp_sel_extract_lasso(&app.canvas, app.poly_pts, app.poly_count, app.color2, &app.sel, app.transparent_selection);
                            app.poly_count = 0;
                            zp_img_free(&shape_backup);
                        } else {
                            zp_history_push(&app.history, &app.canvas);
                            app.dirty = true;
                            zp_img_free(&shape_backup);
                        }
                        needs_redraw = true;
                    }
                    break;
                }

                default:
                    break;
            }
        } while (SDL_PollEvent(&ev));
    }

    /* === cleanup === */
    SDL_StopTextInput();
    zp_ttf_shutdown();
    zp_img_free(&shape_backup);
    zp_sel_clear(&app.sel);
    zp_img_free(&app.clipboard);
    free(app.clipboard_mask);
    zp_history_free(&app.history);
    zp_img_free(&app.canvas);
    zp_img_free(&fb);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/* made by js3, enjoy, yep */
