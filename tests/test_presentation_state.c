#include <waywallen_display_presentation.h>

#include <assert.h>
#include <stdio.h>

static waywallen_composition_config_t config(uint64_t buffer_generation, uint64_t config_generation,
                                             uint32_t size) {
    return (waywallen_composition_config_t) {
        .generation        = config_generation,
        .buffer_generation = buffer_generation,
        .source_rect       = { 0.0f, 0.0f, (float)size, (float)size },
        .dest_rect         = { 0.0f, 0.0f, (float)size, (float)size },
        .clear_color       = { 0.1f, 0.2f, 0.3f, 1.0f },
    };
}

static waywallen_presentation_content_t bind(waywallen_presentation_controller_t* controller,
                                             uint64_t buffer_generation, uint64_t content_token,
                                             uint64_t presentation_generation, uint32_t size) {
    waywallen_presentation_controller_begin_incoming(controller,
                                                     buffer_generation,
                                                     content_token,
                                                     presentation_generation,
                                                     size,
                                                     size,
                                                     10,
                                                     true);
    const waywallen_composition_config_t value = config(buffer_generation, buffer_generation, size);
    assert(waywallen_presentation_controller_apply_config(controller, &value) ==
           WAYWALLEN_PRESENTATION_CONFIG_STAGED);
    waywallen_presentation_content_t content = { 0 };
    assert(waywallen_presentation_controller_incoming_for(controller, buffer_generation, &content));
    return content;
}

static waywallen_presentation_accept_result_t
submit(waywallen_presentation_controller_t*    controller,
       const waywallen_presentation_content_t* content, bool transition, bool retained,
       uint64_t* serial) {
    const waywallen_presentation_prepare_result_t prepared =
        waywallen_presentation_controller_prepare(
            controller, content, transition, retained, serial);
    assert(prepared == WAYWALLEN_PRESENTATION_PREPARE_DIRECT ||
           prepared == WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    assert(waywallen_presentation_controller_promote(controller, *serial));
    waywallen_presentation_content_t accepted = { 0 };
    return waywallen_presentation_controller_accept(controller, *serial, &accepted);
}

static void first_content_is_direct_and_commits_after_accept(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    const waywallen_presentation_content_t content = bind(controller, 1, 10, 1, 100);
    uint64_t                               serial  = 0;
    assert(waywallen_presentation_controller_prepare(controller, &content, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_DIRECT);
    assert(waywallen_presentation_controller_committed_token(controller) == 0);
    assert(waywallen_presentation_controller_promote(controller, serial));
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, serial, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_CONTENT_CHANGED);
    assert(waywallen_presentation_controller_committed_token(controller) == 10);
    waywallen_presentation_controller_free(controller);
}

static void different_content_transitions_after_commit(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    assert(submit(controller, &first, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_ACCEPT_CONTENT_CHANGED);
    waywallen_presentation_content_t next = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &next, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    assert(waywallen_presentation_controller_promote(controller, serial));
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, serial, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_CONTENT_CHANGED);
    assert(waywallen_presentation_controller_committed_token(controller) == 20);
    waywallen_presentation_controller_free(controller);
}

static void same_content_rebind_is_direct(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 33, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t rebound = bind(controller, 2, 33, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &rebound, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_DIRECT);
    assert(waywallen_presentation_controller_promote(controller, serial));
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, serial, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_REBOUND);
    waywallen_presentation_controller_free(controller);
}

static void fast_return_cancels_uncommitted_target(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t second = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &second, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    const uint64_t                   stale    = serial;
    waywallen_presentation_content_t returned = bind(controller, 3, 10, 3, 300);
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, stale, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_REJECTED);
    assert(waywallen_presentation_controller_prepare(controller, &returned, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_DIRECT);
    waywallen_presentation_controller_free(controller);
}

