// gpu_selftest: runs the mosh on the CPU and on a GPU and checks that every
// stage gives the same bits. Prints the device, any mismatch and timings.
//   gpu_selftest [auto|cuda|opencl] [width height]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "mosh_core.h"
#include "mosh_gpu.h"

using mosh::Image;

static void parallelFor(int n, const mosh::RangeBody& body) {
    const int nt = std::max(1u, std::thread::hardware_concurrency());
    if (n < 2 || nt < 2) {
        if (n > 0) body(0, n);
        return;
    }
    std::vector<std::thread> th;
    const int per = (n + nt - 1) / nt;
    for (int b = 0; b < n; b += per) th.emplace_back([&, b] { body(b, std::min(n, b + per)); });
    for (auto& t : th) t.join();
}

// Textured picture that pans and rotates with t; scene selects another pattern.
static mosh_gpu::FramePtr frame(int w, int h, int t, int scene) {
    auto img = std::make_shared<Image>(w, h);
    const float a = 0.02f * t, ox = 3.f * t, oy = 1.5f * t;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = std::cos(a) * (x - w / 2) - std::sin(a) * (y - h / 2) + ox;
            const float v = std::sin(a) * (x - w / 2) + std::cos(a) * (y - h / 2) + oy;
            float* p = img->at(x, y);
            if (scene == 0) {
                p[0] = 0.5f + 0.5f * std::sin(u * 0.05f) * std::cos(v * 0.07f);
                p[1] = 0.5f + 0.5f * std::sin((u + v) * 0.031f);
                p[2] = ((int(u) / 16 + int(v) / 16) & 1) ? 0.8f : 0.2f;
            } else {
                p[0] = ((int(u / 24) ^ int(v / 24)) & 3) * 0.3f;
                p[1] = 0.5f + 0.5f * std::cos(u * 0.11f - v * 0.04f);
                p[2] = y < h / 3 ? 0.6f : 0.4f + 0.3f * std::sin(v * 0.2f);
            }
            p[3] = 1.f;
        }
    return img;
}

static size_t diffCount(const Image& a, const Image& b) {
    if (a.w != b.w || a.h != b.h) return (size_t)-1;
    size_t n = 0;
    for (size_t i = 0; i < a.px.size(); ++i)
        if (memcmp(&a.px[i], &b.px[i], 4)) ++n;
    return n;
}

static bool sameField(const mosh::MotionField& a, const mosh::MotionField& b) {
    auto eqf = [](const std::vector<float>& x, const std::vector<float>& y) {
        return x.size() == y.size() && (x.empty() || !memcmp(x.data(), y.data(), x.size() * 4));
    };
    return a.dx == b.dx && a.dy == b.dy && a.intra == b.intra && eqf(a.err, b.err) && eqf(a.fx, b.fx) &&
           eqf(a.fy, b.fy) && eqf(a.sse, b.sse) && eqf(a.var, b.var);
}

