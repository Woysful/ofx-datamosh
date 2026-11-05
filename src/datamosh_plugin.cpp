// OpenFX filter plugin: Datamosh.
// Written against the raw OFX C API so it has no dependency besides the
// OpenFX headers.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "mosh_core.h"

#define PLUGIN_ID "com.github.ofxdatamosh.Datamosh"
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 5
#define PLUGIN_VERSION_STRING "1.5.0"

static OfxHost* gHost = nullptr;
static OfxImageEffectSuiteV1* gEffect = nullptr;
static OfxPropertySuiteV1* gProp = nullptr;
static OfxParameterSuiteV1* gParam = nullptr;
static OfxMultiThreadSuiteV1* gThread = nullptr;

// ---------------------------------------------------------------- params

namespace P {
const char* kCutMode = "cutMode";           // choice: auto / manual
const char* kCutFrame = "cutFrame";         // int
const char* kCutThreshold = "cutThreshold"; // double
const char* kMoshLength = "moshLength";     // int frames
const char* kBlockSize = "blockSize";       // choice: 8/16/32/64
const char* kSearchRange = "searchRange";   // int px
const char* kMotionScale = "motionScale";   // double
const char* kResidual = "residual";         // double
const char* kRepeat = "bloomRepeat";        // int: decode each P-frame N times
const char* kIntra = "intraThreshold";      // double 0..1
const char* kSmooth = "smoothVectors";      // bool
const char* kFillFlat = "fillFlat";         // bool
const char* kMoshStyle = "moshStyle";       // choice: intra + half-pel / P-only, full-pel
const char* kIntraRate = "intraRate";       // double 0..1: encoder-style intra decision
const char* kSubpel = "halfPelMotion";      // bool
const char* kRecoverStart = "recoverStart"; // double 0..1
const char* kRecoverMode = "recoverMode";   // choice
const char* kRecoverCurve = "recoverCurve"; // choice
const char* kRecoverSoftness = "recoverSoftness"; // double 0..1
const char* kRevealInvert = "revealInvert"; // bool
const char* kDirection = "recoverDirection"; // choice: Wipe / Slice direction
const char* kMaxSteps = "maxSteps";         // int
const char* kClamp = "clampLevels";         // bool
const char* kSeed = "seed";                 // int
const char* kView = "view";                 // choice: result / motion vectors / diagnostics

const char* kGrpCut = "grpCut";
const char* kGrpMosh = "grpMosh";
const char* kGrpMotion = "grpMotion";
const char* kGrpRecovery = "grpRecovery";
const char* kGrpAdvanced = "grpAdvanced";
}  // namespace P

static const int kDefaultRecovery = (int)mosh::Recovery::Natural;
static const int kStylePOnly = 1;

struct StyleDefaults {
    double residual;
    int recovery;
};
static const StyleDefaults kStyleDefaults[2] = {
    {1.0, (int)mosh::Recovery::Natural},        // intra + half-pel
    {0.6, (int)mosh::Recovery::MotionRefresh},  // P-only, full-pel
};

static const int kBlockSizes[] = {8, 16, 32, 64};

// ---------------------------------------------------------------- threading

struct PfCtx {
    int n;
    const mosh::RangeBody* body;
};

static void pfThread(unsigned idx, unsigned total, void* arg) {
    PfCtx* c = static_cast<PfCtx*>(arg);
    const int per = (c->n + (int)total - 1) / (int)total;
    const int b = (int)idx * per;
    const int e = std::min(c->n, b + per);
    if (b < e) (*c->body)(b, e);
}

static void hostParallelFor(int n, const mosh::RangeBody& body) {
    unsigned ncpu = 1;
    if (gThread) gThread->multiThreadNumCPUs(&ncpu);
    if (!gThread || ncpu <= 1 || n < 2) {
        if (n > 0) body(0, n);
        return;
    }
    ncpu = std::min<unsigned>(ncpu, (unsigned)n);
    PfCtx c{n, &body};
    if (gThread->multiThread(pfThread, ncpu, &c) != kOfxStatOK) body(0, n);
}

// ---------------------------------------------------------------- param helpers

// Group that parameters defined next are placed in (nullptr: top level).
static const char* gGroup = nullptr;

static OfxPropertySetHandle defineParam(OfxParamSetHandle ps, const char* type, const char* name,
                                        const char* label, const char* hint) {
    OfxPropertySetHandle p = nullptr;
    gParam->paramDefine(ps, type, name, &p);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxParamPropHint, 0, hint);
    if (gGroup) gProp->propSetString(p, kOfxParamPropParent, 0, gGroup);
    return p;
}

static void beginGroup(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, bool open) {
    gGroup = nullptr;
    OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeGroup, name, label, hint);
    gProp->propSetInt(p, kOfxParamPropGroupOpen, 0, open ? 1 : 0);
    gGroup = name;
}

static void defineDouble(OfxParamSetHandle ps, const char* name, const char* label, const char* hint,
                         double def, double mn, double mx) {
    OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeDouble, name, label, hint);
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, def);
    gProp->propSetDouble(p, kOfxParamPropMin, 0, mn);
    gProp->propSetDouble(p, kOfxParamPropMax, 0, mx);
    gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, mn);
    gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, mx);
    gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
}

static void defineInt(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, int def,
                      int mn, int mx) {
    OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeInteger, name, label, hint);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
    gProp->propSetInt(p, kOfxParamPropMin, 0, mn);
    gProp->propSetInt(p, kOfxParamPropMax, 0, mx);
    gProp->propSetInt(p, kOfxParamPropDisplayMin, 0, mn);
    gProp->propSetInt(p, kOfxParamPropDisplayMax, 0, mx);
}

static void defineBool(OfxParamSetHandle ps, const char* name, const char* label, const char* hint, bool def) {
    OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeBoolean, name, label, hint);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def ? 1 : 0);
}

static void defineChoice(OfxParamSetHandle ps, const char* name, const char* label, const char* hint,
                         const std::vector<const char*>& options, int def) {
    OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeChoice, name, label, hint);
    for (size_t i = 0; i < options.size(); ++i) gProp->propSetString(p, kOfxParamPropChoiceOption, (int)i, options[i]);
    gProp->propSetInt(p, kOfxParamPropDefault, 0, def);
}

static double getDouble(OfxParamSetHandle ps, const char* name, double t, double fallback) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) return fallback;
    double v = fallback;
    gParam->paramGetValueAtTime(h, t, &v);
    return v;
}

static int getInt(OfxParamSetHandle ps, const char* name, double t, int fallback) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) return fallback;
    int v = fallback;
    gParam->paramGetValueAtTime(h, t, &v);
    return v;
}

// ---------------------------------------------------------------- host images

// Image fetched from the host, released on destruction.
struct HostImage {
    OfxPropertySetHandle h = nullptr;
    void* data = nullptr;
    OfxRectI b{0, 0, 0, 0};
    int rowBytes = 0;
    int ncomp = 0;
    int bytesPerComp = 0;

    HostImage() = default;
    HostImage(const HostImage&) = delete;
    HostImage& operator=(const HostImage&) = delete;
    ~HostImage() { release(); }

    void swap(HostImage& o) {
        std::swap(h, o.h); std::swap(data, o.data); std::swap(b, o.b);
        std::swap(rowBytes, o.rowBytes); std::swap(ncomp, o.ncomp); std::swap(bytesPerComp, o.bytesPerComp);
    }

    void release() {
        if (h) gEffect->clipReleaseImage(h);
        h = nullptr;
        data = nullptr;
    }

