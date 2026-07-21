#pragma once

// Internal seam between capture.cpp (dispatch) and the per-backend
// translation units. Not part of the public core API.

#include <memory>
#include <vector>

#include "core/audio/capture.h"

namespace audio {
namespace detail {

#ifndef _WIN32
std::vector<CaptureDeviceInfo>  enumerate_alsa();
std::unique_ptr<CaptureBackend> make_alsa(const std::string & device_id);

// True when a jackd is reachable right now (cheap open/close probe).
bool                            jack_server_reachable();
std::unique_ptr<CaptureBackend> make_jack();
#endif

std::vector<CaptureDeviceInfo>  enumerate_usb();
std::unique_ptr<CaptureBackend> make_usb(const std::string & device_id);

#ifdef _WIN32
std::vector<CaptureDeviceInfo>  enumerate_wasapi();
std::unique_ptr<CaptureBackend> make_wasapi(const std::string & device_id);
#endif

} // namespace detail
} // namespace audio
