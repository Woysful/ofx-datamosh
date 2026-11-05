// OpenFX filter plugin: Datamosh.
// Written against the raw OFX C API so it has no dependency besides the
// OpenFX headers.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <ctime>
#include <deque>
#include <clocale>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "mosh_core.h"
#include "mosh_gpu.h"

#define PLUGIN_ID "com.github.ofxdatamosh.Datamosh"
#define PLUGIN_VERSION_MAJOR 1
#define PLUGIN_VERSION_MINOR 5
#define PLUGIN_VERSION_STRING "1.5.0"

static OfxHost* gHost = nullptr;
static OfxImageEffectSuiteV1* gEffect = nullptr;
static OfxPropertySuiteV1* gProp = nullptr;
static OfxParameterSuiteV1* gParam = nullptr;
static OfxMultiThreadSuiteV1* gThread = nullptr;
static OfxMessageSuiteV1* gMessage = nullptr;  // optional

// ---------------------------------------------------------------- params

namespace P {
const char* kCutMode = "cutMode";           // choice: auto / manual / keyframes
const char* kCutFrame = "cutFrame";         // int
const char* kCutKeys = "cutKeys";           // double, its keyframes mark cuts; the value is unused
const char* kCutMarker = "cutMarker";       // hidden one-entry choice: Cut Markers of earlier development builds
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
const char* kMoshType = "moshType";         // choice: transition / repeat
const char* kRepeatFrames = "repeatFrames"; // int: Repeat, P-frames looped
const char* kIntraRate = "intraRate";       // double 0..1: encoder-style intra decision
const char* kSubpel = "halfPelMotion";      // bool
const char* kRecoverStart = "recoverStart"; // double 0..1
const char* kRecoverMode = "recoverMode";   // choice
const char* kRecoverCurve = "recoverCurve"; // choice
const char* kRecoverSoftness = "recoverSoftness"; // double 0..1
const char* kRevealInvert = "revealInvert"; // bool
const char* kDirection = "recoverDirection"; // choice: Wipe / Slice direction
const char* kMaxSteps = "maxSteps";         // int, hidden: kept so old projects load; chains always step frame by frame
const char* kCheckpointMB = "checkpointMemory"; // int MB, shared by all instances
const char* kClamp = "clampLevels";         // bool
const char* kSeed = "seed";                 // int
const char* kView = "view";                 // choice: result / motion vectors / diagnostics
const char* kDevice = "renderDevice";       // choice: auto / CPU / CUDA / OpenCL (mosh_gpu::Preference)
const char* kPresetName = "presetName";     // string
const char* kPresetSave = "presetSave";     // push button
const char* kPresetList = "presetList";     // choice: saved presets
const char* kPresetLoad = "presetLoad";     // push button
const char* kPresetDelete = "presetDelete"; // push button

const char* kGrpPresets = "grpPresets";
const char* kGrpCut = "grpCut";
const char* kGrpMosh = "grpMosh";
const char* kGrpMotion = "grpMotion";
const char* kGrpRecovery = "grpRecovery";
const char* kGrpAdvanced = "grpAdvanced";
}  // namespace P

static const int kDefaultRecovery = (int)mosh::Recovery::Natural;
static const int kStylePOnly = 1;
static const int kCutManual = 1;
static const int kCutKeyframes = 2;
static const int kMoshRepeat = 1;
static const int kMaxRepeatFrames = 30;

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
    int moshType = 0;
    int repeatFrames = 1;
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
    int checkpointMB = 1024;
    bool clampLevels = true;
    int seed = 0;
    int view = 0;
    int device = 0;  // mosh_gpu::Preference; results are the same on every device
};