    bool fetch(OfxImageClipHandle clip, double t) {
        release();
        if (!clip) return false;
        if (gEffect->clipGetImage(clip, t, nullptr, &h) != kOfxStatOK || !h) {
            h = nullptr;
            return false;
        }
        gProp->propGetPointer(h, kOfxImagePropData, 0, &data);
        gProp->propGetIntN(h, kOfxImagePropBounds, 4, &b.x1);
        gProp->propGetInt(h, kOfxImagePropRowBytes, 0, &rowBytes);
        char* s = nullptr;
        gProp->propGetString(h, kOfxImageEffectPropComponents, 0, &s);
        if (s && !strcmp(s, kOfxImageComponentRGBA)) ncomp = 4;
        else if (s && !strcmp(s, kOfxImageComponentRGB)) ncomp = 3;
        else if (s && !strcmp(s, kOfxImageComponentAlpha)) ncomp = 1;
        else ncomp = 0;
        gProp->propGetString(h, kOfxImageEffectPropPixelDepth, 0, &s);
        if (s && !strcmp(s, kOfxBitDepthFloat)) bytesPerComp = 4;
        else if (s && !strcmp(s, kOfxBitDepthShort)) bytesPerComp = 2;
        else if (s && !strcmp(s, kOfxBitDepthByte)) bytesPerComp = 1;
        else bytesPerComp = 0;
        if (!data || ncomp == 0 || bytesPerComp == 0 || width() <= 0 || height() <= 0) {
            release();
            return false;
        }
        return true;
    }

    int width() const { return b.x2 - b.x1; }
    int height() const { return b.y2 - b.y1; }

    // rowBytes may be negative (top-down storage), so the offset is signed.
    unsigned char* row(int y) const {
        return static_cast<unsigned char*>(data) + (ptrdiff_t)(y - b.y1) * (ptrdiff_t)rowBytes;
    }

    // Channel value at image-local (x, y) as float 0..1, without converting the whole frame.
    float sample(int x, int y, int ch) const {
        const unsigned char* p = row(y + b.y1) + (size_t)x * ncomp * bytesPerComp +
                                 (size_t)std::min(ch, ncomp - 1) * bytesPerComp;
        if (bytesPerComp == 4) return *reinterpret_cast<const float*>(p);
        if (bytesPerComp == 2) return *reinterpret_cast<const unsigned short*>(p) / 65535.f;
        return *p / 255.f;
    }

    // Convert to float RGBA over the pixel rectangle r (normally the output
    // image's bounds), so every frame lines up with the output whatever
    // bounds the host gave this one. Pixels outside this image stay 0.
    void toImage(mosh::Image& img, const OfxRectI& r, const mosh::ParallelFor& pf) const {
        img.resize(r.x2 - r.x1, r.y2 - r.y1);
        const int x0 = std::max(r.x1, b.x1), x1 = std::min(r.x2, b.x2);
        const bool fast = ncomp == 4 && bytesPerComp == 4;
        pf(img.h, [&](int y0, int y1) {
            for (int iy = y0; iy < y1; ++iy) {
                const int y = iy + r.y1;
                float* orow = img.at(0, iy);
                if (y < b.y1 || y >= b.y2 || x0 >= x1) {
                    std::fill(orow, orow + (size_t)img.w * 4, 0.f);
                    continue;
                }
                std::fill(orow, orow + (size_t)(x0 - r.x1) * 4, 0.f);
                std::fill(orow + (size_t)(x1 - r.x1) * 4, orow + (size_t)img.w * 4, 0.f);
                const unsigned char* src = row(y) + (size_t)(x0 - b.x1) * ncomp * bytesPerComp;
                float* o = orow + (size_t)(x0 - r.x1) * 4;
                if (fast) {
                    memcpy(o, src, (size_t)(x1 - x0) * 4 * sizeof(float));
                    continue;
                }
                for (int x = x0; x < x1; ++x, o += 4, src += (size_t)ncomp * bytesPerComp) {
                    float c[4] = {0.f, 0.f, 0.f, 1.f};
                    for (int k = 0; k < ncomp; ++k) {
                        float v;
                        if (bytesPerComp == 4) v = reinterpret_cast<const float*>(src)[k];
                        else if (bytesPerComp == 2) v = reinterpret_cast<const unsigned short*>(src)[k] / 65535.f;
                        else v = src[k] / 255.f;
                        c[k] = v;
                    }
                    if (ncomp == 1) { c[3] = c[0]; c[0] = c[1] = c[2] = 0.f; }
                    o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = c[3];
                }
            }
        });
    }

    // Write float RGBA, whose pixel (0,0) sits at (org.x1, org.y1), into this
    // (output) image inside the render window.
    void fromImage(const mosh::Image& img, const OfxRectI& org, const OfxRectI& rw,
                   const mosh::ParallelFor& pf) const {
        const int ry1 = std::max(rw.y1, b.y1), ry2 = std::min(rw.y2, b.y2);
        const int rx1 = std::max(rw.x1, b.x1), rx2 = std::min(rw.x2, b.x2);
        if (ry1 >= ry2 || rx1 >= rx2 || img.empty()) return;
        // Columns that map straight into img without clamping.
        const int cx1 = std::max(rx1, org.x1), cx2 = std::min(rx2, org.x1 + img.w);
        const bool fast = ncomp == 4 && bytesPerComp == 4;
        pf(ry2 - ry1, [&](int j0, int j1) {
            for (int y = ry1 + j0; y < ry1 + j1; ++y) {
                unsigned char* r = row(y);
                const int iy = std::min(std::max(y - org.y1, 0), img.h - 1);
                if (fast && cx1 < cx2) {
                    memcpy(r + (size_t)(cx1 - b.x1) * 4 * sizeof(float), img.at(cx1 - org.x1, iy),
                           (size_t)(cx2 - cx1) * 4 * sizeof(float));
                }
                for (int x = rx1; x < rx2; ++x) {
                    if (fast && x >= cx1 && x < cx2) {
                        x = cx2 - 1;
                        continue;
                    }
                    const int ix = std::min(std::max(x - org.x1, 0), img.w - 1);
                    const float* s = img.at(ix, iy);
                    unsigned char* p = r + (size_t)(x - b.x1) * ncomp * bytesPerComp;
                    float c[4] = {s[0], s[1], s[2], s[3]};
                    if (ncomp == 1) c[0] = s[3];
                    for (int k = 0; k < ncomp; ++k) {
                        if (bytesPerComp == 4) {
                            reinterpret_cast<float*>(p)[k] = c[k];
                        } else {
                            const float v = std::min(std::max(c[k], 0.f), 1.f);
                            if (bytesPerComp == 2) reinterpret_cast<unsigned short*>(p)[k] = (unsigned short)(v * 65535.f + 0.5f);
                            else p[k] = (unsigned char)(v * 255.f + 0.5f);
                        }
                    }
                }
            }
        });
    }
};

using FramePtr = std::shared_ptr<const mosh::Image>;

static FramePtr fetchFrame(OfxImageClipHandle clip, double t, const OfxRectI& r, const mosh::ParallelFor& pf) {
    HostImage hi;
    if (!hi.fetch(clip, t)) return nullptr;
    auto img = std::make_shared<mosh::Image>();
    hi.toImage(*img, r, pf);
    return img;
}

// A frame for cut detection: either already converted, or a raw host image
// (scanning back through the clip only needs a sparse sample of each frame).
struct ScanFrame {
    OfxRectI rect{0, 0, 0, 0};  // the output's pixel rectangle; (x, y) below are relative to it
    HostImage host;
    FramePtr img;

