/* See video.h. */
#include "video.h"

#include <math.h>

#include "pce/pce.h"

bool video_init(Video *v, SDL_Renderer *ren)
{
    SDL_zerop(v);
    v->ren = ren;
    v->frame = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, PCE_FB_MAX_W, PCE_FB_MAX_H);
    v->blur_a = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 64, 56);
    v->blur_b = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 16, 14);
    if (v->blur_a) SDL_SetTextureScaleMode(v->blur_a, SDL_SCALEMODE_LINEAR);
    if (v->blur_b) SDL_SetTextureScaleMode(v->blur_b, SDL_SCALEMODE_LINEAR);
    v->src_w = 256;
    v->src_h = 224;
    return v->frame != NULL;
}

void video_free(Video *v)
{
    if (v->frame) SDL_DestroyTexture(v->frame);
    if (v->sharp) SDL_DestroyTexture(v->sharp);
    if (v->blur_a) SDL_DestroyTexture(v->blur_a);
    if (v->blur_b) SDL_DestroyTexture(v->blur_b);
    SDL_Texture *fx[5] = {v->fx, v->scan, v->mask, v->glow, v->vignette};
    for (int i = 0; i < 5; i++)
        if (fx[i]) SDL_DestroyTexture(fx[i]);
    SDL_zerop(v);
}

void video_upload(Video *v, const uint32_t *fb, int w, int h, int pitch, int wide)
{
    if (w <= 0 || h <= 0) return;
    SDL_Rect r = {0, 0, w, h};
    SDL_UpdateTexture(v->frame, &r, fb, pitch * 4);
    v->src_w = w;
    v->src_h = h;
    v->src_wide = wide;
    v->src_wide_full = wide;   /* (main.c sets it to the grown-out width while growing) */
}

double video_picture_aspect(const Video *v, const Settings *s)
{
    return s->aspect == ASPECT_SQUARE ? 256.0 / (double)v->src_h : 4.0 / 3.0;
}

/* Where the picture goes in a ww x wh window. */
static SDL_FRect place(const Video *v, const Settings *s, int ww, int wh)
{
    float sw = (float)v->src_w, sh = (float)v->src_h;
    if (s->aspect == ASPECT_STRETCH) return (SDL_FRect){0, 0, (float)ww, (float)wh};
    /* aspect of the whole picture: a TV's 4:3 (for the 256 pixels the game normally
     * shows; widescreen adds to the width at the same pixel shape), or square pixels */
    float base = sw - 2.0f * (float)v->src_wide;
    float ar = s->aspect == ASPECT_SQUARE ? sw / sh : 4.0f / 3.0f * sw / base;
    /* Widescreen draws whole pixels a side (an even number), so it falls a little short of
     * the screen's shape (16:9 at 4:3 wants 42.67): widened to fill it. Scaled by the
     * fully-grown picture's shortfall, so it stays even while the picture grows out. */
    if (v->src_wide > 0 && s->scaling == SCALE_FIT && wh > 0) {
        float full_w = 256.0f + 2.0f * (float)(v->src_wide_full > v->src_wide ? v->src_wide_full : v->src_wide);
        float full_ar = s->aspect == ASPECT_SQUARE ? full_w / sh : 4.0f / 3.0f * full_w / 256.0f;
        float fill = ((float)ww / (float)wh) / full_ar;
        if (fill > 1.0f && fill < 1.05f) ar *= fill;
    }
    float dh = (float)wh, dw = dh * ar;
    if (dw > (float)ww) { dw = (float)ww; dh = dw / ar; }
    if (s->scaling == SCALE_WHOLE) {
        /* whole multiples of the picture height (and of the width for square pixels) */
        int k = (int)floorf(dh / sh);
        if (k >= 1) {
            dh = sh * (float)k;
            dw = s->aspect == ASPECT_SQUARE ? sw * (float)k : dh * ar;
        }
    }
    dw = floorf(dw);
    dh = floorf(dh);
    return (SDL_FRect){floorf(((float)ww - dw) / 2), floorf(((float)wh - dh) / 2), dw, dh};
}

/* FILTER_SHARP: blow the picture up by a whole factor with hard edges, then scale the rest
 * of the way smoothly, so pixels stay crisp but all come out the same size. */
