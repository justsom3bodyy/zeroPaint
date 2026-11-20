/* zp_platform.h - linux clipboard (wayland/x11) and file picker bridge */
#ifndef ZP_PLATFORM_H
#define ZP_PLATFORM_H

#include "zp_types.h"

/* === linux desktop helpers === */
bool zp_platform_clipboard_copy(const ZPImage *img);
bool zp_platform_clipboard_paste(ZPImage *out);
bool zp_platform_file_dialog(bool save_mode, char *out_path, size_t out_cap);

#endif
