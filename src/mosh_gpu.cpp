// mosh_gpu.cpp - GPU session, device caches and the GPU versions of the stages.
#include "mosh_gpu.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include "gpu_backend.h"

namespace mosh_gpu {

using gpu::Arg;
using gpu::Buffer;

namespace {

// A source frame in device memory, with its luma pyramid once motion
// estimation asked for it. Keyed by the CPU image it was uploaded from.
struct DevFrame {
    const mosh::Image* key = nullptr;
    std::weak_ptr<const mosh::Image> alive;
    int w = 0, h = 0;
    Buffer rgba;
    std::vector<Buffer> luma;
    std::vector<int> lw, lh;
    uint64_t used = 0;
};

struct DevAcc {
    const void* owner = nullptr;
    double t = 0, cut = 0;
    uint64_t hash = 0;
    int w = 0, h = 0;
    Buffer buf;
    std::vector<uint8_t> refreshed;
    uint64_t used = 0;
};

const size_t kMaxFrames = 3;   // a step needs two; one more keeps the next render's previous frame
const size_t kMaxAccs = 2;
const size_t kMaxPool = 6;
const int kMaxFailures = 3;    // consecutive failed renders before the GPU is given up

}  // namespace

struct Engine {
    std::mutex m;
    std::unique_ptr<gpu::Device> dev;
    bool devAnyType = false;
    std::string triedCuda, triedCl, triedClAny;  // failure reasons, "" = not tried or worked
    bool cudaTried = false, clTried = false, clAnyTried = false;
    std::string broken;  // set once the GPU has been given up
    int failures = 0;
    uint64_t clock = 0;

    std::vector<std::unique_ptr<DevFrame>> frames;
    std::vector<DevAcc> accs;
    std::vector<Buffer> pool;  // free full-frame buffers
    // Block-level scratch, grown as needed.
    Buffer dx, dy, err, flat, rowCost, rowM, rowVx, refine, table, flags, sums, counts, band, start, sign, sample;
    // The session's accumulated picture and its ping-pong partner.
    Buffer acc, accTmp;
    int accW = 0, accH = 0;

    void free(Buffer& b) {
        if (b && dev) dev->release(b);
        b = Buffer();
    }
    // A full-frame buffer from the pool, or a new one.
    bool get(Buffer& b, size_t bytes) {
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].bytes == bytes) {
                b = pool[i];
                pool.erase(pool.begin() + (long)i);
                return true;
            }
        return dev->alloc(b, bytes);
    }
    void put(Buffer& b) {
        if (!b) return;
        if (pool.size() < kMaxPool) pool.push_back(b);
        else dev->release(b);
        b = Buffer();
    }
    bool ensure(Buffer& b, size_t bytes) {
        if (b && b.bytes >= bytes) return true;
        free(b);
        return dev->alloc(b, bytes);
    }
    void dropFrame(DevFrame& f) {
        put(f.rgba);
        for (Buffer& l : f.luma) free(l);
        f.luma.clear();
    }
    void purge() {
        for (auto& f : frames) dropFrame(*f);
        frames.clear();
        for (DevAcc& a : accs) put(a.buf);
        accs.clear();
        put(acc);
        put(accTmp);
        for (Buffer& b : pool) free(b);
        pool.clear();
        for (Buffer* b : {&dx, &dy, &err, &flat, &rowCost, &rowM, &rowVx, &refine, &table, &flags, &sums, &counts,
                          &band, &start, &sign, &sample})
            free(*b);
    }
    void close() {
        purge();
        dev.reset();
    }

    // The device copy of img, uploaded on first use.
    DevFrame* frame(const FramePtr& img) {
        for (size_t i = 0; i < frames.size();) {
            if (frames[i]->alive.expired()) {
                dropFrame(*frames[i]);
                frames.erase(frames.begin() + (long)i);
            } else {
                ++i;
            }
        }
        for (auto& f : frames)
            if (f->key == img.get()) {
                f->used = ++clock;
                return f.get();
            }
        while (frames.size() >= kMaxFrames) {
            auto lru = std::min_element(frames.begin(), frames.end(),
                                        [](const std::unique_ptr<DevFrame>& a, const std::unique_ptr<DevFrame>& b) {
                                            return a->used < b->used;
                                        });
            dropFrame(**lru);
            frames.erase(lru);
        }
        std::unique_ptr<DevFrame> f(new DevFrame);
        f->key = img.get();
        f->alive = img;
        f->w = img->w;
        f->h = img->h;
        const size_t bytes = img->px.size() * sizeof(float);
        if (!get(f->rgba, bytes) || !dev->upload(f->rgba, img->px.data(), bytes)) {
            dropFrame(*f);
            return nullptr;
        }
        f->used = ++clock;
        frames.push_back(std::move(f));
        return frames.back().get();
    }

    bool pyramid(DevFrame& f, int levels) {
        if ((int)f.luma.size() >= levels) return true;
        if (f.luma.empty()) {
            Buffer b;
            if (!dev->alloc(b, (size_t)f.w * f.h * sizeof(float))) return false;
            f.luma.push_back(b);
            f.lw.push_back(f.w);
            f.lh.push_back(f.h);
            if (!dev->run(gpu::kLuma, f.w * f.h, {f.rgba, b})) return false;
        }
        while ((int)f.luma.size() < levels) {
            const int sw = f.lw.back(), sh = f.lh.back();
            const int w = std::max(1, sw / 2), h = std::max(1, sh / 2);
            Buffer b;
            if (!dev->alloc(b, (size_t)w * h * sizeof(float))) return false;
            f.luma.push_back(b);
            f.lw.push_back(w);
            f.lh.push_back(h);
            if (!dev->run(gpu::kHalve, w * h, {f.luma[f.luma.size() - 2], sw, sh, b, w})) return false;
        }
        return true;
    }
};

