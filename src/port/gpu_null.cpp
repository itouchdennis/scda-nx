// A backend that draws nothing (SCDA_GPU=null): the game runs, paces its frames and plays its sound, for testing the
// recompiled code, the kernel and the sound without a renderer. Locks find guest memory as the CPU left it.
#include <algorithm>
#include <chrono>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

namespace {

class NullBackend final : public Backend {
public:
    bool Init(uint32_t width, uint32_t height) override {
        LOGI("gpu: null backend, %ux%u, nothing is drawn", width, height);
        return true;
    }
    void Clear(const State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
    void Draw(const State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override { draws_++; }
    void Copy(const Surface&, const Surface&, uint32_t, uint32_t, uint32_t) override {}
    // D3DDevice_Swap paces the frames itself
    void Present(const Surface&, bool) override {
        frames_++;
        auto now = std::chrono::steady_clock::now();
        if (now - last_ < std::chrono::seconds(10)) return;
        double s = std::chrono::duration<double>(now - last_).count();
        LOGR("gpu: %.1f fps, %.0f draws a frame", (frames_ - last_frames_) / s,
             double(draws_ - last_draws_) / double(std::max<uint64_t>(1, frames_ - last_frames_)));
        last_ = now;
        last_frames_ = frames_;
        last_draws_ = draws_;
    }
    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    uint64_t frames_ = 0, draws_ = 0, last_frames_ = 0, last_draws_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<NullBackend>(); }
void SavePipelineCache() {}

}  // namespace gpu