static SDL_Texture *sharp_copy(Video *v, float dw, float dh)
{
    int k = (int)ceilf(fmaxf(dw / (float)v->src_w, dh / (float)v->src_h));
    if (k < 1) k = 1;
    while (k > 1 && (v->src_w * k > 8192 || v->src_h * k > 8192)) k--;
    int tw = v->src_w * k, th = v->src_h * k;
    if (!v->sharp || v->sharp_w != tw || v->sharp_h != th) {
        if (v->sharp) SDL_DestroyTexture(v->sharp);
        v->sharp = SDL_CreateTexture(v->ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, tw, th);
        if (!v->sharp) return NULL;
        SDL_SetTextureScaleMode(v->sharp, SDL_SCALEMODE_LINEAR);
        v->sharp_w = tw;
        v->sharp_h = th;
    }
    SDL_SetTextureScaleMode(v->frame, SDL_SCALEMODE_NEAREST);
    SDL_SetRenderTarget(v->ren, v->sharp);
    SDL_FRect src = {0, 0, (float)v->src_w, (float)v->src_h};
    SDL_RenderTexture(v->ren, v->frame, &src, NULL);
    SDL_SetRenderTarget(v->ren, NULL);
    return v->sharp;
}

/* The picture's texture for drawing it dw x dh with the chosen filter, and the part of it
 * that holds the picture. */
static SDL_Texture *picture(Video *v, const Settings *s, float dw, float dh, SDL_FRect *part)
{
    if (s->filter == FILTER_SHARP) {
        SDL_Texture *t = sharp_copy(v, dw, dh);
        if (t) {
            *part = (SDL_FRect){0, 0, (float)v->sharp_w, (float)v->sharp_h};
            return t;
        }
    }
    SDL_SetTextureScaleMode(v->frame, s->filter == FILTER_SMOOTH ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    *part = (SDL_FRect){0, 0, (float)v->src_w, (float)v->src_h};
    return v->frame;
}

/* ---- screen effects ---------------------------------------------------------------------
 * Built from passes of the ordinary renderer (no shaders): the picture is drawn at its size on
 * screen into v->fx, softened (composite), brightened to make up for what the effect takes
 * away, and darkened between the game's lines by a scanline profile worked out for every
 * row of the screen; v->fx then goes on the screen, flat or on a curved mesh, under a
 * phosphor mask tiled at the screen's own pixels, a glow (the picture downscaled, added) and
 * darker edges. */

typedef struct Look {
    float scan;       /* darkness at the edges of each game line, 0-1 */
    float mask;       /* phosphor stripes: how far each dims the other two colours */
    float boost;      /* the picture added again, making up for the darkening */
    float glow;       /* halation */
    float vignette;   /* darker toward the edges and corners */
    float curve;      /* how far the glass bulges */
    bool soft;        /* composite: blurred sideways */
} Look;

static Look look_for(const Settings *s)
{
    static const float k_strength[STRENGTH_COUNT] = {0.6f, 1.0f, 1.4f};
    int si = s->effect_strength < 0 || s->effect_strength >= STRENGTH_COUNT ? STRENGTH_MEDIUM : s->effect_strength;
    float k = k_strength[si];
    Look l = {0};
    switch (s->effect) {
    case EFFECT_SCANLINES:
        l.scan = 0.55f * k;
        l.boost = 0.2f * k;
        break;
    case EFFECT_CRT:
    case EFFECT_CURVED:
        l.scan = 0.42f * k;
        l.mask = 0.28f * k;
        l.boost = 0.22f * k;
        l.glow = 0.24f * k;
        l.vignette = 0.32f * k;
        if (s->effect == EFFECT_CURVED) {
            l.curve = 0.035f * k;
            l.vignette = 0.5f * k;
        }
        break;
    case EFFECT_COMPOSITE:
        l.scan = 0.2f * k;
        l.glow = 0.08f * k;
        l.soft = true;
        break;
    default:
        break;
    }
    if (l.scan > 0.9f) l.scan = 0.9f;
    if (l.mask > 0.6f) l.mask = 0.6f;
    if (l.vignette > 0.85f) l.vignette = 0.85f;
    return l;
}

static SDL_Texture *texture_from(SDL_Renderer *ren, const uint32_t *px, int w, int h, SDL_ScaleMode scale)
{
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, w * 4);
    SDL_SetTextureScaleMode(t, scale);
    return t;
}

