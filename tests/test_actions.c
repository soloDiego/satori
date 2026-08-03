// Unit tests for the compositor-free half of the action layer: focus order,
// cycling, close intent. These are the parts with real branches -- wrapping,
// empty lists, focus falling through on close.
//
// input.c is included rather than linked so the static actions and the keybind
// table are reachable. Nothing here talks to a compositor: the actions only
// read and write struct fields. Window handles stay NULL and are never touched.

#include <stdio.h>
#include <string.h>

#include "../src/input.c"

static int failures;

// Actions that ignore their argument still have to be handed one.
#define NOARG ((union satori_arg){0})

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check(bool ok, const char *expr, int line) {
    if (ok) return;
    fprintf(stderr, "  FAIL  %s (line %d)\n", expr, line);
    failures++;
}

// Three windows, newest first, the way window_create prepends them.
struct fixture {
    struct satori satori;
    struct window newest, middle, oldest;
};

static void fixture_init(struct fixture *f) {
    memset(f, 0, sizeof *f);

    f->newest.satori = &f->satori;
    f->middle.satori = &f->satori;
    f->oldest.satori = &f->satori;

    f->newest.next = &f->middle;
    f->middle.next = &f->oldest;

    f->satori.windows = &f->newest;
    f->satori.focused = &f->newest;
    f->satori.focus_dirty = false;
}

static void test_focus_next_walks_then_wraps(void) {
    struct fixture f;
    fixture_init(&f);

    action_focus_next(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.middle);
    action_focus_next(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.oldest);
    action_focus_next(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.newest);
}

static void test_focus_prev_walks_then_wraps(void) {
    struct fixture f;
    fixture_init(&f);

    // From the head, "previous" is the tail.
    action_focus_prev(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.oldest);
    action_focus_prev(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.middle);
    action_focus_prev(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.newest);
}

static void test_cycling_with_nothing_focused(void) {
    struct fixture f;

    fixture_init(&f);
    f.satori.focused = NULL;
    action_focus_next(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.newest);

    fixture_init(&f);
    f.satori.focused = NULL;
    action_focus_prev(&f.satori, NOARG);
    CHECK(f.satori.focused == &f.oldest);
}

static void test_cycling_an_empty_list_is_a_no_op(void) {
    struct satori satori = {0};

    action_focus_next(&satori, NOARG);
    CHECK(satori.focused == NULL);
    CHECK(!satori.focus_dirty);

    action_focus_prev(&satori, NOARG);
    CHECK(satori.focused == NULL);
    CHECK(!satori.focus_dirty);
}

static void test_cycling_a_single_window_stays_put(void) {
    struct satori satori = {0};
    struct window only = { .satori = &satori };
    satori.windows = &only;
    satori.focused = &only;

    action_focus_next(&satori, NOARG);
    CHECK(satori.focused == &only);
    action_focus_prev(&satori, NOARG);
    CHECK(satori.focused == &only);

    // Focus never actually moved, so the manage sequence has nothing to apply.
    CHECK(!satori.focus_dirty);
}

static void test_focus_change_marks_dirty(void) {
    struct fixture f;
    fixture_init(&f);

    action_focus_next(&f.satori, NOARG);
    CHECK(f.satori.focus_dirty);

    // seats_apply_focus clears the flag; re-focusing the same window must not
    // set it again, or every sequence re-sends focus_window.
    f.satori.focus_dirty = false;
    window_focus(&f.satori, f.satori.focused);
    CHECK(!f.satori.focus_dirty);
}

static void test_close_marks_only_the_focused_window(void) {
    struct fixture f;
    fixture_init(&f);
    f.satori.focused = &f.middle;

    action_close_focused(&f.satori, NOARG);
    CHECK(f.middle.close_pending);
    CHECK(!f.newest.close_pending);
    CHECK(!f.oldest.close_pending);

    // Closing is a request, not a removal: the window stays until the server
    // sends closed.
    CHECK(f.satori.windows == &f.newest);
    CHECK(f.satori.focused == &f.middle);
}

static void test_close_with_nothing_focused_is_a_no_op(void) {
    struct satori satori = {0};
    action_close_focused(&satori, NOARG);
    CHECK(satori.focused == NULL);
}

// Losing an output must leave no window pointing at the freed struct, and must
// mark every window for re-proposal -- they are sized against an output that is
// about to stop existing.
static void test_forget_output_drops_references_and_dirties(void) {
    struct fixture f;
    fixture_init(&f);

    struct output going = {0}, staying = {0};
    f.satori.outputs = &going;
    going.next = &staying;

    f.newest.fullscreen = true;
    f.newest.fs_output = &going;
    f.middle.fs_output = &staying;
    f.newest.proposed = f.middle.proposed = f.oldest.proposed = true;

    windows_forget_output(&f.satori, &going);

    // The fullscreen window loses its output; the compositor has already
    // dropped it out of fullscreen, so we only correct our own record.
    CHECK(f.newest.fs_output == NULL);
    CHECK(!f.newest.fullscreen);
    CHECK(f.newest.fullscreen_dirty);

    // A window pinned to a different output keeps it.
    CHECK(f.middle.fs_output == &staying);
    CHECK(!f.middle.fullscreen_dirty);

    // Everything gets re-proposed regardless: the layout changed.
    CHECK(!f.newest.proposed);
    CHECK(!f.middle.proposed);
    CHECK(!f.oldest.proposed);
}

