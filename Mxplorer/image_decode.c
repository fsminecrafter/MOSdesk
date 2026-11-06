/* PNG, JPEG and GIF decoders for pictures in pages. See
 * mxplorer/image_decode.h. Written for this browser: small, bounds-checked
 * against damaged files, no dependencies beyond malloc. */
#include "mxplorer/image_decode.h"
#include "string.h"
#include "stdlib.h"

static uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return a << 24 | r << 16 | g << 8 | b;
}

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

const char* mx_image_format(const uint8_t* d, size_t n) {
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "PNG";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "JPEG";
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "GIF";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "WebP";
    for (size_t i = 0; i < n && i < 256; i++) {
        if (d[i] == '<') return (n - i >= 4 && (!memcmp(d + i, "<svg", 4) || !memcmp(d + i, "<?xm", 4))) ? "SVG" : NULL;
        if (d[i] != ' ' && d[i] != '\t' && d[i] != '\r' && d[i] != '\n' && d[i] != 0xEF && d[i] != 0xBB && d[i] != 0xBF)
            break;
    }
    return NULL;
}

/* ====================================================================== */
/* Inflate (zlib), for PNG                                                */
/* ====================================================================== */

typedef struct {
    const uint8_t* in;
    size_t in_len, in_pos;
    uint64_t bitbuf;
    int bitcnt;
    uint8_t* out;
    size_t out_len, out_cap;
    int err;
} inflate_t;

typedef struct {
    short count[16];
    short symbol[288];
} huff_t;

static int inf_bits(inflate_t* s, int need) {
    uint64_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) { s->err = 1; return 0; }
        val |= (uint64_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1ull << need) - 1));
}

static int inf_decode(inflate_t* s, const huff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static int inf_construct(huff_t* h, const short* length, int n) {
    for (int len = 0; len < 16; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym]) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static void inf_put(inflate_t* s, uint8_t b) {
    if (s->out_len < s->out_cap) s->out[s->out_len++] = b;
    else s->err = 2;            /* more data than the picture needs */
}

static void inf_codes(inflate_t* s, const huff_t* lencode, const huff_t* distcode) {
    static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                    6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = inf_decode(s, lencode);
        if (s->err) return;
        if (sym < 256) {
            inf_put(s, (uint8_t)sym);
            if (s->err) return;
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) { s->err = 1; return; }
            int len = lbase[sym] + inf_bits(s, lext[sym]);
            int dsym = inf_decode(s, distcode);
            if (s->err || dsym < 0 || dsym >= 30) { s->err = 1; return; }
            size_t dist = (size_t)(dbase[dsym] + inf_bits(s, dext[dsym]));
            if (s->err || dist > s->out_len) { s->err = 1; return; }
            while (len--) {
                inf_put(s, s->out[s->out_len - dist]);
                if (s->err) return;
            }
        }
    }
}

static void inf_stored(inflate_t* s) {
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->in_pos + 4 > s->in_len) { s->err = 1; return; }
    unsigned len = s->in[s->in_pos] | (unsigned)s->in[s->in_pos + 1] << 8;
    unsigned nlen = s->in[s->in_pos + 2] | (unsigned)s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFFu) || s->in_pos + len > s->in_len) { s->err = 1; return; }
    while (len--) {
        inf_put(s, s->in[s->in_pos++]);
        if (s->err) return;
    }
}

static void inf_fixed(inflate_t* s) {
    static huff_t lencode, distcode;
    static int built;
    if (!built) {
        short lengths[320];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < 288; sym++) lengths[sym] = 8;
        inf_construct(&lencode, lengths, 288);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        inf_construct(&distcode, lengths, 30);
        built = 1;
    }
    inf_codes(s, &lencode, &distcode);
}

static void inf_dynamic(inflate_t* s) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    huff_t lencode, distcode;
    int nlen = inf_bits(s, 5) + 257, ndist = inf_bits(s, 5) + 1, ncode = inf_bits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) { s->err = 1; return; }
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)inf_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err || inf_construct(&lencode, lengths, 19) != 0) { s->err = 1; return; }
    index = 0;
    while (index < nlen + ndist) {
        int sym = inf_decode(s, &lencode);
        if (s->err || sym < 0) { s->err = 1; return; }
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) { s->err = 1; return; }
                len = lengths[index - 1];
                rep = 3 + inf_bits(s, 2);
            } else if (sym == 17) {
                rep = 3 + inf_bits(s, 3);
            } else {
                rep = 11 + inf_bits(s, 7);
            }
            if (s->err || index + rep > nlen + ndist) { s->err = 1; return; }
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) { s->err = 1; return; }
    int err = inf_construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) { s->err = 1; return; }
    err = inf_construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) { s->err = 1; return; }
    inf_codes(s, &lencode, &distcode);
}