static Settings readSettings(OfxParamSetHandle ps, double t) {
    Settings s;
    s.cutMode = getInt(ps, P::kCutMode, t, 0);
    s.cutFrame = getInt(ps, P::kCutFrame, t, 0);
    s.cutThreshold = (float)getDouble(ps, P::kCutThreshold, t, 0.12);
    s.moshLength = std::max(2, getInt(ps, P::kMoshLength, t, 30));
    s.moshType = getInt(ps, P::kMoshType, t, 0) == kMoshRepeat ? kMoshRepeat : 0;
    s.repeatFrames = std::min(std::max(getInt(ps, P::kRepeatFrames, t, 1), 1), kMaxRepeatFrames);
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
    s.checkpointMB = std::max(0, getInt(ps, P::kCheckpointMB, t, 1024));
    s.clampLevels = getInt(ps, P::kClamp, t, 1) != 0;
    s.seed = getInt(ps, P::kSeed, t, 0);
    s.view = getInt(ps, P::kView, t, 0);
    s.device = std::min(std::max(getInt(ps, P::kDevice, t, 0), 0), 3);
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
    mix(s.clampLevels); mixf(s.intraRate); mix(s.subpel);
    if (s.moshType == kMoshRepeat) {
        mix(0x7e9ea7); mix((uint64_t)s.repeatFrames);
    }
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

// ---------------------------------------------------------------- chain checkpoints

// a is a whole number of frames (at least one) before t.
static bool wholeFramesBefore(double a, double t) {
    const double d = t - a;
    return d > 0.5 && std::fabs(d - std::round(d)) < 1e-6;
}

// Accumulated pictures saved every few frames while a chain renders, so a
// render that jumps into a mosh continues from the nearest one before it
// instead of starting again at the cut. One store for the whole process:
// the memory limit covers every instance, the least recently used go first.
class CheckpointStore {
public:
    // Time of the newest checkpoint of the chain a whole number of frames
    // before t, or -HUGE_VAL.
    double latest(const void* owner, double t, double cut, uint64_t hash) {
        std::lock_guard<std::mutex> g(m_);
        double best = -HUGE_VAL;
        for (const Entry& e : entries_)
            if (e.owner == owner && e.cut == cut && e.hash == hash && e.t > cut && wholeFramesBefore(e.t, t) &&
                e.t > best)
                best = e.t;
        return best;
    }
    bool has(const void* owner, double t, double cut, uint64_t hash) {
        std::lock_guard<std::mutex> g(m_);
        return find(owner, t, cut, hash) != entries_.end();
    }
    bool get(const void* owner, double t, double cut, uint64_t hash, mosh::Image& acc,
             std::vector<uint8_t>& refreshed) {
        std::lock_guard<std::mutex> g(m_);
        auto it = find(owner, t, cut, hash);
        if (it == entries_.end()) return false;
        acc = it->acc;
        refreshed = it->refreshed;
        entries_.splice(entries_.end(), entries_, it);
        return true;
    }
    void put(const void* owner, double t, double cut, uint64_t hash, mosh::Image acc,
             const std::vector<uint8_t>& refreshed, size_t budget) {
        const size_t bytes = acc.px.size() * sizeof(float) + refreshed.size();
        std::lock_guard<std::mutex> g(m_);
        if (find(owner, t, cut, hash) != entries_.end() || bytes > budget) return;
        trimLocked(budget - bytes);
        entries_.push_back({owner, t, cut, hash, std::move(acc), refreshed, bytes});
        bytes_ += bytes;
    }
    // Drops the least recently used checkpoints until at most budget bytes are left.
    void trim(size_t budget) {
        std::lock_guard<std::mutex> g(m_);
        trimLocked(budget);
    }
    void forget(const void* owner) {
        std::lock_guard<std::mutex> g(m_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->owner == owner) {
                bytes_ -= it->bytes;
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    struct Entry {
        const void* owner;
        double t, cut;
        uint64_t hash;
        mosh::Image acc;
        std::vector<uint8_t> refreshed;
        size_t bytes;
    };
    std::list<Entry>::iterator find(const void* owner, double t, double cut, uint64_t hash) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->owner == owner && it->t == t && it->cut == cut && it->hash == hash) return it;
        return entries_.end();
    }
    void trimLocked(size_t budget) {
        while (bytes_ > budget && !entries_.empty()) {
            bytes_ -= entries_.front().bytes;
            entries_.pop_front();
        }
    }
    std::mutex m_;
    std::list<Entry> entries_;  // least recently used first
    size_t bytes_ = 0;
};

static CheckpointStore gCheckpoints;

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

    // Repeat: the looped source frames and their motion fields, which every
    // frame of the mosh steps through again.
    struct HeldFrame {
        double t;
        OfxRectI r;
        FramePtr f;
    };
    std::deque<HeldFrame> held;  // most recently used last
    struct FieldEntry {
        uint64_t hash;
        double t;  // the field's earlier frame
        mosh::MotionField mf;
    };
    std::deque<FieldEntry> fields;
    static constexpr size_t kHeldFrames = 9;
    static constexpr size_t kFieldEntries = 32;

    // Unstrided chain renders in progress. Hosts render several frames at
    // once during playback; a render whose nearest usable picture is an
    // earlier frame still being rendered waits for that frame instead of
    // rebuilding the chain from the cut, which costs a step and a host fetch
    // per frame since the cut.
    struct Busy {
        double t, cut;
        uint64_t hash;
        std::chrono::steady_clock::time_point beat;  // start or last finished step
    };
    using BusyIt = std::list<Busy>::iterator;
    std::list<Busy> busy;
    std::condition_variable busyDone;
    // Frames whose picture went into the CPU or the GPU cache, newest last.
    struct Stored {
        double t, cut;
        uint64_t hash;
    };
    std::deque<Stored> stored;
    static constexpr size_t kStoredEntries = 8;
    // A render that has neither fetched a frame nor finished a step for this
    // long may be waiting on the host itself; its followers stop waiting for it.
    static constexpr std::chrono::seconds kStall{5};

    ~Instance() {
        gCheckpoints.forget(this);
        mosh_gpu::forget(this);
    }

    static bool before(double a, double t) { return wholeFramesBefore(a, t); }
    // Newest picture of the chain before t that a render could continue
    // from: cached on the CPU or the GPU, or a checkpoint.
    double latestStoredLocked(double t, double cut, uint64_t hash) const {
        double best = gCheckpoints.latest(this, t, cut, hash);
        for (const Stored& s : stored)
            if (s.cut == cut && s.hash == hash && s.t >= cut && before(s.t, t) && s.t > best) best = s.t;
        return best;
    }
    void noteStoredLocked(double t, double cut, uint64_t hash) {
        for (auto it = stored.begin(); it != stored.end(); ++it)
            if (it->t == t && it->cut == cut && it->hash == hash) {
                stored.erase(it);
                break;
            }
        stored.push_back({t, cut, hash});
        while (stored.size() > kStoredEntries) stored.pop_front();
    }
    void noteStored(double t, double cut, uint64_t hash) {
        std::lock_guard<std::mutex> g(m);
        noteStoredLocked(t, cut, hash);
    }
    // Registers a render of frame t of a chain. Waits first while an earlier
    // frame of the chain, newer than every stored picture, is being rendered,
    // unless the host aborts this render (scrubbing).
    BusyIt enter(double t, double cut, uint64_t hash, const std::function<bool()>& aborted, double& waitedMs) {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        std::unique_lock<std::mutex> lk(m);
        for (;;) {
            const double have = latestStoredLocked(t, cut, hash);
            const Busy* ahead = nullptr;
            for (const Busy& b : busy)
                if (b.cut == cut && b.hash == hash && b.t > have && before(b.t, t) && (!ahead || b.t > ahead->t))
                    ahead = &b;
            if (!ahead || clock::now() - ahead->beat > kStall) break;
            busyDone.wait_for(lk, std::chrono::milliseconds(100));
            lk.unlock();  // never call the host with m held
            const bool stop = aborted();
            lk.lock();
            if (stop) break;
        }
        waitedMs = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
        busy.push_back({t, cut, hash, clock::now()});
        return std::prev(busy.end());
    }
    void beat(BusyIt it) {
        std::lock_guard<std::mutex> g(m);
        it->beat = std::chrono::steady_clock::now();
    }
    void leave(BusyIt it) {
        {
            std::lock_guard<std::mutex> g(m);
            busy.erase(it);
        }
        busyDone.notify_all();
    }

    void clear() {
        {
            std::lock_guard<std::mutex> g(m);
            pairDiff.clear();
            accCache.clear();
            stored.clear();
            frame.reset();
            held.clear();
            fields.clear();
        }
        busyDone.notify_all();
        gCheckpoints.forget(this);
        // Outside m: a GPU render holds the GPU while it takes m.
        mosh_gpu::forget(this);
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
    FramePtr getHeld(double t, const OfxRectI& r) {
        std::lock_guard<std::mutex> g(m);
        for (auto it = held.begin(); it != held.end(); ++it)
            if (it->t == t && !memcmp(&it->r, &r, sizeof r)) {
                HeldFrame h = *it;
                held.erase(it);
                held.push_back(h);
                return h.f;
            }
        return nullptr;
    }
    void putHeld(double t, const OfxRectI& r, const FramePtr& f) {
        std::lock_guard<std::mutex> g(m);
        for (auto it = held.begin(); it != held.end(); ++it)
            if (it->t == t && !memcmp(&it->r, &r, sizeof r)) {
                held.erase(it);
                break;
            }
        held.push_back({t, r, f});
        while (held.size() > kHeldFrames) held.pop_front();
    }
    bool getField(uint64_t hash, double t, mosh::MotionField& mf) {
        std::lock_guard<std::mutex> g(m);
        for (const FieldEntry& e : fields)
            if (e.hash == hash && e.t == t) {
                mf = e.mf;
                return true;
            }
        return false;
    }
    void putField(uint64_t hash, double t, const mosh::MotionField& mf) {
        std::lock_guard<std::mutex> g(m);
        for (const FieldEntry& e : fields)
            if (e.hash == hash && e.t == t) return;
        fields.push_back({hash, t, mf});
        while (fields.size() > kFieldEntries) fields.pop_front();
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
    // Time of the newest cached picture of the chain a whole number of
    // frames before t, or -HUGE_VAL.
    double latestAcc(double t, double cut, uint64_t hash) {
        std::lock_guard<std::mutex> g(m);
        double best = -HUGE_VAL;
        for (const AccEntry& e : accCache)
            if (e.cutFrame == cut && e.hash == hash && e.t >= cut && before(e.t, t) && e.t > best) best = e.t;
        return best;
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
                noteStoredLocked(t, cut, hash);
                return;
            }
        accCache.push_back({t, cut, hash, std::move(acc), refreshed});
        while (accCache.size() > kAccEntries) accCache.pop_front();
        noteStoredLocked(t, cut, hash);
    }
};

// Marks one render of a chain as in progress for as long as it lives.
struct ChainTicket {
    Instance* in = nullptr;
    Instance::BusyIt it;
    ChainTicket() = default;
    ChainTicket(const ChainTicket&) = delete;
    ChainTicket& operator=(const ChainTicket&) = delete;
    ~ChainTicket() { done(); }
    // Ends the render's turn once its picture is stored.
    void done() {
        if (in) in->leave(it);
        in = nullptr;
    }
    void beat() const {
        if (in) in->beat(it);
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
    int cutKeys = 0;            // keyframes of Cut Markers, -1: host does not report them
    double clipFirst = -1e300;  // first frame of the source clip, if the host says
    // Repeat: from cutFrame on, the loopLen P-frames before it are decoded
    // over and over. The chain then starts at cutFrame - 1 (the last real
    // frame) and times[0] == cutFrame - 1.
    bool repeat = false;
    int loopLen = 0;

    // The two source frames whose motion and residual make the step to
    // the picture of frame tau.
    void pair(double tau, double& prevT, double& curT) const {
        if (!repeat) {
            prevT = tau - 1.0;
            curT = tau;
            return;
        }
        const long k = std::lround(tau - cutFrame);
        const long i = ((k % loopLen) + loopLen) % loopLen;
        prevT = cutFrame - (double)loopLen - 1.0 + (double)i;
        curT = prevT + 1.0;
    }
};

static void finishPlan(Plan& pl) {
    const int L = std::max(1, pl.s.moshLength);
    pl.progress = (pl.t - pl.cutFrame) / (double)L;
    pl.q = recoveryAmount(pl.s, pl.progress);
    pl.passThrough = pl.t < pl.cutFrame - 1e-9 || pl.progress >= 1.0;
    pl.nFrames = std::max(0, (int)std::lround(pl.t - pl.cutFrame));
    pl.repeat = pl.s.moshType == kMoshRepeat;
    if (pl.repeat) {
        // Every looped P-frame needs the frame before it inside the clip.
        const double before = std::floor(pl.cutFrame - 1.0 - pl.clipFirst + 1e-6);
        pl.loopLen = (int)std::min((double)pl.s.repeatFrames, std::max(0.0, before));
        pl.s.repeatFrames = pl.loopLen;  // what the chain really loops, for chainHash
        pl.times.clear();
        pl.steps = 0;
        if (pl.loopLen < 1) pl.passThrough = true;
        if (pl.passThrough) return;
        pl.steps = pl.nFrames + 1;
        for (int i = 0; i <= pl.steps; ++i) pl.times.push_back(pl.cutFrame - 1.0 + (double)i);
        return;
    }
    // One step per frame, always: a chain sampled at a coarser stride could
    // not continue from the previous frame's result and would differ from
    // frame to frame. Checkpoints keep jumps into a mosh cheap instead.
    pl.steps = pl.nFrames;
    pl.times.clear();
    if (pl.steps > 0)
        for (int i = 0; i <= pl.steps; ++i) pl.times.push_back(pl.cutFrame + (double)i);
}

// Keyframes mode: the most recent keyframe of Cut Markers at or before t is
// the first frame of the new shot. A keyframe on the clip's first frame has
// no old picture before it and is skipped. Keyframes of the old Cut Markers
// menu count too.
static void keyframeCut(OfxParamSetHandle ps, Plan& pl) {
    if (!ps) return;
    const double first = pl.clipFirst;
    const double eps = 1e-6;
    bool reported = false;
    pl.cutKeys = 0;
    for (const char* name : {P::kCutKeys, P::kCutMarker}) {
        OfxParamHandle h = nullptr;
        if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) continue;
        unsigned int n = 0;
        if (!gParam->paramGetNumKeys || !gParam->paramGetKeyTime || gParam->paramGetNumKeys(h, &n) != kOfxStatOK)
            continue;
        reported = true;
        pl.cutKeys += (int)n;
        for (unsigned int i = 0; i < n; ++i) {
            OfxTime kt = 0;
            if (gParam->paramGetKeyTime(h, i, &kt) != kOfxStatOK) continue;
            kt = std::round(kt);
            if (kt <= first + eps || kt > pl.t + eps) continue;
            if (!pl.cutKnown || kt > pl.cutFrame) {
                pl.cutKnown = true;
                pl.cutFrame = kt;
            }
        }
    }
    if (!reported) pl.cutKeys = -1;
}

// Moves keyframes of the old Cut Markers menu to Cut Markers, so they show
// and can be edited. The old ones go only once every one is copied.
static void migrateCutKeys(OfxImageEffectHandle inst) {
    static thread_local bool busy = false;  // setting keys sends instance-changed again
    if (busy || !gParam->paramGetNumKeys || !gParam->paramGetKeyTime || !gParam->paramSetValueAtTime ||
        !gParam->paramDeleteAllKeys)
        return;
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    OfxParamHandle from = nullptr, to = nullptr;
    if (!ps || gParam->paramGetHandle(ps, P::kCutMarker, &from, nullptr) != kOfxStatOK || !from ||
        gParam->paramGetHandle(ps, P::kCutKeys, &to, nullptr) != kOfxStatOK || !to)
        return;
    unsigned int n = 0;
    if (gParam->paramGetNumKeys(from, &n) != kOfxStatOK || n == 0) return;
    std::vector<OfxTime> times;
    for (unsigned int i = 0; i < n; ++i) {
        OfxTime kt = 0;
        if (gParam->paramGetKeyTime(from, i, &kt) == kOfxStatOK) times.push_back(kt);
    }
    busy = true;
    gParam->paramEditBegin(ps, "Cut Markers");
    bool ok = times.size() == n;
    for (OfxTime kt : times) ok = gParam->paramSetValueAtTime(to, kt, 0.0) == kOfxStatOK && ok;
    if (ok) gParam->paramDeleteAllKeys(from);
    gParam->paramEditEnd(ps);
    busy = false;
}static Plan makePlan(OfxImageEffectHandle inst, double t) {
    Plan pl;
    pl.t = t;
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    pl.s = readSettings(ps, t);
    OfxImageClipHandle src = nullptr;
    OfxPropertySetHandle cp = nullptr;
    if (gEffect->clipGetHandle(inst, kOfxImageEffectSimpleSourceClipName, &src, &cp) == kOfxStatOK && cp) {
        double r[2] = {0, 0};
        if (gProp->propGetDoubleN(cp, kOfxImageEffectPropFrameRange, 2, r) == kOfxStatOK) pl.clipFirst = r[0];
    }
    if (pl.s.cutMode == kCutManual) {
        pl.cutKnown = true;
        pl.cutFrame = pl.s.cutFrame;
        finishPlan(pl);
    } else if (pl.s.cutMode == kCutKeyframes) {
        keyframeCut(ps, pl);
        if (pl.cutKnown) finishPlan(pl);
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
    gMessage = (OfxMessageSuiteV1*)gHost->fetchSuite(gHost->host, kOfxMessageSuite, 1);
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

static std::vector<std::string> presetMenu();

static OfxStatus describeInContext(OfxImageEffectHandle effect) {
    defineClip(effect, kOfxImageEffectSimpleSourceClipName, true);
    defineClip(effect, kOfxImageEffectOutputClipName, false);

    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(effect, &ps);

    beginGroup(ps, P::kGrpPresets, "Presets",
               "Save your settings under a name and load them on any clip, in any project. A preset holds "
               "everything except Cut Frame, the cut keyframes and View. Presets are stored in presets.ini in "
               "your user settings folder.",
               false);
    {
        OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeString, P::kPresetName, "Preset Name",
                                             "Name for Save Preset. Picking a saved preset below fills it in, so "
                                             "Save Preset then updates that preset.");
        gProp->propSetString(p, kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine);
        gProp->propSetString(p, kOfxParamPropDefault, 0, "");
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
        gProp->propSetInt(p, kOfxParamPropEvaluateOnChange, 0, 0);
    }
    defineParam(ps, kOfxParamTypePushButton, P::kPresetSave, "Save Preset",
                "Save the current settings under Preset Name. A preset with the same name is replaced.");
    {
        OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeChoice, P::kPresetList, "Saved Presets",
                                             "Presets in the presets file, by name. Hosts that do not update "
                                             "menus while running show changes after a restart.");
        const std::vector<std::string> names = presetMenu();
        for (size_t i = 0; i < names.size(); ++i)
            gProp->propSetString(p, kOfxParamPropChoiceOption, (int)i, names[i].c_str());
        gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
        gProp->propSetInt(p, kOfxParamPropEvaluateOnChange, 0, 0);
        // A stored index could name another preset once the file changes.
        gProp->propSetInt(p, kOfxParamPropPersistant, 0, 0);
    }
    defineParam(ps, kOfxParamTypePushButton, P::kPresetLoad, "Load Preset",
                "Set the parameters stored in the preset picked in Saved Presets.");
    defineParam(ps, kOfxParamTypePushButton, P::kPresetDelete, "Delete Preset",
                "Remove the preset picked in Saved Presets from the presets file.");

    beginGroup(ps, P::kGrpCut, "Cut Detection", "Where the keyframe goes missing.", true);
    defineChoice(ps, P::kCutMode, "Cut",
                 "Where the keyframe goes missing. Auto looks back for the most recent hard cut in the clip; "
                 "Manual uses the frame below; Keyframes uses the keyframes of Cut Markers.",
                 {"Auto (scene-change detection)", "Manual frame", "Keyframes (Cut Markers)"}, 0);
    defineInt(ps, P::kCutFrame, "Cut Frame",
              "Manual mode: first frame of the new shot (Repeat: the first repeated frame), in the clip's own "
              "frame numbers (see Diagnostics).",
              0, 0, 1000000);
    {
        // Only the keyframes matter. A number rather than a one-entry menu:
        // hosts show keyframes of numeric parameters in their keyframe views.
        OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeDouble, P::kCutKeys, "Cut Markers",
                                             "Keyframes mode: every keyframe of this parameter marks a cut (the "
                                             "first frame of the new shot; Repeat: where a repeat starts). Add a "
                                             "keyframe at each cut you want moshed; drag a keyframe to move its cut, "
                                             "delete it to drop the cut. The value does nothing, only where the "
                                             "keyframes are. A keyframe on the clip's first frame is ignored.");
        gProp->propSetDouble(p, kOfxParamPropDefault, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropMax, 0, 1.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMin, 0, 0.0);
        gProp->propSetDouble(p, kOfxParamPropDisplayMax, 0, 1.0);
        gProp->propSetString(p, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 1);
    }
    {
        // Cut Markers of earlier development builds: a one-entry menu, whose
        // keyframes hosts do not show. Hidden, kept so that the keyframes of
        // projects made with them still count until they are moved over.
        OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeChoice, P::kCutMarker, "Cut Markers (old)",
                                             "Replaced by Cut Markers.");
        gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "Place keyframe on cut");
        gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 1);
        gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);
    }    defineDouble(ps, P::kCutThreshold, "Cut Threshold",
                 "Auto mode: how different two consecutive frames must be to count as a cut. "
                 "Diagnostics shows the largest difference seen.",
                 0.12, 0.02, 1.0);

    beginGroup(ps, P::kGrpMosh, "Mosh", "How the old picture is dragged and smeared.", true);
    defineChoice(ps, P::kMoshType, "Mosh Type",
                 "Transition: the keyframe of the new shot is missing, so the old picture is dragged along by the "
                 "new shot's motion. Repeat: from the cut frame on, the last P-frames before it are decoded over "
                 "and over (duplicated P-frames), so the picture keeps flowing along their motion and the "
                 "residuals pile up; it needs no second shot. In both, the live clip comes back through Recovery.",
                 {"Transition (missing keyframe)", "Repeat (duplicated P-frames)"}, 0);
    defineInt(ps, P::kRepeatFrames, "Repeat Frames",
              "Repeat: how many of the last P-frames before the cut frame are looped. 1 repeats the motion of the "
              "last frame; more loop a longer stretch of motion. Up to 8 are kept in memory, longer loops fetch "
              "their frames from the host again.",
              1, 1, kMaxRepeatFrames);
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
              "How many frames from the cut the mosh lasts (including recovery).", 30, 2, 600);
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
    defineInt(ps, P::kCheckpointMB, "Jump Cache (MB)",
              "RAM for pictures saved every few frames of a mosh while it renders, so jumping or scrubbing into a "
              "mosh rendered before continues from the nearest one instead of starting again at the cut. Shared "
              "by all Datamosh effects, the oldest go first; 0 turns checkpoints off. Does not change the result.",
              1024, 0, 65536);
    {
        OfxPropertySetHandle p = defineParam(ps, kOfxParamTypeInteger, P::kMaxSteps, "Max Motion Steps",
                                             "No longer used: every frame of the mosh is one motion step.");
        gProp->propSetInt(p, kOfxParamPropDefault, 0, 48);
        gProp->propSetInt(p, kOfxParamPropSecret, 0, 1);
    }
    {
        OfxPropertySetHandle p = defineParam(
            ps, kOfxParamTypeChoice, P::kDevice, "Render Device",
            "Where the mosh is computed. The result is the same on every device. Auto uses an NVIDIA GPU through "
            "CUDA, or another GPU through OpenCL, and the CPU when there is none. Diagnostics shows the device used "
            "and why the GPU was not.");
        const char* opts[] = {"Auto (GPU if available)", "CPU", "GPU: CUDA (NVIDIA)", "GPU: OpenCL (AMD, Intel, NVIDIA)"};
        for (int i = 0; i < 4; ++i) gProp->propSetString(p, kOfxParamPropChoiceOption, i, opts[i]);
        gProp->propSetInt(p, kOfxParamPropDefault, 0, 0);
        gProp->propSetInt(p, kOfxParamPropAnimates, 0, 0);
    }
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
    const int cutMode = currentInt(ps, P::kCutMode, 0);
    setEnabled(ps, P::kCutFrame, cutMode == kCutManual);
    setEnabled(ps, P::kCutKeys, cutMode == kCutKeyframes);
    setEnabled(ps, P::kCutThreshold, cutMode == 0);
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
    setEnabled(ps, P::kRepeatFrames, currentInt(ps, P::kMoshType, 0) == kMoshRepeat);
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
// ---------------------------------------------------------------- presets

