// Offline harness for the datamosh core: reads two raw rgb24 clips, renders a
// transition between them and writes raw rgb24 frames.
//
//   mosh_test <from.rgb> <to.rgb> <width> <height> <frames> <out.rgb>
//             [block=16] [range=48] [recoverStart=0.8] [residual=0.6] [repeat=1] [vectors.rgb]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "mosh_core.h"

static void threadedFor(int n, const mosh::RangeBody& body) {
    const int nt = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), (unsigned)n));
    if (nt <= 1) { if (n > 0) body(0, n); return; }
    std::vector<std::thread> th;
    const int per = (n + nt - 1) / nt;
    for (int i = 0; i < nt; ++i) {
        const int b = i * per, e = std::min(n, b + per);
        if (b < e) th.emplace_back(body, b, e);
    }
    for (auto& t : th) t.join();
}

static bool readClip(const char* path, int w, int h, int n, std::vector<mosh::Image>& frames) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> buf((size_t)w * h * 3);
    frames.assign(n, mosh::Image(w, h));
    for (int i = 0; i < n; ++i) {
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return false; }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char* p = &buf[((size_t)y * w + x) * 3];
                float* o = frames[i].at(x, y);
                o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = 1.f;
            }
    }
    fclose(f);
    return true;
}

static void writeFrame(FILE* f, const mosh::Image& img) {
    std::vector<unsigned char> buf((size_t)img.w * img.h * 3);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const float* p = img.at(x, y);
            unsigned char* o = &buf[((size_t)y * img.w + x) * 3];
            for (int c = 0; c < 3; ++c) o[c] = (unsigned char)(std::min(std::max(p[c], 0.f), 1.f) * 255.f + 0.5f);
        }
    fwrite(buf.data(), 1, buf.size(), f);
}

int main(int argc, char** argv) {
    if (argc < 7) {
        fprintf(stderr,
                "usage: %s from.rgb to.rgb w h frames out.rgb [block] [range] [recoverStart] [residual] [repeat] "
                "[vectors.rgb]\n",
                argv[0]);
        return 1;
    }
    const int w = atoi(argv[3]), h = atoi(argv[4]), n = atoi(argv[5]);
    mosh::MotionParams mp;
    mp.block = argc > 7 ? atoi(argv[7]) : 16;
    mp.range = argc > 8 ? atoi(argv[8]) : 48;
    const double recoverStart = argc > 9 ? atof(argv[9]) : 0.8;
    mosh::StepParams sp;
    sp.residual = argc > 10 ? (float)atof(argv[10]) : 0.6f;
    sp.repeat = argc > 11 ? atoi(argv[11]) : 1;
    FILE* vec = argc > 12 ? fopen(argv[12], "wb") : nullptr;

    std::vector<mosh::Image> from, to;
    if (!readClip(argv[1], w, h, n, from) || !readClip(argv[2], w, h, n, to)) {
        fprintf(stderr, "failed to read input clips\n");
        return 1;
    }
    FILE* out = fopen(argv[6], "wb");
    if (!out) return 1;

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) {
        const double p = n > 1 ? (double)i / (n - 1) : 1.0;
        if (p <= 0.0 || p >= 1.0) {
            writeFrame(out, p <= 0.0 ? from[i] : to[i]);
            if (vec) writeFrame(vec, to[i]);
            continue;
        }
        // Stateless like the plugin: rebuild the whole chain for every frame.
        mosh::Image acc = from[0], scratch, result;
        mosh::MotionField mf;
        for (int k = 1; k <= i; ++k) {
            mf = mosh::estimateMotion(to[k - 1], to[k], mp, threadedFor);
            mosh::moshStep(to[k - 1], to[k], mf, sp, acc, scratch, threadedFor);
        }
        if (mf.bw > 0) {
            double sx = 0, sy = 0, mag = 0; int intra = 0;
            for (size_t k = 0; k < mf.dx.size(); ++k) {
                sx += mf.dx[k]; sy += mf.dy[k]; mag += std::hypot((double)mf.dx[k], (double)mf.dy[k]); intra += mf.intra[k];
            }
            const double nb = (double)mf.dx.size();
            fprintf(stderr, "frame %2d: mean mv (%.1f, %.1f) px, mean |mv| %.1f px, intra %.0f%%\n", i, sx / nb, sy / nb,
                    mag / nb, 100.0 * intra / nb);
        }
        const float q = p < recoverStart ? 0.f : (float)((p - recoverStart) / (1.0 - recoverStart));
        mosh::ComposeParams cp;
        cp.mode = mosh::Recovery::Blocks;
        cp.softness = 0.f;
        cp.block = mp.block;
        cp.seed = 7;
        mosh::compose(acc, to[i], cp, q, result, threadedFor);
        writeFrame(out, result);
        if (vec) {
            mosh::visualizeMotion(mf, mp.range, w, h, result, threadedFor);
            writeFrame(vec, result);
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fclose(out);
    if (vec) fclose(vec);
    fprintf(stderr, "rendered %d frames %dx%d in %.0f ms (%.1f ms/frame avg)\n", n, w, h, ms, ms / n);
    return 0;
}
