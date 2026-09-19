#include "harness.hpp"

#include "../core/gpu_devices.h"

#include <vector>

namespace {

// Shorthand builders so the selection logic is tested with data shape, not
// ggml's real devices (which depend on the machine the tests run on).
inference::GpuDeviceInfo igpu(int idx) { return {idx, "iGPU", "", false, true, 0, 0}; }
inference::GpuDeviceInfo gpu(int idx)  { return {idx, "dGPU", "", true, false, 0, 0}; }

} // namespace

TEST(best_gpu_prefers_discrete)
{
    std::vector<inference::GpuDeviceInfo> v{igpu(0), gpu(1)};
    CHECK_EQ(inference::best_gpu_device(v), 1);
}

TEST(best_gpu_falls_back_to_integrated)
{
    std::vector<inference::GpuDeviceInfo> v{igpu(0)};
    CHECK_EQ(inference::best_gpu_device(v), 0);
}

TEST(best_gpu_empty_is_minus_one)
{
    CHECK_EQ(inference::best_gpu_device({}), -1);
}

TEST(enumerate_indexes_match_position)
{
    // Whatever the machine has (possibly nothing), the returned index of each
    // entry must equal its position in the list — the same contiguity whisper
    // relies on for gpu_device.
    auto v = inference::enumerate_gpu_devices();
    for (size_t i = 0; i < v.size(); ++i) CHECK_EQ(v[i].index, (int) i);
    if (v.empty()) CHECK_EQ(inference::best_gpu_device(v), -1);
}