// Parameters a preset stores. Cut Frame and the cut keyframes belong to one
// clip and View is a debug switch, so they are left out.
struct PresetParam {
    const char* name;
    bool isDouble;
};
static const PresetParam kPresetParams[] = {
    {P::kCutMode, false},     {P::kCutThreshold, true},
    {P::kMoshType, false},    {P::kRepeatFrames, false},
    {P::kMoshStyle, false},   {P::kMoshLength, false},    {P::kResidual, true},     {P::kIntraRate, true},
    {P::kRepeat, false},      {P::kMotionScale, true},    {P::kClamp, false},       {P::kBlockSize, false},
    {P::kSearchRange, false}, {P::kSubpel, false},        {P::kIntra, true},        {P::kSmooth, false},
    {P::kFillFlat, false},    {P::kRecoverMode, false},   {P::kRecoverStart, true}, {P::kRecoverCurve, false},
    {P::kRecoverSoftness, true}, {P::kRevealInvert, false}, {P::kDirection, false}, {P::kSeed, false},
};

#ifdef _WIN32
typedef std::wstring PathStr;
static std::string pathToUtf8(const PathStr& p) {
    if (p.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(n, 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, p.c_str(), (int)p.size(), &out[0], n, nullptr, nullptr);
    return out;
}
static FILE* openFile(const PathStr& p, const char* mode) {
    const std::wstring m(mode, mode + strlen(mode));
    return _wfopen(p.c_str(), m.c_str());
}
static bool replaceFile(const PathStr& from, const PathStr& to) {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static void makeDir(const PathStr& p) { _wmkdir(p.c_str()); }
static void removeFile(const PathStr& p) { _wremove(p.c_str()); }
#else
typedef std::string PathStr;
static std::string pathToUtf8(const PathStr& p) { return p; }
static FILE* openFile(const PathStr& p, const char* mode) { return fopen(p.c_str(), mode); }
static bool replaceFile(const PathStr& from, const PathStr& to) { return rename(from.c_str(), to.c_str()) == 0; }
static void makeDir(const PathStr& p) { mkdir(p.c_str(), 0755); }
static void removeFile(const PathStr& p) { remove(p.c_str()); }
#endif

// presets.ini in the user's settings folder (created on first save):
//   Windows %APPDATA%\ofx-datamosh, macOS ~/Library/Application Support/ofx-datamosh,
//   elsewhere $XDG_CONFIG_HOME/ofx-datamosh or ~/.config/ofx-datamosh.
// OFX_DATAMOSH_PRESETS overrides the whole path.
static PathStr presetFile(bool create) {
#ifdef _WIN32
    if (const wchar_t* e = _wgetenv(L"OFX_DATAMOSH_PRESETS")) if (*e) return e;
    const wchar_t* app = _wgetenv(L"APPDATA");
    if (!app || !*app) return PathStr();
    const PathStr dir = PathStr(app) + L"\\ofx-datamosh";
    if (create) makeDir(dir);
    return dir + L"\\presets.ini";
#else
    if (const char* e = getenv("OFX_DATAMOSH_PRESETS")) if (*e) return e;
    const char* home = getenv("HOME");
#ifdef __APPLE__
    if (!home || !*home) return PathStr();
    const PathStr base = PathStr(home) + "/Library/Application Support";
#else
    const char* xdg = getenv("XDG_CONFIG_HOME");
    PathStr base;
    if (xdg && *xdg) base = xdg;
    else if (home && *home) base = PathStr(home) + "/.config";
    else return PathStr();
#endif
    const PathStr dir = base + "/ofx-datamosh";
    if (create)
        for (size_t i = 1; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == '/') makeDir(dir.substr(0, i));
    return dir + "/presets.ini";
#endif
}

typedef std::vector<std::pair<std::string, std::string>> PresetValues;
typedef std::vector<std::pair<std::string, PresetValues>> PresetFile;  // sections in file order

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// The file always uses '.' as decimal point, whatever locale the host runs in.
static char localeDecimalPoint() {
    const std::lconv* lc = std::localeconv();
    return lc && lc->decimal_point && lc->decimal_point[0] ? lc->decimal_point[0] : '.';
}

static bool parseNumber(const std::string& text, double& out) {
    std::string t = trim(text);
    const char dp = localeDecimalPoint();
    for (char& ch : t)
        if (ch == '.') ch = dp;
    if (t.empty()) return false;
    char* end = nullptr;
    out = strtod(t.c_str(), &end);
    return end && *end == '\0';
}

// Shortest form that reads back as the same double.
static std::string formatNumber(double d) {
    std::string s;
    const char dp = localeDecimalPoint();
    for (int prec = 6; prec <= 17; ++prec) {
        char buf[64];
        snprintf(buf, sizeof buf, "%.*g", prec, d);
        s = buf;
        for (char& ch : s)
            if (ch == dp) ch = '.';
        double r = 0;
        if (parseNumber(s, r) && r == d) break;
    }
    return s;
}

static PresetFile readPresetFile(const PathStr& path) {
    PresetFile pf;
    FILE* fp = path.empty() ? nullptr : openFile(path, "rb");
    if (!fp) return pf;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    fclose(fp);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            pf.push_back({trim(line.substr(1, line.size() - 2)), {}});
        } else if (!pf.empty()) {
            const size_t eq = line.find('=');
            if (eq != std::string::npos) pf.back().second.push_back({trim(line.substr(0, eq)), trim(line.substr(eq + 1))});
        }
    }
    return pf;
}

