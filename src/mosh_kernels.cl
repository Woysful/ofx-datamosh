// mosh_kernels.cl - GPU versions of the per-pixel stages in mosh_core.h.
//
// One source for CUDA (compiled to PTX with NVRTC, --fmad=false) and
// OpenCL C 1.2. Every kernel does the same float operations in the same order
// as the CPU code, without contraction into fused multiply-adds, so results
// match it bit for bit; the per-block decisions stay on the CPU. All kernels
// are 1D: work item gid < n.

#if defined(__CUDACC__) || defined(__CUDACC_RTC__)
#define KERNEL extern "C" __global__ void
#define FN static __device__ __forceinline__
#define G
#define GID ((int)(blockIdx.x * blockDim.x + threadIdx.x))
#define FLOORF floorf
#define FABSF fabsf
#define SQRTF sqrtf
#define ROUNDF roundf
#define INF_F __int_as_float(0x7f800000)
typedef unsigned int u32;
#else
#pragma OPENCL FP_CONTRACT OFF
#define KERNEL __kernel void
#define FN static inline
#define G __global
#define GID ((int)get_global_id(0))
#define FLOORF floor
#define FABSF fabs
#define SQRTF sqrt
#define ROUNDF round
#define INF_F as_float(0x7f800000)
typedef unsigned int u32;
#endif

FN int imin(int a, int b) { return a < b ? a : b; }
FN int imax(int a, int b) { return a > b ? a : b; }
FN int iabs(int a) { return a < 0 ? -a : a; }
FN int iclamp(int v, int lo, int hi) { return imin(imax(v, lo), hi); }
// std::max(v, 0.f) and std::min(v, 1.f), NaN and signed zeros included.
FN float clamp01(float v) {
    v = v < 0.f ? 0.f : v;
    return 1.f < v ? 1.f : v;
}
FN float smoothstep01(float v) {
    v = clamp01(v);
    return v * v * (3.f - 2.f * v);
}

FN float lumaAt(G const float* L, int w, int h, int x, int y) {
    return L[iclamp(y, 0, h - 1) * w + iclamp(x, 0, w - 1)];
}
FN G const float* pixAt(G const float* im, int w, int h, int x, int y) {
    return im + ((size_t)iclamp(y, 0, h - 1) * w + iclamp(x, 0, w - 1)) * 4;
}
FN float bilinearMix(float a, float b, float c, float d, float ax, float ay) {
    return (a + ax * (b - a)) * (1.f - ay) + (c + ax * (d - c)) * ay;
}
FN float bilinearLuma(G const float* L, int w, int h, float x, float y) {
    const int x0 = (int)FLOORF(x), y0 = (int)FLOORF(y);
    const float ax = x - (float)x0, ay = y - (float)y0;
    return bilinearMix(lumaAt(L, w, h, x0, y0), lumaAt(L, w, h, x0 + 1, y0), lumaAt(L, w, h, x0, y0 + 1),
                       lumaAt(L, w, h, x0 + 1, y0 + 1), ax, ay);
}

FN float blockHash(int bx, int by, int seed) {
    u32 h = (u32)bx * 73856093u ^ (u32)by * 19349663u ^ (u32)seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return (float)(h & 0xffffffu) / 16777216.f;
}

// ---------------------------------------------------------------- motion estimation