    void set(const FramePtr& p) {
        host.release();
        img = p;
    }
    bool fetch(OfxImageClipHandle clip, double t) {
        img.reset();
        return host.fetch(clip, t);
    }
    int width() const { return rect.x2 - rect.x1; }
    int height() const { return rect.y2 - rect.y1; }
    float sample(int x, int y, int ch) const {
        if (img) return img->at(x, y)[ch];
        const int hx = x + rect.x1 - host.b.x1, hy = y + rect.y1 - host.b.y1;
        if (hx < 0 || hy < 0 || hx >= host.width() || hy >= host.height()) return 0.f;
        return host.sample(hx, hy, ch);
    }
    void swap(ScanFrame& o) {
        host.swap(o.host);
        img.swap(o.img);
    }
};

// Subsampled mean absolute RGB difference between two frames.
static float quickDiff(const ScanFrame& a, const ScanFrame& b) {
    if (a.width() != b.width() || a.height() != b.height()) return -1.f;
    const int step = std::max(2, a.width() / 160);
    double s = 0;
    long n = 0;
    for (int y = 0; y < a.height(); y += step)
        for (int x = 0; x < a.width(); x += step) {
            for (int c = 0; c < 3; ++c) s += std::fabs(a.sample(x, y, c) - b.sample(x, y, c));
            ++n;
        }
    return n ? (float)(s / (3.0 * n)) : 0.f;
}

static float meanAbsDiff(const mosh::Image& a, const mosh::Image& b) {
    if (a.empty() || b.empty() || a.w != b.w || a.h != b.h) return -1.f;
    double s = 0;
    const int step = 4;
    long n = 0;
    for (int y = 0; y < a.h; y += step)
        for (int x = 0; x < a.w; x += step) {
            const float* p = a.at(x, y);
            const float* q = b.at(x, y);
            s += std::fabs(p[0] - q[0]) + std::fabs(p[1] - q[1]) + std::fabs(p[2] - q[2]);
            ++n;
        }
    return n ? (float)(s / (3.0 * n)) : 0.f;
}

// ---------------------------------------------------------------- settings

struct Settings {
    int cutMode = 0;
    int cutFrame = 0;
    float cutThreshold = 0.12f;
    int moshLength = 30;
    int block = 16;
    int range = 48;
    float motionScale = 1.f;
    float residual = 1.f;
    int repeat = 1;
    float intraThreshold = 0.35f;
    bool smooth = true;
    bool fillFlat = true;
    float intraRate = 0.6f;
    bool subpel = true;
    double recoverStart = 0.8;
    int recoverMode = kDefaultRecovery;
    int recoverCurve = 0;
    float recoverSoftness = 0.5f;
    bool revealInvert = false;
    int direction = 0;
    int maxSteps = 48;
    bool clampLevels = true;
    int seed = 0;
    int view = 0;
};

static Settings readSettings(OfxParamSetHandle ps, double t) {
    Settings s;
    s.cutMode = getInt(ps, P::kCutMode, t, 0);
    s.cutFrame = getInt(ps, P::kCutFrame, t, 0);
    s.cutThreshold = (float)getDouble(ps, P::kCutThreshold, t, 0.12);
    s.moshLength = std::max(2, getInt(ps, P::kMoshLength, t, 30));
    const int bi = std::min(std::max(getInt(ps, P::kBlockSize, t, 1), 0), 3);
    s.block = kBlockSizes[bi];
    s.range = getInt(ps, P::kSearchRange, t, 48);
    s.motionScale = (float)getDouble(ps, P::kMotionScale, t, 1.0);
    s.residual = (float)getDouble(ps, P::kResidual, t, 1.0);
    s.repeat = std::max(1, getInt(ps, P::kRepeat, t, 1));
    s.intraThreshold = (float)getDouble(ps, P::kIntra, t, 0.35);
    s.smooth = getInt(ps, P::kSmooth, t, 1) != 0;
    s.fillFlat = getInt(ps, P::kFillFlat, t, 1) != 0;
    s.intraRate = (float)getDouble(ps, P::kIntraRate, t, 0.6);
    s.subpel = getInt(ps, P::kSubpel, t, 1) != 0;
    if (getInt(ps, P::kMoshStyle, t, 0) == kStylePOnly) {
        s.intraRate = 0.f;
        s.subpel = false;
    }
    s.recoverStart = getDouble(ps, P::kRecoverStart, t, 0.8);
    s.recoverMode = std::min(std::max(getInt(ps, P::kRecoverMode, t, kDefaultRecovery), 0),
                             mosh::kRecoveryCount - 1);
    s.recoverCurve = std::min(std::max(getInt(ps, P::kRecoverCurve, t, 0), 0), 3);
    s.recoverSoftness = (float)getDouble(ps, P::kRecoverSoftness, t, 0.5);
    s.revealInvert = getInt(ps, P::kRevealInvert, t, 0) != 0;
    s.direction = std::min(std::max(getInt(ps, P::kDirection, t, 0), 0), mosh::kDirectionCount - 1);
    s.maxSteps = std::max(1, getInt(ps, P::kMaxSteps, t, 48));
    s.clampLevels = getInt(ps, P::kClamp, t, 1) != 0;
    s.seed = getInt(ps, P::kSeed, t, 0);
    s.view = getInt(ps, P::kView, t, 0);
    return s;
}

// Hash of everything that changes the accumulated (pre-recovery) picture.
static uint64_t chainHash(const Settings& s, const OfxRectI& r) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix((uint64_t)(uint32_t)r.x1); mix((uint64_t)(uint32_t)r.y1); mix((uint64_t)(uint32_t)r.x2); mix((uint64_t)(uint32_t)r.y2);
    auto mixf = [&](float f) { uint32_t u; memcpy(&u, &f, 4); mix(u); };
    mix((uint64_t)s.block); mix((uint64_t)s.range); mixf(s.motionScale); mixf(s.residual);
    mix((uint64_t)s.repeat); mixf(s.intraThreshold); mix(s.smooth); mix(s.fillFlat);
    mix((uint64_t)s.maxSteps); mix(s.clampLevels); mixf(s.intraRate); mix(s.subpel);
    const auto mode = (mosh::Recovery)s.recoverMode;
    if (mode == mosh::Recovery::MotionRefresh || mode == mosh::Recovery::Natural) {
        // these modes recover inside the chain
        mix(0x5eed + (uint64_t)mode); mixf((float)s.recoverStart); mix((uint64_t)s.moshLength);
        mix((uint64_t)s.recoverCurve); mixf(s.recoverSoftness); mix((uint64_t)s.seed);
    }
    return h;
}

// Natural recovery heals over the whole mosh, starting at the cut; Recovery
// Curve shapes the pace.
static float naturalAmount(const Settings& s, double p) {
    if (p >= 1.0) return 1.f;
    return mosh::shapeCurve((float)std::max(p, 0.0), (mosh::Curve)s.recoverCurve);
}

static float recoveryAmount(const Settings& s, double p) {
    if (p >= 1.0) return 1.f;
    if (s.recoverStart >= 1.0) return 0.f;
    const float q = (float)std::min(std::max((p - s.recoverStart) / (1.0 - s.recoverStart), 0.0), 1.0);
    return mosh::shapeCurve(q, (mosh::Curve)s.recoverCurve);
}

// ---------------------------------------------------------------- per-instance cache

// Rendering is stateless in principle, but recomputing the whole chain since
// the cut for every frame is O(n^2) and each frame fetched from the host may
// cost a decode. Two caches make sequential playback O(1) per frame without
// changing the result: consecutive-frame differences (cut detection) and the
// accumulated picture of recently rendered frames (chain).
struct Instance {
    std::mutex m;
    std::unordered_map<long, float> pairDiff;  // key f: diff(frame f-1, frame f)
    struct AccEntry {
        double t;
        double cutFrame;
        uint64_t hash;
        mosh::Image acc;
        std::vector<uint8_t> refreshed;  // Motion refresh: blocks already refreshed
    };
    std::deque<AccEntry> accCache;  // most recent last, at most kAccEntries
    // Last source frame converted by a render, so the next frame's render
    // does not fetch and convert it again.
    double frameT = 0;
    OfxRectI frameRect{0, 0, 0, 0};
    FramePtr frame;

