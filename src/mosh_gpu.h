// mosh_gpu.h - the per-pixel stages of mosh_core.h on a GPU.
//
// The CPU keeps every per-block decision (median filtering, flat fill,
// intra decisions, refresh order); the GPU does the pixel work and keeps the
// source frames, their luma pyramids and the accumulated picture in device
// memory between steps and between renders. Results are bit-identical to
// the CPU path.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mosh_core.h"

namespace mosh_gpu {

using FramePtr = std::shared_ptr<const mosh::Image>;

enum class Preference { Auto = 0, Cpu = 1, Cuda = 2, OpenCL = 3 };

struct Engine;
class Session;

// A session on the GPU the preference asks for, waiting while another render
// uses it; nullptr when the work stays on the CPU, with the reason in why
// (empty for Preference::Cpu).
std::unique_ptr<Session> begin(Preference p, std::string& why);

// Exclusive use of the GPU for one render. Every call returns false on a GPU
// error; the render then starts over on the CPU, and the session drops the
// device memory when it ends.
class Session {
public:
    explicit Session(Engine& e);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    std::string label() const;  // "CUDA <device>" or "OPENCL <device>"
    const std::string& error() const { return error_; }
    bool failed() const { return failed_; }

    // The accumulated picture: set from the CPU, restored from / stored in
    // the device cache (store moves it there), or downloaded.
    bool setAcc(const mosh::Image& img);
    bool restoreAcc(const void* owner, double t, double cut, uint64_t hash, std::vector<uint8_t>& refreshed);
    bool storeAcc(const void* owner, double t, double cut, uint64_t hash, const std::vector<uint8_t>& refreshed);
    bool downloadAcc(mosh::Image& out);

    // Same results as mosh::estimateMotion, mosh::moshStep, the pixel loop of
    // naturalAfter / refreshBlend (pull: k = rate in flagged blocks, tail
    // elsewhere), mosh::refreshActivity and mosh::compose.
    bool estimateMotion(const FramePtr& prev, const FramePtr& cur, const mosh::MotionParams& mp,
                        mosh::MotionField& mf, const mosh::ParallelFor& pf);
    bool moshStep(const FramePtr& prev, const FramePtr& cur, const mosh::MotionField& mf, const mosh::StepParams& sp);
    bool pull(const FramePtr& cur, const mosh::MotionField& mf, const std::vector<uint8_t>& flagged, float rate,
              float tail);
    bool refreshActivity(const FramePtr& prev, const FramePtr& cur, const mosh::MotionField& mf,
                         std::vector<float>& sums, std::vector<int>& counts);
    bool compose(const FramePtr& to, const mosh::ComposeParams& cp, float q, mosh::Image& out);

private:
    friend std::unique_ptr<Session> begin(Preference p, std::string& why);
    bool fail(const std::string& what);
    bool ok(bool r);

    Engine& e_;
    std::unique_lock<std::mutex> lock_;
    bool failed_ = false;
    std::string error_;
};

// Drops the cached pictures of one effect instance (owner as given to storeAcc).
void forget(const void* owner);

// Releases the device and its memory.
void shutdown();

}  // namespace mosh_gpu