static void rejected_submission_can_retry(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t next = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &next, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    assert(waywallen_presentation_controller_discard(controller, serial));
    assert(waywallen_presentation_controller_committed_token(controller) == 10);
    assert(waywallen_presentation_controller_prepare(controller, &next, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    waywallen_presentation_controller_free(controller);
}

static void rebind_supersedes_prepared_pool(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t second = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &second, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    const uint64_t                   stale    = serial;
    waywallen_presentation_content_t rebound  = bind(controller, 3, 20, 2, 300);
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, stale, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_REJECTED);
    assert(waywallen_presentation_controller_prepare(controller, &rebound, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    assert(serial != stale);
    waywallen_presentation_controller_free(controller);
}

static void active_transition_keeps_only_latest_incoming(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t second = bind(controller, 2, 20, 2, 200);
    (void)submit(controller, &second, true, true, &serial);

    waywallen_presentation_controller_set_transition_active(controller, true);
    waywallen_presentation_content_t queued = bind(controller, 3, 30, 3, 300);
    assert(waywallen_presentation_controller_prepare(controller, &queued, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_QUEUED);
    waywallen_presentation_content_t latest = bind(controller, 4, 40, 4, 400);
    assert(waywallen_presentation_controller_prepare(controller, &latest, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_QUEUED);

    waywallen_presentation_controller_set_transition_active(controller, false);
    assert(waywallen_presentation_controller_prepare(controller, &latest, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    waywallen_presentation_controller_free(controller);
}

static void promoted_submission_survives_new_incoming_and_retirement(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t second = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &second, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_TRANSITION);
    assert(waywallen_presentation_controller_promote(controller, serial));
    (void)bind(controller, 3, 30, 3, 300);
    waywallen_presentation_controller_retire_incoming(controller, 2);
    waywallen_presentation_content_t accepted = { 0 };
    assert(waywallen_presentation_controller_accept(controller, serial, &accepted) ==
           WAYWALLEN_PRESENTATION_ACCEPT_CONTENT_CHANGED);
    assert(accepted.content_token == 20);
    waywallen_presentation_controller_free(controller);
}

static void missing_outgoing_falls_back_to_direct(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t next = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &next, true, false, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_DIRECT);
    waywallen_presentation_controller_free(controller);
}

static void disabled_transition_is_direct(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_presentation_content_t next = bind(controller, 2, 20, 2, 200);
    assert(waywallen_presentation_controller_prepare(controller, &next, false, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_DIRECT);
    waywallen_presentation_controller_free(controller);
}

static void reset_clears_all_state(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial  = 0;
    waywallen_presentation_content_t content = bind(controller, 1, 10, 1, 100);
    (void)submit(controller, &content, true, true, &serial);
    waywallen_presentation_controller_set_transition_active(controller, true);
    waywallen_presentation_controller_reset(controller);

    assert(waywallen_presentation_controller_committed_token(controller) == 0);
    assert(! waywallen_presentation_controller_transition_active(controller));
    assert(! waywallen_presentation_controller_displayed(controller, &content));
    assert(! waywallen_presentation_controller_incoming_for(controller, 1, &content));
    waywallen_presentation_controller_free(controller);
}

static void stale_content_is_rejected(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    waywallen_presentation_content_t stale = bind(controller, 1, 10, 1, 100);
    (void)bind(controller, 2, 20, 2, 200);
    uint64_t serial = 0;
    assert(waywallen_presentation_controller_prepare(controller, &stale, true, true, &serial) ==
           WAYWALLEN_PRESENTATION_PREPARE_REJECTED);
    assert(! waywallen_presentation_controller_incoming_for(controller, 1, &stale));
    waywallen_presentation_controller_free(controller);
}

static void displayed_config_updates_without_identity_change(void) {
    waywallen_presentation_controller_t* controller = waywallen_presentation_controller_new();
    assert(controller);
    uint64_t                         serial = 0;
    waywallen_presentation_content_t first  = bind(controller, 4, 40, 4, 400);
    (void)submit(controller, &first, true, true, &serial);
    waywallen_composition_config_t updated = config(4, 5, 200);
    assert(waywallen_presentation_controller_apply_config(controller, &updated) ==
           WAYWALLEN_PRESENTATION_CONFIG_DISPLAYED_UPDATED);
    waywallen_presentation_content_t displayed = { 0 };
    assert(waywallen_presentation_controller_displayed(controller, &displayed));
    assert(displayed.config.generation == 5);
    assert(! waywallen_presentation_controller_buffer_changes_with(controller, 4));
    waywallen_presentation_controller_free(controller);
}

int main(void) {
    first_content_is_direct_and_commits_after_accept();
    different_content_transitions_after_commit();
    same_content_rebind_is_direct();
    fast_return_cancels_uncommitted_target();
    rejected_submission_can_retry();
    rebind_supersedes_prepared_pool();
    active_transition_keeps_only_latest_incoming();
    promoted_submission_survives_new_incoming_and_retirement();
    missing_outgoing_falls_back_to_direct();
    disabled_transition_is_direct();
    reset_clears_all_state();
    stale_content_is_rejected();
    displayed_config_updates_without_identity_change();
    puts("test_presentation_state: OK");
    return 0;
}