    static constexpr size_t kAccEntries = 2;
    static constexpr size_t kMaxDiffs = 100000;

    void clear() {
        std::lock_guard<std::mutex> g(m);
        pairDiff.clear();
        accCache.clear();
        frame.reset();
    }
    FramePtr getFrame(double t, const OfxRectI& r) {
        std::lock_guard<std::mutex> g(m);
        if (!frame || frameT != t || memcmp(&frameRect, &r, sizeof r)) return nullptr;
        return frame;
    }
    void putFrame(double t, const OfxRectI& r, const FramePtr& f) {
        std::lock_guard<std::mutex> g(m);
        frameT = t;
        frameRect = r;
        frame = f;
    }
    bool getDiff(long f, float& d) {
        std::lock_guard<std::mutex> g(m);
        auto it = pairDiff.find(f);
        if (it == pairDiff.end()) return false;
        d = it->second;
        return true;
    }
    void putDiff(long f, float d) {
        std::lock_guard<std::mutex> g(m);
        if (pairDiff.size() > kMaxDiffs) pairDiff.clear();
        pairDiff[f] = d;
    }
    bool getAcc(double t, double cut, uint64_t hash, mosh::Image& out, std::vector<uint8_t>& refreshed) {
        std::lock_guard<std::mutex> g(m);
        for (const AccEntry& e : accCache)
            if (e.t == t && e.cutFrame == cut && e.hash == hash) {
                out = e.acc;
                refreshed = e.refreshed;
                return true;
            }
        return false;
    }
    void putAcc(double t, double cut, uint64_t hash, mosh::Image acc, const std::vector<uint8_t>& refreshed) {
        std::lock_guard<std::mutex> g(m);
        for (AccEntry& e : accCache)
            if (e.t == t && e.cutFrame == cut && e.hash == hash) {
                e.acc = std::move(acc);
                e.refreshed = refreshed;
                return;
            }
        accCache.push_back({t, cut, hash, std::move(acc), refreshed});
        while (accCache.size() > kAccEntries) accCache.pop_front();
    }
};

static Instance* instanceData(OfxImageEffectHandle inst) {
    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(inst, &props);
    void* p = nullptr;
    if (props) gProp->propGetPointer(props, kOfxPropInstanceData, 0, &p);
    return static_cast<Instance*>(p);
}

// ---------------------------------------------------------------- debug text

// 3x5 pixel font, enough for the diagnostics overlay.
static const char* kFontChars = "0123456789.-: ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char* kFont[] = {
    "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
    "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111",
    "000000000000010", "000000111000000", "000010000010000", "000000000000000",
    "010101111101101", "110101110101110", "111100100100111", "110101101101110", "111100111100111",
    "111100111100100", "111100101101111", "101101111101101", "111010010010111", "001001001101111",
    "101101110101101", "100100100100111", "101111111101101", "110101101101101", "111101101101111",
    "111101111100100", "111101101111001", "110101110101101", "111100111001111", "111010010010010",
    "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010",
    "111001010100111",
};

// OFX images are stored bottom-up, so row 0 of mosh::Image is the bottom
// line; y here counts from the top of the picture.
static void drawText(mosh::Image& img, int x0, int yTop, int scale, const std::string& text) {
    auto put = [&](int x, int y, float v) {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h) return;
        float* p = img.at(x, img.h - 1 - y);
        p[0] = p[1] = p[2] = v;
        p[3] = 1.f;
    };
    const int bw = (int)text.size() * 4 * scale + 2 * scale, bh = 7 * scale;
    for (int y = yTop - scale; y < yTop - scale + bh; ++y)
        for (int x = x0 - scale; x < x0 - scale + bw; ++x) put(x, y, 0.f);
    for (size_t ci = 0; ci < text.size(); ++ci) {
        const char* pos = strchr(kFontChars, (char)toupper((unsigned char)text[ci]));
        if (!pos) continue;
        const char* g = kFont[pos - kFontChars];
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 3; ++c) {
                if (g[r * 3 + c] != '1') continue;
                for (int yy = 0; yy < scale; ++yy)
                    for (int xx = 0; xx < scale; ++xx)
                        put(x0 + (int)ci * 4 * scale + c * scale + xx, yTop + r * scale + yy, 1.f);
            }
    }
}

static std::string fmt(const char* f, double v) {
    char buf[64];
    snprintf(buf, sizeof buf, f, v);
    return buf;
}

// ---------------------------------------------------------------- plan

struct Plan {
    Settings s;
    double t = 0;
    bool cutKnown = false;      // manual mode knows up front; auto decides in render
    double cutFrame = 0;        // first frame of the new shot
    float scanMaxDiff = -1.f;   // largest consecutive-frame difference seen while scanning
    int scanned = 0;            // frames looked at by cut detection this render
    double progress = 0;        // 0..1 through the mosh
    int nFrames = 0;            // frames since the cut
    int steps = 0;              // motion steps to run
    std::vector<double> times;  // steps + 1 frames, ascending, times[0] == cutFrame
    float q = 0.f;              // recovery progress
    bool passThrough = true;    // output == source
};

static void finishPlan(Plan& pl) {
    const int L = std::max(1, pl.s.moshLength);
    pl.progress = (pl.t - pl.cutFrame) / (double)L;
    pl.q = recoveryAmount(pl.s, pl.progress);
    pl.passThrough = pl.t < pl.cutFrame - 1e-9 || pl.progress >= 1.0;
    pl.nFrames = std::max(0, (int)std::lround(pl.t - pl.cutFrame));
    pl.steps = std::min(pl.nFrames, pl.s.maxSteps);
    pl.times.clear();
    if (pl.steps > 0) {
        for (int i = 0; i <= pl.steps; ++i) {
            const double ft = pl.cutFrame + std::round((double)i * (double)pl.nFrames / (double)pl.steps);
            if (pl.times.empty() || ft > pl.times.back()) pl.times.push_back(ft);
        }
        pl.steps = (int)pl.times.size() - 1;
    }
}

static Plan makePlan(OfxImageEffectHandle inst, double t) {
    Plan pl;
    pl.t = t;
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    pl.s = readSettings(ps, t);
    if (pl.s.cutMode == 1) {
        pl.cutKnown = true;
        pl.cutFrame = pl.s.cutFrame;
        finishPlan(pl);
    }
    return pl;
}

// Auto cut detection: walk back from t and stop at the most recent pair of
// consecutive frames that differ a lot. That pair is where the removed
// keyframe would have been. cur (frame t) and prev (frame t - 1) are used
// when already converted, saving host fetches.
static void detectCut(OfxImageClipHandle src, Instance* cache, Plan& pl, const OfxRectI& rect, const FramePtr& cur,
                      const FramePtr& prev) {
    pl.cutKnown = false;
    pl.scanMaxDiff = -1.f;
    const int L = std::max(1, pl.s.moshLength);
    ScanFrame a, b;
    a.rect = b.rect = rect;
    bool haveA = false;
    for (int k = 0; k < L; ++k) {
        const double fa = pl.t - k;
        float d;
        if (!cache || !cache->getDiff((long)std::lround(fa), d)) {
            if (!haveA) {
                if (k == 0 && cur) a.set(cur);
                else if (!a.fetch(src, fa)) return;
            }
            if (k == 0 && prev) b.set(prev);
            else if (!b.fetch(src, fa - 1.0)) break;
            d = quickDiff(a, b);
            ++pl.scanned;
            if (cache) cache->putDiff((long)std::lround(fa), d);
            a.swap(b);
            haveA = true;
        } else {
            haveA = false;  // a no longer corresponds to fa - 1
        }
        pl.scanMaxDiff = std::max(pl.scanMaxDiff, d);
        if (d > pl.s.cutThreshold) {
            pl.cutKnown = true;
            pl.cutFrame = fa;
            break;
        }
    }
    if (pl.cutKnown) finishPlan(pl);
    else pl.passThrough = true;
}
// ---------------------------------------------------------------- actions