KERNEL k_luma(int n, G const float* rgba, G float* L) {
    const int i = GID;
    if (i >= n) return;
    G const float* p = rgba + (size_t)i * 4;
    L[i] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

KERNEL k_halve(int n, G const float* src, int sw, int sh, G float* dst, int dw) {
    const int i = GID;
    if (i >= n) return;
    const int x = i % dw, y = i / dw;
    dst[i] = 0.25f * (lumaAt(src, sw, sh, 2 * x, 2 * y) + lumaAt(src, sw, sh, 2 * x + 1, 2 * y) +
                      lumaAt(src, sw, sh, 2 * x, 2 * y + 1) + lumaAt(src, sw, sh, 2 * x + 1, 2 * y + 1));
}

// Sum of |cur - prev| over the ws x ws window at (cx, cy), prev displaced by
// (vx, vy). With useBound it gives up once the partial cost reaches bound
// (pt: the vector penalty): partial sums only grow, so such a candidate
// cannot win and the choice is unchanged.
FN float sadWindow(G const float* Lp, int pw, int ph, G const float* Lc, int cw, int ch, int cx, int cy, int vx,
                   int vy, int ws, float norm, int useBound, float pt, float bound, int* over) {
    float s = 0.f;
    *over = 0;
    for (int yy = 0; yy < ws; ++yy) {
        for (int xx = 0; xx < ws; ++xx)
            s += FABSF(lumaAt(Lc, cw, ch, cx + xx, cy + yy) - lumaAt(Lp, pw, ph, cx + xx + vx, cy + yy + vy));
        if (useBound && s * norm + pt >= bound) {
            *over = 1;
            return s;
        }
    }
    return s;
}

// One work item per block and candidate row: the best candidate of the row
// (first one on ties, the parent vector skipped), like the CPU loop over
// that row would find it.
KERNEL k_search_rows(int n, G const float* Lp, int pw, int ph, G const float* Lc, int cw, int ch, G const int* dx,
                     G const int* dy, int bw, int block, int d, int ws, int r, float norm, float lambda,
                     G float* rowCost, G float* rowM, G int* rowVx) {
    const int gid = GID;
    if (gid >= n) return;
    const int rows = 2 * r + 1;
    const int i = gid / rows, row = gid % rows;
    const int bx = i % bw, by = i / bw;
    const int hw = ws / 2;
    const int cx = ((bx * block + block / 2) / d) - hw;
    const int cy = ((by * block + block / 2) / d) - hw;
    const int pdx = dx[i] / d, pdy = dy[i] / d;
    const int vy = pdy - r + row;
    float best = INF_F, bestM = 0.f;
    int bestVx = pdx;
    for (int vx = pdx - r; vx <= pdx + r; ++vx) {
        if (vx == pdx && vy == pdy) continue;
        const float pt = lambda * (float)(iabs(vx) + iabs(vy)) * (float)d;
        int over;
        const float s = sadWindow(Lp, pw, ph, Lc, cw, ch, cx, cy, vx, vy, ws, norm, 1, pt, best, &over);
        if (over) continue;
        const float m = s * norm;
        const float c = m + lambda * (float)(iabs(vx) + iabs(vy)) * (float)d;
        if (c < best) { best = c; bestM = m; bestVx = vx; }
    }
    rowCost[gid] = best;
    rowM[gid] = bestM;
    rowVx[gid] = bestVx;
}

// Per block: parent vector, zero vector, then the rows in order; writes
// this level's vector (full-res px) and its mean abs error.
KERNEL k_search_pick(int n, G const float* Lp, int pw, int ph, G const float* Lc, int cw, int ch, G int* dx,
                     G int* dy, int bw, int block, int d, int ws, int r, float norm, float lambda,
                     G const float* rowCost, G const float* rowM, G const int* rowVx, G float* err) {
    const int i = GID;
    if (i >= n) return;
    const int rows = 2 * r + 1;
    const int bx = i % bw, by = i / bw;
    const int hw = ws / 2;
    const int cx = ((bx * block + block / 2) / d) - hw;
    const int cy = ((by * block + block / 2) / d) - hw;
    const int pdx = dx[i] / d, pdy = dy[i] / d;
    int over;
    int bvx = pdx, bvy = pdy;
    float bm = sadWindow(Lp, pw, ph, Lc, cw, ch, cx, cy, pdx, pdy, ws, norm, 0, 0.f, 0.f, &over) * norm;
    float bc = bm + lambda * (float)(iabs(pdx) + iabs(pdy)) * (float)d;
    if (pdx != 0 || pdy != 0) {
        const float m0 = sadWindow(Lp, pw, ph, Lc, cw, ch, cx, cy, 0, 0, ws, norm, 0, 0.f, 0.f, &over) * norm;
        const float c0 = m0 + lambda * (float)(iabs(0) + iabs(0)) * (float)d;
        if (c0 < bc) { bc = c0; bm = m0; bvx = 0; bvy = 0; }
    }
    if (bm > 0.004f)
        for (int row = 0; row < rows; ++row) {
            const float c = rowCost[i * rows + row];
            if (c < bc) {
                bc = c;
                bm = rowM[i * rows + row];
                bvx = rowVx[i * rows + row];
                bvy = pdy - r + row;
            }
        }
    dx[i] = bvx * d;
    dy[i] = bvy * d;
    err[i] = bm;
}

// Luma spread of each block below thr (full-res luma, clamped at the edges).
KERNEL k_flat(int n, G const float* L, int w, int h, int bw, int block, float thr, G int* flat) {
    const int i = GID;
    if (i >= n) return;
    const int bx = i % bw, by = i / bw;
    float mn = 1e9f, mx = -1e9f;
    for (int yy = 0; yy < block; ++yy)
        for (int xx = 0; xx < block; ++xx) {
            const float v = lumaAt(L, w, h, bx * block + xx, by * block + yy);
            mn = v < mn ? v : mn;
            mx = mx < v ? v : mx;
        }
    flat[i] = (mx - mn) < thr ? 1 : 0;
}

// Sums for refineBlock: work item k of block i (10 per block) writes
// out[i * 11 + k]: k < 9 the summed squared error of half-pel candidate k,
// k == 9 the sums of luma and squared luma (out[.. + 9], out[.. + 10]).
KERNEL k_refine(int n, G const float* Lp, int pw, int ph, G const float* Lc, int cw, G const int* dx,
                G const int* dy, int bw, int block, int curW, int curH, int halfPel, G float* out) {
    const int gid = GID;
    if (gid >= n) return;
    const int i = gid / 10, k = gid % 10;
    const int bx = i % bw, by = i / bw;
    const int x0 = bx * block, y0 = by * block;
    const int x1 = imin(x0 + block, curW), y1 = imin(y0 + block, curH);
    G float* o = out + (size_t)i * 11;
    if (k == 9) {
        float m = 0.f, m2 = 0.f;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const float l = Lc[y * cw + x];
                m += l;
                m2 += l * l;
            }
        o[9] = m;
        o[10] = m2;
        return;
    }
    if (k > 0 && !halfPel) {
        o[k] = 0.f;
        return;
    }
    float vx = (float)dx[i], vy = (float)dy[i];
    if (k > 0) {
        int j = k - 1;
        if (j >= 4) ++j;
        vx = vx + 0.5f * (float)(j % 3 - 1);
        vy = vy + 0.5f * (float)(j / 3 - 1);
    }
    const int ix = (int)FLOORF(vx), iy = (int)FLOORF(vy);
    const int hx = vx != (float)ix, hy = vy != (float)iy;
    const int fast = ((!hx && !hy) || halfPel) && x0 + ix >= 0 && y0 + iy >= 0 && x1 + ix <= pw && y1 + iy <= ph;
    float s = 0.f;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            float pp;
            if (fast) {
                if (!hx && !hy) pp = Lp[(y + iy) * pw + x + ix];
                else pp = bilinearLuma(Lp, pw, ph, (float)(x + ix) + (hx ? 0.5f : 0.f), (float)(y + iy) + (hy ? 0.5f : 0.f));
            } else {
                pp = bilinearLuma(Lp, pw, ph, (float)x + vx, (float)y + vy);
            }
            const float e = Lc[y * cw + x] - pp;
            s += e * e;
        }
    o[k] = s;
}