static bool writePresetFile(const PathStr& path, const PresetFile& pf) {
    std::string text = "# OFX Datamosh presets. Written by the plugin; one section per preset.\n";
    for (const auto& sec : pf) {
        text += "\n[" + sec.first + "]\n";
        for (const auto& kv : sec.second) text += kv.first + " = " + kv.second + "\n";
    }
#ifdef _WIN32
    const PathStr tmp = path + L".tmp";
#else
    const PathStr tmp = path + ".tmp";
#endif
    FILE* fp = openFile(tmp, "wb");
    if (!fp) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), fp) == text.size();
    if (fclose(fp) != 0 || !ok || !replaceFile(tmp, path)) {
        removeFile(tmp);
        return false;
    }
    return true;
}

// The text goes in as the format itself: some hosts show the format string
// without expanding arguments.
static void showMessage(OfxImageEffectHandle inst, const char* type, const std::string& text) {
    if (!gMessage || !gMessage->message) return;
    std::string fmtText;
    for (char ch : text) {
        fmtText += ch;
        if (ch == '%') fmtText += '%';
    }
    gMessage->message(inst, type, nullptr, fmtText.c_str());
}

// Saved Presets menu: entry 0 is a prompt, then the names in the presets
// file, sorted. A pick is resolved by the label the host holds at the picked
// index, not by the index itself, so a menu older than the file (another
// instance saved or deleted, or a host that keeps the menu it got when the
// plugin was described) still loads the preset the user saw.
static std::mutex gPresetMutex;  // serialises read-modify-write of the file
static const char* kPresetPrompt = "Pick a saved preset";
static const size_t kPresetNameMax = 64;