static OfxStatus onLoad() {
    if (!gHost) return kOfxStatErrMissingHostFeature;
    gEffect = (OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gProp = (OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gParam = (OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    gThread = (OfxMultiThreadSuiteV1*)gHost->fetchSuite(gHost->host, kOfxMultiThreadSuite, 1);
    if (!gEffect || !gProp || !gParam) return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

static OfxStatus describe(OfxImageEffectHandle effect) {
    // Every frame depends on earlier frames of the clip; a host that only
    // hands out the current frame cannot run this effect.
    int temporal = 1;
    if (gHost->host && gProp->propGetInt(gHost->host, kOfxImageEffectPropTemporalClipAccess, 0, &temporal) == kOfxStatOK &&
        !temporal)
        return kOfxStatErrMissingHostFeature;

    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(effect, &props);

    gProp->propSetString(props, kOfxPropLabel, 0, "Datamosh");
    gProp->propSetString(props, kOfxPropShortLabel, 0, "Datamosh");
    gProp->propSetString(props, kOfxPropLongLabel, 0, "Datamosh");
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Datamosh");
    gProp->propSetString(props, kOfxPropPluginDescription, 0,
                         "Datamosh. Simulates a dropped keyframe at a cut: the previous picture is dragged along "
                         "the motion vectors of the new shot, with P-frame residuals smearing in the new colours, "
                         "until the real picture refreshes block by block. Apply to a clip that contains a cut; "
                         "the cut is detected automatically or set manually.");

    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);

    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthShort);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 2, kOfxBitDepthByte);

    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
    // Any order works, but in order each frame costs one motion step instead
    // of a chain since the cut (OFX 1.4: 2 = prefers sequential).
    gProp->propSetInt(props, kOfxImageEffectInstancePropSequentialRender, 0, 2);
    return kOfxStatOK;
}

static void defineClip(OfxImageEffectHandle effect, const char* name, bool temporal) {
    OfxPropertySetHandle cp = nullptr;
    gEffect->clipDefine(effect, name, &cp);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentRGB);
    if (temporal) gProp->propSetInt(cp, kOfxImageEffectPropTemporalClipAccess, 0, 1);
}

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
    defineClip(effect, kOfxImageEffectSimpleSourceClipName, true);
    defineClip(effect, kOfxImageEffectOutputClipName, false);

    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(effect, &ps);

    beginGroup(ps, P::kGrpCut, "Cut Detection", "Where the keyframe goes missing.", true);
    defineChoice(ps, P::kCutMode, "Cut",
                 "Where the keyframe goes missing. Auto looks back for the most recent hard cut in the clip; "
                 "Manual uses the frame below.",
                 {"Auto (scene-change detection)", "Manual frame"}, 0);
    defineInt(ps, P::kCutFrame, "Cut Frame",
              "Manual mode: first frame of the new shot, in the clip's own frame numbers (see Diagnostics).",
              0, 0, 1000000);
    defineDouble(ps, P::kCutThreshold, "Cut Threshold",
                 "Auto mode: how different two consecutive frames must be to count as a cut. "
                 "Diagnostics shows the largest difference seen.",
                 0.12, 0.02, 1.0);

    beginGroup(ps, P::kGrpMosh, "Mosh", "How the old picture is dragged and smeared.", true);
    defineChoice(ps, P::kMoshStyle, "Mosh Style",
                 "Intra + half-pel: the new shot replaces the old picture from the first frame after the cut, "
                 "the way it does in a real stream with a deleted keyframe: encoder-style intra/inter mode "
                 "decisions per macroblock and half-pixel motion compensation. "
                 "P-only, full-pel: the old picture is smeared by whole-pixel motion compensation alone until "
                 "recovery starts, as in versions before 1.5. "
                 "Switching also moves Residual Strength and Recovery to the style's defaults if they were left "
                 "at the other style's defaults.",
                 {"Intra + half-pel (new shot from the cut)", "P-only, full-pel (smear until recovery)"}, 0);
    defineInt(ps, P::kMoshLength, "Mosh Length",
              "How many frames after the cut the mosh lasts (including recovery).", 30, 2, 600);
    defineDouble(ps, P::kResidual, "Residual Strength",
                 "How much of each P-frame residual bleeds in. 0 = pure smear of the old picture, "
                 "1 = codec-accurate, higher overdrives the colours.",
                 1.0, 0.0, 3.0);
    defineDouble(ps, P::kIntraRate, "New Data Rate",
                 "How readily blocks of the new shot are coded intra (fresh picture) instead of predicted, "
                 "from the very first frame after the cut. This is how the new shot replaces the old one in a "
                 "real mosh: fresh blocks appear where prediction fails and are then dragged along with the "
                 "rest. 0 = never, about 0.5 behaves like an Xvid encoder, higher replaces faster.",
                 0.6, 0.0, 1.0);
    defineInt(ps, P::kRepeat, "Bloom (P-frame Repeat)",
              "Decode every P-frame this many times, like duplicated P-frames in a hex-edited stream. "
              "Makes the picture flow further and the colours bloom.",
              1, 1, 12);
    defineDouble(ps, P::kMotionScale, "Motion Scale",
                 "Multiplies the motion vectors. Above 1 exaggerates the drag, negative inverts it.", 1.0, -4.0, 4.0);
    defineBool(ps, P::kClamp, "Clamp Levels", "Keep accumulated colours inside 0..1, like an 8-bit decoder.", true);

    beginGroup(ps, P::kGrpMotion, "Motion Estimation", "How motion vectors are found in the new shot.", true);
    defineChoice(ps, P::kBlockSize, "Block Size",
                 "Macroblock size in pixels. Larger blocks look chunkier and render faster.",
                 {"8x8", "16x16", "32x32", "64x64"}, 1);
    defineInt(ps, P::kSearchRange, "Search Range", "Maximum motion per frame step, in pixels.", 48, 4, 256);
    defineDouble(ps, P::kIntra, "Intra Threshold",
                 "Blocks whose motion prediction is worse than this show the new picture directly, "
                 "like intra blocks in a P-frame. 1 disables.",
                 0.35, 0.02, 1.0);
    defineBool(ps, P::kSubpel, "Half-pixel Motion",
               "Half-pixel vectors with interpolation, like MPEG-4 and H.264. The smear softens as it travels "
               "instead of staying pixel-sharp.",
               true);
    defineBool(ps, P::kSmooth, "Smooth Vectors", "Median-filter the vector field so the flow stays coherent.", true);
    defineBool(ps, P::kFillFlat, "Fill Flat Areas",
               "Give textureless blocks the motion of their neighbours, as encoders do, so flat sky and walls flow too.",
               true);

    beginGroup(ps, P::kGrpRecovery, "Recovery", "How the real picture of the new shot comes back.", true);
    defineChoice(ps, P::kRecoverMode, "Recovery",
                 "How the real picture returns. Random intra refresh: macroblocks are refreshed in random "
                 "order. Crossfade: linear blend. IDR frame: the whole picture at once, like the next "
                 "keyframe. Motion-adaptive intra refresh: macroblocks where the new shot moves are refreshed "
                 "first, inside the mosh, and keep being predicted afterwards, so the smear flows through them. "
                 "High-frequency first: the high band (edges, texture) of the new shot arrives over the smeared "
                 "low band, then the low band follows. Luma-keyed refresh: the new shot shows through its bright "
                 "areas first. Low-frequency first: the low band (colours) washes in under the smeared detail, "
                 "then the high band follows. Residual-ordered refresh: areas with the smallest residual against "
                 "the new shot settle first, the worst mismatches last. Intra refresh sweep: a refresh front "
                 "sweeps across the frame with a ragged macroblock edge, like periodic intra refresh. Slice "
                 "displacement: slices of macroblocks shift the new shot in and push the mosh out. Encoder intra "
                 "decisions: the stream heals the way a real one does, the encoder codes more and more "
                 "macroblocks as intra until the whole picture is fresh.",
                 {"Random intra refresh", "Crossfade", "IDR frame", "Motion-adaptive intra refresh",
                  "High-frequency first", "Luma-keyed refresh", "Low-frequency first", "Residual-ordered refresh",
                  "Intra refresh sweep", "Slice displacement", "Encoder intra decisions"},
                 kDefaultRecovery);
    defineDouble(ps, P::kRecoverStart, "Recovery Start",
                 "Fraction of the mosh at which the real picture starts to come back. Not used by Encoder intra "
                 "decisions, which heals over the whole mosh.",
                 0.8, 0.05, 1.0);
    defineChoice(ps, P::kRecoverCurve, "Recovery Curve",
                 "Pace of the recovery: steady, slow-fast-slow, slow start, or slow end.",
                 {"Linear", "Smooth", "Ease in", "Ease out"}, 0);
    defineDouble(ps, P::kRecoverSoftness, "Softness",
                 "How gradual the recovery is. Random intra refresh: how long each block takes to fade in. "
                 "Motion-adaptive intra refresh: how long the smear lingers in refreshed blocks. High-frequency "
                 "first: how long the old low band stays after the high band arrives. Low-frequency first: how "
                 "long the smeared detail stays after the colours arrive. Luma-keyed refresh, Residual-ordered "
                 "refresh, Intra refresh sweep: width of the soft edge. Slice displacement: how long each slice "
                 "takes to shift in.",
                 0.5, 0.0, 1.0);
    defineBool(ps, P::kRevealInvert, "Dark Areas First",
               "Luma-keyed refresh: reveal dark areas first instead of bright ones.",
               false);
    defineChoice(ps, P::kDirection, "Direction",
                 "Intra refresh sweep and Slice displacement: where the new shot comes in from. Slices are rows "
                 "for left/right and columns for top/bottom; the centre options make neighbouring slices shift "
                 "in opposite ways.",
                 {"Left to right", "Right to left", "Top to bottom", "Bottom to top", "From centre", "To centre"},
                 0);
    defineInt(ps, P::kSeed, "Seed",
              "Changes the random refresh order, the ragged sweep edge, the slices and the random intra blocks "
              "of Encoder intra decisions.", 0, 0, 9999);

    beginGroup(ps, P::kGrpAdvanced, "Performance & Debug", "Speed limits and debug views.", true);
    defineInt(ps, P::kMaxSteps, "Max Motion Steps",
              "Upper bound on frame pairs analysed per output frame when the cache is cold. Lower is faster; "
              "motion is then sampled at a coarser stride.",
              48, 1, 240);
    defineChoice(ps, P::kView, "View", "Debug views: the motion vectors used for the current frame, or numbers.",
                 {"Result", "Motion vectors", "Diagnostics"}, 0);
    gGroup = nullptr;
    return kOfxStatOK;
}

