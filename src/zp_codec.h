/* zp_codec.h - built-in bmp, png, ppm and tga loaders/writers (no libpng/zlib) */
#ifndef ZP_CODEC_H
#define ZP_CODEC_H

#include "zp_types.h"

/* === file io === */
bool zp_load_image(const char *path, ZPImage *out);
bool zp_save_image(const char *path, const ZPImage *img);

/* === memory encoders for clipboard pipes === */
uint8_t *zp_encode_png_mem(const ZPImage *img, size_t *out_len);
uint8_t *zp_encode_bmp_mem(const ZPImage *img, size_t *out_len);
bool zp_decode_bmp_mem(const uint8_t *data, size_t len, ZPImage *out);

#endif