static Engine& engine() {
    // Never destroyed: tearing down a driver context while the process exits is not safe.
    static Engine* e = new Engine;
    return *e;
}

// ---------------------------------------------------------------- session

Session::Session(Engine& e) : e_(e), lock_(e.m, std::defer_lock) {}

Session::~Session() {
    if (!lock_.owns_lock()) return;
    Engine& e = e_;
    if (failed_) {
        e.purge();
        if (++e.failures >= kMaxFailures) {
            e.broken = "GPU off after errors: " + error_;
            e.close();
        }
    } else {
        e.failures = 0;
        e.put(e.acc);
        e.put(e.accTmp);
    }
}

std::string Session::label() const {
    if (!e_.dev) return std::string();
    return std::string(e_.dev->api() == gpu::Api::Cuda ? "CUDA " : "OPENCL ") + e_.dev->name();
}

bool Session::fail(const std::string& what) {
    if (!failed_) {
        failed_ = true;
        error_ = what;
    }
    return false;
}

bool Session::ok(bool r) {
    if (failed_) return false;
    if (!r) return fail(e_.dev ? e_.dev->error() : std::string("no device"));
    return true;
}

bool Session::setAcc(const mosh::Image& img) {
    if (failed_) return false;
    Engine& e = e_;
    const size_t bytes = img.px.size() * sizeof(float);
    if (e.acc && e.acc.bytes != bytes) e.put(e.acc);
    if (!e.acc && !ok(e.get(e.acc, bytes))) return false;
    e.accW = img.w;
    e.accH = img.h;
    return ok(e.dev->upload(e.acc, img.px.data(), bytes));
}

bool Session::restoreAcc(const void* owner, double t, double cut, uint64_t hash, std::vector<uint8_t>& refreshed) {
    if (failed_) return false;
    Engine& e = e_;
    for (size_t i = 0; i < e.accs.size(); ++i) {
        DevAcc& a = e.accs[i];
        if (a.owner != owner || a.t != t || a.cut != cut || a.hash != hash) continue;
        // The cache entry becomes the working picture; the next store puts it back.
        e.put(e.acc);
        e.acc = a.buf;
        e.accW = a.w;
        e.accH = a.h;
        refreshed = a.refreshed;
        e.accs.erase(e.accs.begin() + (long)i);
        return true;
    }
    return false;
}

