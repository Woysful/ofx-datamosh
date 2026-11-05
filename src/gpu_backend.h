// gpu_backend.h - minimal CUDA / OpenCL compute backends.
//
// The drivers are loaded at run time (nvcuda.dll / libcuda.so.1,
// OpenCL.dll / libOpenCL.so.1), so the plugin has no build or load-time
// dependency on either; without a driver it simply stays on the CPU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>

namespace gpu {

enum class Api { Cuda, OpenCL };

// Kernels of mosh_kernels.cl, in the order of kKernelNames.
enum Kernel {
    kLuma, kHalve, kSearchRows, kSearchPick, kFlat, kRefine, kStep, kRefreshAct, kPull,
    kBlurRows, kBlurCols, kBands, kSlice, kSettleDiff, kGather, kCompose, kKernelCount
};

struct Buffer {
    uint64_t h = 0;  // CUdeviceptr or cl_mem
    size_t bytes = 0;
    explicit operator bool() const { return h != 0; }
};

struct Arg {
    enum Kind { Buf, Int, Float } kind;
    uint64_t b = 0;
    int i = 0;
    float f = 0.f;
    Arg(const Buffer& x) : kind(Buf), b(x.h) {}
    Arg(int x) : kind(Int), i(x) {}
    Arg(float x) : kind(Float), f(x) {}
};

// One device with its own queue (CUDA: the device's primary context and a
// non-blocking stream, so the host's own GPU work is not serialised with
// ours). Not thread-safe: callers serialise access.
class Device {
public:
    virtual ~Device() {}
    virtual Api api() const = 0;
    const std::string& name() const { return name_; }
    // Last error, for diagnostics.
    const std::string& error() const { return err_; }

    virtual bool alloc(Buffer& b, size_t bytes) = 0;
    virtual void release(Buffer& b) = 0;
    virtual bool upload(const Buffer& b, const void* src, size_t bytes) = 0;
    virtual bool download(const Buffer& b, void* dst, size_t bytes) = 0;  // waits for the queue
    virtual bool copy(const Buffer& dst, const Buffer& src, size_t bytes) = 0;
    // Runs kernel k over n work items; n is passed as its first argument.
    virtual bool run(Kernel k, int n, std::initializer_list<Arg> args) = 0;

protected:
    std::string name_, err_;
};

// Opens the first suitable device of api; nullptr with a reason otherwise.
// OpenCL takes GPUs only unless anyType is set, then falls back to other
// device types (CPU runtimes). OFX_DATAMOSH_GPU_DEVICE picks a device by
// part of its name.
std::unique_ptr<Device> openDevice(Api api, bool anyType, std::string& why);

}  // namespace gpu
