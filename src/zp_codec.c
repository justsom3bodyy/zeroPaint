/* zp_codec.c - zero-dep bmp, png (stored deflate), ppm and tga codecs */
#include "zp_codec.h"
#include "zp_raster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* === endian and checksum helpers === */
static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void wr_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint32_t zp_crc32(const uint8_t *buf, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t zp_adler32(const uint8_t *buf, size_t len) {
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; ++i) {
        s1 = (s1 + buf[i]) % 65521u;
        s2 = (s2 + s1) % 65521u;
    }
    return (s2 << 16) | s1;
}

/* === png writer (no zlib needed) === */
/* writes valid uncompressed deflate blocks so any viewer opens it cleanly */
uint8_t *zp_encode_png_mem(const ZPImage *img, size_t *out_len) {
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0) return NULL;
    size_t row_raw = 1 + (size_t)img->w * 3;
    size_t raw_len = row_raw * (size_t)img->h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return NULL;

    for (int y = 0; y < img->h; ++y) {
        uint8_t *dst = raw + (size_t)y * row_raw;
        dst[0] = 0;
        const uint32_t *src = img->pixels + (size_t)y * (size_t)img->w;
        for (int x = 0; x < img->w; ++x) {
            uint32_t c = src[x];
            dst[1 + x * 3 + 0] = (uint8_t)ZP_R(c);
            dst[1 + x * 3 + 1] = (uint8_t)ZP_G(c);
            dst[1 + x * 3 + 2] = (uint8_t)ZP_B(c);
        }
    }

    size_t nblocks = (raw_len + 65534) / 65535;
    size_t zlen = 2 + nblocks * 5 + raw_len + 4;
    size_t total = 8 + (12 + 13) + (12 + zlen) + 12;
    uint8_t *png = (uint8_t *)malloc(total);
    if (!png) {
        free(raw);
        return NULL;
    }

    size_t pos = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(png + pos, sig, 8); pos += 8;

    wr_u32_be(png + pos, 13); pos += 4;
    size_t ihdr_start = pos;
    memcpy(png + pos, "IHDR", 4); pos += 4;
    wr_u32_be(png + pos, (uint32_t)img->w); pos += 4;
    wr_u32_be(png + pos, (uint32_t)img->h); pos += 4;
    png[pos++] = 8;
    png[pos++] = 2;
    png[pos++] = 0;
    png[pos++] = 0;
    png[pos++] = 0;
    wr_u32_be(png + pos, zp_crc32(png + ihdr_start, pos - ihdr_start)); pos += 4;

    wr_u32_be(png + pos, (uint32_t)zlen); pos += 4;
    size_t idat_start = pos;
    memcpy(png + pos, "IDAT", 4); pos += 4;
    png[pos++] = 0x78;
    png[pos++] = 0x01;

    size_t off = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        size_t chunk = raw_len - off;
        if (chunk > 65535) chunk = 65535;
        bool last = (b + 1 == nblocks);
        png[pos++] = last ? 0x01 : 0x00;
        wr_u16_le(png + pos, (uint16_t)chunk); pos += 2;
        wr_u16_le(png + pos, (uint16_t)(~chunk & 0xFFFFu)); pos += 2;
        memcpy(png + pos, raw + off, chunk); pos += chunk;
        off += chunk;
    }
    wr_u32_be(png + pos, zp_adler32(raw, raw_len)); pos += 4;
    wr_u32_be(png + pos, zp_crc32(png + idat_start, pos - idat_start)); pos += 4;

    wr_u32_be(png + pos, 0); pos += 4;
    size_t iend_start = pos;
    memcpy(png + pos, "IEND", 4); pos += 4;
    wr_u32_be(png + pos, zp_crc32(png + iend_start, 4)); pos += 4;

    free(raw);
    *out_len = pos;
    return png;
}