double Session::latestAcc(const void* owner, double t, double cut, uint64_t hash) const {
    double best = -HUGE_VAL;
    if (failed_) return best;
    for (const DevAcc& a : e_.accs) {
        const double d = t - a.t;
        if (a.owner == owner && a.cut == cut && a.hash == hash && a.t >= cut && d > 0.5 &&
            std::fabs(d - std::round(d)) < 1e-6 && a.t > best)
            best = a.t;
    }
    return best;
}

bool Session::storeAcc(const void* owner, double t, double cut, uint64_t hash, const std::vector<uint8_t>& refreshed) {
    if (failed_ || !e_.acc) return false;
    Engine& e = e_;
    for (size_t i = 0; i < e.accs.size(); ++i)
        if (e.accs[i].owner == owner && e.accs[i].t == t && e.accs[i].cut == cut && e.accs[i].hash == hash) {
            e.put(e.accs[i].buf);
            e.accs.erase(e.accs.begin() + (long)i);
            break;
        }
    // At most kMaxAccs entries per instance and twice that overall; the oldest go first.
    auto evictOldest = [&](bool sameOwner) {
        auto victim = e.accs.end();
        for (auto it = e.accs.begin(); it != e.accs.end(); ++it)
            if ((!sameOwner || it->owner == owner) && (victim == e.accs.end() || it->used < victim->used)) victim = it;
        if (victim == e.accs.end()) return false;
        e.put(victim->buf);
        e.accs.erase(victim);
        return true;
    };
    while (std::count_if(e.accs.begin(), e.accs.end(), [&](const DevAcc& a) { return a.owner == owner; }) >=
               (long)kMaxAccs &&
           evictOldest(true)) {
    }
    while (e.accs.size() >= kMaxAccs * 2 && evictOldest(false)) {
    }
    DevAcc a;
    a.owner = owner;
    a.t = t;
    a.cut = cut;
    a.hash = hash;
    a.w = e.accW;
    a.h = e.accH;
    a.buf = e.acc;
    a.refreshed = refreshed;
    a.used = ++e.clock;
    e.acc = Buffer();
    e.accs.push_back(std::move(a));
    return true;
}

bool Session::downloadAcc(mosh::Image& out) {
    if (failed_ || !e_.acc) return fail("no picture");
    out.resize(e_.accW, e_.accH);
    return ok(e_.dev->download(e_.acc, out.px.data(), out.px.size() * sizeof(float)));
}

