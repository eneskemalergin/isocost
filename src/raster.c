/*
 * PNG backend.
 *
 * Fills (glyphs, markers, polygons) use exact signed-area coverage: every
 * edge adds its area to an accumulation row and a running sum gives each
 * pixel's coverage, as in font-rs. Strokes use the distance to each segment
 * (round caps and joins). Each primitive writes an 8-bit coverage mask with
 * max-combine, so overlapping pieces of one stroke never blend twice; the
 * mask is then composited once with the primitive's color and alpha.
 */
#include "canvas.h"
#include "platform.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int w, h;
    unsigned char *rgb;       /* rows of 1 + 3w bytes: a PNG filter-type byte, then RGB */
    size_t row_bytes;
    unsigned char *mask;
    int x0, y0, x1, y1;       /* dirty box, half-open */
    int cx0, cy0, cx1, cy1;   /* clip box */
} Raster;

static void dirty(Raster *r, int x0, int y0, int x1, int y1) {
    if (x0 < r->x0) r->x0 = x0;
    if (y0 < r->y0) r->y0 = y0;
    if (x1 > r->x1) r->x1 = x1;
    if (y1 > r->y1) r->y1 = y1;
}

static int clip_box(const Raster *r, float fx0, float fy0, float fx1, float fy1, int *x0, int *y0, int *x1, int *y1) {
    *x0 = (int)floorf(fx0);
    *y0 = (int)floorf(fy0);
    *x1 = (int)ceilf(fx1) + 1;
    *y1 = (int)ceilf(fy1) + 1;
    if (*x0 < r->cx0) *x0 = r->cx0;
    if (*y0 < r->cy0) *y0 = r->cy0;
    if (*x1 > r->cx1) *x1 = r->cx1;
    if (*y1 > r->cy1) *y1 = r->cy1;
    return *x0 < *x1 && *y0 < *y1;
}

static inline void mask_max(Raster *r, int x, int y, float v) {
    unsigned char *m = &r->mask[(size_t)y * (size_t)r->w + (size_t)x];
    unsigned char q = (unsigned char)(v * 255.0f + 0.5f);
    if (q > *m) *m = q;
}

static void commit(Raster *r, Rgb color, float alpha) {
    unsigned alpha8 = (unsigned)lrintf((alpha < 0 ? 0 : alpha > 1 ? 1 : alpha) * 255.0f);
    if (r->x0 >= r->x1) return;
    for (int y = r->y0; y < r->y1; y++) {
        unsigned char *m = &r->mask[(size_t)y * (size_t)r->w];
        unsigned char *p = &r->rgb[(size_t)y * r->row_bytes + 1 + (size_t)r->x0 * 3];
        for (int x = r->x0; x < r->x1; x++, p += 3) {
            if (!m[x]) continue;
            /* Integer blend: a in 0..65025 (coverage times alpha, both 0..255), rounded. */
            unsigned a = (unsigned)m[x] * alpha8, keep = 65025u - a;
            p[0] = (unsigned char)((p[0] * keep + color.r * a + 32512u) / 65025u);
            p[1] = (unsigned char)((p[1] * keep + color.g * a + 32512u) / 65025u);
            p[2] = (unsigned char)((p[2] * keep + color.b * a + 32512u) / 65025u);
            m[x] = 0;
        }
    }
    r->x0 = r->w;
    r->y0 = r->h;
    r->x1 = r->y1 = 0;
}

/* ---- strokes -------------------------------------------------------------- */

static void capsule(Raster *r, float ax, float ay, float bx, float by, float hw) {
    int x0, y0, x1, y1;
    float pad = hw + 1.0f, dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    if (!clip_box(r, fminf(ax, bx) - pad, fminf(ay, by) - pad, fmaxf(ax, bx) + pad, fmaxf(ay, by) + pad, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
            if (t < 0) t = 0;
            else if (t > 1) t = 1;
            float ex = ax + t * dx - px, ey = ay + t * dy - py;
            float cov = hw + 0.5f - sqrtf(ex * ex + ey * ey);
            if (cov > 0) mask_max(r, x, y, cov > 1 ? 1 : cov);
        }
    }
    dirty(r, x0, y0, x1, y1);
}

