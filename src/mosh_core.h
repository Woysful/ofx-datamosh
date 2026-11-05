// mosh_core.h - host-independent datamosh simulation.
//
// Real datamosh happens when a decoder applies the motion vectors and
// residuals of P-frames to the wrong reference picture (the I-frame was
// removed), or decodes the same P-frame several times (duplicated P-frames,
// the "bloom"). We reproduce both: estimate block motion between frames of the
// "motion" clip like an encoder would, then drag a reference picture along
// those vectors while adding the prediction residual of each step.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

// SSE2 is part of every x86-64 CPU. MOSH_NO_SIMD builds the plain loops, which
// give the same results.
#if !defined(MOSH_NO_SIMD) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#define MOSH_SSE 1
#else
#define MOSH_SSE 0
#endif

namespace mosh {

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;  // RGBA interleaved, row-major

    Image() = default;
    Image(int W, int H) : w(W), h(H), px((size_t)W * H * 4, 0.f) {}

    bool empty() const { return w <= 0 || h <= 0; }
    void resize(int W, int H) {
        if (W != w || H != h) {
            w = W;
            h = H;
            px.assign((size_t)W * H * 4, 0.f);
        }
    }
    float* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    const float* clampedAt(int x, int y) const {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return at(x, y);
    }
};

// Splits [0,n) across threads; body(begin, end) must be thread-safe.
using RangeBody = std::function<void(int, int)>;
using ParallelFor = std::function<void(int n, const RangeBody& body)>;

inline void serialFor(int n, const RangeBody& body) {
    if (n > 0) body(0, n);
}

struct MotionField {
    int block = 16;  // full-res block size in px
    int bw = 0, bh = 0;
    // Block (bx,by) of the current frame is predicted from the previous
    // frame displaced by (dx,dy), in full-res pixels.
    std::vector<int> dx, dy;
    // Mean absolute luma error of the best match, 0..1. Encoders code blocks
    // with a hopeless prediction as intra blocks; we flag them the same way.
    std::vector<float> err;
    std::vector<uint8_t> intra;
    // Optional, filled when MotionParams asks for them:
    std::vector<float> fx, fy;  // sub-pixel vectors (half-pel), replace dx/dy in the mosh step
    std::vector<float> sse;     // mean squared luma error of the chosen prediction
    std::vector<float> var;     // luma variance of the block itself (what intra coding costs)

    bool subpel() const { return !fx.empty(); }

    int index(int bx, int by) const { return by * bw + bx; }
};

struct Luma {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return v[(size_t)y * w + x];
    }
};

inline Luma toLuma(const Image& img, const ParallelFor& pf) {
    Luma L;
    L.w = img.w;
    L.h = img.h;
    L.v.resize((size_t)L.w * L.h);
    pf(L.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < L.w; ++x) {
                const float* p = img.at(x, y);
                L.v[(size_t)y * L.w + x] = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
            }
    });
    return L;
}

inline Luma halve(const Luma& src, const ParallelFor& pf) {
    Luma L;
    L.w = std::max(1, src.w / 2);
    L.h = std::max(1, src.h / 2);
    L.v.resize((size_t)L.w * L.h);
    pf(L.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < L.w; ++x)
                L.v[(size_t)y * L.w + x] =
                    0.25f * (src.at(2 * x, 2 * y) + src.at(2 * x + 1, 2 * y) + src.at(2 * x, 2 * y + 1) +
                             src.at(2 * x + 1, 2 * y + 1));
    });
    return L;
}

// levels[k] is downsampled by 2^k.
inline std::vector<Luma> buildPyramid(const Image& img, int levels, const ParallelFor& pf) {
    std::vector<Luma> pyr;
    pyr.push_back(toLuma(img, pf));
    for (int k = 1; k < levels; ++k) pyr.push_back(halve(pyr.back(), pf));
    return pyr;
}

struct MotionParams {
    int block = 16;      // macroblock size, px
    int range = 48;      // max displacement, px
    bool smooth = true;  // 3x3 median on the vector field
    float intraThreshold = 0.35f;  // mean abs luma error above which a block is intra (1 = never)
    // Encoders give textureless blocks the vector predicted from their
    // neighbours (it costs no bits), which is why flat sky flows in a real
    // mosh. We do the same: blocks whose luma spread is below flatThreshold
    // inherit the median vector of textured blocks around them.
    bool fillFlat = true;
    float flatThreshold = 0.015f;
    // Refine vectors to half a pixel; the mosh step then interpolates, which
    // is what makes the smear of a real decoder soften as it travels.
    bool halfPel = false;
    // Encoder-style intra decision, 0 = off: a block of the new shot is coded
    // intra when predicting it costs more than coding it from scratch.
    float intraRate = 0.f;
    bool blockStats = false;  // fill sse/var even when intraRate is 0
};

// Thresholds of the encoder intra decision for an eagerness e in (0,1]:
// intra when sse > k * var + bias. e around 0.5 matches an MPEG-4 ASP encoder.
struct IntraRule {
    float k = 0.f, bias = 0.f;
};
inline IntraRule intraRule(float e) {
    e = std::min(std::max(e, 0.f), 1.f);
    IntraRule r;
    r.k = 2.5f * (1.f - e);
    r.bias = 0.008f * (1.f - e) * (1.f - e) + 0.0002f;
    return r;
}

inline float bilinearMix(float a, float b, float c, float d, float ax, float ay) {
    return (a + ax * (b - a)) * (1.f - ay) + (c + ax * (d - c)) * ay;
}

inline float bilinearLuma(const Luma& L, float x, float y) {
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float ax = x - (float)x0, ay = y - (float)y0;
    float a, b, c, d;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < L.w && y0 + 1 < L.h) {
        const float* p = &L.v[(size_t)y0 * L.w + x0];
        a = p[0]; b = p[1]; c = p[L.w]; d = p[L.w + 1];
    } else {
        a = L.at(x0, y0); b = L.at(x0 + 1, y0); c = L.at(x0, y0 + 1); d = L.at(x0 + 1, y0 + 1);
    }
    return bilinearMix(a, b, c, d, ax, ay);
}

// Hierarchical block matching: full search at the coarsest pyramid level,
// then +-2 steps of refinement at every finer level down to full resolution.
// Each block is matched on a window of at least 4x4 samples at every level, so
// small blocks still get a meaningful match at coarse levels.
//
// The work is split into stages so a GPU can run the per-pixel ones while the
// per-block decisions stay in one place (here):
//   newField, pyramidLevels      sizes
//   searchLevel (per level)      pixels: block search
//   flatBlocks                   pixels: which blocks are textureless
//   postProcessField             blocks: median smoothing, flat fill, intra threshold
//   refineField                  pixels: half-pel refinement, block statistics
//   finishIntra                  blocks: encoder intra decision
inline MotionField newField(int w, int h, const MotionParams& mp) {
    MotionField mf;
    mf.block = std::max(4, mp.block);
    mf.bw = (w + mf.block - 1) / mf.block;
    mf.bh = (h + mf.block - 1) / mf.block;
    const size_t nb = (size_t)mf.bw * mf.bh;
    mf.dx.assign(nb, 0);
    mf.dy.assign(nb, 0);
    mf.err.assign(nb, 0.f);
    mf.intra.assign(nb, 0);
    return mf;
}