bool Session::estimateMotion(const FramePtr& prev, const FramePtr& cur, const mosh::MotionParams& mp,
                             mosh::MotionField& mf, const mosh::ParallelFor& pf) {
    if (failed_) return false;
    Engine& e = e_;
    gpu::Device& d = *e.dev;
    DevFrame* P = e.frame(prev);
    if (!ok(P != nullptr)) return false;
    DevFrame* C = e.frame(cur);
    if (!ok(C != nullptr)) return false;
    P = e.frame(prev);  // still cached: the cache holds more frames than a step uses
    if (!P || P->w != C->w || P->h != C->h) return fail("frame sizes differ");
    const int w = C->w, h = C->h;

    mf = mosh::newField(w, h, mp);
    const int levels = mosh::pyramidLevels(w, h);
    if (!ok(e.pyramid(*P, levels)) || !ok(e.pyramid(*C, levels))) return false;
    const int nb = mf.bw * mf.bh;
    const size_t nbI = (size_t)nb * sizeof(int);
    if (!ok(e.ensure(e.dx, nbI)) || !ok(e.ensure(e.dy, nbI)) || !ok(e.ensure(e.err, nbI))) return false;
    if (!ok(d.upload(e.dx, mf.dx.data(), nbI)) || !ok(d.upload(e.dy, mf.dy.data(), nbI))) return false;

    for (int lvl = levels - 1; lvl >= 0; --lvl) {
        const int dd = 1 << lvl;
        const int ws = mosh::searchWindow(mf, lvl);
        const int r = mosh::searchRadius(mp, lvl, levels);
        const float norm = 1.f / float(ws * ws);
        const int rows = 2 * r + 1;
        if ((long long)nb * rows > INT_MAX) return fail("search too large");
        const size_t nr = (size_t)nb * rows * sizeof(float);
        if (!ok(e.ensure(e.rowCost, nr)) || !ok(e.ensure(e.rowM, nr)) || !ok(e.ensure(e.rowVx, nr))) return false;
        const Buffer &Lp = P->luma[lvl], &Lc = C->luma[lvl];
        const int pw = P->lw[lvl], ph = P->lh[lvl], cw = C->lw[lvl], chh = C->lh[lvl];
        if (!ok(d.run(gpu::kSearchRows, nb * rows,
                      {Lp, pw, ph, Lc, cw, chh, e.dx, e.dy, mf.bw, mf.block, dd, ws, r, norm, mosh::kMotionLambda,
                       e.rowCost, e.rowM, e.rowVx})))
            return false;
        if (!ok(d.run(gpu::kSearchPick, nb,
                      {Lp, pw, ph, Lc, cw, chh, e.dx, e.dy, mf.bw, mf.block, dd, ws, r, norm, mosh::kMotionLambda,
                       e.rowCost, e.rowM, e.rowVx, e.err})))
            return false;
    }
    std::vector<int> flatI;
    if (mp.fillFlat) {
        if (!ok(e.ensure(e.flat, nbI))) return false;
        if (!ok(d.run(gpu::kFlat, nb, {C->luma[0], w, h, mf.bw, mf.block, mp.flatThreshold, e.flat}))) return false;
        flatI.resize(nb);
    }
    if (!ok(d.download(e.dx, mf.dx.data(), nbI)) || !ok(d.download(e.dy, mf.dy.data(), nbI)) ||
        !ok(d.download(e.err, mf.err.data(), nbI)))
        return false;
    std::vector<uint8_t> flat;
    if (mp.fillFlat) {
        if (!ok(d.download(e.flat, flatI.data(), nbI))) return false;
        flat.assign(flatI.begin(), flatI.end());
    }
    mosh::postProcessField(mf, flat, mp, pf);

    if (mosh::needsRefine(mp)) {
        const size_t nOut = (size_t)nb * 11;
        if (!ok(e.ensure(e.refine, nOut * sizeof(float)))) return false;
        if (!ok(d.upload(e.dx, mf.dx.data(), nbI)) || !ok(d.upload(e.dy, mf.dy.data(), nbI))) return false;
        if ((long long)nb * 10 > INT_MAX) return fail("too many blocks");
        if (!ok(d.run(gpu::kRefine, nb * 10,
                      {P->luma[0], P->lw[0], P->lh[0], C->luma[0], C->lw[0], e.dx, e.dy, mf.bw, mf.block, w, h,
                       mp.halfPel ? 1 : 0, e.refine})))
            return false;
        std::vector<float> sums(nOut);
        if (!ok(d.download(e.refine, sums.data(), nOut * sizeof(float)))) return false;
        mosh::prepareRefine(mf, mp);
        for (int by = 0; by < mf.bh; ++by)
            for (int bx = 0; bx < mf.bw; ++bx) {
                const int i = mf.index(bx, by);
                const int x0 = bx * mf.block, y0 = by * mf.block;
                const int x1 = std::min(x0 + mf.block, w), y1 = std::min(y0 + mf.block, h);
                const float cnt = float((x1 - x0) * (y1 - y0));
                const float* s = &sums[(size_t)i * 11];
                mosh::refineBlock(mf, mp, i, s, s[9], s[10], cnt);
            }
        mosh::finishIntra(mf, mp);
    }
    return true;
}

