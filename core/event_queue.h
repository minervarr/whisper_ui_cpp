#pragma once

#include <mutex>
#include <vector>

namespace inference { struct Result; }

namespace core {

// Cross-thread notification, drained once per frame (GUI) or per poll (CLI).
// Replaces the old PostMessageW(WM_TRANSCRIBE_DONE/...) mechanism.
struct AppEvent {
    enum Kind {
        TranscribeDone,   // result != nullptr; receiver takes ownership (delete)
        ModelLoaded,
        ModelFailed,
    } kind = TranscribeDone;

    inference::Result* result = nullptr;
};

class EventQueue {
public:
    void push(AppEvent ev)
    {
        std::lock_guard<std::mutex> lk(mu_);
        events_.push_back(ev);
    }

    // Returns queued events in push order and empties the queue.
    std::vector<AppEvent> drain()
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<AppEvent> out;
        out.swap(events_);
        return out;
    }

private:
    std::mutex            mu_;
    std::vector<AppEvent> events_;
};

// Process-wide queue instance shared by loaders/workers and the frame loop.
inline EventQueue& events()
{
    static EventQueue q;
    return q;
}

} // namespace core