inline int pyramidLevels(int w, int h) {
    int levels = 1;
    while (levels < 4 && (std::min(w, h) >> levels) >= 32) ++levels;
    return levels;
}

static const float kMotionLambda = 0.0008f;  // per px of vector length, in mean-abs-diff units

// Search radius and match window at pyramid level lvl (of levels).
inline int searchRadius(const MotionParams& mp, int lvl, int levels) {
    const int d = 1 << lvl;
    return lvl == levels - 1 ? std::max(2, (mp.range + d - 1) / d) : 2;
}
inline int searchWindow(const MotionField& mf, int lvl) { return std::max(4, mf.block / (1 << lvl)); }

// One level of the search; mf.dx/dy hold the parent level's vectors (in
// full-res pixels) on entry and this level's on return.
inline void searchLevel(const Luma& Lp, const Luma& Lc, int lvl, int levels, const MotionParams& mp, MotionField& mf,
                        const ParallelFor& pf) {
    const int d = 1 << lvl;
    const int ws = searchWindow(mf, lvl);
    const int half = ws / 2;
    const int r = searchRadius(mp, lvl, levels);
    const float norm = 1.f / float(ws * ws);
    const float lambda = kMotionLambda;

    pf(mf.bh, [&](int b0, int b1) {
        for (int by = b0; by < b1; ++by) {
            for (int bx = 0; bx < mf.bw; ++bx) {
                const int i = mf.index(bx, by);
                // window centre in this level's coordinates
                const int cx = ((bx * mf.block + mf.block / 2) / d) - half;
                const int cy = ((by * mf.block + mf.block / 2) / d) - half;
                const int pdx = mf.dx[i] / d, pdy = mf.dy[i] / d;  // parent vector at this level

                const bool curInside = cx >= 0 && cy >= 0 && cx + ws <= Lc.w && cy + ws <= Lc.h;
                // Sum of |cur - prev| over the window. With a bound, gives up as soon as the
                // partial cost reaches it: partial sums of non-negative terms only grow, so
                // such a candidate could never win and the result is the same.
                auto sad = [&](int vx, int vy, float bound, int pen, bool& over) {
                    float s = 0.f;
                    over = false;
                    const bool inside = curInside && cx + vx >= 0 && cy + vy >= 0 && cx + vx + ws <= Lp.w &&
                                        cy + vy + ws <= Lp.h;
                    for (int yy = 0; yy < ws; ++yy) {
                        if (inside) {
                            const float* pc = &Lc.v[(size_t)(cy + yy) * Lc.w + cx];
                            const float* pp = &Lp.v[(size_t)(cy + yy + vy) * Lp.w + cx + vx];
                            for (int xx = 0; xx < ws; ++xx) s += std::fabs(pc[xx] - pp[xx]);
                        } else {
                            for (int xx = 0; xx < ws; ++xx)
                                s += std::fabs(Lc.at(cx + xx, cy + yy) - Lp.at(cx + xx + vx, cy + yy + vy));
                        }
                        if (pen >= 0 && s * norm + lambda * float(pen) * float(d) >= bound) {
                            over = true;
                            return s;
                        }
                    }
                    return s;
                };
                auto mad = [&](int vx, int vy) {
                    bool over;
                    return sad(vx, vy, 0.f, -1, over) * norm;
                };
                auto cost = [&](int vx, int vy, float m) {
                    return m + lambda * float(std::abs(vx) + std::abs(vy)) * float(d);
                };

                int bvx = pdx, bvy = pdy;
                float bm = mad(pdx, pdy);
                float bc = cost(pdx, pdy, bm);
                if (pdx != 0 || pdy != 0) {  // zero vector is always a candidate
                    const float m0 = mad(0, 0);
                    const float c0 = cost(0, 0, m0);
                    if (c0 < bc) { bc = c0; bm = m0; bvx = 0; bvy = 0; }
                }
                auto consider = [&](int vx, int vy, float sum) {
                    const float m = sum * norm;
                    const float c = cost(vx, vy, m);
                    if (c < bc) { bc = c; bm = m; bvx = vx; bvy = vy; }
                };
                if (bm > 0.004f) {
                    for (int vy = pdy - r; vy <= pdy + r; ++vy) {
                        int vx = pdx - r;
#if MOSH_SSE
                        // Four horizontally adjacent candidates at once, one per lane. Every lane
                        // adds the same terms in the same order as the scalar loop, so its sums are
                        // identical. The batch gives up on a lane against the best cost from before
                        // the batch; that bound is never lower than the one the scalar loop would
                        // use, and a lane that would have been given up there cannot beat the best
                        // cost by the time it is considered, so the result is the same.
                        for (; vx + 3 <= pdx + r; vx += 4) {
                            if (!(curInside && cx + vx >= 0 && cy + vy >= 0 && cx + vx + 3 + ws <= Lp.w &&
                                  cy + vy + ws <= Lp.h)) {
                                for (int k = 0; k < 4; ++k) {
                                    if (vx + k == pdx && vy == pdy) continue;
                                    bool over;
                                    const float sum = sad(vx + k, vy, bc, std::abs(vx + k) + std::abs(vy), over);
                                    if (!over) consider(vx + k, vy, sum);
                                }
                                continue;
                            }
                            alignas(16) float pt[4];
                            for (int k = 0; k < 4; ++k) pt[k] = lambda * float(std::abs(vx + k) + std::abs(vy)) * float(d);
                            const __m128 vpt = _mm_load_ps(pt), vnorm = _mm_set1_ps(norm), vbound = _mm_set1_ps(bc);
                            const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
                            __m128 s = _mm_setzero_ps(), over = _mm_setzero_ps();
                            for (int yy = 0; yy < ws; ++yy) {
                                const float* pc = &Lc.v[(size_t)(cy + yy) * Lc.w + cx];
                                const float* pp = &Lp.v[(size_t)(cy + yy + vy) * Lp.w + cx + vx];
                                for (int xx = 0; xx < ws; ++xx)
                                    s = _mm_add_ps(s, _mm_and_ps(_mm_sub_ps(_mm_set1_ps(pc[xx]), _mm_loadu_ps(pp + xx)), absMask));
                                over = _mm_or_ps(over, _mm_cmpge_ps(_mm_add_ps(_mm_mul_ps(s, vnorm), vpt), vbound));
                                if (_mm_movemask_ps(over) == 15) break;
                            }
                            alignas(16) float sums[4];
                            _mm_store_ps(sums, s);
                            const int om = _mm_movemask_ps(over);
                            for (int k = 0; k < 4; ++k) {
                                if ((vx + k == pdx && vy == pdy) || (om >> k & 1)) continue;
                                consider(vx + k, vy, sums[k]);
                            }
                        }
#endif
                        for (; vx <= pdx + r; ++vx) {
                            if (vx == pdx && vy == pdy) continue;
                            bool over;
                            const float sum = sad(vx, vy, bc, std::abs(vx) + std::abs(vy), over);
                            if (!over) consider(vx, vy, sum);
                        }
                    }
                }
                mf.dx[i] = bvx * d;
                mf.dy[i] = bvy * d;
                mf.err[i] = bm;
            }
        }
    });
}

