#include "core/gpu_devices.h"

#include <utility>

#include "ggml-backend.h"

namespace inference {

std::vector<GpuDeviceInfo> enumerate_gpu_devices()
{
    std::vector<GpuDeviceInfo> out;

    // Same rule as whisper_backend_init_gpu (src/whisper.cpp): only GPU and
    // iGPU device types count, in ggml registration order. The registry is
    // lazily populated on first access, so no explicit backend load is
    // needed — this is exactly what whisper's own "devices = N" log sees.
    int cnt = 0;
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
            type != GGML_BACKEND_DEVICE_TYPE_IGPU) {
            continue;
        }

        GpuDeviceInfo info;
        info.index = cnt++;
        const char * name = ggml_backend_dev_name(dev);
        info.name = name ? name : "";
        const char * desc = ggml_backend_dev_description(dev);
        info.description = desc ? desc : "";
        info.discrete   = type == GGML_BACKEND_DEVICE_TYPE_GPU;
        info.integrated = type == GGML_BACKEND_DEVICE_TYPE_IGPU;
        size_t free_mem = 0, total_mem = 0;
        ggml_backend_dev_memory(dev, &free_mem, &total_mem);
        info.memory_free  = free_mem;
        info.memory_total = total_mem;

        out.push_back(std::move(info));
    }
    return out;
}

int best_gpu_device(const std::vector<GpuDeviceInfo> & devices)
{
    for (const auto & d : devices) if (d.discrete)   return d.index;
    for (const auto & d : devices) if (d.integrated) return d.index;
    return -1;
}

} // namespace inference