static std::vector<std::string> presetMenu() {
    std::vector<std::string> m;
    {
        std::lock_guard<std::mutex> g(gPresetMutex);
        for (const auto& sec : readPresetFile(presetFile(false))) m.push_back(sec.first);
    }
    auto lower = [](std::string s) {
        for (char& ch : s)
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        return s;
    };
    std::sort(m.begin(), m.end(), [&](const std::string& a, const std::string& b) {
        const std::string la = lower(a), lb = lower(b);
        return la != lb ? la < lb : a < b;
    });
    m.erase(std::unique(m.begin(), m.end()), m.end());
    m.insert(m.begin(), kPresetPrompt);
    return m;
}

static OfxPropertySetHandle presetListProps(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    OfxParamHandle h = nullptr;
    OfxPropertySetHandle props = nullptr;
    if (!ps || gParam->paramGetHandle(ps, P::kPresetList, &h, &props) != kOfxStatOK) return nullptr;
    return props;
}

// The menu entries as the host holds them for this instance.
static std::vector<std::string> hostMenu(OfxPropertySetHandle props) {
    std::vector<std::string> m;
    int n = 0;
    if (!props || gProp->propGetDimension(props, kOfxParamPropChoiceOption, &n) != kOfxStatOK) return m;
    for (int i = 0; i < n; ++i) {
        char* s = nullptr;
        if (gProp->propGetString(props, kOfxParamPropChoiceOption, i, &s) != kOfxStatOK) break;
        m.push_back(s ? s : "");
    }
    return m;
}

// Bring this instance's menu in line with the presets file, for hosts that
// update a choice menu at run time; returns the menu the host holds
// afterwards. Hosts such as Natron append an entry on every write to the
// option property, so the menu is cleared first and only touched when it
// differs. Entries a host refuses to drop are blanked.
static std::vector<std::string> refreshPresetMenu(OfxImageEffectHandle inst) {
    OfxPropertySetHandle props = presetListProps(inst);
    if (!props) return {};
    const std::vector<std::string> m = presetMenu();
    const std::vector<std::string> cur = hostMenu(props);
    if (cur == m) return cur;
    gProp->propReset(props, kOfxParamPropChoiceOption);
    for (size_t i = 0; i < m.size(); ++i)
        if (gProp->propSetString(props, kOfxParamPropChoiceOption, (int)i, m[i].c_str()) != kOfxStatOK) break;
    const std::vector<std::string> now = hostMenu(props);
    for (size_t i = m.size(); i < now.size(); ++i) gProp->propSetString(props, kOfxParamPropChoiceOption, (int)i, "");
    return hostMenu(props);
}

static int menuIndexOf(const std::vector<std::string>& menu, const std::string& name) {
    for (size_t i = 1; i < menu.size(); ++i)
        if (menu[i] == name) return (int)i;
    return -1;
}

static void setPick(OfxParamSetHandle ps, int idx) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, P::kPresetList, &h, nullptr) == kOfxStatOK && h) gParam->paramSetValue(h, idx);
}

// Name of the preset picked in Saved Presets, or "" for the prompt.
static std::string pickedPreset(OfxImageEffectHandle inst, OfxParamSetHandle ps) {
    const int idx = currentInt(ps, P::kPresetList, 0);
    const std::vector<std::string> menu = hostMenu(presetListProps(inst));
    if (idx < 1 || idx >= (int)menu.size() || menu[(size_t)idx] == kPresetPrompt) return std::string();
    return menu[(size_t)idx];
}
static std::string cleanPresetName(const std::string& raw) {
    std::string s;
    for (char ch : raw) s += (ch == '[' || ch == ']' || ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
    s = trim(s);
    if (s.size() > kPresetNameMax) s = trim(s.substr(0, kPresetNameMax));
    return s;
}

static std::string currentString(OfxParamSetHandle ps, const char* name) {
    OfxParamHandle h = nullptr;
    char* v = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) != kOfxStatOK || !h) return std::string();
    if (gParam->paramGetValue(h, &v) != kOfxStatOK || !v) return std::string();
    return v;
}

static void setString(OfxParamSetHandle ps, const char* name, const std::string& v) {
    OfxParamHandle h = nullptr;
    if (gParam->paramGetHandle(ps, name, &h, nullptr) == kOfxStatOK && h) gParam->paramSetValue(h, v.c_str());
}

static void savePreset(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const std::string name = cleanPresetName(currentString(ps, P::kPresetName));
    if (name.empty()) {
        showMessage(inst, kOfxMessageMessage, "Datamosh: type a name in Preset Name first.");
        return;
    }
    if (name == kPresetPrompt) {
        showMessage(inst, kOfxMessageMessage, "Datamosh: pick another name for the preset.");
        return;
    }
    PresetValues vals;
    for (const PresetParam& pp : kPresetParams) {
        OfxParamHandle h = nullptr;
        if (gParam->paramGetHandle(ps, pp.name, &h, nullptr) != kOfxStatOK || !h) continue;
        std::string v;
        if (pp.isDouble) {
            double d = 0;
            if (gParam->paramGetValue(h, &d) != kOfxStatOK) continue;
            v = formatNumber(d);
        } else {
            int i = 0;
            if (gParam->paramGetValue(h, &i) != kOfxStatOK) continue;
            v = std::to_string(i);
        }
        vals.push_back({pp.name, v});
    }
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    if (const std::tm* lt = std::localtime(&now)) std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M", lt);
    vals.insert(vals.begin(), {"saved", std::string(stamp) + ", plugin " PLUGIN_VERSION_STRING});

    const PathStr path = presetFile(true);
    if (path.empty()) {
        showMessage(inst, kOfxMessageError, "Datamosh: no user settings folder found, preset not saved.");
        return;
    }
    {
        std::lock_guard<std::mutex> g(gPresetMutex);
        PresetFile pf = readPresetFile(path);
        bool replaced = false;
        for (auto& sec : pf)
            if (sec.first == name) {
                sec.second = vals;
                replaced = true;
            }
        if (!replaced) pf.push_back({name, vals});
        if (!writePresetFile(path, pf)) {
            showMessage(inst, kOfxMessageError, "Datamosh: could not write " + pathToUtf8(path) + ".");
            return;
        }
    }
    const int idx = menuIndexOf(refreshPresetMenu(inst), name);
    const bool listed = idx > 0;
    if (listed) setPick(ps, idx);
    setString(ps, P::kPresetName, name);
    showMessage(inst, kOfxMessageMessage,
                "Datamosh: preset \"" + name + "\" saved." +
                    (listed ? "" : " It appears in Saved Presets after the host restarts."));
}

static void loadPreset(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const std::string name = pickedPreset(inst, ps);
    if (name.empty()) {
        showMessage(inst, kOfxMessageMessage, "Datamosh: pick a preset in Saved Presets first.");
        return;
    }
    const PresetFile pf = readPresetFile(presetFile(false));
    const PresetValues* vals = nullptr;
    for (const auto& sec : pf)
        if (sec.first == name) vals = &sec.second;
    if (!vals) {
        const int idx = menuIndexOf(refreshPresetMenu(inst), name);
        setPick(ps, idx > 0 ? idx : 0);
        showMessage(inst, kOfxMessageMessage, "Datamosh: preset \"" + name + "\" is not in the presets file any more.");
        return;
    }
    gParam->paramEditBegin(ps, "Load Preset");
    for (const auto& kv : *vals) {
        const PresetParam* pp = nullptr;
        for (const PresetParam& c : kPresetParams)
            if (kv.first == c.name) pp = &c;
        OfxParamHandle h = nullptr;
        if (!pp || gParam->paramGetHandle(ps, pp->name, &h, nullptr) != kOfxStatOK || !h) continue;
        double d = 0;
        if (!parseNumber(kv.second, d) || !std::isfinite(d)) continue;
        if (pp->isDouble) gParam->paramSetValue(h, d);
        else if (d == std::floor(d) && std::fabs(d) < 1e9) gParam->paramSetValue(h, (int)d);
    }
    gParam->paramEditEnd(ps);
    updateEnabled(inst);
}