bool Session::moshStep(const FramePtr& prev, const FramePtr& cur, const mosh::MotionField& mf,
                       const mosh::StepParams& sp) {
    if (failed_) return false;
    Engine& e = e_;
    gpu::Device& d = *e.dev;
    DevFrame* P = e.frame(prev);
    DevFrame* C = e.frame(cur);
    P = P ? e.frame(prev) : nullptr;
    if (!ok(P && C)) return false;
    const int w = C->w, h = C->h;
    if (P->w != w || P->h != h || e.accW != w || e.accH != h || !e.acc) return fail("frame sizes differ");
    const int nb = mf.bw * mf.bh;
    std::vector<float> table((size_t)nb * 4, 0.f);
    const bool subpel = mf.subpel();
    for (int i = 0; i < nb; ++i) {
        float* t = &table[(size_t)i * 4];
        t[0] = sp.intraBlocks && mf.intra[i] ? 1.f : 0.f;
        if (subpel) {
            t[1] = mf.fx[i] * sp.motionScale;
            t[2] = mf.fy[i] * sp.motionScale;
        } else {
            t[1] = (float)std::lround(mf.dx[i] * sp.motionScale);
            t[2] = (float)std::lround(mf.dy[i] * sp.motionScale);
        }
    }
    if (!ok(e.ensure(e.table, table.size() * sizeof(float))) ||
        !ok(d.upload(e.table, table.data(), table.size() * sizeof(float))))
        return false;
    if (e.accTmp && e.accTmp.bytes != e.acc.bytes) e.put(e.accTmp);
    if (!e.accTmp && !ok(e.get(e.accTmp, e.acc.bytes))) return false;
    const int n = std::max(1, sp.repeat);
    for (int k = 0; k < n; ++k) {
        if (!ok(d.run(gpu::kStep, w * h,
                      {P->rgba, C->rgba, e.acc, e.accTmp, w, h, mf.bw, mf.bh, mf.block, e.table, subpel ? 1 : 0,
                       sp.residual, sp.clampLevels ? 1 : 0})))
            return false;
        std::swap(e.acc, e.accTmp);
    }
    return true;
}

bool Session::pull(const FramePtr& cur, const mosh::MotionField& mf, const std::vector<uint8_t>& flagged, float rate,
                   float tail) {
    if (failed_) return false;
    Engine& e = e_;
    DevFrame* C = e.frame(cur);
    if (!ok(C != nullptr)) return false;
    const int nb = mf.bw * mf.bh;
    if ((int)flagged.size() != nb || e.accW != C->w || e.accH != C->h) return fail("block count differs");
    std::vector<int> f(flagged.begin(), flagged.end());
    if (!ok(e.ensure(e.flags, f.size() * sizeof(int))) || !ok(e.dev->upload(e.flags, f.data(), f.size() * sizeof(int))))
        return false;
    return ok(e.dev->run(gpu::kPull, C->w * C->h,
                         {e.acc, C->rgba, C->w, mf.bw, mf.bh, mf.block, e.flags, rate, tail}));
}

bool Session::refreshActivity(const FramePtr& prev, const FramePtr& cur, const mosh::MotionField& mf,
                              std::vector<float>& sums, std::vector<int>& counts) {
    if (failed_) return false;
    Engine& e = e_;
    DevFrame* P = e.frame(prev);
    DevFrame* C = e.frame(cur);
    P = P ? e.frame(prev) : nullptr;
    if (!ok(P && C)) return false;
    if (P->w != C->w || P->h != C->h) return fail("frame sizes differ");
    const int nb = mf.bw * mf.bh;
    if (!ok(e.ensure(e.sums, (size_t)nb * sizeof(float))) || !ok(e.ensure(e.counts, (size_t)nb * sizeof(int))))
        return false;
    if (!ok(e.dev->run(gpu::kRefreshAct, nb, {P->rgba, C->rgba, C->w, C->h, mf.bw, mf.block, e.sums, e.counts})))
        return false;
    sums.assign(nb, 0.f);
    counts.assign(nb, 0);
    return ok(e.dev->download(e.sums, sums.data(), (size_t)nb * sizeof(float))) &&
           ok(e.dev->download(e.counts, counts.data(), (size_t)nb * sizeof(int)));
}