// Blocks whose full-res luma spread is below mp.flatThreshold.
inline std::vector<uint8_t> flatBlocks(const Luma& L0, const MotionField& mf, const MotionParams& mp,
                                       const ParallelFor& pf) {
    std::vector<uint8_t> flat((size_t)mf.bw * mf.bh, 0);
    pf(mf.bh, [&](int b0, int b1) {
        for (int by = b0; by < b1; ++by)
            for (int bx = 0; bx < mf.bw; ++bx) {
                float mn = 1e9f, mx = -1e9f;
                for (int yy = 0; yy < mf.block; ++yy)
                    for (int xx = 0; xx < mf.block; ++xx) {
                        const float v = L0.at(bx * mf.block + xx, by * mf.block + yy);
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                    }
                flat[mf.index(bx, by)] = (mx - mn) < mp.flatThreshold ? 1 : 0;
            }
    });
    return flat;
}

// Median smoothing, flat fill (flat from flatBlocks, may be empty when
// mp.fillFlat is off) and the intra threshold.
inline void postProcessField(MotionField& mf, std::vector<uint8_t> flat, const MotionParams& mp,
                             const ParallelFor& pf) {
    const size_t nb = (size_t)mf.bw * mf.bh;
    if (mp.smooth && mf.bw >= 3 && mf.bh >= 3) {
        std::vector<int> sx(nb), sy(nb);
        pf(mf.bh, [&](int b0, int b1) {
            int vx[9], vy[9];
            for (int by = b0; by < b1; ++by)
                for (int bx = 0; bx < mf.bw; ++bx) {
                    int n = 0;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox) {
                            const int nx = std::min(std::max(bx + ox, 0), mf.bw - 1);
                            const int ny = std::min(std::max(by + oy, 0), mf.bh - 1);
                            vx[n] = mf.dx[mf.index(nx, ny)];
                            vy[n] = mf.dy[mf.index(nx, ny)];
                            ++n;
                        }
                    std::nth_element(vx, vx + 4, vx + 9);
                    std::nth_element(vy, vy + 4, vy + 9);
                    sx[mf.index(bx, by)] = vx[4];
                    sy[mf.index(bx, by)] = vy[4];
                }
        });
        mf.dx.swap(sx);
        mf.dy.swap(sy);
    }

    if (mp.fillFlat && flat.size() == nb) {
        // Grow vectors from textured blocks into flat ones, one ring per pass.
        std::vector<int> nx(nb), ny(nb);
        std::vector<uint8_t> nflat(nb);
        for (int pass = 0; pass < 256; ++pass) {
            int changed = 0;
            nx = mf.dx; ny = mf.dy; nflat = flat;
            std::vector<int> changes(mf.bh, 0);
            pf(mf.bh, [&](int b0, int b1) {
                int vx[8], vy[8];
                for (int by = b0; by < b1; ++by)
                    for (int bx = 0; bx < mf.bw; ++bx) {
                        const int i = mf.index(bx, by);
                        if (!flat[i]) continue;
                        int n = 0;
                        for (int oy = -1; oy <= 1; ++oy)
                            for (int ox = -1; ox <= 1; ++ox) {
                                if (!ox && !oy) continue;
                                const int qx = bx + ox, qy = by + oy;
                                if (qx < 0 || qy < 0 || qx >= mf.bw || qy >= mf.bh) continue;
                                const int j = mf.index(qx, qy);
                                if (flat[j]) continue;
                                vx[n] = mf.dx[j]; vy[n] = mf.dy[j]; ++n;
                            }
                        if (n == 0) continue;
                        std::nth_element(vx, vx + n / 2, vx + n);
                        std::nth_element(vy, vy + n / 2, vy + n);
                        nx[i] = vx[n / 2]; ny[i] = vy[n / 2]; nflat[i] = 0;
                        ++changes[by];
                    }
            });
            for (int c : changes) changed += c;
            mf.dx.swap(nx); mf.dy.swap(ny); flat.swap(nflat);
            if (!changed) break;
        }
    }

    if (mp.intraThreshold < 1.f)
        for (size_t i = 0; i < nb; ++i) mf.intra[i] = mf.err[i] > mp.intraThreshold ? 1 : 0;
}

inline bool needsRefine(const MotionParams& mp) { return mp.halfPel || mp.intraRate > 0.f || mp.blockStats; }

