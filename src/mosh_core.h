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
};

// Hierarchical block matching: full search at the coarsest pyramid level,
// then +-2 steps of refinement at every finer level down to full resolution.
// Each block is matched on a window of at least 4x4 samples at every level, so
// small blocks still get a meaningful match at coarse levels.
inline MotionField estimateMotion(const Image& prev, const Image& cur, const MotionParams& mp,
                                  const ParallelFor& pf) {
    MotionField mf;
    mf.block = std::max(4, mp.block);
    mf.bw = (cur.w + mf.block - 1) / mf.block;
    mf.bh = (cur.h + mf.block - 1) / mf.block;
    const size_t nb = (size_t)mf.bw * mf.bh;
    mf.dx.assign(nb, 0);
    mf.dy.assign(nb, 0);
    mf.err.assign(nb, 0.f);
    mf.intra.assign(nb, 0);

    int levels = 1;
    while (levels < 4 && (std::min(cur.w, cur.h) >> levels) >= 32) ++levels;
    const std::vector<Luma> Pp = buildPyramid(prev, levels, pf);
    const std::vector<Luma> Pc = buildPyramid(cur, levels, pf);

    const float lambda = 0.0008f;  // per px of vector length, in mean-abs-diff units

    for (int lvl = levels - 1; lvl >= 0; --lvl) {
        const int d = 1 << lvl;
        const Luma& Lp = Pp[lvl];
        const Luma& Lc = Pc[lvl];
        const int ws = std::max(4, mf.block / d);  // match window at this level
        const int half = ws / 2;
        const bool coarsest = (lvl == levels - 1);
        const int r = coarsest ? std::max(2, (mp.range + d - 1) / d) : 2;
        const float norm = 1.f / float(ws * ws);

        pf(mf.bh, [&](int b0, int b1) {
            for (int by = b0; by < b1; ++by) {
                for (int bx = 0; bx < mf.bw; ++bx) {
                    const int i = mf.index(bx, by);
                    // window centre in this level's coordinates
                    const int cx = ((bx * mf.block + mf.block / 2) / d) - half;
                    const int cy = ((by * mf.block + mf.block / 2) / d) - half;
                    const int pdx = mf.dx[i] / d, pdy = mf.dy[i] / d;  // parent vector at this level

                    auto mad = [&](int vx, int vy) {
                        float s = 0.f;
                        for (int yy = 0; yy < ws; ++yy)
                            for (int xx = 0; xx < ws; ++xx)
                                s += std::fabs(Lc.at(cx + xx, cy + yy) - Lp.at(cx + xx + vx, cy + yy + vy));
                        return s * norm;
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
                    if (bm > 0.004f) {
                        for (int vy = pdy - r; vy <= pdy + r; ++vy) {
                            for (int vx = pdx - r; vx <= pdx + r; ++vx) {
                                if (vx == pdx && vy == pdy) continue;
                                const float m = mad(vx, vy);
                                const float c = cost(vx, vy, m);
                                if (c < bc) { bc = c; bm = m; bvx = vx; bvy = vy; }
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

    if (mp.fillFlat) {
        const Luma& L0 = Pc[0];
        std::vector<uint8_t> flat(nb, 0);
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
    return mf;
}

struct StepParams {
    float motionScale = 1.f;   // multiply motion vectors
    float residual = 1.f;      // how much of the P-frame residual to add
    int repeat = 1;            // decode the same P-frame this many times (bloom)
    bool clampLevels = true;   // keep RGB within [0,1]
    bool intraBlocks = true;   // let hopeless blocks show the incoming picture
};

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
                const int dx = (int)std::lround(mf.dx[i] * sp.motionScale);
                const int dy = (int)std::lround(mf.dy[i] * sp.motionScale);
                const float* a = accIn.clampedAt(x + dx, y + dy);
                const float* p = prev.clampedAt(x + dx, y + dy);
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

enum class Recovery { Blocks = 0, Dissolve = 1, HardCut = 2 };

inline float blockHash(int bx, int by, int seed) {
    uint32_t h = (uint32_t)bx * 73856093u ^ (uint32_t)by * 19349663u ^ (uint32_t)seed * 83492791u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return float(h & 0xffffffu) / 16777216.f;
}

// Bring the real incoming picture back. q in [0,1] is recovery progress.
// Blocks: macroblocks pop back one by one, like an intra-refresh stream.
inline void compose(const Image& acc, const Image& to, Recovery mode, float q, int block, int seed,
                    Image& out, const ParallelFor& pf) {
    out.resize(to.w, to.h);
    q = std::min(std::max(q, 0.f), 1.f);
    pf(to.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < to.w; ++x) {
                const float* a = acc.clampedAt(x, y);
                const float* t = to.at(x, y);
                float* o = out.at(x, y);
                float k;  // weight of the incoming picture
                switch (mode) {
                    case Recovery::Blocks:
                        k = (q >= 1.f || (q > 0.f && blockHash(x / block, y / block, seed) < q)) ? 1.f : 0.f;
                        break;
                    case Recovery::Dissolve:
                        k = q;
                        break;
                    default:
                        k = q > 0.f ? 1.f : 0.f;
                        break;
                }
                for (int ch = 0; ch < 4; ++ch) o[ch] = a[ch] + k * (t[ch] - a[ch]);
            }
        }
    });
}

// Debug view: direction as colour (x -> red, y -> green), intra blocks white.
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
