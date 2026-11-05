// Minimal OpenFX host for exercising the plugin outside Resolve.
//   mock_host <plugin.ofx> <from.rgb> <to.rgb> <w> <h> <frames> <out.rgb> [view]
// The Source clip is 20 frames of "from" followed by "to", starting at timeline
// frame 1000; the plugin has to find the cut itself.
// Images are handed to the plugin bottom-up, like real OFX hosts do.
#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

struct PropSet {
    std::map<std::string, std::vector<std::string>> s;
    std::map<std::string, std::vector<double>> d;
    std::map<std::string, std::vector<int>> i;
    std::map<std::string, std::vector<void*>> p;
    std::vector<std::unique_ptr<std::string>> owned;  // for returned char*
};
static PropSet* PS(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
static OfxPropertySetHandle H(PropSet* p) { return reinterpret_cast<OfxPropertySetHandle>(p); }

template <class T>
static void setAt(std::vector<T>& v, int idx, const T& val) {
    if ((int)v.size() <= idx) v.resize(idx + 1);
    v[idx] = val;
}

static OfxStatus propSetPointer(OfxPropertySetHandle h, const char* n, int idx, void* v) { setAt(PS(h)->p[n], idx, v); return kOfxStatOK; }
static bool gFixedChoices = false;  // MOCK_FIXED_CHOICES: menus cannot grow after describeInContext
static OfxStatus propSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v) {
    if (gFixedChoices && !strcmp(n, kOfxParamPropChoiceOption) && idx >= (int)PS(h)->s[n].size()) return kOfxStatErrBadIndex;
    setAt(PS(h)->s[n], idx, std::string(v)); return kOfxStatOK; }
