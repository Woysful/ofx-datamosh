// Minimal OpenFX host for exercising the plugin outside Resolve.
//   mock_host <plugin.ofx> <from.rgb> <to.rgb> <w> <h> <frames> <out.rgb> [view]
// The Source clip is 20 frames of "from" followed by "to", starting at timeline
// frame 1000; the plugin has to find the cut itself.
// Images are handed to the plugin bottom-up, like real OFX hosts do.
#include <dlfcn.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
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
static OfxStatus propSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v) { setAt(PS(h)->s[n], idx, std::string(v)); return kOfxStatOK; }
static OfxStatus propSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v) { setAt(PS(h)->d[n], idx, v); return kOfxStatOK; }
static OfxStatus propSetInt(OfxPropertySetHandle h, const char* n, int idx, int v) { setAt(PS(h)->i[n], idx, v); return kOfxStatOK; }
static OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { for (int k = 0; k < c; ++k) propSetPointer(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { for (int k = 0; k < c; ++k) propSetString(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { for (int k = 0; k < c; ++k) propSetDouble(h, n, k, v[k]); return kOfxStatOK; }
static OfxStatus propSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { for (int k = 0; k < c; ++k) propSetInt(h, n, k, v[k]); return kOfxStatOK; }

static OfxStatus propGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v) {
    auto it = PS(h)->p.find(n); if (it == PS(h)->p.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing pointer prop %s\n", n); return kOfxStatErrUnknown; }
    *v = it->second[idx]; return kOfxStatOK; }
static OfxStatus propGetString(OfxPropertySetHandle h, const char* n, int idx, char** v) {
    auto it = PS(h)->s.find(n); if (it == PS(h)->s.end() || idx >= (int)it->second.size()) { fprintf(stderr, "  [host] missing string prop %s\n", n); return kOfxStatErrUnknown; }
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
static OfxStatus propReset(OfxPropertySetHandle h, const char* n) { PS(h)->s.erase(n); PS(h)->d.erase(n); PS(h)->i.erase(n); PS(h)->p.erase(n); return kOfxStatOK; }
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
    bool isTransition = false;
};
struct ParamSet {
    PropSet props;
    std::map<std::string, std::unique_ptr<Param>> params;
};
static double gT0 = 1000.0, gDur = 35.0;  // transition timing on the mock timeline

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
    Param* p = reinterpret_cast<Param*>(ph);
    va_list ap; va_start(ap, t);
    if (p->type == kOfxParamTypeDouble) {
        double* out = va_arg(ap, double*);
        *out = p->isTransition ? std::min(1.0, std::max(0.0, (t - gT0) / gDur)) : p->dval;
    } else {
        int* out = va_arg(ap, int*);
        *out = p->ival;
    }
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus paramGetValue(OfxParamHandle ph, ...) {
    Param* p = reinterpret_cast<Param*>(ph);
    va_list ap; va_start(ap, ph);
    if (p->type == kOfxParamTypeDouble) *va_arg(ap, double*) = p->dval; else *va_arg(ap, int*) = p->ival;
    va_end(ap);
    return kOfxStatOK;
}
static OfxStatus unsupported(...) { return kOfxStatErrUnsupported; }

static OfxParameterSuiteV1 gParamSuite = {
    paramDefine, paramGetHandle, paramSetGetPropertySet, paramGetPropertySet,
    paramGetValue, paramGetValueAtTime,
    (OfxStatus(*)(OfxParamHandle, OfxTime, ...))unsupported,            // derivative
    (OfxStatus(*)(OfxParamHandle, OfxTime, OfxTime, ...))unsupported,   // integral
    (OfxStatus(*)(OfxParamHandle, ...))unsupported,                     // setValue
    (OfxStatus(*)(OfxParamHandle, OfxTime, ...))unsupported,            // setValueAtTime
    (OfxStatus(*)(OfxParamHandle, unsigned int*))unsupported,
    (OfxStatus(*)(OfxParamHandle, unsigned int, OfxTime*))unsupported,
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
static OfxStatus clipGetImage(OfxImageClipHandle ch, OfxTime t, const OfxRectD*, OfxPropertySetHandle* out) {
    Clip* c = reinterpret_cast<Clip*>(ch);
    c->requested.push_back(t);
    auto live = std::make_unique<Clip::Live>();
    live->data.resize((size_t)c->w * c->h * 4, 0.f);
    if (!c->frames.empty()) {
        int idx = (int)std::lround(t - gT0);
        idx = std::min(std::max(idx, 0), (int)c->frames.size() - 1);
        live->data = c->frames[idx].rgba;
    }
    PropSet& p = live->props;
    p.p[kOfxImagePropData] = {live->data.data()};
    p.i[kOfxImagePropBounds] = {0, 0, c->w, c->h};
    p.i[kOfxImagePropRegionOfDefinition] = {0, 0, c->w, c->h};
    p.i[kOfxImagePropRowBytes] = {c->w * 4 * (int)sizeof(float)};
    p.s[kOfxImageEffectPropComponents] = {kOfxImageComponentRGBA};
    p.s[kOfxImageEffectPropPixelDepth] = {kOfxBitDepthFloat};
    p.d[kOfxImagePropPixelAspectRatio] = {1.0};
    *out = H(&p);
    c->live.push_back(std::move(live));
    return kOfxStatOK;
}
static OfxStatus clipReleaseImage(OfxPropertySetHandle img) {
    // find owner: brute force over all clips is overkill; just leave it, memory is freed at clip teardown
    (void)img;
    return kOfxStatOK;
}
static OfxStatus clipGetRoD(OfxImageClipHandle ch, OfxTime, OfxRectD* r) { Clip* c = reinterpret_cast<Clip*>(ch); *r = {0, 0, (double)c->w, (double)c->h}; return kOfxStatOK; }
static int abortFn(OfxImageEffectHandle) { return 0; }
static OfxStatus memAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) { *h = reinterpret_cast<OfxImageMemoryHandle>(malloc(n)); return kOfxStatOK; }
static OfxStatus memFree(OfxImageMemoryHandle h) { free(h); return kOfxStatOK; }
static OfxStatus memLock(OfxImageMemoryHandle h, void** p) { *p = h; return kOfxStatOK; }
static OfxStatus memUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

static OfxImageEffectSuiteV1 gEffectSuite = {
    getPropertySet, getParamSet, clipDefine, clipGetHandle, clipGetPropertySet, clipGetImage, clipReleaseImage,
    clipGetRoD, abortFn, memAlloc, memFree, memLock, memUnlock,
};

static PropSet gHostProps;
static const void* fetchSuite(OfxPropertySetHandle, const char* name, int ver) {
    if (!strcmp(name, kOfxPropertySuite) && ver == 1) return &gPropSuite;
    if (!strcmp(name, kOfxParameterSuite) && ver == 1) return &gParamSuite;
    if (!strcmp(name, kOfxImageEffectSuite) && ver == 1) return &gEffectSuite;
    return nullptr;  // no multithread suite: plugin must fall back to serial
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
    if (argc < 8) { fprintf(stderr, "usage: %s plugin.ofx from.rgb to.rgb w h frames out.rgb [view]\n", argv[0]); return 1; }
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

    gHostProps.s[kOfxPropName] = {"mock.host"};
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
    printf("\nparams (%zu):", inst.params.params.size());
    for (auto& p : inst.params.params) printf(" %s", p.first.c_str());
    printf("\n");

    // Host-side parameters: Transition, plus defaults for the plugin's own.
    for (auto& kv : inst.params.params) {
        Param& p = *kv.second;
        if (p.type == kOfxParamTypeDouble) { auto it = p.props.d.find(kOfxParamPropDefault); p.dval = it != p.props.d.end() ? it->second[0] : 0.0; }
        else { auto it = p.props.i.find(kOfxParamPropDefault); p.ival = it != p.props.i.end() ? it->second[0] : 0; }
    }
    {
        auto p = std::make_unique<Param>(); p->type = kOfxParamTypeDouble; p->isTransition = true;
        inst.params.params[kOfxImageEffectTransitionParamName] = std::move(p);
    }
    inst.params.params["view"]->ival = view;

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
    for (Clip* c : {from, to, out}) {
        c->props.d[kOfxImageEffectPropFrameRange] = {gT0, gT0 + gDur};
        c->props.d[kOfxImageEffectPropFrameRate] = {30.0};
        c->props.s[kOfxImageEffectPropComponents] = {kOfxImageComponentRGBA};
        c->props.s[kOfxImageEffectPropPixelDepth] = {kOfxBitDepthFloat};
    }
    CHECK(pl->mainEntry(kOfxActionCreateInstance, &inst, nullptr, nullptr), "createInstance");

    FILE* fo = fopen(argv[7], "wb");
    if (!fo) return 1;
    for (int i = 0; i < total; ++i) {
        const double t = gT0 + i;
        PropSet inArgs, outArgs;
        inArgs.d[kOfxPropTime] = {t};
        inArgs.i[kOfxImageEffectPropRenderWindow] = {0, 0, w, h};
        inArgs.d[kOfxImageEffectPropRenderScale] = {1.0, 1.0};
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
            toRgb(fr.rgba);
            fwrite(rgbOut.data(), 1, rgbOut.size(), fo);
            continue;
        }

        from->requested.clear(); to->requested.clear(); out->live.clear();
        CHECK(pl->mainEntry(kOfxImageEffectActionRender, &inst, H(&inArgs), nullptr), "render");
        if (out->live.empty()) { fprintf(stderr, "render did not fetch output image\n"); return 1; }
        toRgb(out->live.back()->data);
        fwrite(rgbOut.data(), 1, rgbOut.size(), fo);
        if (i == 1 || i == total / 2) {
            printf("frame %d: fetched From at", i); for (double x : from->requested) printf(" %.0f", x);
            printf(" | To at"); for (double x : to->requested) printf(" %.0f", x); printf("\n");
        }
        from->live.clear(); to->live.clear();
    }
    fclose(fo);
    CHECK(pl->mainEntry(kOfxActionDestroyInstance, &inst, nullptr, nullptr), "destroy");
    CHECK(pl->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr), "unload");
    printf("done\n");
    return 0;
}
