#include "FrameSlotRetirement.hpp"

#include <cassert>
#include <cstdio>

static void waits_for_slot_reuse() {
    for (int framesInFlight : { 1, 2, 3 }) {
        FrameSlotRetirement retirement { 0, 10 };
        for (int offset = 1; offset < framesInFlight; ++offset) {
            assert(! retirement.ready(offset, 10 + std::uint64_t(offset)));
        }
        assert(retirement.ready(0, 10 + std::uint64_t(framesInFlight)));
        assert(! retirement.ready(0, 10));
    }
}

static void queue_releases_once() {
    FrameSlotRetirementQueue<int> queue;
    queue.retire(11, { 0, 5 });
    queue.retire(22, { 1, 6 });

    int released = 0;
    queue.collect(1, 6, [&released](int) {
        ++released;
    });
    assert(released == 0);
    queue.collect(0, 7, [&released](int value) {
        assert(value == 11);
        ++released;
    });
    assert(released == 1);
    queue.collect(0, 8, [&released](int) {
        ++released;
    });
    assert(released == 1);
    queue.collect(1, 9, [&released](int value) {
        assert(value == 22);
        ++released;
    });
    assert(released == 2);
    assert(queue.empty());

    queue.retire(33, { 0, 10 });
    queue.retire(44, { 1, 10 });
    queue.drain([&released](int) {
        ++released;
    });
    assert(released == 4);
    assert(queue.empty());
}

int main() {
    waits_for_slot_reuse();
    queue_releases_once();
    std::puts("test_frame_slot_retirement: OK");
    return 0;
}