// Half-pel refinement and per-block statistics (sse, var) on full-res luma.
inline void refineField(const Luma& Lp, const Luma& Lc, int curW, int curH, const MotionParams& mp, MotionField& mf,
                        const ParallelFor& pf) {
    const size_t nb = (size_t)mf.bw * mf.bh;
    if (mp.halfPel) {
        mf.fx.assign(nb, 0.f);
        mf.fy.assign(nb, 0.f);
    }
    mf.sse.assign(nb, 0.f);
    mf.var.assign(nb, 0.f);
    // prev luma sampled at +0.5 px in x, y and both: every half-pel
    // candidate then reads a plane instead of interpolating. Values are
    // computed with bilinearLuma itself, so they are bit-identical.
    std::vector<float> halfPlanes[3];
    if (mp.halfPel) {
        for (auto& hp : halfPlanes) hp.resize((size_t)Lp.w * Lp.h);
        pf(Lp.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const size_t row = (size_t)y * Lp.w;
                for (int x = 0; x < Lp.w; ++x) {
                    if (x + 1 < Lp.w && y + 1 < Lp.h) {
                        const float* p = &Lp.v[row + x];
                        const float a = p[0], b = p[1], c = p[Lp.w], d = p[Lp.w + 1];
                        halfPlanes[0][row + x] = bilinearMix(a, b, c, d, 0.5f, 0.f);
                        halfPlanes[1][row + x] = bilinearMix(a, b, c, d, 0.f, 0.5f);
                        halfPlanes[2][row + x] = bilinearMix(a, b, c, d, 0.5f, 0.5f);
                    } else {
                        halfPlanes[0][row + x] = bilinearLuma(Lp, x + 0.5f, (float)y);
                        halfPlanes[1][row + x] = bilinearLuma(Lp, (float)x, y + 0.5f);
                        halfPlanes[2][row + x] = bilinearLuma(Lp, x + 0.5f, y + 0.5f);
                    }
                }
            }
        });
    }
    pf(mf.bh, [&](int b0, int b1) {
        for (int by = b0; by < b1; ++by)
            for (int bx = 0; bx < mf.bw; ++bx) {
                const int i = mf.index(bx, by);
                const int x0 = bx * mf.block, y0 = by * mf.block;
                const int x1 = std::min(x0 + mf.block, curW), y1 = std::min(y0 + mf.block, curH);
                const float cnt = float((x1 - x0) * (y1 - y0));
                auto sseAt = [&](float vx, float vy) {
                    float s = 0.f;
                    // Split v into an integer offset and a half: x + v == (x + ix) + hx exactly.
                    const int ix = (int)std::floor(vx), iy = (int)std::floor(vy);
                    const bool hx = vx != (float)ix, hy = vy != (float)iy;
                    const float* plane = !hx && !hy ? Lp.v.data()
                                         : hx && !hy ? halfPlanes[0].data()
                                         : !hx && hy ? halfPlanes[1].data()
                                                     : halfPlanes[2].data();
                    const bool fast = ((!hx && !hy) || mp.halfPel) && x0 + ix >= 0 && y0 + iy >= 0 &&
                                      x1 + ix <= Lp.w && y1 + iy <= Lp.h && Lc.w == curW;
                    for (int y = y0; y < y1; ++y) {
                        const float* pc = &Lc.v[(size_t)y * Lc.w];
                        if (fast) {
                            const float* pp = plane + (size_t)(y + iy) * Lp.w + ix;
                            for (int x = x0; x < x1; ++x) {
                                const float d = pc[x] - pp[x];
                                s += d * d;
                            }
                        } else {
                            for (int x = x0; x < x1; ++x) {
                                const float d = pc[x] - bilinearLuma(Lp, x + vx, y + vy);
                                s += d * d;
                            }
                        }
                    }
                    return s / cnt;
                };
                float vx = (float)mf.dx[i], vy = (float)mf.dy[i];
                float best = sseAt(vx, vy);
                if (mp.halfPel) {
                    const float cx = vx, cy = vy;
                    for (int oy = -1; oy <= 1; ++oy)
                        for (int ox = -1; ox <= 1; ++ox) {
                            if (!ox && !oy) continue;
                            const float e = sseAt(cx + 0.5f * ox, cy + 0.5f * oy);
                            if (e < best) { best = e; vx = cx + 0.5f * ox; vy = cy + 0.5f * oy; }
                        }
                    mf.fx[i] = vx;
                    mf.fy[i] = vy;
                }
                float m = 0.f, m2 = 0.f;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const float l = Lc.v[(size_t)y * Lc.w + x];
                        m += l;
                        m2 += l * l;
                    }
                m /= cnt;
                mf.sse[i] = best;
                mf.var[i] = std::max(0.f, m2 / cnt - m * m);
            }
    });
}

inline void finishIntra(MotionField& mf, const MotionParams& mp) {
    if (!(mp.intraRate > 0.f) || mf.sse.size() != mf.intra.size()) return;
    const IntraRule r = intraRule(mp.intraRate);
    for (size_t i = 0; i < mf.intra.size(); ++i)
        if (mf.sse[i] > r.k * mf.var[i] + r.bias) mf.intra[i] = 1;
}

inline MotionField estimateMotion(const Image& prev, const Image& cur, const MotionParams& mp,
                                  const ParallelFor& pf) {
    MotionField mf = newField(cur.w, cur.h, mp);
    const int levels = pyramidLevels(cur.w, cur.h);
    const std::vector<Luma> Pp = buildPyramid(prev, levels, pf);
    const std::vector<Luma> Pc = buildPyramid(cur, levels, pf);
    for (int lvl = levels - 1; lvl >= 0; --lvl) searchLevel(Pp[lvl], Pc[lvl], lvl, levels, mp, mf, pf);
    postProcessField(mf, mp.fillFlat ? flatBlocks(Pc[0], mf, mp, pf) : std::vector<uint8_t>(), mp, pf);
    if (needsRefine(mp)) {
        refineField(Pp[0], Pc[0], cur.w, cur.h, mp, mf, pf);
        finishIntra(mf, mp);
    }
    return mf;
}
struct StepParams {
    float motionScale = 1.f;   // multiply motion vectors
    float residual = 1.f;      // how much of the P-frame residual to add
    int repeat = 1;            // decode the same P-frame this many times (bloom)
    bool clampLevels = true;   // keep RGB within [0,1]
    bool intraBlocks = true;   // let hopeless blocks show the incoming picture
};

inline void bilinear(const Image& im, float x, float y, float* o) {
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float ax = x - (float)x0, ay = y - (float)y0;
    const float *a, *b, *c, *d;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < im.w && y0 + 1 < im.h) {
        a = im.at(x0, y0); b = a + 4; c = a + (size_t)im.w * 4; d = c + 4;
    } else {
        a = im.clampedAt(x0, y0); b = im.clampedAt(x0 + 1, y0);
        c = im.clampedAt(x0, y0 + 1); d = im.clampedAt(x0 + 1, y0 + 1);
    }
    for (int k = 0; k < 4; ++k) o[k] = (a[k] + ax * (b[k] - a[k])) * (1.f - ay) + (c[k] + ax * (d[k] - c[k])) * ay;
}

#if MOSH_SSE
inline __m128 bilinear4(const Image& im, float x, float y) {
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float ax = x - (float)x0, ay = y - (float)y0;
    const float *a, *b, *c, *d;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < im.w && y0 + 1 < im.h) {
        a = im.at(x0, y0); b = a + 4; c = a + (size_t)im.w * 4; d = c + 4;
    } else {
        a = im.clampedAt(x0, y0); b = im.clampedAt(x0 + 1, y0);
        c = im.clampedAt(x0, y0 + 1); d = im.clampedAt(x0 + 1, y0 + 1);
    }
    const __m128 va = _mm_loadu_ps(a), vb = _mm_loadu_ps(b), vc = _mm_loadu_ps(c), vd = _mm_loadu_ps(d);
    const __m128 vax = _mm_set1_ps(ax);
    const __m128 top = _mm_add_ps(va, _mm_mul_ps(vax, _mm_sub_ps(vb, va)));
    const __m128 bot = _mm_add_ps(vc, _mm_mul_ps(vax, _mm_sub_ps(vd, vc)));
    return _mm_add_ps(_mm_mul_ps(top, _mm_set1_ps(1.f - ay)), _mm_mul_ps(bot, _mm_set1_ps(ay)));
}
#endif