/* zlib stream -> out (exactly out_cap bytes expected). Returns bytes made. */
static size_t zlib_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if (in_len < 2 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31) return 0;
    inflate_t s;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.in_len = in_len;
    s.in_pos = 2;
    s.out = out;
    s.out_cap = out_cap;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) break;
        if (type == 0) inf_stored(&s);
        else if (type == 1) inf_fixed(&s);
        else if (type == 2) inf_dynamic(&s);
        else s.err = 1;
    } while (!last && !s.err);
    return s.out_len;
}

/* ====================================================================== */
/* PNG                                                                    */
/* ====================================================================== */

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p,
        pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static int png_unfilter(uint8_t* data, uint32_t rows, size_t row_bytes, int bpp) {
    uint8_t* prev = NULL;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t* line = data + (size_t)y * (row_bytes + 1);
        int filter = line[0];
        uint8_t* cur = line + 1;
        for (size_t i = 0; i < row_bytes; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0;
            int b = prev ? prev[i] : 0;
            int c = prev && i >= (size_t)bpp ? prev[i - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: return 0;
            }
        }
        prev = cur;
    }
    return 1;
}

static uint32_t* png_decode(const uint8_t* d, size_t n, int* out_w, int* out_h, int* has_alpha,
                            const char** why) {
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    uint32_t palette[256];
    int palette_count = 0;
    for (int i = 0; i < 256; i++) palette[i] = 0xFF000000u;
    int trns = 0;
    uint16_t trns_key[3] = {0, 0, 0};
    uint8_t* idat = NULL;
    size_t idat_len = 0, idat_cap = 0;
    uint32_t* px = NULL;
    uint8_t* raw = NULL;
    *why = "damaged PNG";
    while (pos + 12 <= n) {
        uint32_t len = be32(d + pos);
        const uint8_t* type = d + pos + 4;
        const uint8_t* body = d + pos + 8;
        if (len > n - pos - 12) break;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(body);
            h = be32(body + 4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            palette_count = (int)(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < palette_count; i++)
                palette[i] = argb(255, body[i * 3], body[i * 3 + 1], body[i * 3 + 2]);
        } else if (!memcmp(type, "tRNS", 4)) {
            trns = 1;
            if (ctype == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++)
                    palette[i] = (palette[i] & 0x00FFFFFFu) | (uint32_t)body[i] << 24;
            } else if (ctype == 0 && len >= 2) {
                trns_key[0] = (uint16_t)(body[0] << 8 | body[1]);
            } else if (ctype == 2 && len >= 6) {
                for (int i = 0; i < 3; i++) trns_key[i] = (uint16_t)(body[i * 2] << 8 | body[i * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_len + len > idat_cap) {
                size_t cap = idat_cap ? idat_cap * 2 : 65536;
                while (cap < idat_len + len) cap *= 2;
                uint8_t* grown = (uint8_t*)malloc(cap);
                if (!grown) { *why = "not enough memory"; goto fail; }
                if (idat_len) memcpy(grown, idat, idat_len);
                free(idat);
                idat = grown;
                idat_cap = cap;
            }
            memcpy(idat + idat_len, body, len);
            idat_len += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        pos += 12 + len;
    }
    int channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (!w || !h || !channels || !idat || interlace > 1 ||
        !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) ||
        (ctype == 3 && depth == 16) || ((ctype == 2 || ctype == 4 || ctype == 6) && depth < 8))
        goto fail;
    if ((uint64_t)w * h > MX_IMAGE_MAX_PIXELS) { *why = "picture too large"; goto fail; }

    static const int ax0[7] = {0, 4, 0, 2, 0, 1, 0}, ay0[7] = {0, 0, 4, 0, 2, 0, 1};
    static const int adx[7] = {8, 8, 4, 4, 2, 2, 1}, ady[7] = {8, 8, 8, 4, 4, 2, 2};
    int passes = interlace ? 7 : 1;
    int bits = depth * channels;
    int bpp = bits >= 8 ? bits / 8 : 1;
    size_t total = 0;
    for (int p = 0; p < passes; p++) {
        uint32_t x0 = interlace ? (uint32_t)ax0[p] : 0, y0 = interlace ? (uint32_t)ay0[p] : 0;
        uint32_t dx = interlace ? (uint32_t)adx[p] : 1, dy = interlace ? (uint32_t)ady[p] : 1;
        uint32_t pw = w > x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > y0 ? (h - y0 + dy - 1) / dy : 0;
        if (pw && ph) total += (size_t)ph * (1 + ((size_t)pw * bits + 7) / 8);
    }
    raw = (uint8_t*)malloc(total);
    px = (uint32_t*)malloc((size_t)w * h * 4u);
    if (!raw || !px) { *why = "not enough memory"; goto fail; }
    size_t made = zlib_inflate(idat, idat_len, raw, total);
    if (made < total) memset(raw + made, 0, total - made);    /* show what decoded */
    if (!made) goto fail;

    int alpha = ctype == 4 || ctype == 6 || trns;
    size_t at = 0;
    for (int p = 0; p < passes; p++) {
        uint32_t x0 = interlace ? (uint32_t)ax0[p] : 0, y0 = interlace ? (uint32_t)ay0[p] : 0;
        uint32_t dx = interlace ? (uint32_t)adx[p] : 1, dy = interlace ? (uint32_t)ady[p] : 1;
        uint32_t pw = w > x0 ? (w - x0 + dx - 1) / dx : 0, ph = h > y0 ? (h - y0 + dy - 1) / dy : 0;
        if (!pw || !ph) continue;
        size_t row_bytes = ((size_t)pw * bits + 7) / 8;
        if (!png_unfilter(raw + at, ph, row_bytes, bpp)) goto fail;
        for (uint32_t y = 0; y < ph; y++) {
            const uint8_t* row = raw + at + (size_t)y * (row_bytes + 1) + 1;
            uint32_t* dst = px + (size_t)(y0 + y * dy) * w;
            for (uint32_t x = 0; x < pw; x++) {
                uint32_t v[4] = {0, 0, 0, 0}, raw16[4] = {0, 0, 0, 0};
                if (depth < 8) {
                    size_t bit = (size_t)x * depth;
                    uint32_t s = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
                    raw16[0] = s;
                    v[0] = ctype == 3 ? s : s * 255u / ((1u << depth) - 1);
                } else {
                    for (int c = 0; c < channels; c++) {
                        if (depth == 16) {
                            raw16[c] = (uint32_t)row[(x * channels + c) * 2] << 8 | row[(x * channels + c) * 2 + 1];
                            v[c] = raw16[c] >> 8;
                        } else {
                            raw16[c] = v[c] = row[x * channels + c];
                        }
                    }
                }
                uint32_t c;
                switch (ctype) {
                case 0:
                    c = argb(trns && raw16[0] == trns_key[0] ? 0 : 255, v[0], v[0], v[0]);
                    break;
                case 2:
                    c = argb(trns && raw16[0] == trns_key[0] && raw16[1] == trns_key[1] &&
                             raw16[2] == trns_key[2] ? 0 : 255, v[0], v[1], v[2]);
                    break;
                case 3: c = palette[v[0] & 255]; break;
                case 4: c = argb(v[1], v[0], v[0], v[0]); break;
                default: c = argb(v[3], v[0], v[1], v[2]); break;
                }
                dst[x0 + x * dx] = c;
            }
        }
        at += (size_t)ph * (row_bytes + 1);
    }
    free(raw);
    free(idat);
    *out_w = (int)w;
    *out_h = (int)h;
    *has_alpha = alpha;
    return px;
fail:
    free(raw);
    free(idat);
    free(px);
    return NULL;
}

/* ====================================================================== */
/* JPEG (baseline and progressive Huffman)                               */
/* ====================================================================== */

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

typedef struct {
    uint8_t lut_len[256];          /* codes up to 8 bits: length, 0 = longer */
    uint8_t lut_val[256];
    int32_t maxcode[18];
    int32_t valptr[17];
    int32_t mincode[17];
    uint8_t vals[256];
    int present;
} jhuff_t;

typedef struct {
    int id, hs, vs, tq;
    int bw, bh;                    /* blocks allocated (whole MCUs) */
    int cbw, cbh;                  /* blocks with picture in them */
    int16_t* coef;
    uint8_t* pixels;               /* bw*8 x bh*8 after the inverse DCT */
    int pred;
    int dc_table, ac_table;
} jcomp_t;

typedef struct {
    const uint8_t* d;
    size_t n, pos;
    uint32_t buf;
    int cnt;
    int marker;
    uint16_t qt[4][64];            /* natural order */
    jhuff_t dc[4], ac[4];
    jcomp_t comp[4];
    int ncomp;
    int w, h, hmax, vmax, mcux, mcuy;
    int progressive;
    int restart;
    int eobrun;
    int adobe, adobe_transform;
} jpeg_t;

static void jb_fill(jpeg_t* j) {
    while (j->cnt <= 24) {
        uint32_t byte = 0;
        if (!j->marker && j->pos < j->n) {
            byte = j->d[j->pos];
            if (byte == 0xFF) {
                uint8_t next = j->pos + 1 < j->n ? j->d[j->pos + 1] : 0;
                if (next == 0x00) {
                    j->pos += 2;
                } else {
                    j->marker = 1;      /* stays at the marker for the parser */
                    byte = 0;
                }
            } else {
                j->pos++;
            }
        }
        j->buf |= byte << (24 - j->cnt);
        j->cnt += 8;
    }
}

static int jb_bits(jpeg_t* j, int n) {
    if (!n) return 0;
    jb_fill(j);
    int v = (int)(j->buf >> (32 - n));
    j->buf <<= n;
    j->cnt -= n;
    return v;
}

static int jb_bit(jpeg_t* j) { return jb_bits(j, 1); }

static int jb_extend(int v, int s) {
    return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static void jb_reset(jpeg_t* j) {
    j->buf = 0;
    j->cnt = 0;
    j->marker = 0;
}

static int jhuff_build(jhuff_t* t, const uint8_t* counts, const uint8_t* vals, int total) {
    memset(t, 0, sizeof(*t));
    memcpy(t->vals, vals, (size_t)total);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k;
        t->mincode[l] = code;
        for (int i = 0; i < counts[l - 1]; i++) {
            if (l <= 8) {
                int shift = 8 - l;
                for (int f = 0; f < (1 << shift); f++) {
                    int idx = (code << shift) | f;
                    if (idx < 256) { t->lut_len[idx] = (uint8_t)l; t->lut_val[idx] = vals[k]; }
                }
            }
            code++;
            k++;
        }
        t->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        if (code > (1 << l)) return 0;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->present = 1;
    return 1;
}

static int jhuff_decode(jpeg_t* j, const jhuff_t* t) {
    jb_fill(j);
    uint32_t peek = j->buf >> 24;
    int len = t->lut_len[peek];
    if (len) {
        j->buf <<= len;
        j->cnt -= len;
        return t->lut_val[peek];
    }
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | jb_bit(j);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l])
            return t->vals[t->valptr[l] + code - t->mincode[l]];
    }
    return -1;
}

