#include "PresentationState.hpp"
#include "FrameSlotRetirement.hpp"

#include <cassert>
#include <cstdio>

static PresentationState::Config config(std::uint64_t bufferGeneration,
                                        std::uint64_t configGeneration, int size) {
    PresentationState::Config value;
    value.valid            = true;
    value.bufferGeneration = bufferGeneration;
    value.configGeneration = configGeneration;
    value.sourceRect       = QRectF(0, 0, size, size);
    value.destRect         = QRectF(0, 0, size, size);
    value.clearColor       = QColor::fromRgbF(0.1, 0.2, 0.3, 1.0);
    return value;
}

static PresentationState::Content bind(PresentationState& state, std::uint64_t bufferGeneration,
                                       std::uint64_t contentToken,
                                       std::uint64_t presentationGeneration, int size) {
    state.beginIncoming(
        bufferGeneration, contentToken, presentationGeneration, size, size, 10, true);
    assert(state.applyConfig(config(bufferGeneration, bufferGeneration, size)) ==
           PresentationState::ConfigResult::Staged);
    PresentationState::Content value;
    assert(state.incomingFor(bufferGeneration, value));
    return value;
}

static PresentationState::AcceptResult submit(PresentationState&                state,
                                              const PresentationState::Content& content,
                                              bool transition, bool retained,
                                              std::uint64_t& serial) {
    const auto prepared = state.prepare(content, transition, retained, serial);
    assert(prepared == PresentationState::PrepareResult::Direct ||
           prepared == PresentationState::PrepareResult::Transition);
    assert(state.promote(serial));
    PresentationState::Content accepted;
    return state.accept(serial, accepted);
}

static void test_first_content_is_direct_and_commits_after_accept() {
    PresentationState state;
    const auto        content = bind(state, 1, 10, 1, 100);
    std::uint64_t     serial  = 0;
    assert(state.prepare(content, true, true, serial) == PresentationState::PrepareResult::Direct);
    assert(state.committedToken() == 0);
    assert(state.promote(serial));
    assert(state.committedToken() == 0);
    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::ContentChanged);
    assert(state.committedToken() == 10);
}

static void test_different_content_transitions_after_committed_content() {
    PresentationState state;
    std::uint64_t     serial = 0;
    assert(submit(state, bind(state, 1, 10, 1, 100), true, true, serial) ==
           PresentationState::AcceptResult::ContentChanged);

    const auto next = bind(state, 2, 20, 2, 200);
    assert(state.prepare(next, true, true, serial) == PresentationState::PrepareResult::Transition);
    assert(state.committedToken() == 10);
    assert(state.promote(serial));
    assert(state.committedToken() == 10);
    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::ContentChanged);
    assert(state.committedToken() == 20);
}

static void test_same_content_rebind_is_direct() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 33, 1, 100), true, true, serial);
    const auto rebound = bind(state, 2, 33, 2, 200);
    assert(state.prepare(rebound, true, true, serial) == PresentationState::PrepareResult::Direct);
    assert(state.promote(serial));
    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::Rebound);
    assert(state.committedToken() == 33);
}

static void test_fast_return_cancels_uncommitted_target() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);

    const auto second = bind(state, 2, 20, 2, 200);
    assert(state.prepare(second, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    const auto stale = serial;

    const auto                 returned = bind(state, 3, 10, 3, 300);
    PresentationState::Content accepted;
    assert(state.accept(stale, accepted) == PresentationState::AcceptResult::Rejected);
    assert(state.prepare(returned, true, true, serial) == PresentationState::PrepareResult::Direct);
}

static void test_rejected_submission_can_retry() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);
    const auto next = bind(state, 2, 20, 2, 200);
    assert(state.prepare(next, true, true, serial) == PresentationState::PrepareResult::Transition);
    assert(state.discard(serial));
    assert(state.committedToken() == 10);
    assert(state.prepare(next, true, true, serial) == PresentationState::PrepareResult::Transition);
}

