#pragma once

// GPU device enumeration, numbered exactly the way whisper.cpp's
// `whisper_context_params.gpu_device` counts them (GPU + iGPU devices only,
// in ggml enumeration order). Keep this module the source of truth so the
// CLI, the GUI picker and the model loader all agree on what "device 0/1"
// means.

#include <cstddef>
#include <string>
#include <vector>

namespace inference {

struct GpuDeviceInfo {
    // Value to pass as whisper's gpu_device (0 = first GPU/iGPU, 1 = next...).
    int         index = 0;
    // Short ggml name, e.g. "Vulkan0".
    std::string name;
    // Full backend description, e.g. "NVIDIA GeForce RTX 3050 Laptop GPU ...".
    std::string description;
    bool        discrete   = false;   // GGML_BACKEND_DEVICE_TYPE_GPU
    bool        integrated = false;   // GGML_BACKEND_DEVICE_TYPE_IGPU
    size_t      memory_free  = 0;
    size_t      memory_total = 0;
};

// Lists the available GPU/iGPU devices in whisper's counting order.
// Empty when the build has no GPU backend or none is present (CPU only).
std::vector<GpuDeviceInfo> enumerate_gpu_devices();

// -1 when `devices` is empty; otherwise the index of the first discrete GPU
// (preferred) or, failing that, the first integrated one.
int best_gpu_device(const std::vector<GpuDeviceInfo> & devices);

} // namespace inference