// ---------------------------------------------------------------- mosh step

FN void bilinear4(G const float* im, int w, int h, float x, float y, float* o) {
    const int x0 = (int)FLOORF(x), y0 = (int)FLOORF(y);
    const float ax = x - (float)x0, ay = y - (float)y0;
    G const float* a = pixAt(im, w, h, x0, y0);
    G const float* b = pixAt(im, w, h, x0 + 1, y0);
    G const float* c = pixAt(im, w, h, x0, y0 + 1);
    G const float* d = pixAt(im, w, h, x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k) o[k] = (a[k] + ax * (b[k] - a[k])) * (1.f - ay) + (c[k] + ax * (d[k] - c[k])) * ay;
}

// table: per block {intra, x offset, y offset, unused}; offsets are the
// scaled vector (subpel) or its rounded value.
KERNEL k_step(int n, G const float* prev, G const float* cur, G const float* accIn, G float* accOut, int w, int h,
              int bw, int bh, int block, G const float* table, int subpel, float residual, int clampLevels) {
    const int gid = GID;
    if (gid >= n) return;
    const int x = gid % w, y = gid / w;
    const int i = imin(y / block, bh - 1) * bw + imin(x / block, bw - 1);
    G const float* c = cur + (size_t)gid * 4;
    G float* o = accOut + (size_t)gid * 4;
    G const float* t = table + (size_t)i * 4;
    if (t[0] != 0.f) {
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
        return;
    }
    float a[4], p[4];
    if (subpel) {
        const float fx = (float)x + t[1], fy = (float)y + t[2];
        bilinear4(accIn, w, h, fx, fy, a);
        bilinear4(prev, w, h, fx, fy, p);
    } else {
        const int dx = (int)t[1], dy = (int)t[2];
        G const float* pa = pixAt(accIn, w, h, x + dx, y + dy);
        G const float* pp = pixAt(prev, w, h, x + dx, y + dy);
        for (int k = 0; k < 4; ++k) { a[k] = pa[k]; p[k] = pp[k]; }
    }
    for (int ch = 0; ch < 3; ++ch) {
        float v = a[ch] + residual * (c[ch] - p[ch]);
        if (clampLevels) v = clamp01(v);
        o[ch] = v;
    }
    o[3] = a[3];
}

