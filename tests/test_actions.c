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

    if (failures) {
        printf("FAIL  %d check(s)\n", failures);
        return 1;
    }
    printf("  ok    focus cycling, focus dirty tracking, close intent\n\nPASS\n");
    return 0;
}
