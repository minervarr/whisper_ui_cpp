#include "harness.hpp"

#include "core/event_queue.h"

#include <thread>

TEST(event_queue_push_drain_order)
{
    core::EventQueue q;
    const int N = 100;
    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            core::AppEvent ev;
            ev.kind   = core::AppEvent::TranscribeDone;
            // Smuggle the sequence number through the pointer field; the
            // test never dereferences it.
            ev.result = reinterpret_cast<inference::Result*>(static_cast<intptr_t>(i + 1));
            q.push(ev);
        }
    });
    producer.join();

    std::vector<core::AppEvent> got = q.drain();
    CHECK_EQ(got.size(), (size_t) N);
    bool in_order = true;
    for (int i = 0; i < N; ++i) {
        if (reinterpret_cast<intptr_t>(got[(size_t) i].result) != i + 1) in_order = false;
    }
    CHECK(in_order);

    CHECK(q.drain().empty());
}

TEST(event_queue_drain_empties)
{
    core::EventQueue q;
    q.push({core::AppEvent::ModelLoaded, nullptr});
    CHECK_EQ(q.drain().size(), (size_t) 1);
    CHECK(q.drain().empty());
}