/* The scanline profile for a picture `th` screen pixels tall showing `rows` game lines: each
 * screen row's brightness, averaged over 8 points in it so that the lines come out even
 * whatever the scale. */
static SDL_Texture *scanlines(Video *v, int th, int rows, float depth)
{
    if (v->scan && v->scan_h == th && v->scan_rows == rows && v->scan_depth == depth) return v->scan;
    if (v->scan) SDL_DestroyTexture(v->scan);
    v->scan = NULL;
    uint32_t *px = SDL_malloc((size_t)th * 4);
    if (!px) return NULL;
    for (int y = 0; y < th; y++) {
        float acc = 0;
        for (int k = 0; k < 8; k++) {
            float g = ((float)y + ((float)k + 0.5f) / 8.0f) * (float)rows / (float)th;
            float c = cosf(3.14159265f * (g - floorf(g)));     /* 1 at a line's edges, 0 at its middle */
            acc += 1.0f - depth * c * c * c * c;
        }
        uint32_t b = (uint32_t)(acc / 8.0f * 255.0f + 0.5f);
        px[y] = 0xFF000000u | b << 16 | b << 8 | b;
    }
    v->scan = texture_from(v->ren, px, 1, th, SDL_SCALEMODE_NEAREST);
    SDL_free(px);
    if (v->scan) SDL_SetTextureBlendMode(v->scan, SDL_BLENDMODE_MOD);
    v->scan_h = th;
    v->scan_rows = rows;
    v->scan_depth = depth;
    return v->scan;
}

/* Aperture-grille stripes, red, green, blue, each dimming the other two colours. */
static SDL_Texture *phosphors(Video *v, float depth)
{
    if (v->mask && v->mask_depth == depth) return v->mask;
    if (v->mask) SDL_DestroyTexture(v->mask);
    uint32_t o = (uint32_t)((1.0f - depth) * 255.0f + 0.5f);
    uint32_t px[3] = {0xFF000000u | 255u << 16 | o << 8 | o, 0xFF000000u | o << 16 | 255u << 8 | o,
                      0xFF000000u | o << 16 | o << 8 | 255u};
    v->mask = texture_from(v->ren, px, 3, 1, SDL_SCALEMODE_NEAREST);
    if (v->mask) SDL_SetTextureBlendMode(v->mask, SDL_BLENDMODE_MOD);
    v->mask_depth = depth;
    return v->mask;
}

/* Black, clear in the middle, darkening toward the edges and most into the corners. */
static SDL_Texture *vignette(Video *v)
{
    if (v->vignette) return v->vignette;
    enum { N = 128 };
    static uint32_t px[N * N];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float fx = fabsf(((float)x + 0.5f) / N * 2 - 1), fy = fabsf(((float)y + 0.5f) / N * 2 - 1);
            float d = powf(powf(fx, 5) + powf(fy, 5), 0.2f), t = (d - 0.55f) / 0.5f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            px[y * N + x] = (uint32_t)(t * t * (3 - 2 * t) * 255.0f) << 24;
        }
    v->vignette = texture_from(v->ren, px, N, N, SDL_SCALEMODE_LINEAR);
    if (v->vignette) SDL_SetTextureBlendMode(v->vignette, SDL_BLENDMODE_BLEND);
    return v->vignette;
}

/* `tex` over dst at `alpha`, on curved glass: a mesh whose sides bow outward (x scaled by
 * 1 - k y^2, y by 1 - k x^2, from the middle); k = 0 is flat. (Geometry takes its alpha from
 * the vertices, not the texture's alpha mod.) */