// ---------------------------------------------------------------- in-chain recovery

KERNEL k_refresh_act(int n, G const float* prev, G const float* cur, int w, int h, int bw, int block, G float* sums,
                     G int* counts) {
    const int i = GID;
    if (i >= n) return;
    const int bx = i % bw, by = i / bw;
    float s = 0.f;
    int cnt = 0;
    for (int y = by * block; y < imin((by + 1) * block, h); y += 2)
        for (int x = bx * block; x < imin((bx + 1) * block, w); x += 2) {
            G const float* c = cur + ((size_t)y * w + x) * 4;
            G const float* p = prev + ((size_t)y * w + x) * 4;
            s += FABSF(c[0] - p[0]) + FABSF(c[1] - p[1]) + FABSF(c[2] - p[2]);
            ++cnt;
        }
    sums[i] = s;
    counts[i] = cnt;
}

// acc += k * (cur - acc), k = rate in flagged blocks, tail elsewhere; k <= 0 leaves the pixel alone.
KERNEL k_pull(int n, G float* acc, G const float* cur, int w, int bw, int bh, int block, G const int* flagged,
              float rate, float tail) {
    const int gid = GID;
    if (gid >= n) return;
    const int x = gid % w, y = gid / w;
    const int i = imin(y / block, bh - 1) * bw + imin(x / block, bw - 1);
    const float k = flagged[i] ? rate : tail;
    if (k <= 0.f) return;
    G float* a = acc + (size_t)gid * 4;
    G const float* c = cur + (size_t)gid * 4;
    for (int ch = 0; ch < 4; ++ch) a[ch] += k * (c[ch] - a[ch]);
}

// ---------------------------------------------------------------- compose

// Separable box blur, edges clamped: rows of src into dst, one work item per row.
KERNEL k_blur_rows(int n, G const float* src, G float* dst, int w, int h, int r, float inv) {
    const int y = GID;
    if (y >= n) return;
    float s[4] = {0.f, 0.f, 0.f, 0.f};
    for (int k = -r; k <= r; ++k) {
        G const float* p = pixAt(src, w, h, k, y);
        for (int c = 0; c < 4; ++c) s[c] += p[c];
    }
    for (int x = 0; x < w; ++x) {
        G float* o = dst + ((size_t)y * w + x) * 4;
        for (int c = 0; c < 4; ++c) o[c] = s[c] * inv;
        G const float* add = pixAt(src, w, h, x + r + 1, y);
        G const float* sub = pixAt(src, w, h, x - r, y);
        for (int c = 0; c < 4; ++c) s[c] += add[c] - sub[c];
    }
}

KERNEL k_blur_cols(int n, G const float* src, G float* dst, int w, int h, int r, float inv) {
    const int x = GID;
    if (x >= n) return;
    float s[4] = {0.f, 0.f, 0.f, 0.f};
    for (int k = -r; k <= r; ++k) {
        G const float* p = pixAt(src, w, h, x, k);
        for (int c = 0; c < 4; ++c) s[c] += p[c];
    }
    for (int y = 0; y < h; ++y) {
        G float* o = dst + ((size_t)y * w + x) * 4;
        for (int c = 0; c < 4; ++c) o[c] = s[c] * inv;
        G const float* add = pixAt(src, w, h, x, y + r + 1);
        G const float* sub = pixAt(src, w, h, x, y - r);
        for (int c = 0; c < 4; ++c) s[c] += add[c] - sub[c];
    }
}

// Structure first / Colour first: low band and high band blended separately.
KERNEL k_bands(int n, G const float* acc, G const float* to, G const float* lowA, G const float* lowT, G float* out,
               float kc, float kd) {
    const int gid = GID;
    if (gid >= n) return;
    const size_t o4 = (size_t)gid * 4;
    for (int ch = 0; ch < 4; ++ch) {
        const float a = acc[o4 + ch], t = to[o4 + ch], la = lowA[o4 + ch], lt = lowT[o4 + ch];
        const float low = la + kc * (lt - la);
        const float high = (a - la) + kd * ((t - lt) - (a - la));
        out[o4 + ch] = low + high;
    }
}