static void stroke_polyline(Raster *r, const float *pts, int n, int closed, float width, float dash_on, float dash_off) {
    float hw = width / 2, phase = 0;
    int on = 1, segments = closed ? n : n - 1;
    for (int i = 0; i < segments; i++) {
        float ax = pts[2 * i], ay = pts[2 * i + 1];
        float bx = pts[2 * ((i + 1) % n)], by = pts[2 * ((i + 1) % n) + 1];
        if (dash_on <= 0 && dash_off <= 0) {
            capsule(r, ax, ay, bx, by, hw);
            continue;
        }
        float len = hypotf(bx - ax, by - ay), t = 0;
        while (t < len) {
            float period = on ? dash_on : dash_off;
            float step = fminf(period - phase, len - t);
            if (step < 0) step = 0;
            if (on) {
                float s0 = t / len, s1 = (t + step) / len;
                capsule(r, ax + (bx - ax) * s0, ay + (by - ay) * s0, ax + (bx - ax) * s1, ay + (by - ay) * s1, hw);
            }
            t += step;
            phase += step;
            if (phase >= period - 1e-4f) {
                phase = 0;
                on = !on;
            }
            if (step == 0 && period <= 0) on = !on;
        }
    }
}

static void disk(Raster *r, float cx, float cy, float radius, int ring, float hw) {
    int x0, y0, x1, y1;
    float pad = radius + hw + 1;
    if (!clip_box(r, cx - pad, cy - pad, cx + pad, cy + pad, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float d = hypotf((float)x + 0.5f - cx, (float)y + 0.5f - cy);
            float cov = ring ? hw + 0.5f - fabsf(d - radius) : radius + 0.5f - d;
            if (cov > 0) mask_max(r, x, y, cov > 1 ? 1 : cov);
        }
    dirty(r, x0, y0, x1, y1);
}

static void fill_rect(Raster *r, float fx, float fy, float fw, float fh) {
    int x0, y0, x1, y1;
    if (!clip_box(r, fx, fy, fx + fw, fy + fh, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        float cy = fminf((float)y + 1, fy + fh) - fmaxf((float)y, fy);
        if (cy <= 0) continue;
        for (int x = x0; x < x1; x++) {
            float cx = fminf((float)x + 1, fx + fw) - fmaxf((float)x, fx);
            if (cx > 0) mask_max(r, x, y, fminf(cx, 1) * fminf(cy, 1));
        }
    }
    dirty(r, x0, y0, x1, y1);
}

/* ---- exact-area fills ------------------------------------------------------ */

typedef struct {
    float *seg;               /* x0 y0 x1 y1 per edge */
    size_t count, capacity;
    float minx, miny, maxx, maxy;
    float px, py, sx, sy;     /* pen and subpath start, for the outline sink */
    float ox, oy, scale;      /* glyph transform: px = ox + fx*scale, py = oy - fy*scale */
    int vertical;             /* then turn 90 degrees counterclockwise about (rx, ry) */
    float rx, ry;
} Edges;

static void edge(Edges *e, float x0, float y0, float x1, float y1) {
    if (e->count == e->capacity) {
        e->capacity = e->capacity ? e->capacity * 2 : 256;
        e->seg = xrealloc(e->seg, e->capacity * 4 * sizeof(float));
    }
    float *s = e->seg + e->count * 4;
    s[0] = x0;
    s[1] = y0;
    s[2] = x1;
    s[3] = y1;
    e->count++;
    e->minx = fminf(e->minx, fminf(x0, x1));
    e->maxx = fmaxf(e->maxx, fmaxf(x0, x1));
    e->miny = fminf(e->miny, fminf(y0, y1));
    e->maxy = fmaxf(e->maxy, fmaxf(y0, y1));
}

static void edges_reset(Edges *e) {
    e->count = 0;
    e->minx = e->miny = INFINITY;
    e->maxx = e->maxy = -INFINITY;
}

static void accumulate_line(float *acc, int w, int h, float x0, float y0, float x1, float y1) {
    float dir = 1;
    if (y0 == y1) return;
    if (y0 > y1) {
        float t;
        dir = -1;
        t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    float dxdy = (x1 - x0) / (y1 - y0), x = x0;
    if (y0 < 0) x -= y0 * dxdy;
    int ystart = y0 < 0 ? 0 : (int)y0;
    int yend = (int)ceilf(y1);
    if (yend > h) yend = h;
    for (int y = ystart; y < yend; y++) {
        float *row = acc + (size_t)y * (size_t)w;
        float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float xnext = x + dxdy * dy, d = dy * dir;
        float xa = fminf(x, xnext), xb = fmaxf(x, xnext);
        float xa_floor = floorf(xa), xb_ceil = ceilf(xb);
        int xai = (int)xa_floor, xbi = (int)xb_ceil;
        if (xai < 0) xai = 0;
        if (xbi > w - 1) xbi = w - 1;
        if (xbi <= xai + 1) {
            float xmf = 0.5f * (x + xnext) - xa_floor;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            float s = 1.0f / (xb - xa), x0f = xa - xa_floor;
            float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
            float x1f = xb - xb_ceil + 1, am = 0.5f * s * x1f * x1f;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1 - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) row[xi] += d * s;
                float a2 = a1 + (float)(xbi - xai - 3) * s;
                row[xbi - 1] += d * (1 - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

/* Rasterize the collected edges into the mask; gamma < 1 thickens thin coverage (text). */
static void fill_edges(Raster *r, const Edges *e, float gamma) {
    float gamma_lut[256];
    if (gamma != 1)
        for (int i = 0; i < 256; i++) gamma_lut[i] = powf((float)i / 255.0f, gamma);
    if (!e->count || !(e->maxx > e->minx) || !(e->maxy > e->miny)) return;
    int bx0 = (int)floorf(e->minx) - 1, bx1 = (int)ceilf(e->maxx) + 2;
    int by0 = (int)floorf(e->miny), by1 = (int)ceilf(e->maxy) + 1;
    if (by0 < r->cy0) by0 = r->cy0;
    if (by1 > r->cy1) by1 = r->cy1;
    if (by0 >= by1 || bx1 <= r->cx0 || bx0 >= r->cx1) return;
    int w = bx1 - bx0 + 2, h = by1 - by0;
    float *acc = xcalloc((size_t)w * (size_t)h, sizeof(float));
    for (size_t i = 0; i < e->count; i++) {
        const float *s = e->seg + i * 4;
        accumulate_line(acc, w, h, s[0] - (float)bx0, s[1] - (float)by0, s[2] - (float)bx0, s[3] - (float)by0);
    }
    int xs = bx0 < r->cx0 ? r->cx0 : bx0, xe = bx0 + w > r->cx1 ? r->cx1 : bx0 + w;
    for (int y = 0; y < h; y++) {
        float sum = 0;
        const float *row = acc + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            sum += row[x];
            int px = bx0 + x;
            if (px < xs || px >= xe) continue;
            float cov = fabsf(sum);
            if (cov > 1) cov = 1;
            if (cov > 0.002f) {
                if (gamma != 1) cov = gamma_lut[(int)(cov * 255.0f + 0.5f)];
                mask_max(r, px, by0 + y, cov);
            }
        }
    }
    dirty(r, xs, by0, xe, by1);
    free(acc);
}

static void fill_polygon(Raster *r, const float *pts, int n) {
    Edges e = {0};
    edges_reset(&e);
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        edge(&e, pts[2 * i], pts[2 * i + 1], pts[2 * j], pts[2 * j + 1]);
    }
    fill_edges(r, &e, 1);
    free(e.seg);
}

/* ---- text ----------------------------------------------------------------- */

/* Font units to pixels. */
static void glyph_point(const Edges *e, float fx, float fy, float *x, float *y) {
    float hx = e->ox + fx * e->scale, hy = e->oy - fy * e->scale;
    if (e->vertical) {
        *x = e->rx + (hy - e->ry);
        *y = e->ry - (hx - e->rx);
    } else {
        *x = hx;
        *y = hy;
    }
}

static void sink_move(void *ctx, float x, float y) {
    Edges *e = ctx;
    glyph_point(e, x, y, &e->px, &e->py);
    e->sx = e->px;
    e->sy = e->py;
}

static void sink_line(void *ctx, float x, float y) {
    Edges *e = ctx;
    float nx, ny;
    glyph_point(e, x, y, &nx, &ny);
    edge(e, e->px, e->py, nx, ny);
    e->px = nx;
    e->py = ny;
}

static void sink_quad(void *ctx, float cx, float cy, float x, float y) {
    Edges *e = ctx;
    float qx, qy, nx, ny;
    glyph_point(e, cx, cy, &qx, &qy);
    glyph_point(e, x, y, &nx, &ny);
    float dev = hypotf(qx - (e->px + nx) / 2, qy - (e->py + ny) / 2);
    int steps = (int)ceilf(sqrtf(dev / 0.15f));
    if (steps < 1) steps = 1;
    if (steps > 24) steps = 24;
    float x0 = e->px, y0 = e->py;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps, u = 1 - t;
        float bx = u * u * x0 + 2 * u * t * qx + t * t * nx;
        float by = u * u * y0 + 2 * u * t * qy + t * t * ny;
        edge(e, e->px, e->py, bx, by);
        e->px = bx;
        e->py = by;
    }
}

static void sink_close(void *ctx) {
    Edges *e = ctx;
    if (e->px != e->sx || e->py != e->sy) edge(e, e->px, e->py, e->sx, e->sy);
    e->px = e->sx;
    e->py = e->sy;
}

static const OutlineSink raster_sink = {sink_move, sink_line, sink_quad, sink_close};

static float text_start(const Op *op) {
    if (op->anchor == ANCHOR_START) return op->x;
    float w = font_width(op->text, op->size, op->bold);
    return op->anchor == ANCHOR_MIDDLE ? op->x - w / 2 : op->x - w;
}

static void text_edges(const Op *op, Edges *e) {
    PlacedGlyph glyphs[512];
    size_t n = font_layout(op->text, op->size, op->bold, glyphs, 512);
    float x = text_start(op);
    edges_reset(e);
    e->scale = op->size / (float)font_units_per_em(op->bold);
    e->oy = op->y;
    e->vertical = op->vertical;
    e->rx = op->x;
    e->ry = op->y;
    for (size_t i = 0; i < n; i++) {
        e->ox = x + glyphs[i].x;
        font_outline(op->bold, glyphs[i].glyph, &raster_sink, e);
    }
}

static void dilate_mask(Raster *r, int radius) {
    int bx0 = r->x0 - radius, by0 = r->y0 - radius, bx1 = r->x1 + radius, by1 = r->y1 + radius;
    if (bx0 < r->cx0) bx0 = r->cx0;
    if (by0 < r->cy0) by0 = r->cy0;
    if (bx1 > r->cx1) bx1 = r->cx1;
    if (by1 > r->cy1) by1 = r->cy1;
    int w = bx1 - bx0, h = by1 - by0;
    if (w <= 0 || h <= 0) return;
    unsigned char *tmp = xcalloc((size_t)w * (size_t)h, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char m = 0;
            for (int k = -radius; k <= radius; k++) {
                int xx = x + k;
                if (xx >= 0 && xx < w) {
                    unsigned char v = r->mask[(size_t)(y + by0) * (size_t)r->w + (size_t)(xx + bx0)];
                    if (v > m) m = v;
                }
            }
            tmp[(size_t)y * (size_t)w + (size_t)x] = m;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char m = 0;
            for (int k = -radius; k <= radius; k++) {
                int yy = y + k;
                if (yy >= 0 && yy < h && tmp[(size_t)yy * (size_t)w + (size_t)x] > m) m = tmp[(size_t)yy * (size_t)w + (size_t)x];
            }
            int boosted = m * 2 > 255 ? 255 : m * 2;
            r->mask[(size_t)(y + by0) * (size_t)r->w + (size_t)(x + bx0)] = (unsigned char)boosted;
        }
    dirty(r, bx0, by0, bx1, by1);
    free(tmp);
}

static void draw_text(Raster *r, const Op *op, Rgb background) {
    Edges e = {0};
    /* Small text gets slightly heavier coverage so hairline stems stay visible. */
    float gamma = op->size < 20 ? 0.8f : op->size < 32 ? 0.9f : 1.0f;
    text_edges(op, &e);
    if (op->halo > 0) {
        fill_edges(r, &e, 1);
        dilate_mask(r, (int)ceilf(op->halo));
        commit(r, background, 0.9f);
    }
    fill_edges(r, &e, gamma);
    commit(r, op->fill_color, op->fill_alpha);
    free(e.seg);
}

/* ---- display list ----------------------------------------------------------- */

static void rasterize(const Scene *scene, Raster *r) {
    for (size_t i = 0; i < scene->ops.count; i++) {
        const Op *op = scene->ops.items[i];
        switch (op->kind) {
        case OP_CLIP:
            r->cx0 = (int)floorf(op->x);
            r->cy0 = (int)floorf(op->y);
            r->cx1 = (int)ceilf(op->x + op->w);
            r->cy1 = (int)ceilf(op->y + op->h);
            if (r->cx0 < 0) r->cx0 = 0;
            if (r->cy0 < 0) r->cy0 = 0;
            if (r->cx1 > r->w) r->cx1 = r->w;
            if (r->cy1 > r->h) r->cy1 = r->h;
            break;
        case OP_UNCLIP:
            r->cx0 = r->cy0 = 0;
            r->cx1 = r->w;
            r->cy1 = r->h;
            break;
        case OP_RECT:
            if (op->fill) {
                fill_rect(r, op->x, op->y, op->w, op->h);
                commit(r, op->fill_color, op->fill_alpha);
            }
            if (op->stroke) {
                float p[8] = {op->x, op->y, op->x + op->w, op->y, op->x + op->w, op->y + op->h, op->x, op->y + op->h};
                stroke_polyline(r, p, 4, 1, op->stroke_width, op->dash_on, op->dash_off);
                commit(r, op->stroke_color, op->stroke_alpha);
            }
            break;
        case OP_CIRCLE:
            if (op->fill) {
                disk(r, op->x, op->y, op->r, 0, 0);
                commit(r, op->fill_color, op->fill_alpha);
            }
            if (op->stroke) {
                disk(r, op->x, op->y, op->r, 1, op->stroke_width / 2);
                commit(r, op->stroke_color, op->stroke_alpha);
            }
            break;
        case OP_PATH:
            if (op->fill && op->count >= 3) {
                fill_polygon(r, op->points, op->count);
                commit(r, op->fill_color, op->fill_alpha);
            }
            if (op->stroke && op->count >= 2) {
                stroke_polyline(r, op->points, op->count, op->closed, op->stroke_width, op->dash_on, op->dash_off);
                commit(r, op->stroke_color, op->stroke_alpha);
            }
            break;
        case OP_TEXT:
            draw_text(r, op, scene->background);
            break;
        }
    }
}

/* ---- PNG ---------------------------------------------------------------- */

static void be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void png_chunk(Str *png, const char *type, const unsigned char *data, size_t n) {
    unsigned char word[4];
    be32(word, (uint32_t)n);
    str_appendn(png, (char *)word, 4);
    unsigned long crc = crc32_bytes(0, (const unsigned char *)type, 4);
    crc = crc32_bytes(crc, data, n);
    str_appendn(png, type, 4);
    if (n) str_appendn(png, (const char *)data, n);
    be32(word, (uint32_t)crc);
    str_appendn(png, (char *)word, 4);
}

int render_png(const Scene *scene, const char *path, Workspace *shared) {
    Raster r = {0};
    int w = scene->width, h = scene->height;
    size_t stride = (size_t)w * 3, row_bytes = stride + 1;
    Workspace *ws = shared ? shared : workspace_new();
    r.w = w;
    r.h = h;
    /* The image is laid out as PNG scanlines, so it is filtered and compressed in place. */
    if (ws->image_capacity < row_bytes * (size_t)h) {
        free(ws->image);
        ws->image_capacity = row_bytes * (size_t)h;
        ws->image = xmalloc(ws->image_capacity);
    }
    /* Every drawing operation clears the mask pixels it wrote, so a reused mask is already zero. */
    if (ws->mask_capacity < (size_t)w * (size_t)h) {
        free(ws->mask);
        ws->mask_capacity = (size_t)w * (size_t)h;
        ws->mask = xcalloc(ws->mask_capacity, 1);
    }
    r.rgb = ws->image;
    r.row_bytes = row_bytes;
    r.mask = ws->mask;
    r.x0 = w;
    r.y0 = h;
    r.cx1 = w;
    r.cy1 = h;
    r.rgb[0] = 2; /* Up */
    for (size_t x = 0; x < (size_t)w; x++) {
        r.rgb[1 + 3 * x] = scene->background.r;
        r.rgb[1 + 3 * x + 1] = scene->background.g;
        r.rgb[1 + 3 * x + 2] = scene->background.b;
    }
    for (int y = 1; y < h; y++) memcpy(r.rgb + (size_t)y * row_bytes, r.rgb, row_bytes);
    rasterize(scene, &r);

    /*
     * Up filter on every row. On the full real z-fastq set (44 figures) it was
     * 33% faster than choosing between Sub, Up, and Paeth per row, for 2% larger
     * files: figures are mostly flat color, so the row above predicts best.
     */
    /* Bottom-up, so every row still sees the unfiltered row above it. Row 0 has nothing above: Up = raw. */
    for (int y = h - 1; y > 0; y--) {
        unsigned char *row = r.rgb + (size_t)y * row_bytes + 1;
        const unsigned char *above = row - row_bytes;
        for (size_t x = 0; x < stride; x++) row[x] = (unsigned char)(row[x] - above[x]);
    }
    Str z = {0}, png = {0};
    zlib_compress(r.rgb, row_bytes * (size_t)h, ws, &z);
    str_appendn(&png, "\x89PNG\r\n\x1a\n", 8);
    unsigned char ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* RGB */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(&png, "IHDR", ihdr, 13);
    unsigned char phys[9];
    uint32_t ppm = (uint32_t)lround(scene->dpi / 0.0254);
    be32(phys, ppm);
    be32(phys + 4, ppm);
    phys[8] = 1;  /* unit: metre */
    png_chunk(&png, "pHYs", phys, 9);
    png_chunk(&png, "IDAT", (unsigned char *)z.data, z.length);
    png_chunk(&png, "IEND", NULL, 0);
    int ok = pf_write_file_atomic(path, png.data, png.length);
    str_free(&z);
    str_free(&png);
    if (!shared) workspace_free(ws);
    return ok;
}