/* One block of one scan. Returns 0 on a damaged stream. */
static int jpeg_block(jpeg_t* j, jcomp_t* c, int16_t* blk, int ss, int se, int ah, int al) {
    if (!j->progressive) {
        int t = jhuff_decode(j, &j->dc[c->dc_table]);
        if (t < 0 || t > 16) return 0;
        c->pred += t ? jb_extend(jb_bits(j, t), t) : 0;
        blk[0] = (int16_t)c->pred;
        for (int k = 1; k < 64;) {
            int rs = jhuff_decode(j, &j->ac[c->ac_table]);
            if (rs < 0) return 0;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return 0;
            blk[zigzag[k++]] = (int16_t)jb_extend(jb_bits(j, s), s);
        }
        return 1;
    }
    if (ss == 0) {                                  /* DC */
        if (!ah) {
            int t = jhuff_decode(j, &j->dc[c->dc_table]);
            if (t < 0 || t > 16) return 0;
            c->pred += t ? jb_extend(jb_bits(j, t), t) : 0;
            blk[0] = (int16_t)(c->pred * (1 << al));
        } else if (jb_bit(j)) {
            blk[0] = (int16_t)(blk[0] | (1 << al));
        }
        return 1;
    }
    if (!ah) {                                      /* AC, first pass */
        if (j->eobrun) { j->eobrun--; return 1; }
        for (int k = ss; k <= se;) {
            int rs = jhuff_decode(j, &j->ac[c->ac_table]);
            if (rs < 0) return 0;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) {
                    j->eobrun = (1 << r) - 1;
                    if (r) j->eobrun += jb_bits(j, r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return 0;
            blk[zigzag[k++]] = (int16_t)(jb_extend(jb_bits(j, s), s) * (1 << al));
        }
        return 1;
    }
    /* AC refinement */
    int bit = 1 << al;
    if (j->eobrun) {
        j->eobrun--;
        for (int k = ss; k <= se; k++) {
            int16_t* p = &blk[zigzag[k]];
            if (*p && jb_bit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
        }
        return 1;
    }
    int k = ss;
    do {
        int rs = jhuff_decode(j, &j->ac[c->ac_table]);
        if (rs < 0) return 0;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r) j->eobrun += jb_bits(j, r);
                r = 64;
            }
        } else {
            if (s != 1) return 0;
            s = jb_bit(j) ? bit : -bit;
        }
        while (k <= se) {
            int16_t* p = &blk[zigzag[k++]];
            if (*p) {
                if (jb_bit(j) && !(*p & bit)) *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (!r) { *p = (int16_t)s; break; }
                r--;
            }
        }
    } while (k <= se);
    return 1;
}