KERNEL k_slice(int n, G const float* acc, G const float* to, G float* out, int w, int h, int columns,
               G const int* band, G const float* start, G const int* sign, float q, float dur) {
    const int gid = GID;
    if (gid >= n) return;
    const int x = gid % w, y = gid / w;
    const int b = band[columns ? x : y];
    const int len = columns ? h : w;
    const int pos = columns ? y : x;
    const float lp = q >= 1.f ? 1.f : smoothstep01((q - start[b]) / dur);
    const int e = (int)ROUNDF(lp * (float)len);
    const int p = sign[b] > 0 ? pos : len - 1 - pos;
    const int in = p < e;
    const int shift = in ? (len - e) : -e;
    const int sp = pos + (sign[b] > 0 ? shift : -shift);
    G const float* src = in ? to : acc;
    G const float* c = columns ? pixAt(src, w, h, x, sp) : pixAt(src, w, h, sp, y);
    G float* o = out + (size_t)gid * 4;
    for (int ch = 0; ch < 4; ++ch) o[ch] = c[ch];
}

// Settle: mean abs RGB difference in channel 0, the rest zero.
KERNEL k_settle_diff(int n, G const float* acc, G const float* to, G float* diff) {
    const int gid = GID;
    if (gid >= n) return;
    G const float* a = acc + (size_t)gid * 4;
    G const float* t = to + (size_t)gid * 4;
    G float* o = diff + (size_t)gid * 4;
    o[0] = (FABSF(a[0] - t[0]) + FABSF(a[1] - t[1]) + FABSF(a[2] - t[2])) / 3.f;
    o[1] = 0.f; o[2] = 0.f; o[3] = 0.f;
}

KERNEL k_gather(int n, G const float* src, int stride, G float* dst) {
    const int i = GID;
    if (i >= n) return;
    dst[i] = src[(size_t)i * stride];
}

// Direction values as in mosh::Direction.
FN float wipeCoord(int d, int x, int y, int w, int h) {
    const float u = ((float)x + 0.5f) / (float)imax(1, w);
    const float v = ((float)y + 0.5f) / (float)imax(1, h);
    switch (d) {
        case 1: return 1.f - u;
        case 2: return v;
        case 3: return 1.f - v;
        case 4:
        case 5: {
            const float ax = (u - 0.5f) * (float)w, ay = (v - 0.5f) * (float)h;
            const float r = SQRTF(ax * ax + ay * ay) / (0.5f * SQRTF((float)w * (float)w + (float)h * (float)h));
            return d == 4 ? clamp01(r) : 1.f - clamp01(r);
        }
        default: return u;
    }
}

// Recovery modes as in mosh::Recovery; key is the blurred picture (Luma
// reveal) or the blurred difference (Settle).
KERNEL k_compose(int n, G const float* acc, G const float* to, G const float* key, G float* out, int w, int h,
                 int mode, float q, int block, int seed, float bw, float fw, float ww, float jag, int invert,
                 float keyNorm, int direction) {
    const int gid = GID;
    if (gid >= n) return;
    const int x = gid % w, y = gid / w;
    G const float* a = acc + (size_t)gid * 4;
    G const float* t = to + (size_t)gid * 4;
    float k;
    switch (mode) {
        case 0: {
            const float hh = blockHash(x / block, y / block, seed);
            if (q >= 1.f) k = 1.f;
            else if (bw <= 0.f) k = (q > 0.f && hh < q) ? 1.f : 0.f;
            else k = clamp01((q * (1.f + bw) - hh) / bw);
            break;
        }
        case 1: k = q; break;
        case 3:
        case 10: k = q >= 1.f ? 1.f : 0.f; break;
        case 5: {
            G const float* kp = key + (size_t)gid * 4;
            float l = clamp01(0.299f * kp[0] + 0.587f * kp[1] + 0.114f * kp[2]);
            if (invert) l = 1.f - l;
            k = smoothstep01((q * (1.f + fw) - (1.f - l)) / fw);
            break;
        }
        case 7: {
            const float dd = clamp01(key[(size_t)gid * 4] * keyNorm);
            k = smoothstep01((q * (1.f + fw) - dd) / fw);
            break;
        }
        case 8: {
            const float u = wipeCoord(direction, x, y, w, h);
            const float hh = blockHash(x / block, y / block, seed + 59);
            const float pos = (u + jag * hh) / (1.f + jag);
            k = smoothstep01((q * (1.f + ww) - pos) / ww);
            break;
        }
        default: k = q > 0.f ? 1.f : 0.f; break;
    }
    G float* o = out + (size_t)gid * 4;
    for (int ch = 0; ch < 4; ++ch) o[ch] = a[ch] + k * (t[ch] - a[ch]);
}