static OfxStatus propSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v) { setAt(PS(h)->d[n], idx, v); return kOfxStatOK; }
static OfxStatus propSetInt(OfxPropertySetHandle h, const char* n, int idx, int v) { setAt(PS(h)->i[n], idx, v); return kOfxStatOK; }
static OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { for (int k = 0; k < c; ++k) propSetPointer(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { for (int k = 0; k < c; ++k) propSetString(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { for (int k = 0; k < c; ++k) propSetDouble(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { for (int k = 0; k < c; ++k) propSetInt(h, n, k, v[k]); return kOfxStatOK; }

static OfxStatus propGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v) {
    auto it = PS(h)->p.find(n); if (it == PS(h)->p.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing pointer prop %s\n", n); return kOfxStatErrUnknown; }
    *v = it->second[idx]; return kOfxStatOK; }
static std::mutex gHostMutex;  // MOCK_CONCURRENT: guards what renders share in the host
// MOCK_SCRUB: a host lock the UI thread holds while it calls the plugin, and
// that frame fetches and parameter reads take too.
static std::mutex gUiMutex;
static bool gScrub = false;
static OfxStatus propGetString(OfxPropertySetHandle h, const char* n, int idx, char** v) {
    auto it = PS(h)->s.find(n); if (it == PS(h)->s.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing string prop %s\n", n); return kOfxStatErrUnknown; }
    std::lock_guard<std::mutex> g(gHostMutex);
    PS(h)->owned.emplace_back(new std::string(it->second[idx])); *v = const_cast<char*>(PS(h)->owned.back()->c_str()); return kOfxStatOK; }
static OfxStatus propGetDouble(OfxPropertySetHandle h, const char* n, int idx, double* v) {
    auto it = PS(h)->d.find(n); if (it == PS(h)->d.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing double prop %s\n", n); return kOfxStatErrUnknown; }
    *v = it->second[idx]; return kOfxStatOK; }
static OfxStatus propGetInt(OfxPropertySetHandle h, const char* n, int idx, int* v) {
    auto it = PS(h)->i.find(n); if (it == PS(h)->i.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing int prop %s\n", n); return kOfxStatErrUnknown; }
    *v = it->second[idx]; return kOfxStatOK; }
static OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { for (int k = 0; k < c; ++k) if (propGetPointer(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus propGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v) { for (int k = 0; k < c; ++k) if (propGetString(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { for (int k = 0; k < c; ++k) if (propGetDouble(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus propGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { for (int k = 0; k < c; ++k) if (propGetInt(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
static OfxStatus propReset(OfxPropertySetHandle h, const char* n) {
    if ((gFixedChoices || getenv("MOCK_NO_PROPRESET")) && !strcmp(n, kOfxParamPropChoiceOption)) return kOfxStatErrUnsupported;
    PS(h)->s.erase(n); PS(h)->d.erase(n); PS(h)->i.erase(n); PS(h)->p.erase(n); return kOfxStatOK; }
static OfxStatus propGetDimension(OfxPropertySetHandle h, const char* n, int* c) {
    PropSet* p = PS(h); *c = 0;
    if (p->s.count(n)) *c = (int)p->s[n].size(); else if (p->d.count(n)) *c = (int)p->d[n].size();
    else if (p->i.count(n)) *c = (int)p->i[n].size(); else if (p->p.count(n)) *c = (int)p->p[n].size();
    return kOfxStatOK; }

static OfxPropertySuiteV1 gPropSuite = {
    propSetPointer, propSetString, propSetDouble, propSetInt, propSetPointerN, propSetStringN, propSetDoubleN, propSetIntN,
    propGetPointer, propGetString, propGetDouble, propGetInt, propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN,
    propReset, propGetDimension,
};

// ------------------------------------------------------------------ params
struct Param {
    std::string type;
    PropSet props;
    double dval = 0;
    int ival = 0;
    std::string sval;
    bool isTransition = false;
    std::vector<double> keys;  // MOCK_KEYS: keyframe times; each key toggles an int/bool value
};
static bool gNoKeyApi = false;  // MOCK_NO_KEYAPI: host does not report keyframes
struct ParamSet {
    PropSet props;
    std::map<std::string, std::unique_ptr<Param>> params;
};
static double gT0 = 1000.0, gDur = 35.0;  // transition timing on the mock timeline
static double gScale = 1.0;            // MOCK_SCALE
static int gPad = 0, gOffX = 0, gOffY = 0;  // MOCK_PAD, MOCK_OFFSET (pixel origin of every image)

static OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* props) {
    auto* set = reinterpret_cast<ParamSet*>(ps);
    auto p = std::make_unique<Param>();
    p->type = type;
    Param* raw = p.get();
    set->params[name] = std::move(p);
    if (props) *props = H(&raw->props);
    return kOfxStatOK;
}
static OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* h, OfxPropertySetHandle* props) {
    auto* set = reinterpret_cast<ParamSet*>(ps);
    auto it = set->params.find(name);
    if (it == set->params.end()) { fprintf(stderr, "  [host] unknown param %s\n", name); return kOfxStatErrUnknown; }
    *h = reinterpret_cast<OfxParamHandle>(it->second.get());
    if (props) *props = H(&it->second->props);
    return kOfxStatOK;
}
static OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle* props) { *props = H(&reinterpret_cast<ParamSet*>(ps)->props); return kOfxStatOK; }
static OfxStatus paramGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* props) { *props = H(&reinterpret_cast<Param*>(p)->props); return kOfxStatOK; }
static OfxStatus paramGetValueAtTime(OfxParamHandle ph, OfxTime t, ...) {
    if (gScrub) std::lock_guard<std::mutex> ui(gUiMutex);
    Param* p = reinterpret_cast<Param*>(ph);
    va_list ap; va_start(ap, t);
    if (p->type == kOfxParamTypeDouble) {
        double* out = va_arg(ap, double*);
        *out = p->isTransition ? std::min(1.0, std::max(0.0, (t - gT0) / gDur)) : p->dval;
    } else if (p->type == kOfxParamTypeString) {
        *va_arg(ap, char**) = const_cast<char*>(p->sval.c_str());
    } else {
        int* out = va_arg(ap, int*);
        int flips = 0;
        for (double k : p->keys) flips += k <= t + 1e-9;
        *out = p->ival ^ (flips & 1);
    }
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus paramGetNumKeys(OfxParamHandle ph, unsigned int* n) {
    if (gNoKeyApi) return kOfxStatErrUnsupported;
    *n = (unsigned)reinterpret_cast<Param*>(ph)->keys.size();
    return kOfxStatOK;
}
static OfxStatus paramGetKeyTime(OfxParamHandle ph, unsigned int i, OfxTime* t) {
    Param* p = reinterpret_cast<Param*>(ph);
    if (gNoKeyApi) return kOfxStatErrUnsupported;
    if (i >= p->keys.size()) return kOfxStatErrBadIndex;
    *t = p->keys[i];
    return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle ph, ...) {
    Param* p = reinterpret_cast<Param*>(ph);
    va_list ap; va_start(ap, ph);
    if (p->type == kOfxParamTypeDouble) *va_arg(ap, double*) = p->dval;
    else if (p->type == kOfxParamTypeString) *va_arg(ap, char**) = const_cast<char*>(p->sval.c_str());
    else *va_arg(ap, int*) = p->ival;
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus unsupported(...) { return kOfxStatErrUnsupported; }
static OfxStatus paramSetValue(OfxParamHandle ph, ...) {
    Param* p = reinterpret_cast<Param*>(ph);
    va_list ap; va_start(ap, ph);
    if (p->type == kOfxParamTypeDouble) p->dval = va_arg(ap, double);
    else if (p->type == kOfxParamTypeString) p->sval = va_arg(ap, const char*);
    else p->ival = va_arg(ap, int);
    va_end(ap);
    return kOfxStatOK;
}

static OfxParameterSuiteV1 gParamSuite = {
    paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet,
    paramGetValue, paramGetValueAtTime,
    (OfxStatus(*)(OfxParamHandle, OfxTime, ...))unsupported,            // derivative
    (OfxStatus(*)(OfxParamHandle, OfxTime, OfxTime, ...))unsupported,   // integral
    paramSetValue,                                                      // setValue
    (OfxStatus(*)(OfxParamHandle, OfxTime, ...))unsupported,            // setValueAtTime
    paramGetNumKeys,
    paramGetKeyTime,
    (OfxStatus(*)(OfxParamHandle, OfxTime, int, int*))unsupported,
    (OfxStatus(*)(OfxParamHandle, OfxTime))unsupported,
    (OfxStatus(*)(OfxParamHandle))unsupported,
    (OfxStatus(*)(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*))unsupported,
    (OfxStatus(*)(OfxParamSetHandle, const char*))unsupported,
    (OfxStatus(*)(OfxParamSetHandle))unsupported,
};

// ------------------------------------------------------------------ clips & images
struct Frame { std::vector<float> rgba; };
struct Clip {
    std::string name;
    PropSet props;
    int w = 0, h = 0;
    std::vector<Frame> frames;  // indexed by t - gT0, clamped
    struct Live { PropSet props; std::vector<float> data; };
    std::vector<std::unique_ptr<Live>> live;
    std::vector<double> requested;
};
struct Effect {
    PropSet props;
    ParamSet params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
    Clip* out = nullptr;
};

static OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* p) { *p = H(&reinterpret_cast<Effect*>(e)->props); return kOfxStatOK; }
static OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* p) { *p = reinterpret_cast<OfxParamSetHandle>(&reinterpret_cast<Effect*>(e)->params); return kOfxStatOK; }
static OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* props) {
    Effect* ef = reinterpret_cast<Effect*>(e);
    auto c = std::make_unique<Clip>(); c->name = name;
    if (props) *props = H(&c->props);
    ef->clips[name] = std::move(c);
    return kOfxStatOK;
}
static OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* h, OfxPropertySetHandle* props) {
    Effect* ef = reinterpret_cast<Effect*>(e);
    auto it = ef->clips.find(name);
    if (it == ef->clips.end()) return kOfxStatErrUnknown;
    *h = reinterpret_cast<OfxImageClipHandle>(it->second.get());
    if (props) *props = H(&it->second->props);
    return kOfxStatOK;
}
static OfxStatus clipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* p) { *p = H(&reinterpret_cast<Clip*>(c)->props); return kOfxStatOK; }
static std::vector<Clip*> gClips;           // every clip, for clipReleaseImage
static int gFetchMs = 0;                    // MOCK_FETCH_MS: cost of each source frame fetch
static thread_local Clip::Live* tlOutImage; // output image fetched by this thread's render
static thread_local int tlFetches;          // source frames fetched by this thread's render
static OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime t, const OfxRectD*, OfxPropertySetHandle* out) {
    Clip* c = reinterpret_cast<Clip*>(ch);
    const bool isOut = c->name == kOfxImageEffectOutputClipName;
    if (gScrub) std::lock_guard<std::mutex> ui(gUiMutex);
    if (!isOut) {
        ++tlFetches;
        if (gFetchMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(gFetchMs));
    }
    auto live = std::make_unique<Clip::Live>();
    // MOCK_SCALE: images at a render scale (nearest-neighbour downsample).
    // MOCK_PAD: source images carry an extra border of garbage around the
    // output's rectangle, so their bounds differ from the output's.
    const int sw = std::max(1, (int)std::lround(c->w * gScale)), sh = std::max(1, (int)std::lround(c->h * gScale));
    const int pad = c->frames.empty() ? 0 : gPad;
    const int bw = sw + 2 * pad, bh = sh + 2 * pad;
    live->data.assign((size_t)bw * bh * 4, 0.37f);
    if (!c->frames.empty()) {
        int idx = (int)std::lround(t - gT0);
        idx = std::min(std::max(idx, 0), (int)c->frames.size() - 1);
        const std::vector<float>& f = c->frames[idx].rgba;
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const int fx = std::min(c->w - 1, (int)(x / gScale)), fy = std::min(c->h - 1, (int)(y / gScale));
                memcpy(&live->data[((size_t)(y + pad) * bw + x + pad) * 4], &f[((size_t)fy * c->w + fx) * 4], 4 * sizeof(float));
            }
    }
    PropSet& p = live->props;
    p.p[kOfxImagePropData] = {live->data.data()};
    p.i[kOfxImagePropBounds] = {gOffX - pad, gOffY - pad, gOffX + sw + pad, gOffY + sh + pad};
    p.i[kOfxImagePropRegionOfDefinition] = p.i[kOfxImagePropBounds];
    p.i[kOfxImagePropRowBytes] = {bw * 4 * (int)sizeof(float)};
    p.s[kOfxImageEffectPropComponents] = {kOfxImageComponentRGBA};
    p.s[kOfxImageEffectPropPixelDepth] = {kOfxBitDepthFloat};
    p.d[kOfxImagePropPixelAspectRatio] = {1.0};
    *out = H(&p);
    std::lock_guard<std::mutex> g(gHostMutex);
    c->requested.push_back(t);
    if (isOut) tlOutImage = live.get();
    c->live.push_back(std::move(live));
    return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle img) {
    // Source images are freed; output images stay until the host has read them.
    std::lock_guard<std::mutex> g(gHostMutex);
    for (Clip* c : gClips) {
        if (c->name == kOfxImageEffectOutputClipName) continue;
        for (size_t i = 0; i < c->live.size(); ++i)
            if (H(&c->live[i]->props) == img) {
                c->live.erase(c->live.begin() + (long)i);
                return kOfxStatOK;
            }
    }
    return kOfxStatOK;
}
static OfxStatus clipGetRoD(OfxImageClipHandle ch, OfxTime, OfxRectD* r) { Clip* c = reinterpret_cast<Clip*>(ch); *r = {0, 0, (double)c->w, (double)c->h}; return kOfxStatOK; }
// MOCK_SCRUB: the host aborts a render once its deadline passes.
static thread_local std::chrono::steady_clock::time_point tlAbortAt = std::chrono::steady_clock::time_point::max();
static thread_local bool tlAborted = false;
static int abortFn(OfxImageEffectHandle) {
    if (std::chrono::steady_clock::now() < tlAbortAt) return 0;
    tlAborted = true;
    return 1;
}
static OfxStatus memAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) { *h = reinterpret_cast<OfxImageMemoryHandle>(malloc(n)); return kOfxStatOK; }
static OfxStatus memFree(OfxImageMemoryHandle h) { free(h); return kOfxStatOK; }
static OfxStatus memLock(OfxImageMemoryHandle h, void** p) { *p = h; return kOfxStatOK; }
static OfxStatus memUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

static OfxImageEffectSuiteV1 gEffectSuite = {
    getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage, clipReleaseImage,
    clipGetRoD, abortFn, memAlloc, memFree, memLock, memUnlock,
};

// Optional multithread suite (MOCK_THREADS=n) so the plugin's parallel paths run as in a real host.
static unsigned gThreads = 0;
static OfxStatus mtMultiThread(OfxThreadFunctionV1 func, unsigned int n, void* arg) {
    std::vector<std::thread> th;
    for (unsigned i = 0; i < n; ++i) th.emplace_back([=] { func(i, n, arg); });
    for (auto& t : th) t.join();
    return kOfxStatOK;
}
static OfxStatus mtNumCPUs(unsigned int* n) { *n = gThreads; return kOfxStatOK; }
static OfxStatus mtIndex(unsigned int* i) { *i = 0; return kOfxStatOK; }
static int mtIsSpawned(void) { return 0; }
static OfxStatus mtMutexCreate(OfxMutexHandle* m, int) { *m = reinterpret_cast<OfxMutexHandle>(new std::recursive_mutex); return kOfxStatOK; }
static OfxStatus mtMutexDestroy(const OfxMutexHandle m) { delete reinterpret_cast<std::recursive_mutex*>(m); return kOfxStatOK; }
static OfxStatus mtMutexLock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex*>(m)->lock(); return kOfxStatOK; }
static OfxStatus mtMutexUnLock(const OfxMutexHandle m) { reinterpret_cast<std::recursive_mutex*>(m)->unlock(); return kOfxStatOK; }
static OfxStatus mtMutexTryLock(const OfxMutexHandle m) { return reinterpret_cast<std::recursive_mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
static OfxMultiThreadSuiteV1 gThreadSuite = {
    mtMultiThread, mtNumCPUs, mtIndex, mtIsSpawned, mtMutexCreate, mtMutexDestroy, mtMutexLock, mtMutexUnLock, mtMutexTryLock,
};

static OfxStatus mockMessage(void*, const char* type, const char*, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    printf("[message %s] ", type);
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
    return kOfxStatOK;
}
static OfxMessageSuiteV1 gMessageSuite = {mockMessage};

static PropSet gHostProps;
static const void* fetchSuite(OfxPropertySetHandle, const char* name, int ver) {
    if (!strcmp(name, kOfxPropertySuite) && ver == 1) return &gPropSuite;
    if (!strcmp(name, kOfxParameterSuite) && ver == 1) return &gParamSuite;
    if (!strcmp(name, kOfxImageEffectSuite) && ver == 1) return &gEffectSuite;
    if (!strcmp(name, kOfxMultiThreadSuite) && ver == 1 && gThreads > 0) return &gThreadSuite;
    if (!strcmp(name, kOfxMessageSuite) && ver == 1) return &gMessageSuite;
    return nullptr;  // without MOCK_THREADS the plugin must fall back to serial
}

static bool readClip(const char* path, int w, int h, int n, Clip& c) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    c.w = w; c.h = h;
    std::vector<unsigned char> buf((size_t)w * h * 3);
    c.frames.resize(n);
    for (int i = 0; i < n; ++i) {
        if (fread(buf.data(), 1, buf.size(), f) != buf.size()) { fclose(f); return false; }
        c.frames[i].rgba.resize((size_t)w * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t src = ((size_t)y * w + x) * 3;
                const size_t dst = ((size_t)(h - 1 - y) * w + x) * 4;
                c.frames[i].rgba[dst + 0] = buf[src + 0] / 255.f;
                c.frames[i].rgba[dst + 1] = buf[src + 1] / 255.f;
                c.frames[i].rgba[dst + 2] = buf[src + 2] / 255.f;
                c.frames[i].rgba[dst + 3] = 1.f;
            }
    }
    fclose(f);
    return true;
}

#define CHECK(st, what) do { OfxStatus _s = (st); if (_s != kOfxStatOK && _s != kOfxStatReplyDefault) { fprintf(stderr, "%s failed: %d\n", what, (int)_s); return 1; } } while (0)

int main(int argc, char** argv) {
    if (argc < 8) { fprintf(stderr, "usage: %s plugin.ofx from.rgb to.rgb w h frames out.rgb [view] [param=value ...]\n", argv[0]); return 1; }
    const int w = atoi(argv[4]), h = atoi(argv[5]), n = atoi(argv[6]);
    const int view = argc > 8 ? atoi(argv[8]) : 0;
    const bool filter = true;
    const int lead = 20;  // filter mode: frames of "from" before the cut
    const int total = filter ? n + lead : n;
    std::vector<unsigned char> rgbOut((size_t)w * h * 3);
    gDur = total - 1;
    auto toRgb = [&](const std::vector<float>& img) {  // bottom-up RGBA -> top-down rgb24
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c)
                    rgbOut[((size_t)y * w + x) * 3 + c] =
                        (unsigned char)(std::min(std::max(img[((size_t)(h - 1 - y) * w + x) * 4 + c], 0.f), 1.f) * 255.f + 0.5f);
    };

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto getNum = (int (*)())dlsym(lib, "OfxGetNumberOfPlugins");
    auto getPlugin = (OfxPlugin * (*)(int)) dlsym(lib, "OfxGetPlugin");
    if (!getNum || !getPlugin) { fprintf(stderr, "missing OFX entry points\n"); return 1; }
    printf("plugins: %d\n", getNum());
    OfxPlugin* pl = getPlugin(0);
    printf("id: %s v%d.%d api %s/%d\n", pl->pluginIdentifier, pl->pluginVersionMajor, pl->pluginVersionMinor, pl->pluginApi, pl->apiVersion);

    if (const char* th = getenv("MOCK_THREADS")) gThreads = (unsigned)atoi(th);
    if (const char* e = getenv("MOCK_SCALE")) gScale = atof(e);
    if (const char* e = getenv("MOCK_PAD")) gPad = atoi(e);
    if (const char* e = getenv("MOCK_OFFSET")) gOffX = gOffY = atoi(e);
    if (const char* e = getenv("MOCK_FETCH_MS")) gFetchMs = atoi(e);
    gNoKeyApi = getenv("MOCK_NO_KEYAPI") != nullptr;
    if (const char* e = getenv("MOCK_LOCALE")) if (!setlocale(LC_ALL, e)) fprintf(stderr, "locale %s not available\n", e);
    gHostProps.s[kOfxPropName] = {"mock.host"};
    gHostProps.i[kOfxImageEffectPropTemporalClipAccess] = {getenv("MOCK_NO_TEMPORAL") ? 0 : 1};
    OfxHost host{H(&gHostProps), fetchSuite};
    pl->setHost(&host);
    CHECK(pl->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr), "load");

    Effect descriptor;
    CHECK(pl->mainEntry(kOfxActionDescribe, &descriptor, nullptr, nullptr), "describe");
    printf("label: %s\n", descriptor.props.s[kOfxPropLabel][0].c_str());
    for (auto& c : descriptor.props.s[kOfxImageEffectPropSupportedContexts]) printf("context: %s\n", c.c_str());

    Effect inst;
    const char* ctx = filter ? kOfxImageEffectContextFilter : kOfxImageEffectContextTransition;
    inst.props.s[kOfxImageEffectPropContext] = {ctx};
    PropSet dicArgs;
    dicArgs.s[kOfxImageEffectPropContext] = {ctx};
    CHECK(pl->mainEntry(kOfxImageEffectActionDescribeInContext, &inst, H(&dicArgs), nullptr), "describeInContext");
    printf("clips:");
    for (auto& c : inst.clips) printf(" %s", c.first.c_str());
    gFixedChoices = getenv("MOCK_FIXED_CHOICES") != nullptr;
    printf("\nparams (%zu):", inst.params.params.size());
    for (auto& p : inst.params.params) printf(" %s", p.first.c_str());
    printf("\n");

    // Host-side parameters: Transition, plus defaults for the plugin's own.
    for (auto& kv : inst.params.params) {
        Param& p = *kv.second;
        if (p.type == kOfxParamTypeDouble) { auto it = p.props.d.find(kOfxParamPropDefault); p.dval = it != p.props.d.end() ? it->second[0] : 0.0; }
        else if (p.type == kOfxParamTypeString) { auto it = p.props.s.find(kOfxParamPropDefault); p.sval = it != p.props.s.end() ? it->second[0] : ""; }
        else { auto it = p.props.i.find(kOfxParamPropDefault); p.ival = it != p.props.i.end() ? it->second[0] : 0; }
    }
    {
        auto p = std::make_unique<Param>(); p->type = kOfxParamTypeDouble; p->isTransition = true;
        inst.params.params[kOfxImageEffectTransitionParamName] = std::move(p);
    }
    inst.params.params["view"]->ival = view;
    for (int a = 9; a < argc; ++a) {  // param=value overrides
        const char* eq = strchr(argv[a], '=');
        auto it = eq ? inst.params.params.find(std::string(argv[a], eq - argv[a])) : inst.params.params.end();
        if (it == inst.params.params.end()) { fprintf(stderr, "unknown param override: %s\n", argv[a]); return 1; }
        if (it->second->type == kOfxParamTypeDouble) it->second->dval = atof(eq + 1);
        else if (it->second->type == kOfxParamTypeString) it->second->sval = eq + 1;
        else it->second->ival = atoi(eq + 1);
    }

    if (const char* e = getenv("MOCK_KEYS")) {  // name=t1,t2,...
        const char* eq = strchr(e, '=');
        auto it = eq ? inst.params.params.find(std::string(e, eq - e)) : inst.params.params.end();
        if (it == inst.params.params.end()) { fprintf(stderr, "bad MOCK_KEYS: %s\n", e); return 1; }
        for (const char* q = eq + 1; *q;) {
            char* end = nullptr;
            it->second->keys.push_back(strtod(q, &end));
            if (end == q) break;
            q = *end == ',' ? end + 1 : end;
        }
    }

    Clip* out = inst.clips[kOfxImageEffectOutputClipName].get();
    Clip *from = nullptr, *to = nullptr, *source = nullptr;
    Clip fromData, toData;
    if (!readClip(argv[2], w, h, n, fromData) || !readClip(argv[3], w, h, n, toData)) { fprintf(stderr, "cannot read clips\n"); return 1; }
    if (filter) {
        source = inst.clips[kOfxImageEffectSimpleSourceClipName].get();
        source->w = w; source->h = h;
        for (int i = 0; i < lead; ++i) source->frames.push_back(fromData.frames[std::min(i, n - 1)]);
        for (int i = 0; i < n; ++i) source->frames.push_back(toData.frames[i]);
        from = to = source;
    } else {
        from = inst.clips[kOfxImageEffectTransitionSourceFromClipName].get();
        to = inst.clips[kOfxImageEffectTransitionSourceToClipName].get();
        *from = std::move(fromData); from->name = kOfxImageEffectTransitionSourceFromClipName;
        *to = std::move(toData); to->name = kOfxImageEffectTransitionSourceToClipName;
    }
    out->w = w; out->h = h;
    for (auto& kv : inst.clips) gClips.push_back(kv.second.get());
    for (Clip* c : {from, to, out}) {
        c->props.d[kOfxImageEffectPropFrameRange] = {gT0, gT0 + gDur};
        c->props.d[kOfxImageEffectPropFrameRate] = {30.0};
        c->props.s[kOfxImageEffectPropComponents] = {kOfxImageComponentRGBA};
        c->props.s[kOfxImageEffectPropPixelDepth] = {kOfxBitDepthFloat};
    }
    CHECK(pl->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr), "createInstance");
    if (getenv("MOCK_PRESETS")) {  // save, pick, load, delete named presets (OFX_DATAMOSH_PRESETS: file)
        auto& P = inst.params.params;
        auto press = [&](const char* name) {
            PropSet ic;
            ic.s[kOfxPropName] = {name};
            ic.s[kOfxPropChangeReason] = {kOfxChangeUserEdited};
            const OfxStatus st = pl->mainEntry(kOfxActionInstanceChanged, &inst, H(&ic), nullptr);
            if (st != kOfxStatOK) printf("instanceChanged(%s) failed: %d\n", name, st);
        };
        auto show = [&](const char* what) {
            printf("%s: residual=%.3f recoverMode=%d blockSize=%d seed=%d moshStyle=%d cutMode=%d cutThreshold=%.3f cutFrame=%d view=%d intraRate.enabled=%d\n",
                   what, P["residual"]->dval, P["recoverMode"]->ival, P["blockSize"]->ival, P["seed"]->ival,
                   P["moshStyle"]->ival, P["cutMode"]->ival, P["cutThreshold"]->dval, P["cutFrame"]->ival, P["view"]->ival,
                   P["intraRate"]->props.i[kOfxParamPropEnabled][0]);
        };
        auto menu = [&]() {
            printf("  menu:");
            for (auto& o : P["presetList"]->props.s[kOfxParamPropChoiceOption]) printf(" [%s]", o.c_str());
            printf("  pick=%d name=\"%s\"\n", P["presetList"]->ival, P["presetName"]->sval.c_str());
        };
        auto pick = [&](int idx) { P["presetList"]->ival = idx; press("presetList"); };
        menu();
        P["residual"]->dval = 0.3; P["recoverMode"]->ival = 4; P["blockSize"]->ival = 2; P["seed"]->ival = 7;
        P["moshStyle"]->ival = 1; P["cutMode"]->ival = 1; P["cutFrame"]->ival = 1010; P["cutThreshold"]->dval = 0.3;
        printf("== save without a name\n");
        press("presetSave");
        printf("== save \"  My [look]  \"\n");
        P["presetName"]->sval = "  My [look]  ";
        press("presetSave");
        menu();
        printf("== save \"Second\"\n");
        P["residual"]->dval = 0.5; P["cutMode"]->ival = 0; P["cutThreshold"]->dval = 0.2;
        P["presetName"]->sval = "Second";
        press("presetSave");
        menu();
        P["residual"]->dval = 0.9; P["recoverMode"]->ival = 0; P["blockSize"]->ival = 1; P["seed"]->ival = 0;
        P["moshStyle"]->ival = 0; P["cutMode"]->ival = 2; P["cutThreshold"]->dval = 0.5; P["cutFrame"]->ival = 0; P["view"]->ival = 2;
        P["presetName"]->sval = "";
        show("changed");
        printf("== pick 1, load\n");
        pick(1);
        menu();
        press("presetLoad");
        show("after load");
        printf("== pick 2, load\n");
        pick(2);
        press("presetLoad");
        show("after load");
        printf("== pick 1, delete, then load 1\n");
        pick(1);
        press("presetDelete");
        menu();
        P["presetList"]->ival = 1;
        press("presetLoad");
        printf("== load with nothing picked\n");
        P["presetList"]->ival = 0;
        press("presetLoad");
        printf("== save \"My  look\" again\n");
        P["presetName"]->sval = "My  look";
        press("presetSave");
        menu();
        if (const char* file = getenv("OFX_DATAMOSH_PRESETS")) {
            printf("== another instance adds \"Aaa\" (sorts first); pick 1 in the old menu, load\n");
            if (FILE* fp = fopen(file, "a")) { fputs("\n[Aaa]\nresidual = 0.11\n", fp); fclose(fp); }
            pick(1);
            menu();
            press("presetLoad");
            show("after load");
        }
        return 0;
    }
    if (getenv("MOCK_PARAMS")) {  // dump parameter layout: group, enabled state
        auto dump = [&]() {
            for (auto& kv : inst.params.params) {
                auto& pp = kv.second->props;
                auto par = pp.s.find(kOfxParamPropParent);
                auto en = pp.i.find(kOfxParamPropEnabled);
                printf("  %-16s %-12s group=%-12s enabled=%s\n", kv.first.c_str(), kv.second->type.c_str() + 12,
                       par != pp.s.end() ? par->second[0].c_str() : "-",
                       en != pp.i.end() ? (en->second[0] ? "yes" : "NO") : "default");
            }
        };
        dump();
        inst.params.params["cutMode"]->ival = 1;
        inst.params.params["recoverMode"]->ival = 5;
        PropSet ic;
        ic.s[kOfxPropName] = {"recoverMode"};
        ic.s[kOfxPropChangeReason] = {kOfxChangeUserEdited};
        CHECK(pl->mainEntry(kOfxActionInstanceChanged, &inst, H(&ic), nullptr), "instanceChanged");
        printf("after cutMode=Manual, recoverMode=Luma reveal:\n");
        dump();
        // Mosh Style switching: parameters at the old style's defaults follow.
        auto& P = inst.params.params;
        auto styleTo = [&](int v) {
            P["moshStyle"]->ival = v;
            PropSet sc;
            sc.s[kOfxPropName] = {"moshStyle"};
            sc.s[kOfxPropChangeReason] = {kOfxChangeUserEdited};
            const OfxStatus st = pl->mainEntry(kOfxActionInstanceChanged, &inst, H(&sc), nullptr);
            if (st != kOfxStatOK) printf("instanceChanged failed: %d\n", st);
            printf("style=%d residual=%.2f recoverMode=%d intraRate.enabled=%d\n", v, P["residual"]->dval,
                   P["recoverMode"]->ival, P["intraRate"]->props.i[kOfxParamPropEnabled][0]);
        };
        P["recoverMode"]->ival = 10;
        styleTo(1);
        styleTo(0);
        P["residual"]->dval = 0.8;
        styleTo(1);
        return 0;
    }

    FILE* fo = fopen(argv[7], "wb");
    if (!fo) return 1;
    double renderMs = 0;
    int renders = 0;
    const bool times = getenv("MOCK_TIMES") != nullptr;  // per-frame render time and source fetches
    const int concurrent = getenv("MOCK_CONCURRENT") ? atoi(getenv("MOCK_CONCURRENT")) : 0;
    if (const char* sc = getenv("MOCK_SCRUB")) {
        // Like a user clicking around the timeline: renders of random frames
        // (some in short runs) on n threads, some aborted by the host part-way,
        // while a UI thread purges the plugin's caches holding a host lock that
        // frame fetches and parameter reads also take. Each frame rendered
        // to the end goes to its slot (out.rgb.mask marks them); a frame
        // rendered twice with different results counts as a mismatch.
        const int renderCount = atoi(sc);
        const int nth = concurrent > 0 ? concurrent : 4;
        const size_t frameBytes = (size_t)w * h * 4 * sizeof(float);
        std::atomic<int> started{0}, finished{0}, abortedN{0}, mismatched{0};
        std::atomic<bool> failed{false}, done{false};
        std::mutex outM;
        std::vector<uint64_t> sums((size_t)total, 0);
        gScrub = true;
        const auto wall0 = std::chrono::steady_clock::now();
        auto worker = [&](int k) {
            std::mt19937 rng(1234u + 7919u * (unsigned)k);
            int last = (int)(rng() % (unsigned)total), run = 0;
            for (int r; (r = started++) < renderCount && !failed;) {
                int i;
                if (run > 0) { i = (last + 1) % total; --run; }
                else { i = (int)(rng() % (unsigned)total); if (rng() % 2) run = (int)(rng() % 6); }
                last = i;
                const double t = gT0 + i;
                PropSet inArgs, outArgs, idOut;
                inArgs.d[kOfxPropTime] = {t};
                inArgs.i[kOfxImageEffectPropRenderWindow] = {gOffX, gOffY, gOffX + w, gOffY + h};
                inArgs.d[kOfxImageEffectPropRenderScale] = {1.0, 1.0};
                inArgs.i[kOfxPropIsInteractive] = {1};
                pl->mainEntry(kOfxImageEffectActionGetFramesNeeded, &inst, H(&inArgs), H(&outArgs));
                tlOutImage = nullptr;
                tlAborted = false;
                tlAbortAt = rng() % 3 == 0 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(rng() % 40)
                                           : std::chrono::steady_clock::time_point::max();
                if (pl->mainEntry(kOfxImageEffectActionIsIdentity, &inst, H(&inArgs), H(&idOut)) == kOfxStatOK) {
                    ++finished;
                    continue;
                }
                if (pl->mainEntry(kOfxImageEffectActionRender, &inst, H(&inArgs), nullptr) != kOfxStatOK || !tlOutImage) {
                    fprintf(stderr, "render %d (frame %d) failed\n", r, i);
                    failed = true;
                    return;
                }
                if (tlAborted) {
                    ++abortedN;
                } else {
                    uint64_t hs = 1469598103934665603ull;
                    const unsigned char* b = reinterpret_cast<const unsigned char*>(tlOutImage->data.data());
                    for (size_t q = 0; q < frameBytes; ++q) { hs ^= b[q]; hs *= 1099511628211ull; }
                    std::lock_guard<std::mutex> g(outM);
                    if (!sums[(size_t)i]) {
                        sums[(size_t)i] = hs;
                        fseek(fo, (long)(frameBytes * (size_t)i), SEEK_SET);
                        fwrite(tlOutImage->data.data(), 1, frameBytes, fo);
                    } else if (sums[(size_t)i] != hs) {
                        ++mismatched;
                        fprintf(stderr, "frame %d rendered differently\n", i);
                    }
                }
                {
                    std::lock_guard<std::mutex> g(gHostMutex);
                    for (size_t q = 0; q < out->live.size(); ++q)
                        if (out->live[q].get() == tlOutImage) { out->live.erase(out->live.begin() + (long)q); break; }
                }
                ++finished;
            }
        };
        std::thread ui([&]() {
            std::mt19937 rng(99);
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2 + rng() % 14));
                std::lock_guard<std::mutex> g(gUiMutex);
                pl->mainEntry(kOfxActionPurgeCaches, &inst, nullptr, nullptr);
            }
        });
        std::thread watchdog([&]() {
            int seen = -1;
            auto since = std::chrono::steady_clock::now();
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const int f = finished;
                if (f != seen) { seen = f; since = std::chrono::steady_clock::now(); continue; }
                if (std::chrono::steady_clock::now() - since > std::chrono::seconds(30)) {
                    printf("HANG: no render finished for 30 s (%d of %d done)\n", f, renderCount);
                    fflush(stdout);
                    std::_Exit(3);
                }
            }
        });
        std::vector<std::thread> th;
        for (int k = 0; k < nth; ++k) th.emplace_back(worker, k);
        for (auto& x : th) x.join();
        done = true;
        ui.join();
        watchdog.join();
        fclose(fo);
        const std::string maskPath = std::string(argv[7]) + ".mask";
        if (FILE* fm = fopen(maskPath.c_str(), "wb")) {
            for (uint64_t v : sums) fputc(v ? 1 : 0, fm);
            fclose(fm);
        }
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
        printf("scrub: %d renders, %d aborted, %d mismatched, wall %.0f ms\n", (int)finished, (int)abortedN,
               (int)mismatched, wall);
        if (failed || mismatched) return 1;
        CHECK(pl->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr), "destroy");
        CHECK(pl->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr), "unload");
        printf("done\n");
        return 0;
    }    if (concurrent > 0) {
        // Like a host playing back: n renders in flight, started in frame
        // order, finishing in any order. Output is written at each frame's slot.
        const size_t frameBytes = getenv("MOCK_FLOAT") ? (size_t)w * h * 4 * sizeof(float) : rgbOut.size();
        std::atomic<int> next{0};
        std::atomic<bool> failed{false};
        std::mutex outM;
        const auto wall0 = std::chrono::steady_clock::now();
        auto worker = [&]() {
            std::vector<unsigned char> rgb(rgbOut.size());
            for (int i; (i = next++) < total && !failed;) {
                const double t = gT0 + i;
                PropSet inArgs, outArgs, idOut;
                inArgs.d[kOfxPropTime] = {t};
                inArgs.i[kOfxImageEffectPropRenderWindow] = {gOffX, gOffY, gOffX + w, gOffY + h};
                inArgs.d[kOfxImageEffectPropRenderScale] = {1.0, 1.0};
                inArgs.i[kOfxPropIsInteractive] = {0};
                pl->mainEntry(kOfxImageEffectActionGetFramesNeeded, &inst, H(&inArgs), H(&outArgs));
                const std::vector<float>* img = nullptr;
                tlFetches = 0;
                tlOutImage = nullptr;
                const auto r0 = std::chrono::steady_clock::now();
                if (pl->mainEntry(kOfxImageEffectActionIsIdentity, &inst, H(&inArgs), H(&idOut)) == kOfxStatOK) {
                    img = &source->frames[std::min(i, (int)source->frames.size() - 1)].rgba;
                } else {
                    if (pl->mainEntry(kOfxImageEffectActionRender, &inst, H(&inArgs), nullptr) != kOfxStatOK || !tlOutImage) {
                        fprintf(stderr, "render %d failed\n", i);
                        failed = true;
                        return;
                    }
                    img = &tlOutImage->data;
                }
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
                std::lock_guard<std::mutex> g(outM);
                if (getenv("MOCK_FLOAT")) {
                    fseek(fo, (long)(frameBytes * i), SEEK_SET);
                    fwrite(img->data(), 1, frameBytes, fo);
                } else {
                    toRgb(*img);
                    fseek(fo, (long)(frameBytes * i), SEEK_SET);
                    fwrite(rgbOut.data(), 1, frameBytes, fo);
                }
                if (tlOutImage) {
                    std::lock_guard<std::mutex> h(gHostMutex);
                    for (size_t k = 0; k < out->live.size(); ++k)
                        if (out->live[k].get() == tlOutImage) { out->live.erase(out->live.begin() + (long)k); break; }
                    renderMs += ms;
                    ++renders;
                }
                if (times) printf("frame %d t=%.0f %.1f ms fetches %d\n", i, t, ms, tlFetches);
            }
        };
        std::vector<std::thread> th;
        for (int k = 0; k < concurrent; ++k) th.emplace_back(worker);
        for (auto& x : th) x.join();
        fclose(fo);
        if (failed) return 1;
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
        printf("render: %d calls, %.0f ms total, %.1f ms avg, wall %.0f ms\n", renders, renderMs,
               renders ? renderMs / renders : 0.0, wall);
        CHECK(pl->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr), "destroy");
        CHECK(pl->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr), "unload");
        printf("done\n");
        return 0;
    }
    // MOCK_ORDER=i,j,...: render frames in this order (like jumping around the
    // timeline); each frame's output still goes to its own slot.
    std::vector<int> order;
    if (const char* e = getenv("MOCK_ORDER"))
        for (const char* p = e; *p;) {
            char* end = nullptr;
            const long v = strtol(p, &end, 10);
            if (end == p) break;
            if (v >= 0 && v < total) order.push_back((int)v);
            p = *end == ',' ? end + 1 : end;
        }
    const size_t slotBytes = getenv("MOCK_FLOAT") ? (size_t)w * h * 4 * sizeof(float) : rgbOut.size();
    const int count = order.empty() ? total : (int)order.size();
    for (int k = 0; k < count; ++k) {
        const int i = order.empty() ? k : order[k];
        if (!order.empty()) fseek(fo, (long)(slotBytes * (size_t)i), SEEK_SET);
        const double t = gT0 + i;
        PropSet inArgs, outArgs;
        inArgs.d[kOfxPropTime] = {t};
        const int sw = std::max(1, (int)std::lround(w * gScale)), sh = std::max(1, (int)std::lround(h * gScale));
        inArgs.i[kOfxImageEffectPropRenderWindow] = {gOffX, gOffY, gOffX + sw, gOffY + sh};
        inArgs.d[kOfxImageEffectPropRenderScale] = {gScale, gScale};
        inArgs.i[kOfxPropIsInteractive] = {0};

        CHECK(pl->mainEntry(kOfxImageEffectActionGetFramesNeeded, &inst, H(&inArgs), H(&outArgs)), "getFramesNeeded");
        if (i == 1 || i == total / 2) {
            for (auto& kv : outArgs.d) {
                printf("frame %d framesNeeded[%s]:", i, kv.first.c_str());
                for (size_t k = 0; k + 1 < kv.second.size(); k += 2) printf(" [%.0f..%.0f]", kv.second[k], kv.second[k + 1]);
                printf("\n");
            }
        }

        PropSet idOut;
        OfxStatus idSt = pl->mainEntry(kOfxImageEffectActionIsIdentity, &inst, H(&inArgs), H(&idOut));
        if (idSt == kOfxStatOK) {
            Clip* src = inst.clips[idOut.s[kOfxPropName][0]].get();
            printf("frame %d: identity -> %s\n", i, src->name.c_str());
            const Frame& fr = src->frames[std::min(i, (int)src->frames.size() - 1)];
            if (getenv("MOCK_FLOAT")) fwrite(fr.rgba.data(), sizeof(float), fr.rgba.size(), fo);
            else {
                toRgb(fr.rgba);
                fwrite(rgbOut.data(), 1, rgbOut.size(), fo);
            }
            continue;
        }

        from->requested.clear(); to->requested.clear(); out->live.clear();
        if (getenv("MOCK_PURGE"))  // render every frame from a cold cache, like scrubbing
            CHECK(pl->mainEntry(kOfxActionPurgeCaches, &inst, nullptr, nullptr), "purgeCaches");
        const auto r0 = std::chrono::steady_clock::now();
        tlFetches = 0;
        CHECK(pl->mainEntry(kOfxImageEffectActionRender, &inst, H(&inArgs), nullptr), "render");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
        renderMs += ms;
        ++renders;
        if (times) printf("frame %d t=%.0f %.1f ms fetches %d\n", i, t, ms, tlFetches);
        if (out->live.empty()) { fprintf(stderr, "render did not fetch output image\n"); return 1; }
        if (getenv("MOCK_FLOAT")) {  // raw float RGBA of the output image, for exact comparisons
            const std::vector<float>& s = out->live.back()->data;
            fwrite(s.data(), sizeof(float), s.size(), fo);
        } else if (gScale != 1.0) {  // nearest-neighbour back to full size
            const int sw = std::max(1, (int)std::lround(w * gScale)), sh = std::max(1, (int)std::lround(h * gScale));
            const std::vector<float>& s = out->live.back()->data;
            std::vector<float> up((size_t)w * h * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    memcpy(&up[((size_t)y * w + x) * 4], &s[((size_t)std::min(sh - 1, (int)(y * gScale)) * sw + std::min(sw - 1, (int)(x * gScale))) * 4], 4 * sizeof(float));
            toRgb(up);
            fwrite(rgbOut.data(), 1, rgbOut.size(), fo);
        } else {
            toRgb(out->live.back()->data);
            fwrite(rgbOut.data(), 1, rgbOut.size(), fo);
        }
        if (i == 1 || i == total / 2) {
            printf("frame %d: fetched From at", i); for (double x : from->requested) printf(" %.0f", x);
            printf(" | To at"); for (double x : to->requested) printf(" %.0f", x); printf("\n");
        }
        from->live.clear(); to->live.clear();
    }
    fclose(fo);
    printf("render: %d calls, %.0f ms total, %.1f ms avg\n", renders, renderMs, renders ? renderMs / renders : 0.0);
    CHECK(pl->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr), "destroy");
    CHECK(pl->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr), "unload");
    printf("done\n");
    return 0;
}