// One "P-frame decode" on the wrong reference:
//   accOut(x) = accIn(x + mv) + residual * (cur(x) - prev(x + mv))
inline void moshStepOnce(const Image& prev, const Image& cur, const MotionField& mf, const StepParams& sp,
                         const Image& accIn, Image& accOut, const ParallelFor& pf) {
    accOut.resize(cur.w, cur.h);
    pf(cur.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int by = std::min(y / mf.block, mf.bh - 1);
            for (int x = 0; x < cur.w; ++x) {
                const int bx = std::min(x / mf.block, mf.bw - 1);
                const int i = mf.index(bx, by);
                const float* c = cur.at(x, y);
                float* o = accOut.at(x, y);
                if (sp.intraBlocks && mf.intra[i]) {
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
                    continue;
                }
#if MOSH_SSE
                // Same operations in the same order as below, one channel per lane.
                {
                    __m128 va, vp;
                    if (mf.subpel()) {
                        const float fx = x + mf.fx[i] * sp.motionScale, fy = y + mf.fy[i] * sp.motionScale;
                        va = bilinear4(accIn, fx, fy);
                        vp = bilinear4(prev, fx, fy);
                    } else {
                        const int dx = (int)std::lround(mf.dx[i] * sp.motionScale);
                        const int dy = (int)std::lround(mf.dy[i] * sp.motionScale);
                        va = _mm_loadu_ps(accIn.clampedAt(x + dx, y + dy));
                        vp = _mm_loadu_ps(prev.clampedAt(x + dx, y + dy));
                    }
                    __m128 v = _mm_add_ps(va, _mm_mul_ps(_mm_set1_ps(sp.residual), _mm_sub_ps(_mm_loadu_ps(c), vp)));
                    // max(0, v) and min(1, v) in this operand order match std::max(v, 0.f) and
                    // std::min(v, 1.f), signed zeros and NaN included.
                    if (sp.clampLevels) v = _mm_min_ps(_mm_set1_ps(1.f), _mm_max_ps(_mm_setzero_ps(), v));
                    const __m128 rgb = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
                    _mm_storeu_ps(o, _mm_or_ps(_mm_and_ps(rgb, v), _mm_andnot_ps(rgb, va)));
                    continue;
                }
#endif
                float ia[4], ip[4];
                const float* a;
                const float* p;
                if (mf.subpel()) {
                    const float fx = x + mf.fx[i] * sp.motionScale, fy = y + mf.fy[i] * sp.motionScale;
                    bilinear(accIn, fx, fy, ia);
                    bilinear(prev, fx, fy, ip);
                    a = ia;
                    p = ip;
                } else {
                    const int dx = (int)std::lround(mf.dx[i] * sp.motionScale);
                    const int dy = (int)std::lround(mf.dy[i] * sp.motionScale);
                    a = accIn.clampedAt(x + dx, y + dy);
                    p = prev.clampedAt(x + dx, y + dy);
                }
                for (int ch = 0; ch < 3; ++ch) {
                    float v = a[ch] + sp.residual * (c[ch] - p[ch]);
                    if (sp.clampLevels) v = std::min(std::max(v, 0.f), 1.f);
                    o[ch] = v;
                }
                o[3] = a[3];
            }
        }
    });
}

// Applies the step sp.repeat times (duplicated P-frames). acc is updated in
// place; scratch is reused between calls.
inline void moshStep(const Image& prev, const Image& cur, const MotionField& mf, const StepParams& sp, Image& acc,
                     Image& scratch, const ParallelFor& pf) {
    const int n = std::max(1, sp.repeat);
    for (int k = 0; k < n; ++k) {
        moshStepOnce(prev, cur, mf, sp, acc, scratch, pf);
        std::swap(acc, scratch);
    }
}

enum class Recovery {
    Blocks = 0,
    Dissolve = 1,
    HardCut = 2,
    MotionRefresh = 3,
    StructureFirst = 4,
    LumaReveal = 5,
    ColourFirst = 6,
    Settle = 7,
    Wipe = 8,
    Slice = 9,
    Natural = 10,
};
static const int kRecoveryCount = 11;
enum class Curve { Linear = 0, Smooth = 1, EaseIn = 2, EaseOut = 3 };
enum class Direction { LeftToRight = 0, RightToLeft = 1, TopToBottom = 2, BottomToTop = 3, FromCentre = 4, ToCentre = 5 };
static const int kDirectionCount = 6;

inline float clamp01(float v) { return std::min(std::max(v, 0.f), 1.f); }
inline float smoothstep01(float v) {
    v = clamp01(v);
    return v * v * (3.f - 2.f * v);
}

inline float shapeCurve(float q, Curve c) {
    q = clamp01(q);
    switch (c) {
        case Curve::Smooth: return smoothstep01(q);
        case Curve::EaseIn: return q * q;
        case Curve::EaseOut: return 1.f - (1.f - q) * (1.f - q);
        default: return q;
    }
}

inline float blockHash(int bx, int by, int seed) {
    uint32_t h = (uint32_t)bx * 73856093u ^ (uint32_t)by * 19349663u ^ (uint32_t)seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return float(h & 0xffffffu) / 16777216.f;
}

// Separable box blur of all four channels, edges clamped.
inline void boxBlur(const Image& src, int r, Image& out, const ParallelFor& pf) {
    out.resize(src.w, src.h);
    if (r <= 0) {
        out.px = src.px;
        return;
    }
    Image tmp(src.w, src.h);
    const float inv = 1.f / float(2 * r + 1);
    pf(src.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float s[4] = {0.f, 0.f, 0.f, 0.f};
            for (int k = -r; k <= r; ++k) {
                const float* p = src.clampedAt(k, y);
                for (int c = 0; c < 4; ++c) s[c] += p[c];
            }
            for (int x = 0; x < src.w; ++x) {
                float* o = tmp.at(x, y);
                for (int c = 0; c < 4; ++c) o[c] = s[c] * inv;
                const float* add = src.clampedAt(x + r + 1, y);
                const float* sub = src.clampedAt(x - r, y);
                for (int c = 0; c < 4; ++c) s[c] += add[c] - sub[c];
            }
        }
    });
    pf(src.w, [&](int x0, int x1) {
        for (int x = x0; x < x1; ++x) {
            float s[4] = {0.f, 0.f, 0.f, 0.f};
            for (int k = -r; k <= r; ++k) {
                const float* p = tmp.clampedAt(x, k);
                for (int c = 0; c < 4; ++c) s[c] += p[c];
            }
            for (int y = 0; y < src.h; ++y) {
                float* o = out.at(x, y);
                for (int c = 0; c < 4; ++c) o[c] = s[c] * inv;
                const float* add = tmp.clampedAt(x, y + r + 1);
                const float* sub = tmp.clampedAt(x, y - r);
                for (int c = 0; c < 4; ++c) s[c] += add[c] - sub[c];
            }
        }
    });
}