static void deletePreset(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const std::string name = pickedPreset(inst, ps);
    if (name.empty()) {
        showMessage(inst, kOfxMessageMessage, "Datamosh: pick a preset in Saved Presets first.");
        return;
    }
    const PathStr path = presetFile(false);
    {
        std::lock_guard<std::mutex> g(gPresetMutex);
        PresetFile pf = readPresetFile(path);
        const size_t before = pf.size();
        pf.erase(std::remove_if(pf.begin(), pf.end(),
                                [&](const std::pair<std::string, PresetValues>& s) { return s.first == name; }),
                 pf.end());
        if (pf.size() != before && !writePresetFile(path, pf)) {
            showMessage(inst, kOfxMessageError, "Datamosh: could not write " + pathToUtf8(path) + ".");
            return;
        }
    }
    const bool listed = menuIndexOf(refreshPresetMenu(inst), name) > 0;
    setPick(ps, 0);
    showMessage(inst, kOfxMessageMessage,
                "Datamosh: preset \"" + name + "\" deleted." +
                    (listed ? " It leaves the Saved Presets menu after the host restarts." : ""));
}

// Picking a preset fills in its name, so Save Preset updates it.
static void presetPicked(OfxImageEffectHandle inst) {
    OfxParamSetHandle ps = nullptr;
    gEffect->getParamSet(inst, &ps);
    if (!ps) return;
    const std::string name = pickedPreset(inst, ps);
    if (name.empty()) return;
    setString(ps, P::kPresetName, name);
    // The menu may be older than the file; update it and keep the pick on
    // the same preset.
    const int idx = menuIndexOf(refreshPresetMenu(inst), name);
    if (idx > 0 && idx != currentInt(ps, P::kPresetList, 0)) setPick(ps, idx);
}
static OfxStatus createInstance(OfxImageEffectHandle inst) {
    OfxPropertySetHandle props = nullptr;
    gEffect->getPropertySet(inst, &props);
    if (props) gProp->propSetPointer(props, kOfxPropInstanceData, 0, new Instance());
    updateEnabled(inst);
    refreshPresetMenu(inst);
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
    if (pl.s.view != 0 || (!pl.cutKnown && pl.s.cutMode == 0) || !pl.passThrough) return kOfxStatReplyDefault;
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
            const auto mode = (mosh::Recovery)pl.s.recoverMode;
            if (pl.repeat) {
                for (int k = 0; k <= pl.loopLen; ++k) ts.push_back(pl.cutFrame - 1.0 - (double)k);
                // recovery inside the chain brings back the live clip at every step
                if (mode == mosh::Recovery::MotionRefresh || mode == mosh::Recovery::Natural)
                    ts.insert(ts.end(), pl.times.begin(), pl.times.end());
            } else {
                ts.insert(ts.end(), pl.times.begin(), pl.times.end());
            }
        }
    } else if (pl.s.cutMode == 0) {
        // The cut is not known yet: anything it could need, Repeat's looped
        // frames before it included.
        const int L = std::max(1, pl.s.moshLength);
        const int extra = pl.s.moshType == kMoshRepeat ? pl.s.repeatFrames : 0;
        for (int k = 1; k <= L + 1 + extra; ++k) ts.push_back(t - k);
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
    double startT = -HUGE_VAL;       // frame of the cached picture the chain continued from
    bool fromCheckpoint = false;     // ... which was a checkpoint
    bool aborted = false;
    std::vector<uint8_t> refreshed;  // Motion refresh state, carried along the chain
    std::string device;              // where it was computed, for Diagnostics
};

// Where and how often a render saves checkpoints of its chain.
struct CheckpointPlan {
    const void* owner = nullptr;
    double cut = 0;
    uint64_t hash = 0;
    size_t budget = 0;
    long every = 0;  // frames between checkpoints, counted from the cut; 0: none

    bool due(double ft) const {
        if (!owner || every <= 0) return false;
        const long i = std::lround(ft - cut);
        return i > 0 && i % every == 0 && std::fabs(ft - cut - (double)i) < 1e-6;
    }
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
    const ChainTicket* ticket = nullptr;  // told about every finished step
    const CheckpointPlan* checkpoints = nullptr;
    // Repeat: the looped frames (heldFrom..heldTo) are kept in, and their
    // motion fields cached by, this instance.
    Instance* held = nullptr;
    double heldFrom = 0, heldTo = -1;
    Instance* fields = nullptr;
    uint64_t hash = 0;
    mutable int fetched = 0;              // frames fetched from the host by get()

    FramePtr get(double t) const {
        if (cur && t == tCur) return cur;
        if (prev && t == tPrev) return prev;
        const bool keep = held && t >= heldFrom - 1e-6 && t <= heldTo + 1e-6;
        if (keep)
            if (FramePtr f = held->getHeld(t, rect)) return f;
        ++fetched;
        FramePtr f = fetchFrame(clip, t, rect, pf);
        if (keep && f) held->putHeld(t, rect, f);
        if (ticket) ticket->beat();
        return f;
    }
};

// Unit steps from frame a to frame t, a whole number of frames before it:
// the tail of the chain from the cut.
static std::vector<double> stepsFrom(double a, double t) {
    const long n = std::lround(t - a);
    std::vector<double> ts;
    ts.reserve((size_t)n + 1);
    for (long i = 0; i < n; ++i) ts.push_back(a + (double)i);
    ts.push_back(t);
    return ts;
}