/* Skips a restart marker after `interval` units. */
static void jpeg_restart(jpeg_t* j) {
    jb_reset(j);
    while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7))
        j->pos++;
    if (j->pos + 1 < j->n) j->pos += 2;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
    j->eobrun = 0;
}

static int jpeg_scan(jpeg_t* j, const uint8_t* sos, size_t len) {
    int ns = sos[0];
    if (ns < 1 || ns > 4 || len < (size_t)(1 + ns * 2 + 3)) return 0;
    jcomp_t* in_scan[4];
    for (int i = 0; i < ns; i++) {
        int id = sos[1 + i * 2], tables = sos[2 + i * 2];
        in_scan[i] = NULL;
        for (int c = 0; c < j->ncomp; c++)
            if (j->comp[c].id == id) in_scan[i] = &j->comp[c];
        if (!in_scan[i]) return 0;
        in_scan[i]->dc_table = (tables >> 4) & 3;
        in_scan[i]->ac_table = tables & 3;
    }
    int ss = sos[1 + ns * 2], se = sos[2 + ns * 2];
    int ah = sos[3 + ns * 2] >> 4, al = sos[3 + ns * 2] & 15;
    if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
    if (ss > 63 || se > 63 || ss > se) return 0;
    for (int i = 0; i < ns; i++) {
        if ((ss == 0 && !ah && !j->dc[in_scan[i]->dc_table].present) ||
            ((ss > 0 || !j->progressive) && !j->ac[in_scan[i]->ac_table].present))
            return 0;
        in_scan[i]->pred = 0;
    }
    jb_reset(j);
    j->eobrun = 0;
    int todo = j->restart;
    if (ns == 1) {
        jcomp_t* c = in_scan[0];
        for (int by = 0; by < c->cbh; by++)
            for (int bx = 0; bx < c->cbw; bx++) {
                if (!jpeg_block(j, c, c->coef + ((size_t)by * c->bw + bx) * 64, ss, se, ah, al)) return 0;
                if (j->restart && --todo == 0) { jpeg_restart(j); todo = j->restart; }
            }
    } else {
        for (int my = 0; my < j->mcuy; my++)
            for (int mx = 0; mx < j->mcux; mx++) {
                for (int i = 0; i < ns; i++) {
                    jcomp_t* c = in_scan[i];
                    for (int y = 0; y < c->vs; y++)
                        for (int x = 0; x < c->hs; x++) {
                            int bx = mx * c->hs + x, by = my * c->vs + y;
                            if (!jpeg_block(j, c, c->coef + ((size_t)by * c->bw + bx) * 64, ss, se, ah, al))
                                return 0;
                        }
                }
                if (j->restart && --todo == 0) { jpeg_restart(j); todo = j->restart; }
            }
    }
    /* continue after the entropy-coded data, at the next marker */
    while (j->pos + 1 < j->n && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 &&
                                  !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)))
        j->pos++;
    return 1;
}