// Natural recovery: the stream heals the way a real one does after a lost
// keyframe. fresh[] marks blocks that already carry data of the new shot
// (they were coded intra, or their content was dragged in from such a
// block). Each step:
//   - the encoder grows more eager to code blocks intra (intraRule),
//   - stale blocks next to fresh ones turn intra with a probability that
//     rises with q, so new-shot regions grow outwards like blobs,
//   - a few stale blocks anywhere are seeded at random (periodic intra
//     refresh), so static areas far from any motion heal too.
// q is the healing progress over the whole mosh. frame keeps the random
// choices stable across renders.
struct NaturalStep {
    float baseRate = 0.6f;  // New Data Rate
    float q = 0.f;
    float motionScale = 1.f;
    int seed = 0;
    long frame = 0;
};

inline void naturalIntra(MotionField& mf, const NaturalStep& ns, const std::vector<uint8_t>& fresh) {
    const size_t nb = mf.intra.size();
    const float q = clamp01(ns.q);
    if (q <= 0.f) return;
    if (q >= 1.f) {
        std::fill(mf.intra.begin(), mf.intra.end(), 1);
        return;
    }
    const IntraRule r = intraRule(ns.baseRate + (1.f - ns.baseRate) * q * q);
    const float grow = 0.4f * q * q;       // per fresh neighbour, per step
    const float seedP = 0.02f * q * q;     // random refresh of isolated blocks
    const bool stats = mf.sse.size() == nb && mf.var.size() == nb;
    const bool haveFresh = fresh.size() == nb;
    std::vector<uint8_t> out = mf.intra;
    for (int by = 0; by < mf.bh; ++by)
        for (int bx = 0; bx < mf.bw; ++bx) {
            const int i = mf.index(bx, by);
            if (out[i] || (haveFresh && fresh[i])) continue;
            if (stats && mf.sse[i] > r.k * mf.var[i] + r.bias) { out[i] = 1; continue; }
            int n = 0;
            if (haveFresh)
                for (int oy = -1; oy <= 1; ++oy)
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int qx = bx + ox, qy = by + oy;
                        if ((ox || oy) && qx >= 0 && qy >= 0 && qx < mf.bw && qy < mf.bh && fresh[mf.index(qx, qy)])
                            ++n;
                    }
            const float p = 1.f - std::pow(1.f - grow, (float)n) * (1.f - seedP);
            if (blockHash(bx, by, ns.seed * 7919 + (int)ns.frame) < p) out[i] = 1;
        }
    mf.intra.swap(out);
}

// After the step: carry the fresh marks along the motion, and fade in what
// is still stale over the last stretch so the end of the mosh does not pop.
inline void naturalAfter(const Image& cur, const MotionField& mf, const NaturalStep& ns, std::vector<uint8_t>& fresh,
                         Image& acc, const ParallelFor& pf) {
    const size_t nb = mf.intra.size();
    if (fresh.size() != nb) fresh.assign(nb, 0);
    std::vector<uint8_t> next(nb, 0);
    for (int by = 0; by < mf.bh; ++by)
        for (int bx = 0; bx < mf.bw; ++bx) {
            const int i = mf.index(bx, by);
            if (mf.intra[i]) { next[i] = 1; continue; }
            const float vx = (mf.subpel() ? mf.fx[i] : (float)mf.dx[i]) * ns.motionScale;
            const float vy = (mf.subpel() ? mf.fy[i] : (float)mf.dy[i]) * ns.motionScale;
            const int sx = (int)std::floor(((float)bx + 0.5f) + vx / (float)mf.block);
            const int sy = (int)std::floor(((float)by + 0.5f) + vy / (float)mf.block);
            if (sx >= 0 && sy >= 0 && sx < mf.bw && sy < mf.bh) next[i] = fresh[mf.index(sx, sy)];
        }
    fresh.swap(next);

    const float tail = smoothstep01((ns.q - 0.85f) / 0.15f);
    if (tail <= 0.f) return;
    pf(cur.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int by = std::min(y / mf.block, mf.bh - 1);
            for (int x = 0; x < cur.w; ++x) {
                if (fresh[mf.index(std::min(x / mf.block, mf.bw - 1), by)]) continue;
                float* a = acc.at(x, y);
                const float* c = cur.at(x, y);
                for (int ch = 0; ch < 4; ++ch) a[ch] += tail * (c[ch] - a[ch]);
            }
        }
    });
}
// Motion refresh: the stream "recovers" the way an intra-refresh encoder
// does, inside the decode loop instead of as a composite on top of it. Blocks
// where the new shot changes the most are refreshed first; refreshed blocks
// are pulled towards the real picture every step but still get decoded as
// P-blocks, so the smear around them keeps flowing in until it is washed out.
struct RefreshStep {
    float q = 0.f;          // recovery progress at this step, 0..1
    float softness = 0.5f;  // 0 = refreshed blocks snap to the real picture
    float jitter = 0.15f;   // share of randomness in the refresh order
    int seed = 0;
};