/* === bmp codec === */
uint8_t *zp_encode_bmp_mem(const ZPImage *img, size_t *out_len) {
    if (!img || !img->pixels || img->w <= 0 || img->h <= 0) return NULL;
    size_t row_stride = ((size_t)img->w * 3 + 3u) & ~3u;
    size_t img_size = row_stride * (size_t)img->h;
    size_t file_size = 54 + img_size;
    uint8_t *buf = (uint8_t *)calloc(file_size, 1);
    if (!buf) return NULL;

    buf[0] = 'B'; buf[1] = 'M';
    wr_u32_le(buf + 2, (uint32_t)file_size);
    wr_u32_le(buf + 10, 54);
    wr_u32_le(buf + 14, 40);
    wr_u32_le(buf + 18, (uint32_t)img->w);
    wr_u32_le(buf + 22, (uint32_t)img->h);
    wr_u16_le(buf + 26, 1);
    wr_u16_le(buf + 28, 24);
    wr_u32_le(buf + 34, (uint32_t)img_size);

    for (int y = 0; y < img->h; ++y) {
        uint8_t *dst = buf + 54 + (size_t)(img->h - 1 - y) * row_stride;
        const uint32_t *src = img->pixels + (size_t)y * (size_t)img->w;
        for (int x = 0; x < img->w; ++x) {
            uint32_t c = src[x];
            dst[x * 3 + 0] = (uint8_t)ZP_B(c);
            dst[x * 3 + 1] = (uint8_t)ZP_G(c);
            dst[x * 3 + 2] = (uint8_t)ZP_R(c);
        }
    }
    *out_len = file_size;
    return buf;
}

bool zp_decode_bmp_mem(const uint8_t *data, size_t len, ZPImage *out) {
    if (!data || len < 54 || data[0] != 'B' || data[1] != 'M') return false;
    uint32_t offset = rd_u32_le(data + 10);
    int32_t w = (int32_t)rd_u32_le(data + 18);
    int32_t h_raw = (int32_t)rd_u32_le(data + 22);
    uint16_t bpp = rd_u16_le(data + 28);
    uint32_t comp = rd_u32_le(data + 30);
    bool top_down = (h_raw < 0);
    int h = top_down ? -h_raw : h_raw;

    /* guard against bogus header dims before allocating */
    if (w < 1 || h < 1 || w > ZP_MAX_DIM || h > ZP_MAX_DIM) return false;
    if ((bpp != 24 && bpp != 32) || comp != 0) return false;

    size_t bytes_pp = bpp / 8;
    size_t row_stride = ((size_t)w * bytes_pp + 3u) & ~3u;
    if ((size_t)offset > len || row_stride * (size_t)h > len - (size_t)offset) return false;

    if (!zp_img_init(out, w, h, ZP_COL_WHITE)) return false;
    for (int y = 0; y < h; ++y) {
        int sy = top_down ? y : (h - 1 - y);
        const uint8_t *src = data + offset + (size_t)sy * row_stride;
        uint32_t *dst = out->pixels + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            uint8_t b = src[x * bytes_pp + 0];
            uint8_t g = src[x * bytes_pp + 1];
            uint8_t r = src[x * bytes_pp + 2];
            dst[x] = ZP_RGB(r, g, b);
        }
    }
    return true;
}

/* === ppm and tga decoders === */
static bool zp_decode_ppm(const uint8_t *data, size_t len, ZPImage *out) {
    if (len < 11 || data[0] != 'P' || data[1] != '6') return false;
    size_t i = 2;
    int vals[3] = { 0, 0, 0 };
    for (int k = 0; k < 3; ++k) {
        while (i < len && (isspace(data[i]) || data[i] == '#')) {
            if (data[i] == '#') { while (i < len && data[i] != '\n') i++; }
            else i++;
        }
        if (i >= len || !isdigit(data[i])) return false;
        int v = 0;
        while (i < len && isdigit(data[i])) { v = v * 10 + (data[i++] - '0'); }
        vals[k] = v;
    }
    if (i < len && isspace(data[i])) i++;
    int w = vals[0], h = vals[1], maxv = vals[2];
    if (w < 1 || h < 1 || w > ZP_MAX_DIM || h > ZP_MAX_DIM || maxv != 255) return false;
    if ((size_t)w * (size_t)h * 3 > len - i) return false;

    if (!zp_img_init(out, w, h, ZP_COL_WHITE)) return false;
    const uint8_t *p = data + i;
    for (size_t k = 0; k < (size_t)w * (size_t)h; ++k) {
        out->pixels[k] = ZP_RGB(p[k * 3], p[k * 3 + 1], p[k * 3 + 2]);
    }
    return true;
}