static void draw_on_glass(SDL_Renderer *ren, SDL_Texture *tex, SDL_FRect dst, float k, float alpha)
{
    if (k <= 0) {
        SDL_SetTextureAlphaModFloat(tex, alpha);
        SDL_RenderTexture(ren, tex, NULL, &dst);
        SDL_SetTextureAlphaModFloat(tex, 1.0f);
        return;
    }
    enum { NX = 48, NY = 36 };
    static SDL_Vertex vtx[(NX + 1) * (NY + 1)];
    static int idx[NX * NY * 6];
    int n = 0, ni = 0;
    for (int j = 0; j <= NY; j++)
        for (int i = 0; i <= NX; i++) {
            float u = (float)i / NX, w = (float)j / NY, x = u * 2 - 1, y = w * 2 - 1;
            float sx = x * (1 - k * y * y), sy = y * (1 - k * x * x);
            vtx[n++] = (SDL_Vertex){{dst.x + (sx + 1) * 0.5f * dst.w, dst.y + (sy + 1) * 0.5f * dst.h}, {1, 1, 1, alpha},
                                    {u, w}};
        }
    for (int j = 0; j < NY; j++)
        for (int i = 0; i < NX; i++) {
            int a = j * (NX + 1) + i, b = a + 1, c = a + NX + 1, d = c + 1;
            idx[ni++] = a; idx[ni++] = b; idx[ni++] = d;
            idx[ni++] = a; idx[ni++] = d; idx[ni++] = c;
        }
    SDL_RenderGeometry(ren, tex, vtx, n, idx, ni);
}

/* `tex` drawn with a blend mode and alpha, then put back as it was. */
static void draw_as(SDL_Renderer *ren, SDL_Texture *tex, const SDL_FRect *part, const SDL_FRect *to, SDL_BlendMode mode,
                    float alpha)
{
    SDL_BlendMode was;
    SDL_GetTextureBlendMode(tex, &was);
    SDL_SetTextureBlendMode(tex, mode);
    SDL_SetTextureAlphaModFloat(tex, alpha < 0 ? 0 : alpha > 1 ? 1 : alpha);
    SDL_RenderTexture(ren, tex, part, to);
    SDL_SetTextureAlphaModFloat(tex, 1.0f);
    SDL_SetTextureBlendMode(tex, was);
}

/* The picture with the screen effect, at dst. False if it could not be (then it is drawn
 * plain). */
static bool draw_effect(Video *v, const Settings *s, SDL_FRect dst)
{
    SDL_Renderer *ren = v->ren;
    Look l = look_for(s);
    int tw = (int)dst.w, th = (int)dst.h;
    if (tw < 8 || th < 8) return false;
    if (!v->fx || v->fx_w != tw || v->fx_h != th) {
        if (v->fx) SDL_DestroyTexture(v->fx);
        v->fx = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, tw, th);
        if (!v->fx) return false;
        SDL_SetTextureScaleMode(v->fx, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(v->fx, SDL_BLENDMODE_NONE);
        v->fx_w = tw;
        v->fx_h = th;
    }
    SDL_FRect part, all = {0, 0, (float)tw, (float)th};
    SDL_Texture *pic = picture(v, s, dst.w, dst.h, &part);   /* (may draw into v->sharp first) */

    /* the picture at its size on screen, softened, brightened, with scanlines */
    SDL_SetRenderTarget(ren, v->fx);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    draw_as(ren, pic, &part, &all, SDL_BLENDMODE_BLEND, 1.0f);
    if (l.soft) {
        /* a composite signal's soft sideways detail: copies a third and three quarters of a
         * game pixel to each side, blended in turn, come to weights of about .39 in the middle,
         * .17-.19 either side and .13 further out */
        static const struct { float off, alpha; } taps[] = {{-0.35f, 0.3f}, {0.35f, 0.25f}, {-0.75f, 0.15f}, {0.75f, 0.13f}};
        float px = all.w / (float)v->src_w;
        for (int i = 0; i < 4; i++) {
            SDL_FRect at = {taps[i].off * px, 0, all.w, all.h};
            draw_as(ren, pic, &part, &at, SDL_BLENDMODE_BLEND, taps[i].alpha);
        }
    }
    if (l.boost > 0) draw_as(ren, pic, &part, &all, SDL_BLENDMODE_ADD, l.boost);
    SDL_Texture *scan = l.scan > 0 ? scanlines(v, th, v->src_h, l.scan) : NULL;
    if (scan) SDL_RenderTexture(ren, scan, NULL, &all);
    SDL_SetRenderTarget(ren, NULL);

    /* on the screen */
    if (l.curve > 0) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderFillRect(ren, &dst);
    }
    draw_on_glass(ren, v->fx, dst, l.curve, 1.0f);
    SDL_Texture *mask = l.mask > 0 ? phosphors(v, l.mask) : NULL;
    if (mask) SDL_RenderTextureTiled(ren, mask, NULL, dst.h >= 1600 ? 2.0f : 1.0f, &dst);
    if (l.glow > 0) {
        if (!v->glow) {
            v->glow = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, 128, 112);
            if (v->glow) SDL_SetTextureScaleMode(v->glow, SDL_SCALEMODE_LINEAR);
        }
        if (v->glow && v->blur_a) {
            SDL_FRect src = {0, 0, (float)v->src_w, (float)v->src_h};
            SDL_SetTextureScaleMode(v->frame, SDL_SCALEMODE_LINEAR);
            SDL_SetRenderTarget(ren, v->glow);
            SDL_RenderTexture(ren, v->frame, &src, NULL);
            SDL_SetRenderTarget(ren, v->blur_a);
            SDL_RenderTexture(ren, v->glow, NULL, NULL);
            SDL_SetRenderTarget(ren, NULL);
            SDL_Texture *g[2] = {v->glow, v->blur_a};
            float amount[2] = {l.glow * 0.5f, l.glow};
            for (int i = 0; i < 2; i++) {
                SDL_SetTextureBlendMode(g[i], SDL_BLENDMODE_ADD);
                draw_on_glass(ren, g[i], dst, l.curve, amount[i]);
                SDL_SetTextureBlendMode(g[i], SDL_BLENDMODE_BLEND);
            }
        }
    }
    SDL_Texture *vig = l.vignette > 0 ? vignette(v) : NULL;
    if (vig) draw_on_glass(ren, vig, dst, l.curve, l.vignette);
    return true;
}