inline void refreshStep(const Image& prev, const Image& cur, const MotionField& mf, const RefreshStep& rs,
                        std::vector<uint8_t>& refreshed, Image& acc, const ParallelFor& pf) {
    const size_t nb = (size_t)mf.bw * mf.bh;
    if (refreshed.size() != nb) refreshed.assign(nb, 0);
    if (rs.q <= 0.f || nb == 0) return;

    std::vector<float> activity(nb, 0.f);
    pf(mf.bh, [&](int b0, int b1) {
        for (int by = b0; by < b1; ++by)
            for (int bx = 0; bx < mf.bw; ++bx) {
                float s = 0.f;
                int n = 0;
                for (int y = by * mf.block; y < std::min((by + 1) * mf.block, cur.h); y += 2)
                    for (int x = bx * mf.block; x < std::min((bx + 1) * mf.block, cur.w); x += 2) {
                        const float* c = cur.at(x, y);
                        const float* p = prev.at(x, y);
                        s += std::fabs(c[0] - p[0]) + std::fabs(c[1] - p[1]) + std::fabs(c[2] - p[2]);
                        ++n;
                    }
                activity[mf.index(bx, by)] = n ? s / float(n) : 0.f;
            }
    });
    // Spread activity over neighbouring blocks so whole regions refresh
    // together instead of a scatter of single blocks.
    std::vector<float> spread(nb, 0.f);
    for (int by = 0; by < mf.bh; ++by)
        for (int bx = 0; bx < mf.bw; ++bx) {
            float s = 0.f, wsum = 0.f;
            for (int oy = -2; oy <= 2; ++oy)
                for (int ox = -2; ox <= 2; ++ox) {
                    const int qx = bx + ox, qy = by + oy;
                    if (qx < 0 || qy < 0 || qx >= mf.bw || qy >= mf.bh) continue;
                    const float w = (ox == 0 && oy == 0) ? 2.f : 1.f / float(std::abs(ox) + std::abs(oy));
                    s += w * activity[mf.index(qx, qy)];
                    wsum += w;
                }
            spread[mf.index(bx, by)] = s / wsum;
        }
    std::vector<int> order(nb);
    for (size_t i = 0; i < nb; ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return spread[a] < spread[b]; });
    const float denom = nb > 1 ? float(nb - 1) : 1.f;
    for (size_t r = 0; r < nb; ++r) {
        const int i = order[r];
        const float score = (1.f - rs.jitter) * (float(r) / denom) +
                            rs.jitter * blockHash(i % mf.bw, i / mf.bw, rs.seed + 101);
        if (rs.q >= 1.f || score > 1.f - rs.q) refreshed[i] = 1;
    }

    // Refreshed blocks converge faster towards the end so nothing is left to pop.
    const float base = 1.f - 0.85f * clamp01(rs.softness);
    const float rate = rs.q >= 1.f ? 1.f : base + (1.f - base) * rs.q * rs.q;
    // Blocks still waiting for their refresh fade in over the last stretch, so
    // the final frame has nothing left to pop.
    const float tail = smoothstep01((rs.q - 0.8f) / 0.2f);
    pf(cur.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int by = std::min(y / mf.block, mf.bh - 1);
            for (int x = 0; x < cur.w; ++x) {
                const float k = refreshed[mf.index(std::min(x / mf.block, mf.bw - 1), by)] ? rate : tail;
                if (k <= 0.f) continue;
                float* a = acc.at(x, y);
                const float* c = cur.at(x, y);
                for (int ch = 0; ch < 4; ++ch) a[ch] += k * (c[ch] - a[ch]);
            }
        }
    });
}

inline float refreshedShare(const std::vector<uint8_t>& refreshed) {
    if (refreshed.empty()) return 0.f;
    size_t n = 0;
    for (uint8_t r : refreshed) n += r;
    return float(n) / float(refreshed.size());
}

struct ComposeParams {
    Recovery mode = Recovery::Blocks;
    float softness = 0.5f;      // width of the soft edge of the reveal, 0..1
    int block = 16;
    int seed = 0;
    bool invertReveal = false;  // Luma reveal: dark areas first
    Direction direction = Direction::LeftToRight;  // Wipe, Slice
};

// Position of (x,y) along a wipe in direction d, 0 = reached first.
inline float wipeCoord(Direction d, int x, int y, int w, int h) {
    const float u = (float(x) + 0.5f) / float(std::max(1, w));
    const float v = (float(y) + 0.5f) / float(std::max(1, h));
    switch (d) {
        case Direction::RightToLeft: return 1.f - u;
        case Direction::TopToBottom: return v;
        case Direction::BottomToTop: return 1.f - v;
        case Direction::FromCentre:
        case Direction::ToCentre: {
            const float ax = (u - 0.5f) * float(w), ay = (v - 0.5f) * float(h);
            const float r = std::sqrt(ax * ax + ay * ay) / (0.5f * std::sqrt(float(w) * w + float(h) * h));
            return d == Direction::FromCentre ? clamp01(r) : 1.f - clamp01(r);
        }
        default: return u;
    }
}

// Slice: the frame is cut into bands of 1-4 macroblocks (rows for
// horizontal directions, columns for vertical ones); every band slides in
// on its own schedule.
struct SliceLayout {
    bool columns = false;
    std::vector<int> band;      // band index per row (or column) of pixels
    std::vector<float> start;   // start time per band
    std::vector<int> sign;      // +1 slides towards +x/+y, -1 the other way
    float dur = 0.5f;
};

inline SliceLayout makeSlices(const ComposeParams& cp, int w, int h, float s) {
    SliceLayout L;
    L.columns = cp.direction == Direction::TopToBottom || cp.direction == Direction::BottomToTop;
    const int n = L.columns ? w : h;
    const int block = std::max(1, cp.block);
    L.band.assign(n, 0);
    L.dur = 0.15f + 0.6f * s;
    int b = 0, pos = 0;
    while (pos < n) {
        const int len = block * (1 + int(blockHash(b, 7, cp.seed + 31) * 4.f));
        for (int i = pos; i < std::min(n, pos + len); ++i) L.band[i] = b;
        L.start.push_back(blockHash(b, 3, cp.seed + 17) * (1.f - L.dur));
        int sg = 1;
        switch (cp.direction) {
            case Direction::RightToLeft:
            case Direction::BottomToTop: sg = -1; break;
            case Direction::FromCentre:
            case Direction::ToCentre: sg = (b & 1) ? -1 : 1; break;
            default: break;
        }
        L.sign.push_back(sg);
        pos += len;
        ++b;
    }
    return L;
}