// Runs motion steps to the pictures of frames `times` (after the first) on
// top of acc, or on top of the GPU's accumulated picture when gpu is set.
// Returns false on a GPU error.
static bool runSteps(OfxImageEffectHandle inst, const FrameSource& fs, const Plan& pl,
                     const std::vector<double>& times, mosh::Image& acc, ChainResult& cr,
                     const mosh::ParallelFor& pf, mosh_gpu::Session* gpu = nullptr) {
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

    if (times.size() < 2) return true;
    // The last few frames used, so consecutive steps do not fetch them again.
    struct Recent {
        double t = 0;
        FramePtr f;
        unsigned used = 0;
    };
    Recent recent[4];
    unsigned clock = 0;
    auto frame = [&](double ft) -> FramePtr {
        for (Recent& r : recent)
            if (r.f && r.t == ft) {
                r.used = ++clock;
                return r.f;
            }
        FramePtr f = fs.get(ft);
        if (!f) return f;
        Recent* slot = &recent[0];
        for (Recent& r : recent)
            if (r.used < slot->used) slot = &r;
        slot->t = ft;
        slot->f = f;
        slot->used = ++clock;
        return f;
    };
    const double L = (double)std::max(1, s.moshLength);
    mosh::Image scratch;
    for (size_t i = 1; i < times.size(); ++i) {
        const double tau = times[i];
        const double p = (tau - pl.cutFrame) / L;
        double prevT, curT;
        pl.pair(tau, prevT, curT);
        FramePtr prev = frame(prevT);
        FramePtr cur = prev ? frame(curT) : nullptr;
        if (!cur) break;
        if (refresh) rs.q = recoveryAmount(s, p);
        if (natural) {
            ns.q = naturalAmount(s, p);
            ns.frame = std::lround(tau);
        }
        // Repeat: recovery inside the chain brings back the live clip, not
        // the looped frames.
        FramePtr live, livePrev;
        if (pl.repeat && (natural || (refresh && rs.q > 0.f))) {
            live = frame(tau);
            if (live && refresh) livePrev = frame(tau - 1.0);
            if (!live || (refresh && !livePrev)) break;
        }
        if (gEffect->abort(inst)) {
            cr.aborted = true;
            break;
        }
        if (i == 1) cr.firstPairDiff = meanAbsDiff(*prev, *cur);
        mosh::MotionField& mf = cr.lastField;
        if (!fs.fields || !fs.fields->getField(fs.hash, prevT, mf)) {
            if (gpu) {
                if (!gpu->estimateMotion(prev, cur, mp, mf, pf)) return false;
            } else {
                mf = mosh::estimateMotion(*prev, *cur, mp, pf);
            }
            if (fs.fields) fs.fields->putField(fs.hash, prevT, mf);
        }
        if (natural && !pl.repeat) mosh::naturalIntra(mf, ns, cr.refreshed);
        if (gpu) {
            if (!gpu->moshStep(prev, cur, mf, sp)) return false;
        } else {
            mosh::moshStep(*prev, *cur, mf, sp, acc, scratch, pf);
        }
        if (natural && pl.repeat) {
            // Healed blocks show the live clip and spread the way the new
            // shot's blocks do in a transition; the looped frames' own intra
            // blocks stay part of the repeat.
            mosh::MotionField heal = mf;
            std::fill(heal.intra.begin(), heal.intra.end(), (uint8_t)0);
            heal.sse.clear();
            heal.var.clear();
            mosh::naturalIntra(heal, ns, cr.refreshed);
            mosh::naturalCarry(heal, ns, cr.refreshed);
            mosh::RefreshPull rp;
            rp.rate = 1.f;
            rp.tail = mosh::naturalTail(ns);
            if (gpu) {
                if (!gpu->pull(live, mf, cr.refreshed, rp.rate, rp.tail)) return false;
            } else {
                mosh::refreshBlend(*live, mf, cr.refreshed, rp, acc, pf);
            }
        } else if (natural) {
            if (gpu) {
                // mosh::naturalAfter, with the fresh blocks left alone.
                mosh::naturalCarry(mf, ns, cr.refreshed);
                const float tail = mosh::naturalTail(ns);
                if (tail > 0.f && !gpu->pull(cur, mf, cr.refreshed, 0.f, tail)) return false;
            } else {
                mosh::naturalAfter(*cur, mf, ns, cr.refreshed, acc, pf);
            }
        }
        if (refresh) {
            const FramePtr& from = live ? livePrev : prev;
            const FramePtr& to = live ? live : cur;
            if (gpu) {
                // mosh::refreshStep
                const size_t nb = (size_t)mf.bw * mf.bh;
                if (cr.refreshed.size() != nb) cr.refreshed.assign(nb, 0);
                if (rs.q > 0.f && nb > 0) {
                    std::vector<float> sums;
                    std::vector<int> counts;
                    if (!gpu->refreshActivity(from, to, mf, sums, counts)) return false;
                    const mosh::RefreshPull rp = mosh::refreshDecide(mf, rs, sums, counts, cr.refreshed);
                    if (!gpu->pull(to, mf, cr.refreshed, rp.rate, rp.tail)) return false;
                }
            } else {
                mosh::refreshStep(*from, *to, mf, rs, cr.refreshed, acc, pf);
            }
        }
        ++cr.stepsDone;
        const CheckpointPlan* ck = fs.checkpoints;
        if (ck && ck->due(tau) && !gCheckpoints.has(ck->owner, tau, ck->cut, ck->hash)) {
            mosh::Image copy;
            if (gpu) {
                if (!gpu->downloadAcc(copy)) return false;
            } else {
                copy = acc;
            }
            gCheckpoints.put(ck->owner, tau, ck->cut, ck->hash, std::move(copy), cr.refreshed, ck->budget);
        }
        if (fs.ticket) fs.ticket->beat();
    }
    return true;
}
// The chain and the recovery on the GPU, with the same result as the CPU
// path of render(); false on a GPU error (result and cr are then partial).
static bool renderOnGpu(OfxImageEffectHandle inst, mosh_gpu::Session& gpu, Instance* cache, const FrameSource& fs,
                        const Plan& pl, bool composeNeeded, const mosh::ComposeParams& cp,
                        mosh::Image& result, ChainResult& cr, const mosh::ParallelFor& pf) {
    const uint64_t hash = chainHash(pl.s, fs.rect);
    const double t = pl.t;
    mosh::Image unused;
    bool ok;
    // Continue from the newest cached picture of the chain, on the GPU or the
    // CPU, or from the newest checkpoint.
    const bool reuse = pl.steps >= 1 && cache;
    const double onGpu = reuse ? gpu.latestAcc(cache, t, pl.cutFrame, hash) : -HUGE_VAL;
    const double onCpu = reuse ? cache->latestAcc(t, pl.cutFrame, hash) : -HUGE_VAL;
    const double onCk = reuse ? gCheckpoints.latest(cache, t, pl.cutFrame, hash) : -HUGE_VAL;
    if (onGpu > -HUGE_VAL && onGpu >= onCpu && onGpu >= onCk &&
        gpu.restoreAcc(cache, onGpu, pl.cutFrame, hash, cr.refreshed)) {
        cr.fromCache = true;
        cr.startT = onGpu;
        ok = runSteps(inst, fs, pl, stepsFrom(onGpu, t), unused, cr, pf, &gpu);
    } else if (onCpu > -HUGE_VAL && onCpu >= onCk && cache->getAcc(onCpu, pl.cutFrame, hash, unused, cr.refreshed)) {
        cr.fromCache = true;
        cr.startT = onCpu;
        ok = gpu.setAcc(unused) && runSteps(inst, fs, pl, stepsFrom(onCpu, t), unused, cr, pf, &gpu);
    } else if (onCk > -HUGE_VAL && gCheckpoints.get(cache, onCk, pl.cutFrame, hash, unused, cr.refreshed)) {
        cr.fromCache = cr.fromCheckpoint = true;
        cr.startT = onCk;
        ok = gpu.setAcc(unused) && runSteps(inst, fs, pl, stepsFrom(onCk, t), unused, cr, pf, &gpu);
    } else {
        cr.refreshed.clear();
        FramePtr ref = fs.get(pl.cutFrame - 1.0);
        ok = gpu.setAcc(ref ? *ref : *fs.cur) && runSteps(inst, fs, pl, pl.times, unused, cr, pf, &gpu);
    }
    if (!ok) return false;
    const bool store = cache && !cr.aborted;
    if (composeNeeded) {
        if (!gpu.compose(fs.cur, cp, pl.q, result)) return false;
    } else {
        if (!gpu.downloadAcc(result)) return false;
    }
    if (store && gpu.storeAcc(cache, t, pl.cutFrame, hash, cr.refreshed)) cache->noteStored(t, pl.cutFrame, hash);
    return !gpu.failed();
}