// Grey out parameters that do nothing with the current choices.
static void setEnabled(OfxParamSetHandle ps, const char* name, bool on) {
    OfxParamHandle h = nullptr;
    OfxPropertySetHandle props = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, &props) != kOfxStatOK || !props) return;
    gProp->propSetInt(props, kOfxParamPropEnabled, 0, on ? 1 : 0);
}

static int currentInt(OfxParamSetHandle ps, const char* name, int fallback) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) return fallback;
    int v = fallback;
    gParam->paramGetValue(h, &v);
    return v;
}

static void updateEnabled(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const bool manual = currentInt(ps, P::kCutMode, 0) == 1;
    setEnabled(ps, P::kCutFrame, manual);
    setEnabled(ps, P::kCutThreshold, !manual);
    const auto mode = (mosh::Recovery)currentInt(ps, P::kRecoverMode, kDefaultRecovery);
    const bool soft = mode != mosh::Recovery::Dissolve && mode != mosh::Recovery::HardCut &&
                      mode != mosh::Recovery::Natural;
    const bool directional = mode == mosh::Recovery::Wipe || mode == mosh::Recovery::Slice;
    setEnabled(ps, P::kRecoverSoftness, soft);
    setEnabled(ps, P::kRevealInvert, mode == mosh::Recovery::LumaReveal);
    setEnabled(ps, P::kDirection, directional);
    setEnabled(ps, P::kSeed, mode == mosh::Recovery::Blocks || mode == mosh::Recovery::MotionRefresh ||
                                 mode == mosh::Recovery::Natural || directional);
    setEnabled(ps, P::kRecoverCurve, mode != mosh::Recovery::HardCut);
    setEnabled(ps, P::kRecoverStart, mode != mosh::Recovery::Natural);
    const bool pOnly = currentInt(ps, P::kMoshStyle, 0) == kStylePOnly;
    setEnabled(ps, P::kIntraRate, !pOnly);
    setEnabled(ps, P::kSubpel, !pOnly);
}

static double currentDouble(OfxParamSetHandle ps, const char* name, double fallback) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) return fallback;
    double v = fallback;
    gParam->paramGetValue(h, &v);
    return v;
}

// Each style has its own defaults for Residual Strength and Recovery. When
// the user switches style, parameters still at the old style's default
// follow; anything the user changed is left alone.
static void applyStyleDefaults(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const int style = currentInt(ps, P::kMoshStyle, 0);
    const StyleDefaults& to = kStyleDefaults[style == kStylePOnly ? 1 : 0];
    const StyleDefaults& from = kStyleDefaults[style == kStylePOnly ? 0 : 1];
    OfxParamHandle h = nullptr;
    gParam->paramEditBegin(ps, "Mosh Style");
    if (std::fabs(currentDouble(ps, P::kResidual, from.residual) - from.residual) < 1e-6 &&
        gParam->paramGetHandle(ps, P::kResidual, &h, nullptr) == kOfxStatOK && h)
        gParam->paramSetValue(h, to.residual);
    if (currentInt(ps, P::kRecoverMode, from.recovery) == from.recovery &&
        gParam->paramGetHandle(ps, P::kRecoverMode, &h, nullptr) == kOfxStatOK && h)
        gParam->paramSetValue(h, to.recovery);
    gParam->paramEditEnd(ps);
}
static OfxStatus createInstance(OfxImageEffectHandle inst) {
    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(inst, &props);
    if (props) gProp->propSetPointer(props, kOfxPropInstanceData, 0, new Instance());
    updateEnabled(inst);
    return kOfxStatOK;
}

static OfxStatus destroyInstance(OfxImageEffectHandle inst) {
    Instance* d = instanceData(inst);
    delete d;
    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(inst, &props);
    if (props) gProp->propSetPointer(props, kOfxPropInstanceData, 0, nullptr);
    return kOfxStatOK;
}