static bool zp_decode_tga(const uint8_t *data, size_t len, ZPImage *out) {
    if (len < 18 || data[2] != 2) return false;
    int id_len = data[0];
    int w = rd_u16_le(data + 12);
    int h = rd_u16_le(data + 14);
    int bpp = data[16];
    bool top_down = (data[17] & 0x20u) != 0;
    if (w < 1 || h < 1 || w > ZP_MAX_DIM || h > ZP_MAX_DIM || (bpp != 24 && bpp != 32)) return false;
    size_t step = (size_t)(bpp / 8);
    size_t off = 18 + (size_t)id_len;
    if (off > len || (size_t)w * (size_t)h * step > len - off) return false;

    if (!zp_img_init(out, w, h, ZP_COL_WHITE)) return false;
    for (int y = 0; y < h; ++y) {
        int sy = top_down ? y : (h - 1 - y);
        const uint8_t *row = data + off + (size_t)sy * (size_t)w * step;
        for (int x = 0; x < w; ++x) {
            out->pixels[(size_t)y * (size_t)w + (size_t)x] =
                ZP_RGB(row[x * step + 2], row[x * step + 1], row[x * step + 0]);
        }
    }
    return true;
}

/* === disk load / save === */
static bool ends_with_ci(const char *s, const char *ext) {
    size_t sl = strlen(s), el = strlen(ext);
    if (sl < el) return false;
    s += sl - el;
    for (size_t i = 0; i < el; ++i) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

bool zp_load_image(const char *path, ZPImage *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    bool ok = false;
    if (nread >= 2 && buf[0] == 'B' && buf[1] == 'M') {
        ok = zp_decode_bmp_mem(buf, nread, out);
    } else if (nread >= 2 && buf[0] == 'P' && buf[1] == '6') {
        ok = zp_decode_ppm(buf, nread, out);
    } else if (ends_with_ci(path, ".tga")) {
        ok = zp_decode_tga(buf, nread, out);
    }
    free(buf);
    return ok;
}

bool zp_save_image(const char *path, const ZPImage *img) {
    if (!path || !img || !img->pixels) return false;

    if (ends_with_ci(path, ".ppm")) {
        FILE *f = fopen(path, "wb");
        if (!f) return false;
        fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
        for (size_t i = 0; i < (size_t)img->w * (size_t)img->h; ++i) {
            uint8_t rgb[3] = { (uint8_t)ZP_R(img->pixels[i]), (uint8_t)ZP_G(img->pixels[i]), (uint8_t)ZP_B(img->pixels[i]) };
            fwrite(rgb, 1, 3, f);
        }
        fclose(f);
        return true;
    }

    if (ends_with_ci(path, ".tga")) {
        FILE *f = fopen(path, "wb");
        if (!f) return false;
        uint8_t hdr[18];
        memset(hdr, 0, sizeof(hdr));
        hdr[2] = 2;
        wr_u16_le(hdr + 12, (uint16_t)img->w);
        wr_u16_le(hdr + 14, (uint16_t)img->h);
        hdr[16] = 24;
        hdr[17] = 0x20;
        fwrite(hdr, 1, 18, f);
        for (size_t i = 0; i < (size_t)img->w * (size_t)img->h; ++i) {
            uint8_t bgr[3] = { (uint8_t)ZP_B(img->pixels[i]), (uint8_t)ZP_G(img->pixels[i]), (uint8_t)ZP_R(img->pixels[i]) };
            fwrite(bgr, 1, 3, f);
        }
        fclose(f);
        return true;
    }

    size_t len = 0;
    uint8_t *data = ends_with_ci(path, ".bmp")
        ? zp_encode_bmp_mem(img, &len)
        : zp_encode_png_mem(img, &len);
    if (!data) return false;

    FILE *f = fopen(path, "wb");
    if (!f) { free(data); return false; }
    bool ok = (fwrite(data, 1, len, f) == len);
    fclose(f);
    free(data);
    return ok;
}