// Text the diagnostics font can draw: upper case, digits, ". - :" and spaces.
static std::string fontSafe(const std::string& s, size_t maxLen) {
    std::string o;
    for (char ch : s) {
        const char u = (char)toupper((unsigned char)ch);
        const bool okCh = (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '.' || u == '-' || u == ':';
        if (okCh) o += u;
        else if (!o.empty() && o.back() != ' ') o += ' ';
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o.size() > maxLen ? o.substr(0, maxLen) : o;
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
         (pl.cutKnown ? "  CUT " + fmt("%.0f", pl.cutFrame) : "  NO CUT") +
         (pl.repeat ? "  REPEAT " + fmt("%.0f", pl.loopLen) : std::string()) +
         (pl.s.cutMode == kCutKeyframes ? (pl.cutKeys < 0 ? std::string("  KEYS UNKNOWN") : "  KEYS " + fmt("%.0f", pl.cutKeys))
                                        : std::string()));
    line("SCAN MAX DIFF " + fmt("%.4f", pl.scanMaxDiff) + "  THR " + fmt("%.3f", pl.s.cutThreshold) +
         "  FETCHED " + fmt("%.0f", pl.scanned));
    line("N " + fmt("%.0f", pl.nFrames) + "  STEPS " + fmt("%.0f", pl.steps) + "  DONE " + fmt("%.0f", cr.stepsDone) +
         (cr.fromCheckpoint ? "  CHECKPOINT " + fmt("%.0f", cr.startT)
          : cr.fromCache    ? "  CACHED " + fmt("%.0f", cr.startT)
                            : "  FULL"));
    line("PAIR DIFF " + fmt("%.4f", cr.firstPairDiff));
    line("MV " + fmt("%.2f", mv) + " PX  INTRA " + fmt("%.0f", intra) + "  Q " + fmt("%.2f", pl.q) +
         "  REC " + fmt("%.0f", pl.s.recoverMode) +
         ((mosh::Recovery)pl.s.recoverMode == mosh::Recovery::MotionRefresh ||
                  (mosh::Recovery)pl.s.recoverMode == mosh::Recovery::Natural
              ? "  REFR " + fmt("%.0f", 100.0 * mosh::refreshedShare(cr.refreshed))
              : std::string()));
    line("IMG " + fmt("%.0f", outW) + "X" + fmt("%.0f", outH) + "  BLK " + fmt("%.0f", pl.s.block));
    line("DEV " + fontSafe(cr.device, 72));
}

// OFX_DATAMOSH_LOG=<file>: a "start" line when a render begins, so a render
// that never ends shows up, and one line when it ends (thread, time spent,
// time waited for an earlier frame, where the chain started, steps, frames
// fetched from the host, device), to see how a host schedules renders.
static const char* logPath() {
    static const char* path = getenv("OFX_DATAMOSH_LOG");
    return path && *path ? path : nullptr;
}
static void logLine(const char* fmtStr, ...) {
    static std::mutex mu;
    static FILE* fp = nullptr;
    static const auto start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(mu);
    if (!fp) fp = fopen(logPath(), "a");
    if (!fp) return;
    const double at = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    fprintf(fp, "%10.1f thr %08x ", at, (unsigned)std::hash<std::thread::id>()(std::this_thread::get_id()));
    va_list ap;
    va_start(ap, fmtStr);
    vfprintf(fp, fmtStr, ap);
    va_end(ap);
    fputc('\n', fp);
    fflush(fp);
}
static void logRender(double t, double ms, double waitedMs, const Plan& pl, const ChainResult& cr, int fetched) {
    if (!logPath()) return;
    char from[32];
    if (pl.passThrough) snprintf(from, sizeof from, "-");
    else if (cr.fromCache) snprintf(from, sizeof from, cr.fromCheckpoint ? "%.0f ckpt" : "%.0f", cr.startT);
    else snprintf(from, sizeof from, "cut");
    logLine("t %.1f %8.1f ms wait %7.1f n %d from %s steps %d fetched %d scanned %d%s %s", t, ms, waitedMs, pl.nFrames,
            from, cr.stepsDone, fetched, pl.scanned, cr.aborted ? " aborted" : "", cr.device.c_str());
}

static OfxStatus render(OfxImageEffectHandle inst, OfxPropertySetHandle inArgs) {
    const auto t0 = std::chrono::steady_clock::now();
    double t = 0;
    gProp->propGetDouble(inArgs, kOfxPropTime, 0, &t);
    if (logPath()) logLine("t %.1f start", t);
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

    if (!pl.cutKnown && pl.s.cutMode == 0) detectCut(src, cache, pl, R, fs.cur, fs.prev);

    const Settings& s = pl.s;
    ChainResult cr;
    double waitedMs = 0;
    auto finish = [&]() {
        logRender(t, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), waitedMs,
                  pl, cr, fs.fetched + 1);
        return kOfxStatOK;
    };

    if (s.view == 0 && pl.passThrough) {
        out.fromImage(*fs.cur, R, rw, pf);
        return finish();
    }

    if (cache && pl.repeat && !pl.passThrough) {
        fs.held = cache;
        fs.heldFrom = pl.cutFrame - (double)pl.loopLen - 1.0;
        fs.heldTo = pl.cutFrame - 1.0;
        fs.fields = cache;
        fs.hash = chainHash(s, R);
    }
    ChainTicket ticket;
    CheckpointPlan ck;
    const size_t budget = (size_t)s.checkpointMB << 20;
    gCheckpoints.trim(budget);
    if (cache && !pl.passThrough) {
        ticket.in = cache;
        ticket.it = cache->enter(t, pl.cutFrame, chainHash(s, R), [&]() { return gEffect->abort(inst) != 0; }, waitedMs);
        fs.ticket = &ticket;
        // As many checkpoints as the budget holds over one mosh, at least
        // kMinEvery frames apart so that saving them costs little.
        constexpr long kMinEvery = 8;
        const size_t frameBytes = (size_t)std::max(0, R.x2 - R.x1) * (size_t)std::max(0, R.y2 - R.y1) * 4 * sizeof(float);
        const size_t fit = frameBytes ? budget / frameBytes : 0;
        if (fit > 0) {
            ck.owner = cache;
            ck.cut = pl.cutFrame;
            ck.hash = chainHash(s, R);
            ck.budget = budget;
            ck.every = std::max(kMinEvery, (long)((std::max(1, s.moshLength) + fit - 1) / fit));
            fs.checkpoints = &ck;
        }
    }
    const auto mode = (mosh::Recovery)s.recoverMode;
    // These modes recover inside the chain; compose only matters at the very end.
    const bool inChain = mode == mosh::Recovery::MotionRefresh || mode == mosh::Recovery::Natural;
    const bool composeNeeded = !pl.passThrough && pl.q > 0.f && !(inChain && pl.q < 1.f);
    mosh::ComposeParams cp;
    cp.mode = mode;
    cp.softness = s.recoverSoftness;
    cp.block = s.block;
    cp.seed = s.seed;
    cp.invertReveal = s.revealInvert;
    cp.direction = (mosh::Direction)s.direction;

    mosh::Image result;
    cr.device = "CPU";
    if (!pl.passThrough && s.view != 1 && (mosh_gpu::Preference)s.device != mosh_gpu::Preference::Cpu) {
        std::string why;
        std::unique_ptr<mosh_gpu::Session> gpu = mosh_gpu::begin((mosh_gpu::Preference)s.device, why);
        if (gpu && renderOnGpu(inst, *gpu, cache, fs, pl, composeNeeded, cp, result, cr, pf)) {
            cr.device = gpu->label();
            gpu.reset();
            ticket.done();
            if (s.view == 2) drawDiagnostics(result, pl, cr, out.width(), out.height());
            out.fromImage(result, R, rw, pf);
            return finish();
        }
        cr = ChainResult();
        cr.device = "CPU: " + (gpu ? gpu->error() : why);
    }

    mosh::Image acc;
    uint64_t hash = 0;
    if (!pl.passThrough) {
        hash = chainHash(s, R);
        // Fast path: an earlier frame's accumulated picture is cached or
        // checkpointed, and the steps from there give exactly the same result.
        const bool reuse = pl.steps >= 1 && cache;
        const double onCpu = reuse ? cache->latestAcc(t, pl.cutFrame, hash) : -HUGE_VAL;
        const double onCk = reuse ? gCheckpoints.latest(cache, t, pl.cutFrame, hash) : -HUGE_VAL;
        if (onCpu > -HUGE_VAL && onCpu >= onCk && cache->getAcc(onCpu, pl.cutFrame, hash, acc, cr.refreshed)) {
            cr.fromCache = true;
            cr.startT = onCpu;
            runSteps(inst, fs, pl, stepsFrom(onCpu, t), acc, cr, pf);
        } else if (onCk > -HUGE_VAL && gCheckpoints.get(cache, onCk, pl.cutFrame, hash, acc, cr.refreshed)) {
            cr.fromCache = cr.fromCheckpoint = true;
            cr.startT = onCk;
            runSteps(inst, fs, pl, stepsFrom(onCk, t), acc, cr, pf);
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
    const bool store = cache && !pl.passThrough && !cr.aborted;

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
            ticket.done();
            mosh::Image vis;
            mosh::visualizeMotion(cr.lastField, s.range, w, h, vis, pf);
            out.fromImage(vis, R, rw, pf);
            return finish();
        }
    }

    if (composeNeeded) {
        mosh::compose(acc, *fs.cur, cp, pl.q, result, pf);
        if (store) cache->putAcc(t, pl.cutFrame, hash, std::move(acc), cr.refreshed);
    } else {
        if (store) cache->putAcc(t, pl.cutFrame, hash, acc, cr.refreshed);
        result = std::move(acc);
    }
    ticket.done();

    if (s.view == 2) drawDiagnostics(result, pl, cr, out.width(), out.height());

    out.fromImage(result, R, rw, pf);
    return finish();
}
static OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    try {
        if (!strcmp(action, kOfxActionLoad)) return onLoad();
        if (!strcmp(action, kOfxActionUnload)) {
            mosh_gpu::shutdown();
            return kOfxStatOK;
        }
        if (!strcmp(action, kOfxActionDescribe)) return describe(effect);
        if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(effect);
        if (!strcmp(action, kOfxActionCreateInstance)) return createInstance(effect);
        if (!strcmp(action, kOfxActionDestroyInstance)) return destroyInstance(effect);
        if (!strcmp(action, kOfxActionInstanceChanged) || !strcmp(action, kOfxActionPurgeCaches) ||
            !strcmp(action, kOfxActionSyncPrivateData)) {
            char* name = nullptr;
            char* reason = nullptr;
            if (inArgs && !strcmp(action, kOfxActionInstanceChanged)) {
                gProp->propGetString(inArgs, kOfxPropName, 0, &name);
                gProp->propGetString(inArgs, kOfxPropChangeReason, 0, &reason);
            }
            // Checkpoint Memory changes no picture, so the caches stay.
            const bool noRender = name && (!strcmp(name, P::kPresetName) || !strcmp(name, P::kPresetSave) ||
                                           !strcmp(name, P::kCheckpointMB) ||
                                           !strcmp(name, P::kPresetList) || !strcmp(name, P::kPresetLoad) ||
                                           !strcmp(name, P::kPresetDelete));
            if (!noRender)
                if (Instance* d = instanceData(effect)) d->clear();
            if (!strcmp(action, kOfxActionInstanceChanged)) {
                migrateCutKeys(effect);
                if (name && reason && !strcmp(name, P::kMoshStyle) && !strcmp(reason, kOfxChangeUserEdited))
                    applyStyleDefaults(effect);
                if (name && !strcmp(name, P::kPresetSave)) savePreset(effect);
                if (name && !strcmp(name, P::kPresetLoad)) loadPreset(effect);
                if (name && !strcmp(name, P::kPresetDelete)) deletePreset(effect);
                if (name && reason && !strcmp(name, P::kPresetList) && !strcmp(reason, kOfxChangeUserEdited))
                    presetPicked(effect);
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