bool Session::compose(const FramePtr& to, const mosh::ComposeParams& cp, float q, mosh::Image& out) {
    if (failed_) return false;
    Engine& e = e_;
    gpu::Device& d = *e.dev;
    DevFrame* T = e.frame(to);
    if (!ok(T != nullptr)) return false;
    const int w = T->w, h = T->h, n = w * h;
    if (e.accW != w || e.accH != h || !e.acc) return fail("frame sizes differ");
    const size_t bytes = (size_t)n * 4 * sizeof(float);
    q = mosh::clamp01(q);
    const float s = mosh::clamp01(cp.softness);
    const int block = std::max(1, cp.block);

    // Every buffer taken here goes back to the pool on the way out.
    std::vector<Buffer> held;
    auto take = [&](Buffer& b) {
        if (!ok(e.get(b, bytes))) return false;
        held.push_back(b);
        return true;
    };
    struct Giveback {
        Engine& e;
        std::vector<Buffer>& v;
        ~Giveback() {
            for (Buffer& b : v) e.put(b);
        }
    } giveback{e, held};
    auto blur = [&](const Buffer& src, int r, const Buffer& dst) {
        if (r <= 0) return ok(d.copy(dst, src, bytes));
        Buffer tmp;
        if (!take(tmp)) return false;
        const float inv = 1.f / float(2 * r + 1);
        return ok(d.run(gpu::kBlurRows, h, {src, tmp, w, h, r, inv})) &&
               ok(d.run(gpu::kBlurCols, w, {tmp, dst, w, h, r, inv}));
    };

    Buffer res;
    if (!take(res)) return false;
    if (cp.mode == mosh::Recovery::StructureFirst || cp.mode == mosh::Recovery::ColourFirst) {
        const bool colourFirst = cp.mode == mosh::Recovery::ColourFirst;
        const int r = mosh::composeBlurRadius(cp);
        Buffer lowA, lowT;
        if (!take(lowA) || !take(lowT) || !blur(e.acc, r, lowA) || !blur(T->rgba, r, lowT)) return false;
        const float lag = 0.1f + 0.5f * s;
        const float kFirst = mosh::smoothstep01(q / (1.f - lag));
        const float kSecond = mosh::smoothstep01((q - lag) / (1.f - lag));
        const float kd = colourFirst ? kSecond : kFirst;
        const float kc = colourFirst ? kFirst : kSecond;
        if (!ok(d.run(gpu::kBands, n, {e.acc, T->rgba, lowA, lowT, res, kc, kd}))) return false;
    } else if (cp.mode == mosh::Recovery::Slice) {
        const mosh::SliceLayout L = mosh::makeSlices(cp, w, h, s);
        if (!ok(e.ensure(e.band, L.band.size() * sizeof(int))) || !ok(e.ensure(e.start, L.start.size() * sizeof(float))) ||
            !ok(e.ensure(e.sign, L.sign.size() * sizeof(int))))
            return false;
        if (!ok(d.upload(e.band, L.band.data(), L.band.size() * sizeof(int))) ||
            !ok(d.upload(e.start, L.start.data(), L.start.size() * sizeof(float))) ||
            !ok(d.upload(e.sign, L.sign.data(), L.sign.size() * sizeof(int))))
            return false;
        if (!ok(d.run(gpu::kSlice, n,
                      {e.acc, T->rgba, res, w, h, L.columns ? 1 : 0, e.band, e.start, e.sign, q, L.dur})))
            return false;
    } else {
        Buffer key = e.acc;  // unused by modes without a key
        float keyNorm = 1.f;
        if (cp.mode == mosh::Recovery::LumaReveal) {
            if (!take(key) || !blur(T->rgba, mosh::composeBlurRadius(cp), key)) return false;
        } else if (cp.mode == mosh::Recovery::Settle) {
            Buffer diff;
            if (!take(diff) || !take(key)) return false;
            if (!ok(d.run(gpu::kSettleDiff, n, {e.acc, T->rgba, diff})) || !blur(diff, mosh::composeBlurRadius(cp), key))
                return false;
            const int ns = (int)(((size_t)n * 4 + 15) / 16);
            if (!ok(e.ensure(e.sample, (size_t)ns * sizeof(float)))) return false;
            if (!ok(d.run(gpu::kGather, ns, {key, 16, e.sample}))) return false;
            std::vector<float> sample(ns);
            if (!ok(d.download(e.sample, sample.data(), sample.size() * sizeof(float)))) return false;
            keyNorm = mosh::settleNorm(sample);
        }
        const float bw = 0.6f * s;
        const float fw = 0.05f + 0.6f * s;
        const float ww = 0.02f + 0.4f * s;
        const float jag = 0.08f;
        if (!ok(d.run(gpu::kCompose, n,
                      {e.acc, T->rgba, key, res, w, h, (int)cp.mode, q, block, cp.seed, bw, fw, ww, jag,
                       cp.invertReveal ? 1 : 0, keyNorm, (int)cp.direction})))
            return false;
    }
    out.resize(w, h);
    return ok(d.download(res, out.px.data(), bytes));
}