static OfxStatus getClipPreferences(OfxImageEffectHandle, OfxPropertySetHandle outArgs) {
    gProp->propSetInt(outArgs, kOfxImageEffectFrameVarying, 0, 1);
    return kOfxStatOK;
}

static OfxStatus isIdentity(OfxImageEffectHandle inst, OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
    double t = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    const Plan pl = makePlan(inst, t);
    if (pl.s.view != 0 || !pl.cutKnown || !pl.passThrough) return kOfxStatReplyDefault;
    gProp->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
    gProp->propSetDouble(outArgs, kOfxPropTime, 0, t);
    return kOfxStatOK;
}

// Tell the host which frames render() may ask for. Timeline hosts only
// guarantee frames announced here.
static OfxStatus getFramesNeeded(OfxImageEffectHandle inst, OfxPropertySetHandle inArgs,
                                 OfxPropertySetHandle outArgs) {
    double t = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    const Plan pl = makePlan(inst, t);

    std::vector<double> ts{t};
    if (pl.cutKnown) {
        if (!pl.passThrough) {
            ts.push_back(pl.cutFrame - 1.0);
            ts.insert(ts.end(), pl.times.begin(), pl.times.end());
        }
    } else {
        const int L = std::max(1, pl.s.moshLength);
        for (int k = 1; k <= L + 1; ++k) ts.push_back(t - k);
    }
    if (pl.s.view != 0) ts.push_back(t - 1.0);

    std::sort(ts.begin(), ts.end());
    ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
    int idx = 0;
    size_t i = 0;
    while (i < ts.size()) {  // coalesce consecutive frames into ranges
        size_t j = i;
        while (j + 1 < ts.size() && ts[j + 1] - ts[j] <= 1.0 + 1e-9) ++j;
        gProp->propSetDouble(outArgs, kOfxImageEffectSimpleSourceClipName, idx++, ts[i]);
        gProp->propSetDouble(outArgs, kOfxImageEffectSimpleSourceClipName, idx++, ts[j]);
        i = j + 1;
    }
    return kOfxStatOK;
}

static mosh::MotionParams motionParams(const Settings& s) {
    mosh::MotionParams mp;
    mp.block = s.block;
    mp.range = s.range;
    mp.smooth = s.smooth;
    mp.intraThreshold = s.intraThreshold;
    mp.fillFlat = s.fillFlat;
    mp.halfPel = s.subpel;
    mp.intraRate = s.intraRate;
    mp.blockStats = (mosh::Recovery)s.recoverMode == mosh::Recovery::Natural;
    return mp;
}

struct ChainResult {
    mosh::MotionField lastField;
    float firstPairDiff = -1.f;
    int stepsDone = 0;
    bool fromCache = false;
    bool aborted = false;
    std::vector<uint8_t> refreshed;  // Motion refresh state, carried along the chain
};

// Source frames for one render, converted to the output's pixel rectangle.
// The frame at the render time is fetched once and shared by every stage;
// the previous frame may come from the instance cache.
struct FrameSource {
    OfxImageClipHandle clip = nullptr;
    OfxRectI rect{0, 0, 0, 0};
    mosh::ParallelFor pf;
    double tCur = 0;
    FramePtr cur;
    double tPrev = 0;
    FramePtr prev;

    FramePtr get(double t) const {
        if (cur && t == tCur) return cur;
        if (prev && t == tPrev) return prev;
        return fetchFrame(clip, t, rect, pf);
    }
};

// Runs motion steps over frames `times` on top of acc.
static void runSteps(OfxImageEffectHandle inst, const FrameSource& fs, const Plan& pl,
                     const std::vector<double>& times, mosh::Image& acc, ChainResult& cr,
                     const mosh::ParallelFor& pf) {
    const Settings& s = pl.s;
    const bool refresh = (mosh::Recovery)s.recoverMode == mosh::Recovery::MotionRefresh;
    mosh::RefreshStep rs;
    rs.softness = s.recoverSoftness;
    rs.seed = s.seed;
    mosh::StepParams sp;
    sp.motionScale = s.motionScale;
    sp.residual = s.residual;
    sp.repeat = s.repeat;
    sp.clampLevels = s.clampLevels;
    const bool natural = (mosh::Recovery)s.recoverMode == mosh::Recovery::Natural;
    sp.intraBlocks = s.intraThreshold < 1.f || s.intraRate > 0.f || natural;
    mosh::NaturalStep ns;
    ns.baseRate = s.intraRate;
    ns.motionScale = s.motionScale;
    ns.seed = s.seed;
    const mosh::MotionParams mp = motionParams(s);

    if (times.size() < 2) return;
    mosh::Image scratch;
    FramePtr prev = fs.get(times[0]);
    if (!prev) return;
    for (size_t i = 1; i < times.size(); ++i) {
        FramePtr cur = fs.get(times[i]);
        if (!cur) break;
        if (gEffect->abort(inst)) {
            cr.aborted = true;
            break;
        }
        if (i == 1) cr.firstPairDiff = meanAbsDiff(*prev, *cur);
        cr.lastField = mosh::estimateMotion(*prev, *cur, mp, pf);
        if (natural) {
            ns.q = naturalAmount(s, (times[i] - pl.cutFrame) / (double)std::max(1, s.moshLength));
            ns.frame = std::lround(times[i]);
            mosh::naturalIntra(cr.lastField, ns, cr.refreshed);
        }
        mosh::moshStep(*prev, *cur, cr.lastField, sp, acc, scratch, pf);
        if (natural) mosh::naturalAfter(*cur, cr.lastField, ns, cr.refreshed, acc, pf);
        if (refresh) {
            rs.q = recoveryAmount(s, (times[i] - pl.cutFrame) / (double)std::max(1, s.moshLength));
            mosh::refreshStep(*prev, *cur, cr.lastField, rs, cr.refreshed, acc, pf);
        }
        prev = std::move(cur);
        ++cr.stepsDone;
    }
}
static void drawDiagnostics(mosh::Image& img, const Plan& pl, const ChainResult& cr, int outW, int outH) {
    double mv = 0;
    int intra = 0;
    if (cr.lastField.bw > 0) {
        for (size_t i = 0; i < cr.lastField.dx.size(); ++i) {
            mv += std::hypot((double)cr.lastField.dx[i], (double)cr.lastField.dy[i]);
            intra += cr.lastField.intra[i];
        }
        mv /= (double)cr.lastField.dx.size();
        intra = (int)(100.0 * intra / (double)cr.lastField.dx.size());
    }
    const int sc = std::max(2, img.h / 180);
    const int lh = 8 * sc;
    int y = 2 * sc;
    auto line = [&](const std::string& s) { drawText(img, 2 * sc, y, sc, s); y += lh; };
    line("DATAMOSH " PLUGIN_VERSION_STRING);
    line("T " + fmt("%.1f", pl.t) + "  PROG " + fmt("%.3f", pl.progress) +
         (pl.cutKnown ? "  CUT " + fmt("%.0f", pl.cutFrame) : "  NO CUT"));
    line("SCAN MAX DIFF " + fmt("%.4f", pl.scanMaxDiff) + "  THR " + fmt("%.3f", pl.s.cutThreshold) +
         "  FETCHED " + fmt("%.0f", pl.scanned));
    line("N " + fmt("%.0f", pl.nFrames) + "  STEPS " + fmt("%.0f", pl.steps) + "  DONE " + fmt("%.0f", cr.stepsDone) +
         (cr.fromCache ? "  CACHED" : "  FULL"));
    line("PAIR DIFF " + fmt("%.4f", cr.firstPairDiff));
    line("MV " + fmt("%.2f", mv) + " PX  INTRA " + fmt("%.0f", intra) + "  Q " + fmt("%.2f", pl.q) +
         "  REC " + fmt("%.0f", pl.s.recoverMode) +
         ((mosh::Recovery)pl.s.recoverMode == mosh::Recovery::MotionRefresh ||
                  (mosh::Recovery)pl.s.recoverMode == mosh::Recovery::Natural
              ? "  REFR " + fmt("%.0f", 100.0 * mosh::refreshedShare(cr.refreshed))
              : std::string()));
    line("IMG " + fmt("%.0f", outW) + "X" + fmt("%.0f", outH) + "  BLK " + fmt("%.0f", pl.s.block));
}