// Bring the real incoming picture back over the moshed one. q in [0,1] is
// recovery progress (already shaped by the recovery curve).
//   Blocks:         macroblocks come back in random order, each fading in
//                   over a window set by softness (0 = they pop).
//   Dissolve:       crossfade.
//   HardCut:        the real picture as soon as recovery starts.
//   MotionRefresh,
//   Natural:        done inside the mosh chain (refreshStep, naturalIntra);
//                   here only the final frame snaps to the real picture.
//   StructureFirst: the edges and texture of the new shot appear first,
//                   painted with the smeared colours; the colours follow.
//   LumaReveal:     the new shot shows through its bright areas first.
//   ColourFirst:    the colours of the new shot wash in first under the
//                   smeared detail; the detail follows.
//   Settle:         areas where the mosh is already close to the new shot
//                   settle first, the worst mismatches last.
//   Wipe:           a wipe with a ragged, macroblock-sized edge.
//   Slice:          bands of macroblocks slide the new shot in and push the
//                   mosh out, each band on its own schedule.
inline void compose(const Image& acc, const Image& to, const ComposeParams& cp, float q, Image& out,
                    const ParallelFor& pf) {
    out.resize(to.w, to.h);
    q = clamp01(q);
    const float s = clamp01(cp.softness);
    const int block = std::max(1, cp.block);

    if (cp.mode == Recovery::StructureFirst || cp.mode == Recovery::ColourFirst) {
        const bool colourFirst = cp.mode == Recovery::ColourFirst;
        // A wider blur for Colour first so the colours arrive as a wash
        // rather than as a blurry copy of the new shot.
        const int r = colourFirst ? std::max(6, 2 * block) : std::max(3, block);
        Image lowA, lowT;
        boxBlur(acc, r, lowA, pf);
        boxBlur(to, r, lowT, pf);
        const float lag = 0.1f + 0.5f * s;  // the second layer starts when the first is this far in
        const float kFirst = smoothstep01(q / (1.f - lag));
        const float kSecond = smoothstep01((q - lag) / (1.f - lag));
        const float kd = colourFirst ? kSecond : kFirst;  // detail
        const float kc = colourFirst ? kFirst : kSecond;  // colour
        pf(to.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < to.w; ++x) {
                    const float* a = acc.at(x, y);
                    const float* t = to.at(x, y);
                    const float* la = lowA.at(x, y);
                    const float* lt = lowT.at(x, y);
                    float* o = out.at(x, y);
                    for (int ch = 0; ch < 4; ++ch) {
                        const float low = la[ch] + kc * (lt[ch] - la[ch]);
                        const float high = (a[ch] - la[ch]) + kd * ((t[ch] - lt[ch]) - (a[ch] - la[ch]));
                        o[ch] = low + high;
                    }
                }
        });
        return;
    }

    if (cp.mode == Recovery::Slice) {
        const SliceLayout L = makeSlices(cp, to.w, to.h, s);
        pf(to.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < to.w; ++x) {
                    const int b = L.band[L.columns ? x : y];
                    const int n = L.columns ? to.h : to.w;
                    const int pos = L.columns ? y : x;
                    const float lp = q >= 1.f ? 1.f : smoothstep01((q - L.start[b]) / L.dur);
                    const int e = (int)std::lround(lp * float(n));  // how far the new shot is in
                    // Position in the band's own frame, measured from the edge it enters at.
                    const int p = L.sign[b] > 0 ? pos : n - 1 - pos;
                    const bool in = p < e;
                    const int shift = in ? (n - e) : -e;  // new shot trails the edge, mosh is pushed ahead
                    const int sp = pos + (L.sign[b] > 0 ? shift : -shift);
                    const float* c = in ? (L.columns ? to.clampedAt(x, sp) : to.clampedAt(sp, y))
                                        : (L.columns ? acc.clampedAt(x, sp) : acc.clampedAt(sp, y));
                    float* o = out.at(x, y);
                    for (int ch = 0; ch < 4; ++ch) o[ch] = c[ch];
                }
        });
        return;
    }

    Image key;
    float keyNorm = 1.f;
    if (cp.mode == Recovery::LumaReveal) boxBlur(to, std::max(2, block / 2), key, pf);
    if (cp.mode == Recovery::Settle) {
        Image diff(to.w, to.h);
        pf(to.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < to.w; ++x) {
                    const float* a = acc.at(x, y);
                    const float* t = to.at(x, y);
                    diff.at(x, y)[0] =
                        (std::fabs(a[0] - t[0]) + std::fabs(a[1] - t[1]) + std::fabs(a[2] - t[2])) / 3.f;
                }
        });
        boxBlur(diff, std::max(2, block / 2), key, pf);
        // Normalise by a high percentile so a few extreme pixels do not
        // squash everything else into "already settled".
        std::vector<float> sample;
        sample.reserve(key.px.size() / 16 + 1);
        for (size_t i = 0; i < key.px.size(); i += 16) sample.push_back(key.px[i]);
        if (!sample.empty()) {
            const size_t k = sample.size() * 95 / 100;
            std::nth_element(sample.begin(), sample.begin() + k, sample.end());
            keyNorm = sample[k] > 1e-4f ? 1.f / sample[k] : 1.f;
        }
    }

    const float bw = 0.6f * s;           // Blocks: fade window per block
    const float fw = 0.05f + 0.6f * s;   // LumaReveal, Settle: soft edge width
    const float ww = 0.02f + 0.4f * s;   // Wipe: soft edge width
    const float jag = 0.08f;             // Wipe: per-block raggedness of the edge
    pf(to.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < to.w; ++x) {
                const float* a = acc.clampedAt(x, y);
                const float* t = to.at(x, y);
                float* o = out.at(x, y);
                float k;  // weight of the incoming picture
                switch (cp.mode) {
                    case Recovery::Blocks: {
                        const float h = blockHash(x / block, y / block, cp.seed);
                        if (q >= 1.f) k = 1.f;
                        else if (bw <= 0.f) k = (q > 0.f && h < q) ? 1.f : 0.f;
                        else k = clamp01((q * (1.f + bw) - h) / bw);
                        break;
                    }
                    case Recovery::Dissolve: k = q; break;
                    case Recovery::MotionRefresh:
                    case Recovery::Natural: k = q >= 1.f ? 1.f : 0.f; break;
                    case Recovery::LumaReveal: {
                        const float* kp = key.at(x, y);
                        float l = clamp01(0.299f * kp[0] + 0.587f * kp[1] + 0.114f * kp[2]);
                        if (cp.invertReveal) l = 1.f - l;
                        k = smoothstep01((q * (1.f + fw) - (1.f - l)) / fw);
                        break;
                    }
                    case Recovery::Settle: {
                        const float d = clamp01(key.at(x, y)[0] * keyNorm);
                        k = smoothstep01((q * (1.f + fw) - d) / fw);
                        break;
                    }
                    case Recovery::Wipe: {
                        const float u = wipeCoord(cp.direction, x, y, to.w, to.h);
                        const float h = blockHash(x / block, y / block, cp.seed + 59);
                        const float pos = (u + jag * h) / (1.f + jag);
                        k = smoothstep01((q * (1.f + ww) - pos) / ww);
                        break;
                    }
                    default: k = q > 0.f ? 1.f : 0.f; break;
                }
                for (int ch = 0; ch < 4; ++ch) o[ch] = a[ch] + k * (t[ch] - a[ch]);
            }
        }
    });
}// Debug view: direction as colour (x -> red, y -> green), intra blocks white.
inline void visualizeMotion(const MotionField& mf, int range, int w, int h, Image& out, const ParallelFor& pf) {
    out.resize(w, h);
    const float s = 0.5f / float(std::max(1, range));
    pf(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int by = std::min(y / mf.block, mf.bh - 1);
            for (int x = 0; x < w; ++x) {
                const int bx = std::min(x / mf.block, mf.bw - 1);
                const int i = mf.index(bx, by);
                float* o = out.at(x, y);
                if (mf.intra[i]) { o[0] = o[1] = o[2] = 1.f; }
                else {
                    o[0] = std::min(std::max(0.5f + mf.dx[i] * s, 0.f), 1.f);
                    o[1] = std::min(std::max(0.5f + mf.dy[i] * s, 0.f), 1.f);
                    o[2] = 0.5f;
                }
                o[3] = 1.f;
            }
        }
    });
}

}  // namespace mosh