// ---------------------------------------------------------------- opening

std::unique_ptr<Session> begin(Preference p, std::string& why) {
    why.clear();
    if (p == Preference::Cpu) return nullptr;
    Engine& e = engine();
    std::unique_lock<std::mutex> lk(e.m);
    if (!e.broken.empty()) {
        why = e.broken;
        return nullptr;
    }
    auto fits = [&]() {
        if (!e.dev) return false;
        if (p == Preference::Cuda) return e.dev->api() == gpu::Api::Cuda;
        if (p == Preference::OpenCL) return e.dev->api() == gpu::Api::OpenCL;
        return e.dev->api() == gpu::Api::Cuda || !e.devAnyType;
    };
    if (!fits()) {
        e.close();
        struct Try {
            gpu::Api api;
            bool anyType;
            bool* tried;
            std::string* failure;
        };
        std::vector<Try> order;
        if (p == Preference::Auto || p == Preference::Cuda) order.push_back({gpu::Api::Cuda, false, &e.cudaTried, &e.triedCuda});
        if (p == Preference::Auto) order.push_back({gpu::Api::OpenCL, false, &e.clTried, &e.triedCl});
        if (p == Preference::OpenCL) order.push_back({gpu::Api::OpenCL, true, &e.clAnyTried, &e.triedClAny});
        std::string reasons;
        for (const Try& t : order) {
            // A driver that is missing or failed once is not tried again in this session.
            if (!*t.tried) {
                *t.tried = true;
                std::string w;
                e.dev = gpu::openDevice(t.api, t.anyType, w);
                *t.failure = e.dev ? std::string() : w;
                if (e.dev) {
                    e.devAnyType = t.anyType;
                    break;
                }
            } else if (t.failure->empty()) {
                // Worked before and was closed for another preference: open again.
                std::string w;
                e.dev = gpu::openDevice(t.api, t.anyType, w);
                if (e.dev) {
                    e.devAnyType = t.anyType;
                    break;
                }
                *t.failure = w;
            }
            if (!reasons.empty()) reasons += ", ";
            reasons += *t.failure;
        }
        if (!e.dev) {
            why = reasons;
            return nullptr;
        }
    }
    std::unique_ptr<Session> s(new Session(e));
    s->lock_ = std::move(lk);
    return s;
}

void forget(const void* owner) {
    Engine& e = engine();
    std::lock_guard<std::mutex> g(e.m);
    for (size_t i = 0; i < e.accs.size();) {
        if (e.accs[i].owner == owner) {
            e.put(e.accs[i].buf);
            e.accs.erase(e.accs.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

void shutdown() {
    Engine& e = engine();
    std::lock_guard<std::mutex> g(e.m);
    e.close();
}

}  // namespace mosh_gpu
