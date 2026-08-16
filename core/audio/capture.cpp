#include "core/audio/capture.h"
#include "core/audio/capture_backends.h"

namespace audio {

std::vector<CaptureDeviceInfo> enumerate_capture_devices()
{
    std::vector<CaptureDeviceInfo> out;

#ifdef _WIN32
    for (auto & d : detail::enumerate_wasapi()) out.push_back(std::move(d));
    for (auto & d : detail::enumerate_usb()) out.push_back(std::move(d));
#elif defined(__ANDROID__)
    // AAudio only — direct USB DAC capture is desktop-only for now.
    for (auto & d : detail::enumerate_aaudio()) out.push_back(std::move(d));
#else
    for (auto & d : detail::enumerate_alsa()) out.push_back(std::move(d));
    if (detail::jack_server_reachable()) {
        out.push_back({BackendKind::Jack, "", "Servidor JACK"});
    }
    for (auto & d : detail::enumerate_usb()) out.push_back(std::move(d));
#endif

    return out;
}

std::unique_ptr<CaptureBackend> make_capture(const CaptureDeviceInfo & dev)
{
    switch (dev.kind) {
#ifdef _WIN32
        case BackendKind::Wasapi: return detail::make_wasapi(dev.id);
        case BackendKind::Usb:    return detail::make_usb(dev.id);
#elif defined(__ANDROID__)
        case BackendKind::Aaudio: return detail::make_aaudio(dev.id);
#else
        case BackendKind::Alsa:   return detail::make_alsa(dev.id);
        case BackendKind::Jack:   return detail::make_jack();
        case BackendKind::Usb:    return detail::make_usb(dev.id);
#endif
        default:                  return nullptr;
    }
}

} // namespace audio
