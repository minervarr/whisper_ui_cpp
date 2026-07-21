#pragma once

#include <memory>
#include <string>
#include <vector>

namespace audio {

enum class BackendKind {
    Wasapi = 0,   // Windows
    Alsa   = 1,   // Linux, direct hw:
    Jack   = 2,   // Linux, client of the user's running jackd (real JACK2)
    Usb    = 3,   // both, libusb straight to the DAC
};

struct CaptureDeviceInfo {
    BackendKind kind;
    std::string id;     // "hw:1,0" | "LIBUSB:vid:pid" | wasapi endpoint id | "" (jack: server)
    std::string name;   // display name, UTF-8
};

// One instance per recording session.
class CaptureBackend {
public:
    virtual ~CaptureBackend() = default;

    // Opens the device and starts the capture thread. Returns "" on success,
    // else a Spanish error message.
    virtual std::string start() = 0;

    // Stops and joins; afterwards take_buffer() holds the whole take.
    virtual void stop() = 0;

    // The accumulated take: mono float 16 kHz. One-shot (moves ownership out).
    virtual std::shared_ptr<std::vector<float>> take_buffer() = 0;

    // Last-chunk peak 0..1 for the level indicator. Thread-safe.
    virtual float peak() const = 0;

    virtual bool running() const = 0;

    // Non-empty when the device died mid-take (Spanish message).
    virtual std::string abort_reason() const = 0;
};

// All backends merged (ALSA + USB lists, plus one JACK entry when a server
// is reachable; WASAPI endpoints on Windows).
std::vector<CaptureDeviceInfo> enumerate_capture_devices();

std::unique_ptr<CaptureBackend> make_capture(const CaptureDeviceInfo & dev);

} // namespace audio