int main(int argc, char** argv) {
    mosh_gpu::Preference pref = mosh_gpu::Preference::Auto;
    if (argc > 1 && !strcmp(argv[1], "cuda")) pref = mosh_gpu::Preference::Cuda;
    if (argc > 1 && !strcmp(argv[1], "opencl")) pref = mosh_gpu::Preference::OpenCL;
    const int w = argc > 3 ? atoi(argv[2]) : 640, h = argc > 3 ? atoi(argv[3]) : 360;

    std::string why;
    std::unique_ptr<mosh_gpu::Session> gpu = mosh_gpu::begin(pref, why);
    if (!gpu) {
        printf("no GPU: %s\n", why.c_str());
        return 2;
    }
    printf("device: %s\n", gpu->label().c_str());

    const int steps = 6;
    std::vector<mosh_gpu::FramePtr> frames;
    frames.push_back(frame(w, h, 0, 0));
    for (int i = 0; i <= steps; ++i) frames.push_back(frame(w, h, i, 1));

    struct Config {
        const char* name;
        int block, range;
        bool halfPel, smooth, fillFlat;
        float intraRate, intraThreshold;
        int repeat;
        float scale, residual;
        bool clamp;
        int natural;  // 0 none, 1 natural, 2 motion refresh
    };
    const Config configs[] = {
        {"default (half-pel, natural)", 16, 48, true, true, true, 0.6f, 0.35f, 1, 1.f, 1.f, true, 1},
        {"P-only full-pel, refresh", 16, 48, false, true, true, 0.f, 0.35f, 1, 1.f, 0.6f, true, 2},
        {"block 8, range 24", 8, 24, true, true, true, 0.6f, 0.35f, 1, 1.f, 1.f, true, 0},
        {"block 64, range 96", 64, 96, true, true, true, 0.9f, 0.35f, 1, 1.f, 1.f, true, 0},
        {"bloom 3, scale -1.5", 16, 48, true, true, true, 0.6f, 0.35f, 3, -1.5f, 1.4f, true, 1},
        {"no smooth/fill, unclamped", 32, 48, false, false, false, 0.f, 0.1f, 1, 0.7f, 2.f, false, 0},
    };
    int failures = 0;
    double cpuMs = 0, gpuMs = 0;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    for (const Config& c : configs) {
        mosh::MotionParams mp;
        mp.block = c.block; mp.range = c.range; mp.halfPel = c.halfPel; mp.smooth = c.smooth;
        mp.fillFlat = c.fillFlat; mp.intraRate = c.intraRate; mp.intraThreshold = c.intraThreshold;
        mp.blockStats = c.natural == 1;
        mosh::StepParams sp;
        sp.repeat = c.repeat; sp.motionScale = c.scale; sp.residual = c.residual; sp.clampLevels = c.clamp;
        sp.intraBlocks = true;
        mosh::NaturalStep ns;
        ns.baseRate = c.intraRate; ns.motionScale = c.scale;
        mosh::RefreshStep rs;

        Image acc = *frames[0], scratch;
        std::vector<uint8_t> stateC, stateG;
        bool ok = gpu->setAcc(*frames[0]);
        bool fieldsSame = true;
        for (int i = 1; i <= steps && ok; ++i) {
            const auto& prev = frames[i];
            const auto& cur = frames[i + 1];
            const float q = float(i) / steps;
            auto t0 = now();
            mosh::MotionField mfC = mosh::estimateMotion(*prev, *cur, mp, parallelFor);
            if (c.natural == 1) { ns.q = q; ns.frame = i; mosh::naturalIntra(mfC, ns, stateC); }
            mosh::moshStep(*prev, *cur, mfC, sp, acc, scratch, parallelFor);
            if (c.natural == 1) mosh::naturalAfter(*cur, mfC, ns, stateC, acc, parallelFor);
            if (c.natural == 2) { rs.q = q; mosh::refreshStep(*prev, *cur, mfC, rs, stateC, acc, parallelFor); }
            auto t1 = now();
            mosh::MotionField mfG;
            ok = gpu->estimateMotion(prev, cur, mp, mfG, parallelFor);
            if (ok && c.natural == 1) mosh::naturalIntra(mfG, ns, stateG);
            ok = ok && gpu->moshStep(prev, cur, mfG, sp);
            if (ok && c.natural == 1) {
                mosh::naturalCarry(mfG, ns, stateG);
                const float tail = mosh::naturalTail(ns);
                if (tail > 0.f) ok = gpu->pull(cur, mfG, stateG, 0.f, tail);
            }
            if (ok && c.natural == 2) {
                const size_t nb = (size_t)mfG.bw * mfG.bh;
                if (stateG.size() != nb) stateG.assign(nb, 0);
                std::vector<float> sums;
                std::vector<int> counts;
                ok = gpu->refreshActivity(prev, cur, mfG, sums, counts);
                if (ok) {
                    const mosh::RefreshPull rp = mosh::refreshDecide(mfG, rs, sums, counts, stateG);
                    ok = gpu->pull(cur, mfG, stateG, rp.rate, rp.tail);
                }
            }
            Image g;
            ok = ok && gpu->downloadAcc(g);
            auto t2 = now();
            cpuMs += ms(t0, t1);
            gpuMs += ms(t1, t2);
            if (!ok) break;
            if (!sameField(mfC, mfG)) fieldsSame = false;
            const size_t d = diffCount(acc, g);
            if (d) {
                printf("  %s: step %d: %zu values differ%s\n", c.name, i, d, fieldsSame ? "" : " (motion field differs)");
                ++failures;
                break;
            }
        }
        if (!ok) {
            printf("GPU error: %s\n", gpu->error().c_str());
            return 1;
        }
        printf("%-30s %s\n", c.name, fieldsSame ? "fields and pictures identical" : "FIELDS DIFFER");
        if (!fieldsSame) ++failures;

        if (&c == &configs[0]) {  // every recovery mode on the final picture
            for (int m = 0; m < mosh::kRecoveryCount; ++m)
                for (float q : {0.3f, 0.75f}) {
                    mosh::ComposeParams cp;
                    cp.mode = (mosh::Recovery)m;
                    cp.block = 16;
                    cp.seed = 3;
                    cp.softness = m == 0 && q < 0.5f ? 0.f : 0.5f;
                    cp.direction = (mosh::Direction)(m % mosh::kDirectionCount);
                    cp.invertReveal = q > 0.5f;
                    Image oc, og;
                    auto t0 = now();
                    mosh::compose(acc, *frames[steps + 1], cp, q, oc, parallelFor);
                    auto t1 = now();
                    if (!gpu->compose(frames[steps + 1], cp, q, og)) {
                        printf("GPU error: %s\n", gpu->error().c_str());
                        return 1;
                    }
                    cpuMs += ms(t0, t1);
                    gpuMs += ms(t1, now());
                    const size_t d = diffCount(oc, og);
                    if (d) {
                        printf("  recovery %d q %.2f: %zu values differ\n", m, q, d);
                        ++failures;
                    }
                }
            printf("%-30s %s\n", "recovery modes", failures ? "see above" : "identical");
        }
    }
    printf("time: CPU %.0f ms, GPU %.0f ms (GPU includes uploads, downloads and first-use setup)\n", cpuMs, gpuMs);
    printf(failures ? "RESULT: %d MISMATCHES\n" : "RESULT: OK\n", failures);
    return failures ? 1 : 0;
}