static OfxStatus render(OfxImageEffectHandle inst, OfxPropertySetHandle inArgs) {
    double t = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    OfxRectI rw{};
    gProp->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &rw.x1);
    double scale[2] = {1.0, 1.0};
    gProp->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, scale);

    OfxImageClipHandle src = nullptr, outClip = nullptr;
    gEffect->clipGetHandle(inst, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
    gEffect->clipGetHandle(inst, kOfxImageEffectOutputClipName, &outClip, nullptr);
    if (!src || !outClip) return kOfxStatFailed;

    HostImage out;
    if (!out.fetch(outClip, t)) return kOfxStatFailed;
    // Every frame is converted to the output's pixel rectangle.
    const OfxRectI R = out.b;

    Instance* cache = instanceData(inst);
    Plan pl = makePlan(inst, t);
    // Proxy / draft renders: keep blocks and vectors the same size relative
    // to the picture.
    if (scale[0] > 0.0 && std::fabs(scale[0] - 1.0) > 1e-6) {
        pl.s.block = std::max(4, (int)std::lround(pl.s.block * scale[0]));
        pl.s.range = std::max(2, (int)std::lround(pl.s.range * scale[0]));
    }

    const mosh::ParallelFor pf = hostParallelFor;
    FrameSource fs;
    fs.clip = src;
    fs.rect = R;
    fs.pf = pf;
    fs.tCur = t;
    fs.cur = fetchFrame(src, t, R, pf);
    if (!fs.cur) return kOfxStatFailed;
    if (cache) {
        fs.tPrev = t - 1.0;
        fs.prev = cache->getFrame(t - 1.0, R);
        cache->putFrame(t, R, fs.cur);
    }

    if (!pl.cutKnown) detectCut(src, cache, pl, R, fs.cur, fs.prev);

    const Settings& s = pl.s;

    if (s.view == 0 && pl.passThrough) {
        out.fromImage(*fs.cur, R, rw, pf);
        return kOfxStatOK;
    }

    mosh::Image acc;
    ChainResult cr;
    const bool unstrided = pl.steps == pl.nFrames;
    uint64_t hash = 0;
    if (!pl.passThrough) {
        hash = chainHash(s, R);
        // Fast path: the previous frame's accumulated picture is cached and the
        // chain is unstrided, so one more step gives exactly the same result.
        if (unstrided && pl.nFrames >= 1 && cache && cache->getAcc(t - 1.0, pl.cutFrame, hash, acc, cr.refreshed)) {
            cr.fromCache = true;
            runSteps(inst, fs, pl, {t - 1.0, t}, acc, cr, pf);
        } else {
            // Reference picture: the last frame before the cut, the one the decoder is stuck with.
            FramePtr ref = fs.get(pl.cutFrame - 1.0);
            acc = ref ? *ref : *fs.cur;
            runSteps(inst, fs, pl, pl.times, acc, cr, pf);
        }
    } else {
        acc = *fs.cur;
    }
    // An aborted chain is incomplete and must not seed later frames.
    const bool store = cache && unstrided && !pl.passThrough && !cr.aborted;

    if (s.view == 1) {
        if (cr.lastField.bw == 0) {
            if (FramePtr a = fs.get(t - 1.0)) {
                cr.lastField = mosh::estimateMotion(*a, *fs.cur, motionParams(s), pf);
                cr.firstPairDiff = meanAbsDiff(*a, *fs.cur);
            }
        }
        if (cr.lastField.bw > 0) {
            const int w = acc.w, h = acc.h;
            if (store) cache->putAcc(t, pl.cutFrame, hash, std::move(acc), cr.refreshed);
            mosh::Image vis;
            mosh::visualizeMotion(cr.lastField, s.range, w, h, vis, pf);
            out.fromImage(vis, R, rw, pf);
            return kOfxStatOK;
        }
    }

    mosh::Image result;
    const auto mode = (mosh::Recovery)s.recoverMode;
    // These modes recover inside the chain; compose only matters at the very end.
    const bool inChain = mode == mosh::Recovery::MotionRefresh || mode == mosh::Recovery::Natural;
    if (!pl.passThrough && pl.q > 0.f && !(inChain && pl.q < 1.f)) {
        mosh::ComposeParams cp;
        cp.mode = mode;
        cp.softness = s.recoverSoftness;
        cp.block = s.block;
        cp.seed = s.seed;
        cp.invertReveal = s.revealInvert;
        cp.direction = (mosh::Direction)s.direction;
        mosh::compose(acc, *fs.cur, cp, pl.q, result, pf);
        if (store) cache->putAcc(t, pl.cutFrame, hash, std::move(acc), cr.refreshed);
    } else {
        if (store) cache->putAcc(t, pl.cutFrame, hash, acc, cr.refreshed);
        result = std::move(acc);
    }

    if (s.view == 2) drawDiagnostics(result, pl, cr, out.width(), out.height());

    out.fromImage(result, R, rw, pf);
    return kOfxStatOK;
}
static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    try {
        if (!strcmp(action, kOfxActionLoad)) return onLoad();
        if (!strcmp(action, kOfxActionUnload)) return kOfxStatOK;
        if (!strcmp(action, kOfxActionDescribe)) return describe(effect);
        if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
        if (!strcmp(action, kOfxActionCreateInstance)) return createInstance(effect);
        if (!strcmp(action, kOfxActionDestroyInstance)) return destroyInstance(effect);
        if (!strcmp(action, kOfxActionInstanceChanged) || !strcmp(action, kOfxActionPurgeCaches) ||
            !strcmp(action, kOfxActionSyncPrivateData)) {
            if (Instance* d = instanceData(effect)) d->clear();
            if (!strcmp(action, kOfxActionInstanceChanged)) {
                char* name = nullptr;
                char* reason = nullptr;
                if (inArgs) {
                    gProp->propGetString(inArgs, kOfxPropName, 0, &name);
                    gProp->propGetString(inArgs, kOfxPropChangeReason, 0, &reason);
                }
                if (name && reason && !strcmp(name, P::kMoshStyle) && !strcmp(reason, kOfxChangeUserEdited))
                    applyStyleDefaults(effect);
                updateEnabled(effect);
            }
            return kOfxStatOK;
        }
        if (!strcmp(action, kOfxImageEffectActionGetClipPreferences)) return getClipPreferences(effect, outArgs);
        if (!strcmp(action, kOfxImageEffectActionGetFramesNeeded)) return getFramesNeeded(effect, inArgs, outArgs);
        if (!strcmp(action, kOfxImageEffectActionIsIdentity)) return isIdentity(effect, inArgs, outArgs);
        if (!strcmp(action, kOfxImageEffectActionRender)) return render(effect, inArgs);
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

static void setHost(OfxHost* host) { gHost = host; }

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi, 1, PLUGIN_ID, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR, setHost, mainEntry,
};

extern "C" {
OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin* OfxGetPlugin(int n) { return n == 0 ? &gPlugin : nullptr; }
}
