/*
 * zlib stream writer: LZ77 with hash chains and one fixed-Huffman block
 * (RFC 1950 and 1951). Used for PNG image data and PDF content streams.
 * Positions inside long matches are not hashed, as in zlib's max_insert_length.
 */
#include "canvas.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *out;
    size_t length;
    uint64_t bits;
    int nbits;
} BitWriter;

static inline void put_bits(BitWriter *w, uint32_t value, int count) {
    w->bits |= (uint64_t)value << w->nbits;
    w->nbits += count;
    while (w->nbits >= 8) {
        w->out[w->length++] = (unsigned char)w->bits;
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

static uint32_t reverse_bits(uint32_t code, int length) {
    uint32_t r = 0;
    for (int i = 0; i < length; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }
    return r;
}

static const int len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

typedef struct {
    uint16_t lit_code[288];
    uint8_t lit_len[288];
    uint16_t len_symbol[259], len_value[259];
    uint8_t len_bits[259];
    uint8_t dist_code[512];   /* zlib's layout: distances 1-256 directly, larger ones by (d - 1) >> 7 */
} Tables;

static Tables tables; /* filled once by canvas_init, read-only afterwards */

static void build_tables(Tables *t) {
    for (int v = 0; v < 288; v++) {
        if (v < 144) { t->lit_code[v] = (uint16_t)reverse_bits(0x30 + (uint32_t)v, 8); t->lit_len[v] = 8; }
        else if (v < 256) { t->lit_code[v] = (uint16_t)reverse_bits(0x190 + (uint32_t)(v - 144), 9); t->lit_len[v] = 9; }
        else if (v < 280) { t->lit_code[v] = (uint16_t)reverse_bits((uint32_t)(v - 256), 7); t->lit_len[v] = 7; }
        else { t->lit_code[v] = (uint16_t)reverse_bits(0xc0 + (uint32_t)(v - 280), 8); t->lit_len[v] = 8; }
    }
    for (int length = 3; length <= 258; length++) {
        int i = 28;
        while (len_base[i] > length) i--;
        t->len_symbol[length] = (uint16_t)(257 + i);
        t->len_bits[length] = (uint8_t)len_extra[i];
        t->len_value[length] = (uint16_t)(length - len_base[i]);
    }
    for (int d = 1; d <= 32768; d++) {
        int i = 29;
        while (dist_base[i] > d) i--;
        if (d <= 256) t->dist_code[d - 1] = (uint8_t)i;
        else t->dist_code[256 + ((d - 1) >> 7)] = (uint8_t)i;
    }
}

/* Length of the common prefix of p and q, at most limit; 8 bytes per step. */
static inline size_t common_length(const unsigned char *p, const unsigned char *q, size_t start, size_t limit) {
    size_t len = start;
    while (len + 8 <= limit) {
        uint64_t a, b;
        memcpy(&a, p + len, 8);
        memcpy(&b, q + len, 8);
        if (a != b) break;
        len += 8;
    }
    while (len < limit && p[len] == q[len]) len++;
    return len;
}

/* Length of the run of byte c starting at p, at most limit. */
static inline size_t run_length(const unsigned char *p, unsigned char c, size_t limit) {
    uint64_t pattern = 0x0101010101010101ULL * c;
    size_t len = 0;
    while (len + 8 <= limit) {
        uint64_t a;
        memcpy(&a, p + len, 8);
        if (a != pattern) break;
        len += 8;
    }
    while (len < limit && p[len] == c) len++;
    return len;
}

static inline void put_literal(BitWriter *w, const Tables *t, int v) { put_bits(w, t->lit_code[v], t->lit_len[v]); }

static inline void put_match(BitWriter *w, const Tables *t, int length, int distance) {
    put_literal(w, t, t->len_symbol[length]);
    if (t->len_bits[length]) put_bits(w, t->len_value[length], t->len_bits[length]);
    int i = distance <= 256 ? t->dist_code[distance - 1] : t->dist_code[256 + ((distance - 1) >> 7)];
    put_bits(w, reverse_bits((uint32_t)i, 5), 5);
    if (dist_extra[i]) put_bits(w, (uint32_t)(distance - dist_base[i]), dist_extra[i]);
}

Workspace *workspace_new(void) { return xcalloc(1, sizeof(Workspace)); }

void workspace_free(Workspace *ws) {
    if (!ws) return;
    free(ws->image);
    free(ws->mask);
    free(ws->out);
    free(ws->head);
    free(ws->prev);
    free(ws);
}

void zlib_compress(const unsigned char *data, size_t n, Workspace *shared, Str *out) {
    /* A chain of 16 measured the same speed as 4 to 32 on the real figures, within 0.4% of the size at 32. */
    enum { WINDOW = 32768, HASH_BITS = 15, MAX_CHAIN = 16 };
    const Tables *t = &tables;
    Workspace *ws = shared ? shared : workspace_new();
    if (!ws->head) ws->head = xmalloc(sizeof(int32_t) << HASH_BITS);
    if (!ws->prev) ws->prev = xmalloc(sizeof(int32_t) * WINDOW);
    int32_t *head = ws->head, *prev = ws->prev;
    BitWriter w = {0};
    uint32_t a = 1, b = 0;
    size_t i = 0, capacity = n + n / 8 + 64; /* fixed Huffman never exceeds 9 bits per byte */
    if (ws->out_capacity < capacity) {
        free(ws->out);
        ws->out = xmalloc(capacity);
        ws->out_capacity = capacity;
    }
    w.out = ws->out;
    for (size_t k = 0; k < (1u << HASH_BITS); k++) head[k] = -1;
    w.out[w.length++] = 0x78;
    w.out[w.length++] = 0x01;
    put_bits(&w, 1, 1); /* final block */
    put_bits(&w, 1, 2); /* fixed Huffman codes */
#define HASH3(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (uint32_t)(p)[2]) & ((1u << HASH_BITS) - 1))
    while (i < n) {
        int best_len = 0, best_dist = 0;
        /* Run of the previous byte: emit it at distance 1 without searching (zlib's RLE strategy). */
        if (i > 0 && i + 2 < n && data[i] == data[i - 1] && data[i + 1] == data[i - 1] && data[i + 2] == data[i - 1]) {
            size_t limit = n - i < 258 ? n - i : 258, run = run_length(data + i, data[i - 1], limit);
            if (run >= 16 || run == limit) {
                best_len = (int)run;
                best_dist = 1;
            }
        }
        if (!best_len && i + 2 < n) {
            int32_t candidate = head[HASH3(data + i)];
            int chain = 0;
            size_t limit = n - i < 258 ? n - i : 258;
            while (candidate >= 0 && i - (size_t)candidate <= WINDOW - 1 && chain++ < MAX_CHAIN) {
                const unsigned char *p = data + candidate, *q = data + i;
                if (p[best_len] == q[best_len] && p[0] == q[0] && p[1] == q[1]) {
                    size_t len = common_length(p, q, 2, limit);
                    if ((int)len > best_len) {
                        best_len = (int)len;
                        best_dist = (int)(i - (size_t)candidate);
                        if (len == limit) break;
                    }
                }
                candidate = prev[candidate % WINDOW];
            }
        }
        size_t advance = best_len >= 3 ? (size_t)best_len : 1;
        if (best_len >= 3) put_match(&w, t, best_len, best_dist);
        else put_literal(&w, t, data[i]);
        size_t insert = advance <= 16 ? advance : 1;
        for (size_t k = 0; k < insert; k++)
            if (i + k + 2 < n) {
                uint32_t h = HASH3(data + i + k);
                prev[(i + k) % WINDOW] = head[h];
                head[h] = (int32_t)(i + k);
            }
        i += advance;
    }
#undef HASH3
    put_literal(&w, t, 256);
    if (w.nbits) put_bits(&w, 0, 8 - w.nbits);
    /* Adler-32 in 16-byte steps: b gains 16a plus the position-weighted sum, which vectorizes. */
    for (size_t k = 0; k < n;) {
        size_t block = n - k < 5552 ? n - k : 5552, e = k + block;
        for (; k + 16 <= e; k += 16) {
            uint32_t sum = 0, weighted = 0;
            for (uint32_t j = 0; j < 16; j++) {
                sum += data[k + j];
                weighted += (16 - j) * data[k + j];
            }
            b += 16 * a + weighted;
            a += sum;
        }
        for (; k < e; k++) {
            a += data[k];
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    w.out[w.length++] = (unsigned char)(b >> 8);
    w.out[w.length++] = (unsigned char)b;
    w.out[w.length++] = (unsigned char)(a >> 8);
    w.out[w.length++] = (unsigned char)a;
    str_appendn(out, (const char *)w.out, w.length);
    if (!shared) workspace_free(ws);
}

static uint32_t crc_table[256];

/* Call once before rendering on several threads. */
void canvas_init(void) {
    build_tables(&tables);
    for (uint32_t k = 0; k < 256; k++) {
        uint32_t c = k;
        for (int j = 0; j < 8; j++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[k] = c;
    }
}

unsigned long crc32_bytes(unsigned long crc, const unsigned char *data, size_t n) {
    uint32_t c = (uint32_t)crc ^ 0xffffffffu;
    while (n--) c = crc_table[(c ^ *data++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