/* cos((2x+1) u pi / 16) * C(u) / 2, in 1/4096 */
static int idct_table[8][8];

static void idct_init(void) {
    static const double cosk[9] = {1.0, 0.980785280, 0.923879533, 0.831469612, 0.707106781,
                                   0.555570233, 0.382683432, 0.195090322, 0.0};
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++) {
            int m = ((2 * x + 1) * u) % 32;
            int sign = 1;
            if (m > 16) m = 32 - m;
            if (m > 8) { m = 16 - m; sign = -1; }
            double v = sign * cosk[m] * (u ? 0.5 : 0.5 * 0.707106781);
            idct_table[x][u] = (int)(v * 4096.0 + (v >= 0 ? 0.5 : -0.5));
        }
}

static void idct_block(const int16_t* in, const uint16_t* q, uint8_t* out, int stride) {
    int64_t tmp[64];
    int deq[64];
    for (int i = 0; i < 64; i++) deq[i] = in[i] * q[i];
    for (int u = 0; u < 8; u++)              /* columns: over v for each u */
        for (int y = 0; y < 8; y++) {
            int64_t s = 0;
            for (int v = 0; v < 8; v++) s += (int64_t)deq[v * 8 + u] * idct_table[y][v];
            tmp[y * 8 + u] = s;          /* scaled by 4096 */
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int64_t s = 0;
            for (int u = 0; u < 8; u++) s += tmp[y * 8 + u] * idct_table[x][u];
            int v = (int)((s + (1ll << 23)) >> 24) + 128;
            out[y * stride + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
}

static uint32_t* jpeg_decode(const uint8_t* d, size_t n, int* out_w, int* out_h, int* has_alpha,
                             const char** why) {
    static int idct_ready;
    if (!idct_ready) { idct_init(); idct_ready = 1; }
    jpeg_t* j = (jpeg_t*)calloc(1, sizeof(jpeg_t));
    uint32_t* px = NULL;
    if (!j) { *why = "not enough memory"; return NULL; }
    j->d = d;
    j->n = n;
    j->pos = 2;
    *why = "damaged JPEG";
    int frame = 0, scans = 0;
    while (j->pos + 4 <= n) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        int m = d[j->pos + 1];
        if (m == 0xFF) { j->pos++; continue; }
        j->pos += 2;
        if (m == 0xD9) break;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
        if (j->pos + 2 > n) break;
        size_t len = (size_t)d[j->pos] << 8 | d[j->pos + 1];
        if (len < 2 || j->pos + len > n) break;
        const uint8_t* b = d + j->pos + 2;
        size_t blen = len - 2;
        j->pos += len;
        if (m == 0xDB) {                                       /* DQT */
            for (size_t p = 0; p < blen;) {
                int pq = b[p] >> 4, tq = b[p] & 3;
                p++;
                for (int k = 0; k < 64 && p < blen; k++) {
                    uint16_t v = pq ? (uint16_t)(b[p] << 8 | b[p + 1]) : b[p];
                    p += pq ? 2 : 1;
                    j->qt[tq][zigzag[k]] = v;
                }
            }
        } else if (m == 0xC4) {                                /* DHT */
            for (size_t p = 0; p + 17 <= blen;) {
                int tc = b[p] >> 4, th = b[p] & 3;
                const uint8_t* counts = b + p + 1;
                int total = 0;
                for (int i = 0; i < 16; i++) total += counts[i];
                if (total > 256 || p + 17 + (size_t)total > blen) goto fail;
                if (!jhuff_build(tc ? &j->ac[th] : &j->dc[th], counts, b + p + 17, total)) goto fail;
                p += 17 + (size_t)total;
            }
        } else if (m == 0xDD && blen >= 2) {                  /* DRI */
            j->restart = b[0] << 8 | b[1];
        } else if (m == 0xEE && blen >= 12 && !memcmp(b, "Adobe", 5)) {
            j->adobe = 1;
            j->adobe_transform = b[11];
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {      /* SOF */
            if (frame || blen < 6) goto fail;
            frame = 1;
            j->progressive = m == 0xC2;
            if (b[0] != 8) { *why = "12-bit JPEG not supported"; goto fail; }
            j->h = b[1] << 8 | b[2];
            j->w = b[3] << 8 | b[4];
            j->ncomp = b[5];
            if (!j->w || !j->h || (j->ncomp != 1 && j->ncomp != 3 && j->ncomp != 4) ||
                blen < 6 + (size_t)j->ncomp * 3)
                goto fail;
            if ((uint64_t)j->w * j->h > MX_IMAGE_MAX_PIXELS) { *why = "picture too large"; goto fail; }
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->ncomp; i++) {
                jcomp_t* c = &j->comp[i];
                c->id = b[6 + i * 3];
                c->hs = b[7 + i * 3] >> 4;
                c->vs = b[7 + i * 3] & 15;
                c->tq = b[8 + i * 3] & 3;
                if (c->hs < 1 || c->hs > 4 || c->vs < 1 || c->vs > 4) goto fail;
                if (c->hs > j->hmax) j->hmax = c->hs;
                if (c->vs > j->vmax) j->vmax = c->vs;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                jcomp_t* c = &j->comp[i];
                c->bw = j->mcux * c->hs;
                c->bh = j->mcuy * c->vs;
                c->cbw = ((j->w * c->hs + j->hmax - 1) / j->hmax + 7) / 8;
                c->cbh = ((j->h * c->vs + j->vmax - 1) / j->vmax + 7) / 8;
                c->coef = (int16_t*)calloc((size_t)c->bw * c->bh * 64, sizeof(int16_t));
                if (!c->coef) { *why = "not enough memory"; goto fail; }
            }
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            *why = "this kind of JPEG (lossless or arithmetic) is not supported";
            goto fail;
        } else if (m == 0xDA) {                                /* SOS */
            if (!frame) goto fail;
            if (!jpeg_scan(j, b, blen)) {
                if (!scans) goto fail;
                break;                      /* show the scans that did decode */
            }
            scans++;
        }
    }
    if (!frame || !scans) goto fail;

    for (int i = 0; i < j->ncomp; i++) {
        jcomp_t* c = &j->comp[i];
        int stride = c->bw * 8;
        c->pixels = (uint8_t*)malloc((size_t)stride * c->bh * 8);
        if (!c->pixels) { *why = "not enough memory"; goto fail; }
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++)
                idct_block(c->coef + ((size_t)by * c->bw + bx) * 64, j->qt[c->tq],
                           c->pixels + (size_t)by * 8 * stride + bx * 8, stride);
        free(c->coef);
        c->coef = NULL;
    }
    px = (uint32_t*)malloc((size_t)j->w * j->h * 4u);
    if (!px) { *why = "not enough memory"; goto fail; }
    int ycc = j->ncomp == 3 ? (!j->adobe || j->adobe_transform != 0) : 0;
    for (int y = 0; y < j->h; y++) {
        uint32_t* dst = px + (size_t)y * j->w;
        for (int x = 0; x < j->w; x++) {
            int v[4];
            for (int i = 0; i < j->ncomp; i++) {
                jcomp_t* c = &j->comp[i];
                size_t stride = (size_t)c->bw * 8;
                if (c->hs == j->hmax && c->vs == j->vmax) {
                    v[i] = c->pixels[(size_t)y * stride + x];
                    continue;
                }
                /* subsampled: bilinear between sample centres, as libjpeg's
                 * "fancy upsampling" does (in 1/256 of a sample) */
                int cw = (j->w * c->hs + j->hmax - 1) / j->hmax;
                int ch = (j->h * c->vs + j->vmax - 1) / j->vmax;
                int fx = ((2 * x + 1) * c->hs * 128) / j->hmax - 128;
                int fy = ((2 * y + 1) * c->vs * 128) / j->vmax - 128;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int x0 = fx >> 8, y0 = fy >> 8, tx = fx & 255, ty = fy & 255;
                int x1 = x0 + 1 < cw ? x0 + 1 : cw - 1, y1 = y0 + 1 < ch ? y0 + 1 : ch - 1;
                if (x0 >= cw) x0 = x1 = cw - 1;
                if (y0 >= ch) y0 = y1 = ch - 1;
                const uint8_t* r0 = c->pixels + (size_t)y0 * stride;
                const uint8_t* r1 = c->pixels + (size_t)y1 * stride;
                int top = r0[x0] * (256 - tx) + r0[x1] * tx;
                int bottom = r1[x0] * (256 - tx) + r1[x1] * tx;
                v[i] = (top * (256 - ty) + bottom * ty + 32768) >> 16;
            }
            int r, g, bl;
            if (j->ncomp == 1) {
                r = g = bl = v[0];
            } else if (j->ncomp == 3 && ycc) {
                int yy = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                r = (yy + 91881 * cr + 32768) >> 16;
                g = (yy - 22554 * cb - 46802 * cr + 32768) >> 16;
                bl = (yy + 116130 * cb + 32768) >> 16;
            } else if (j->ncomp == 3) {
                r = v[0]; g = v[1]; bl = v[2];
            } else {
                /* CMYK, as Adobe writes it (inverted); YCCK first converted */
                int c0 = v[0], m0 = v[1], y0 = v[2], k = v[3];
                if (j->adobe_transform == 2) {
                    int yy = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                    c0 = 255 - ((yy + 91881 * cr + 32768) >> 16);
                    m0 = 255 - ((yy - 22554 * cb - 46802 * cr + 32768) >> 16);
                    y0 = 255 - ((yy + 116130 * cb + 32768) >> 16);
                    c0 = c0 < 0 ? 0 : c0 > 255 ? 255 : c0;
                    m0 = m0 < 0 ? 0 : m0 > 255 ? 255 : m0;
                    y0 = y0 < 0 ? 0 : y0 > 255 ? 255 : y0;
                    c0 = 255 - c0; m0 = 255 - m0; y0 = 255 - y0;
                }
                r = c0 * k / 255; g = m0 * k / 255; bl = y0 * k / 255;
            }
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
            dst[x] = argb(255, (uint32_t)r, (uint32_t)g, (uint32_t)bl);
        }
    }
    *out_w = j->w;
    *out_h = j->h;
    *has_alpha = 0;
    for (int i = 0; i < 4; i++) { free(j->comp[i].coef); free(j->comp[i].pixels); }
    free(j);
    return px;
fail:
    for (int i = 0; i < 4; i++) { free(j->comp[i].coef); free(j->comp[i].pixels); }
    free(j);
    free(px);
    return NULL;
}