SDL_FRect video_draw(Video *v, const Settings *s)
{
    SDL_Renderer *ren = v->ren;
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    SDL_FRect dst = place(v, s, ww, wh);
    SDL_FRect src = {0, 0, (float)v->src_w, (float)v->src_h};

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);

    /* the sides of a wide screen: the picture itself, blurred and dimmed */
    bool covers = dst.x <= 0 && dst.y <= 0 && dst.w >= (float)ww && dst.h >= (float)wh;
    if (s->sides == SIDES_BLUR && !covers && v->blur_a && v->blur_b) {
        SDL_SetTextureScaleMode(v->frame, SDL_SCALEMODE_LINEAR);
        SDL_SetRenderTarget(ren, v->blur_a);
        SDL_RenderTexture(ren, v->frame, &src, NULL);
        SDL_SetRenderTarget(ren, v->blur_b);
        SDL_RenderTexture(ren, v->blur_a, NULL, NULL);
        SDL_SetRenderTarget(ren, NULL);
        /* cover the window keeping the picture's shape, overflowing at the edges */
        float ar = dst.w / dst.h;
        float bw = (float)ww, bh = bw / ar;
        if (bh < (float)wh) { bh = (float)wh; bw = bh * ar; }
        SDL_FRect bg = {((float)ww - bw) / 2, ((float)wh - bh) / 2, bw, bh};
        SDL_RenderTexture(ren, v->blur_b, NULL, &bg);
        ui_box(ren, (SDL_FRect){0, 0, (float)ww, (float)wh}, (SDL_Color){0, 0, 0, 120});
    }

    if (s->effect != EFFECT_OFF && draw_effect(v, s, dst)) return dst;
    SDL_FRect part;
    SDL_Texture *pic = picture(v, s, dst.w, dst.h, &part);
    SDL_RenderTexture(ren, pic, &part, &dst);
    return dst;
}

/* ---- text -------------------------------------------------------------------------- */

int ui_scale(SDL_Renderer *ren)
{
    int ww, wh;
    SDL_GetRenderOutputSize(ren, &ww, &wh);
    int s = wh / 270;
    if (ww / 360 < s) s = ww / 360;
    return s < 1 ? 1 : s;
}

void ui_text(SDL_Renderer *ren, float x, float y, float scale, SDL_Color c, const char *text)
{
    SDL_SetRenderScale(ren, scale, scale);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderDebugText(ren, x / scale, y / scale, text);
    SDL_SetRenderScale(ren, 1.0f, 1.0f);
}

void ui_box(SDL_Renderer *ren, SDL_FRect r, SDL_Color c)
{
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(ren, &r);
}