// Until the layer shell reports an area, a maximized window must still get the
// whole output -- otherwise the first window on a fresh session is sized 0x0.
static void test_usable_area_falls_back_to_the_output(void) {
    struct output out = { .x = 10, .y = 20, .width = 1920, .height = 1080 };
    int32_t x, y, w, h;

    output_usable_area(&out, &x, &y, &w, &h);
    CHECK(x == 10 && y == 20 && w == 1920 && h == 1080);

    // A panel reserving 30px at the top: windows go below it, not under it.
    out.has_area = true;
    out.area_x = 10; out.area_y = 50;
    out.area_width = 1920; out.area_height = 1050;

    output_usable_area(&out, &x, &y, &w, &h);
    CHECK(x == 10 && y == 50 && w == 1920 && h == 1050);
}

// The area windows are sized against changed, so every window is stale.
static void test_invalidate_layout_reproposes_everything(void) {
    struct fixture f;
    fixture_init(&f);

    f.newest.proposed = f.middle.proposed = f.oldest.proposed = true;
    windows_invalidate_layout(&f.satori);

    CHECK(!f.newest.proposed);
    CHECK(!f.middle.proposed);
    CHECK(!f.oldest.proposed);
}

// While a layer surface holds the seat, focusing is either ignored (exclusive)
// or steals the focus the launcher is about to get (non-exclusive). Either way
// the request must not go out, and the intent must survive to the sequence after
// focus_none -- dropping focus_dirty here loses focus for good.
//
// This is the ONLY guard on the deferral: the smoke test cannot see it. fuzzel
// takes focus exclusively, and river ignores our requests outright in that
// state, so removing the guard still passes there. The case it really protects
// is non-exclusive (on-demand) focus -- a bar with clickable modules -- which
// would need a purpose-built layer client to exercise for real.
//
// Runs last, and deliberately: with the guard gone this walks into a request on
// a NULL proxy and dies. A core dump is a red result, not a silent pass.
static void test_focus_defers_while_a_layer_surface_holds_it(void) {
    struct fixture f;
    fixture_init(&f);

    // No handle is ever dereferenced: the deferral returns before any request.
    struct seat seat = { .satori = &f.satori, .layer_focus = true };
    f.satori.seats = &seat;
    f.satori.focus_dirty = true;

    seats_apply_focus(&f.satori);
    CHECK(f.satori.focus_dirty);        // still owed, not silently dropped

    // Nothing to apply is not the same as deferring: an unset dirty flag stays unset.
    f.satori.focus_dirty = false;
    seats_apply_focus(&f.satori);
    CHECK(!f.satori.focus_dirty);
}

// A typo'd table is a keybind that silently does nothing, or a spawn that
// dereferences NULL in /bin/sh. Cheap to rule out.
static void test_keybind_table_is_well_formed(void) {
    size_t n = sizeof keybinds / sizeof keybinds[0];
    CHECK(n > 0);

    bool has_exit = false;
    for (size_t i = 0; i < n; i++) {
        CHECK(keybinds[i].action != NULL);
        CHECK(keybinds[i].modifiers != 0);      // an unmodified key would swallow normal typing
        if (keybinds[i].action == action_spawn) CHECK(keybinds[i].arg.cmd != NULL);
        if (keybinds[i].action == action_exit_session) has_exit = true;

        // Duplicate keysym+modifier pairs: which one fires is compositor policy.
        for (size_t j = i + 1; j < n; j++) {
            CHECK(!(keybinds[i].keysym == keybinds[j].keysym
                    && keybinds[i].modifiers == keybinds[j].modifiers));
        }
    }
    // Satori owns every binding in the session; without this one there is no
    // way to log out.
    CHECK(has_exit);
}

int main(void) {
    printf("== satori unit tests\n");

    test_focus_next_walks_then_wraps();
    test_focus_prev_walks_then_wraps();
    test_cycling_with_nothing_focused();
    test_cycling_an_empty_list_is_a_no_op();
    test_cycling_a_single_window_stays_put();
    test_focus_change_marks_dirty();
    test_close_marks_only_the_focused_window();
    test_close_with_nothing_focused_is_a_no_op();
    test_forget_output_drops_references_and_dirties();
    test_usable_area_falls_back_to_the_output();
    test_invalidate_layout_reproposes_everything();
    test_keybind_table_is_well_formed();
    test_focus_defers_while_a_layer_surface_holds_it();  // may crash if broken; keep last

    if (failures) {
        printf("FAIL  %d check(s)\n", failures);
        return 1;
    }
    printf("  ok    focus cycling, focus dirty tracking, close intent\n"
           "  ok    output removal, usable area, layer focus deferral\n"
           "  ok    keybind table\n\nPASS\n");
    return 0;
}