/* ====================================================================== */
/* GIF (first frame)                                                      */
/* ====================================================================== */

static uint32_t* gif_decode(const uint8_t* d, size_t n, int* out_w, int* out_h, int* has_alpha,
                            const char** why) {
    *why = "damaged GIF";
    if (n < 13) return NULL;
    int w = d[6] | d[7] << 8, h = d[8] | d[9] << 8;
    if (!w || !h) return NULL;
    if ((uint64_t)w * h > MX_IMAGE_MAX_PIXELS) { *why = "picture too large"; return NULL; }
    uint32_t global[256], local[256];
    int gflags = d[10];
    size_t pos = 13;
    int gcount = 0;
    if (gflags & 0x80) {
        gcount = 2 << (gflags & 7);
        if (pos + (size_t)gcount * 3 > n) return NULL;
        for (int i = 0; i < gcount; i++)
            global[i] = argb(255, d[pos + i * 3], d[pos + i * 3 + 1], d[pos + i * 3 + 2]);
        pos += (size_t)gcount * 3;
    }
    int transparent = -1;
    uint32_t* px = (uint32_t*)calloc((size_t)w * h, 4);   /* transparent until drawn */
    uint8_t* idx = NULL;
    uint16_t* prefix = NULL;
    uint8_t* suffix = NULL;
    uint8_t* stack = NULL;
    if (!px) { *why = "not enough memory"; return NULL; }
    while (pos < n) {
        int block = d[pos++];
        if (block == 0x3B) break;
        if (block == 0x21) {                       /* extension */
            if (pos >= n) break;
            int label = d[pos++];
            while (pos < n) {
                int len = d[pos++];
                if (!len) break;
                if (label == 0xF9 && len >= 4 && pos + 4 <= n && (d[pos] & 1)) transparent = d[pos + 3];
                pos += (size_t)len;
            }
            continue;
        }
        if (block != 0x2C || pos + 9 > n) break;
        int fx = d[pos] | d[pos + 1] << 8, fy = d[pos + 2] | d[pos + 3] << 8;
        int fw = d[pos + 4] | d[pos + 5] << 8, fh = d[pos + 6] | d[pos + 7] << 8;
        int flags = d[pos + 8];
        pos += 9;
        const uint32_t* pal = global;
        int count = gcount;
        if (flags & 0x80) {
            count = 2 << (flags & 7);
            if (pos + (size_t)count * 3 > n) break;
            for (int i = 0; i < count; i++)
                local[i] = argb(255, d[pos + i * 3], d[pos + i * 3 + 1], d[pos + i * 3 + 2]);
            pos += (size_t)count * 3;
            pal = local;
        }
        if (pos >= n || !fw || !fh) break;
        int min_code = d[pos++];
        if (min_code < 2 || min_code > 11) break;
        /* gather the sub-blocks */
        size_t start = pos, total = 0;
        while (pos < n && d[pos]) { total += d[pos]; pos += (size_t)d[pos] + 1; }
        pos++;
        uint8_t* data = (uint8_t*)malloc(total + 4);
        idx = (uint8_t*)malloc((size_t)fw * fh);
        prefix = (uint16_t*)malloc(4096 * sizeof(uint16_t));
        suffix = (uint8_t*)malloc(4096);
        stack = (uint8_t*)malloc(4097);
        if (!data || !idx || !prefix || !suffix || !stack) { free(data); *why = "not enough memory"; goto fail; }
        size_t at = 0;
        for (size_t p = start; p < n && d[p] && at < total;) {
            size_t len = d[p];
            if (p + 1 + len > n) len = n - p - 1;
            memcpy(data + at, d + p + 1, len);
            at += len;
            p += d[p] + 1;
        }
        /* LZW */
        int clear = 1 << min_code, eoi = clear + 1;
        int size = min_code + 1, next = eoi + 1, old = -1;
        uint8_t first = 0;
        size_t out = 0, need = (size_t)fw * fh;
        uint32_t bits = 0;
        int nbits = 0;
        size_t bp = 0;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
        while (out < need) {
            while (nbits < size && bp < at) { bits |= (uint32_t)data[bp++] << nbits; nbits += 8; }
            if (nbits < size) break;
            int code = (int)(bits & ((1u << size) - 1));
            bits >>= size;
            nbits -= size;
            if (code == clear) { size = min_code + 1; next = eoi + 1; old = -1; continue; }
            if (code == eoi) break;
            int sp = 0, cur = code;
            if (old < 0) {
                if (code >= clear) break;
                idx[out++] = (uint8_t)code;
                first = (uint8_t)code;
                old = code;
                continue;
            }
            if (code >= next) {
                if (code > next) break;
                stack[sp++] = first;
                cur = old;
            }
            while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
            if (cur >= clear) break;
            stack[sp++] = (uint8_t)cur;
            first = (uint8_t)cur;
            while (sp && out < need) idx[out++] = stack[--sp];
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = first;
                next++;
                if (next == (1 << size) && size < 12) size++;
            }
            old = code;
        }
        free(data);
        /* place the frame; interlaced frames store rows 0,8,..; 4,12,..; 2,6,..; 1,3,.. */
        int interlaced = flags & 0x40;
        static const int istart[4] = {0, 4, 2, 1}, istep[4] = {8, 8, 4, 2};
        int pass = 0, next_row = 0;
        for (int r = 0; r < fh; r++) {
            int y = r;
            if (interlaced) {
                while (pass < 4 && next_row >= fh) { pass++; if (pass < 4) next_row = istart[pass]; }
                if (pass >= 4) break;
                y = next_row;
                next_row += istep[pass];
            }
            if (fy + y >= h) continue;
            for (int x = 0; x < fw && fx + x < w; x++) {
                size_t i = (size_t)r * fw + x;
                if (i >= out) break;
                int c = idx[i];
                if (c == transparent || c >= count) continue;
                px[(size_t)(fy + y) * w + fx + x] = pal[c];
            }
        }
        free(idx); free(prefix); free(suffix); free(stack);
        idx = NULL; prefix = NULL; suffix = NULL; stack = NULL;
        *out_w = w;
        *out_h = h;
        *has_alpha = 1;
        return px;
    }
fail:
    free(idx); free(prefix); free(suffix); free(stack);
    free(px);
    return NULL;
}

uint32_t* mx_image_decode(const uint8_t* data, size_t size, int* w, int* h, int* has_alpha,
                          const char** why) {
    const char* dummy;
    if (!why) why = &dummy;
    const char* format = mx_image_format(data, size);
    if (!format) { *why = "not a picture format this browser knows"; return NULL; }
    if (!strcmp(format, "PNG")) return png_decode(data, size, w, h, has_alpha, why);
    if (!strcmp(format, "JPEG")) return jpeg_decode(data, size, w, h, has_alpha, why);
    if (!strcmp(format, "GIF")) return gif_decode(data, size, w, h, has_alpha, why);
    *why = !strcmp(format, "WebP") ? "WebP pictures are not supported yet"
                                   : "SVG pictures are not supported yet";
    return NULL;
}