static void test_rebind_supersedes_prepared_pool_for_same_content() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);
    const auto first = bind(state, 2, 20, 2, 200);
    assert(state.prepare(first, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    const auto stale = serial;

    const auto                 rebound = bind(state, 3, 20, 2, 300);
    PresentationState::Content accepted;
    assert(state.accept(stale, accepted) == PresentationState::AcceptResult::Rejected);
    assert(state.prepare(rebound, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(serial != stale);
}

static void test_committed_transition_can_prepare_next_content() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);
    (void)submit(state, bind(state, 2, 20, 2, 200), true, true, serial);

    const auto third = bind(state, 3, 30, 3, 300);
    assert(state.prepare(third, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(state.committedToken() == 20);
}

static void test_active_transition_keeps_only_latest_incoming() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);

    const auto second = bind(state, 2, 20, 2, 200);
    assert(state.prepare(second, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(state.promote(serial));
    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::ContentChanged);

    state.setTransitionActive(true);
    const auto queued = bind(state, 3, 30, 3, 300);
    assert(state.prepare(queued, true, true, serial) == PresentationState::PrepareResult::Queued);
    const auto latest = bind(state, 4, 40, 4, 400);
    assert(state.prepare(latest, true, true, serial) == PresentationState::PrepareResult::Queued);

    state.setTransitionActive(false);
    assert(state.prepare(latest, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(state.promote(serial));
    assert(state.committedToken() == 20);
}

static void test_promoted_submission_survives_new_incoming() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);

    const auto second = bind(state, 2, 20, 2, 200);
    assert(state.prepare(second, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(state.promote(serial));
    (void)bind(state, 3, 30, 3, 300);

    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::ContentChanged);
    assert(state.committedToken() == 20);
}

static void test_promoted_submission_survives_pool_retirement() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);

    const auto second = bind(state, 2, 20, 2, 200);
    assert(state.prepare(second, true, true, serial) ==
           PresentationState::PrepareResult::Transition);
    assert(state.promote(serial));
    state.retireIncoming(2);

    PresentationState::Content accepted;
    assert(state.accept(serial, accepted) == PresentationState::AcceptResult::ContentChanged);
}

static void test_active_transition_defers_same_content_rebind() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);
    (void)submit(state, bind(state, 2, 20, 2, 200), true, true, serial);

    state.setTransitionActive(true);
    const auto rebound = bind(state, 3, 20, 3, 300);
    assert(state.prepare(rebound, true, true, serial) == PresentationState::PrepareResult::Queued);
    state.setTransitionActive(false);
    assert(state.prepare(rebound, true, true, serial) == PresentationState::PrepareResult::Direct);
}

static void test_missing_outgoing_shadow_falls_back_to_direct() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 1, 10, 1, 100), true, true, serial);

    const auto next = bind(state, 2, 20, 2, 200);
    assert(state.prepare(next, true, false, serial) == PresentationState::PrepareResult::Direct);
    assert(state.committedToken() == 10);
}

static void test_stale_config_and_frame_are_rejected() {
    PresentationState state;
    const auto        stale = bind(state, 1, 10, 1, 100);
    (void)bind(state, 2, 20, 2, 200);
    std::uint64_t serial = 0;
    assert(state.prepare(stale, true, true, serial) == PresentationState::PrepareResult::Rejected);
    PresentationState::Content out;
    assert(! state.incomingFor(1, out));
}

static void test_displayed_config_updates_without_changing_identity() {
    PresentationState state;
    std::uint64_t     serial = 0;
    (void)submit(state, bind(state, 4, 40, 4, 400), true, true, serial);
    auto updated = config(4, 5, 200);
    assert(state.applyConfig(updated) == PresentationState::ConfigResult::DisplayedUpdated);
    assert(! state.bufferChangesWith(4));
    assert(state.displayed().config.configGeneration == 5);
    assert(state.committedToken() == 40);
}

static void test_frame_slot_retirement_waits_for_slot_reuse() {
    for (int framesInFlight : { 1, 2, 3 }) {
        FrameSlotRetirement retirement { 0, 10 };
        for (int offset = 1; offset < framesInFlight; ++offset) {
            assert(! retirement.ready(offset, 10 + std::uint64_t(offset)));
        }
        assert(retirement.ready(0, 10 + std::uint64_t(framesInFlight)));
        assert(! retirement.ready(0, 10));
    }
}

static void test_frame_slot_retirement_queue_releases_once() {
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
    test_first_content_is_direct_and_commits_after_accept();
    test_different_content_transitions_after_committed_content();
    test_same_content_rebind_is_direct();
    test_fast_return_cancels_uncommitted_target();
    test_rejected_submission_can_retry();
    test_rebind_supersedes_prepared_pool_for_same_content();
    test_committed_transition_can_prepare_next_content();
    test_active_transition_keeps_only_latest_incoming();
    test_promoted_submission_survives_new_incoming();
    test_promoted_submission_survives_pool_retirement();
    test_active_transition_defers_same_content_rebind();
    test_missing_outgoing_shadow_falls_back_to_direct();
    test_stale_config_and_frame_are_rejected();
    test_displayed_config_updates_without_changing_identity();
    test_frame_slot_retirement_waits_for_slot_reuse();
    test_frame_slot_retirement_queue_releases_once();
    std::puts("test_presentation_state: OK");
    return 0;
}
