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

    // Recency matches creation order until something is re-focused, which is
    // what three windows opened in a row and never touched again look like.
    f->newest.mru_next = &f->middle;
    f->middle.mru_next = &f->oldest;

    f->satori.windows = &f->newest;
    f->satori.mru = &f->newest;
    f->satori.focused = &f->newest;
    f->satori.focus_dirty = false;
}

// app_id is only ever read here; nothing in these tests frees it.
static void fixture_app_ids(struct fixture *f, char *newest, char *middle, char *oldest) {
    f->newest.app_id = newest;
    f->middle.app_id = middle;
    f->oldest.app_id = oldest;
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

// The binding records the same intent a client's fullscreen_requested does, and
// only for the focused window. Toggling has to survive a round trip: on, then
// off, with the dirty flag set both ways -- windows_apply_fullscreen clears it
// after each, so a toggle that only sets it once applies nothing the second time.
static void test_toggle_fullscreen_marks_only_the_focused_window(void) {
    struct fixture f;
    fixture_init(&f);
    f.satori.focused = &f.middle;

    action_toggle_fullscreen(&f.satori, NOARG);
    CHECK(f.middle.fullscreen);
    CHECK(f.middle.fullscreen_dirty);
    CHECK(!f.newest.fullscreen_dirty);
    CHECK(!f.oldest.fullscreen_dirty);

    // The sequence applied it and cleared the flag; toggling back must set it again.
    f.middle.fullscreen_dirty = false;
    action_toggle_fullscreen(&f.satori, NOARG);
    CHECK(!f.middle.fullscreen);
    CHECK(f.middle.fullscreen_dirty);
}

// A window the client fullscreened is fullscreen to us too, so the binding
// leaves it, rather than toggling from a stale idea of the state.
static void test_toggle_fullscreen_leaves_a_client_fullscreened_window(void) {
    struct fixture f;
    fixture_init(&f);

    struct output out = {0};
    f.newest.fullscreen = true;         // as win_fullscreen_requested would leave it
    f.newest.fs_output = &out;

    action_toggle_fullscreen(&f.satori, NOARG);
    CHECK(!f.newest.fullscreen);
    CHECK(f.newest.fullscreen_dirty);
    CHECK(f.newest.fs_output == NULL);
}

static void test_toggle_fullscreen_with_nothing_focused_is_a_no_op(void) {
    struct satori satori = {0};
    action_toggle_fullscreen(&satori, NOARG);
    CHECK(satori.focused == NULL);
}

// Floating is a round trip like fullscreen, but proposed is the dirty bit: the
// toggle is only applied because windows_propose re-runs, so a toggle that
// leaves proposed set changes nothing on screen.
static void test_toggle_floating_marks_only_the_focused_window(void) {
    struct fixture f;
    fixture_init(&f);
    f.satori.focused = &f.middle;
    f.newest.proposed = f.middle.proposed = f.oldest.proposed = true;

    action_toggle_floating(&f.satori, NOARG);
    CHECK(f.middle.floating);
    CHECK(!f.middle.proposed);
    CHECK(!f.newest.floating);
    CHECK(f.newest.proposed);       // untouched windows keep their proposal
    CHECK(f.oldest.proposed);

    // The sequence proposed it and set the flag again; toggling back must clear it.
    f.middle.proposed = true;
    action_toggle_floating(&f.satori, NOARG);
    CHECK(!f.middle.floating);
    CHECK(!f.middle.proposed);
}

static void test_toggle_floating_with_nothing_focused_is_a_no_op(void) {
    struct satori satori = {0};
    action_toggle_floating(&satori, NOARG);
    CHECK(satori.focused == NULL);
}

// The first float has to land somewhere sensible: inside the usable area, not
// at the origin and not off the bottom of a screen a bar has shortened.
static void test_float_geometry_centers_inside_the_usable_area(void) {
    struct window win = {0};
    struct output out = { .x = 0, .y = 0, .width = 1920, .height = 1080 };

    out.has_area = true;            // a 40px bar at the top
    out.area_x = 0; out.area_y = 40;
    out.area_width = 1920; out.area_height = 1040;

    window_init_float_geometry(&win, &out);

    CHECK(win.float_width == 1280 && win.float_height == 693);
    CHECK(win.float_x == 320);
    CHECK(win.float_y == 40 + (1040 - 693) / 2);

    // Centered means it fits: no edge lands outside the usable area.
    CHECK(win.float_x >= out.area_x);
    CHECK(win.float_y >= out.area_y);
    CHECK(win.float_x + win.float_width  <= out.area_x + out.area_width);
    CHECK(win.float_y + win.float_height <= out.area_y + out.area_height);
}

// A floating window has to be positioned at its own coordinates, not parked at
// the usable-area origin like a maximized one. This is the only guard on that:
// the smoke test sees the proposal, never the position, so a floating window
// correctly sized and stacked in the top left corner passes everything else.
static void test_position_follows_float_geometry(void) {
    struct output out = { .x = 0, .y = 0, .width = 1920, .height = 1080 };
    out.has_area = true;            // a 40px bar at the top
    out.area_x = 0; out.area_y = 40;
    out.area_width = 1920; out.area_height = 1040;

    struct window win = {0};
    int32_t x, y;

    window_position(&win, &out, &x, &y);
    CHECK(x == 0 && y == 40);       // maximized: the usable-area origin

    win.floating = true;
    win.float_x = 320; win.float_y = 213;
    window_position(&win, &out, &x, &y);
    CHECK(x == 320 && y == 213);
}

// Float coordinates belong to the output they were computed on. Keeping them
// across a removal parks the window off screen on whatever output is left.
static void test_forget_output_drops_float_geometry(void) {
    struct fixture f;
    fixture_init(&f);

    struct output going = {0};
    f.satori.outputs = &going;

    f.newest.floating = true;
    f.newest.float_x = 2200; f.newest.float_y = 300;
    f.newest.float_width = 800; f.newest.float_height = 600;

    windows_forget_output(&f.satori, &going);

    // Still floating -- only the geometry is stale, and it is re-derived on the
    // next proposal because the size is what marks it unset.
    CHECK(f.newest.floating);
    CHECK(f.newest.float_width == 0);
    CHECK(f.newest.float_height == 0);
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

// Focusing reorders the recency list and nothing else. If it reordered
// satori->windows too, Mod+J/K would stop being a ring and start bouncing
// between the last two windows, and stacking order would follow focus.
static void test_focus_promotes_in_the_mru_list_only(void) {
    struct fixture f;
    fixture_init(&f);

    window_focus(&f.satori, &f.oldest);
    CHECK(f.satori.mru == &f.oldest);
    CHECK(f.oldest.mru_next == &f.newest);
    CHECK(f.newest.mru_next == &f.middle);
    CHECK(f.middle.mru_next == NULL);

    // Creation order is untouched.
    CHECK(f.satori.windows == &f.newest);
    CHECK(f.newest.next == &f.middle);
    CHECK(f.middle.next == &f.oldest);

    // Re-focusing the head is a no-op, not a re-link.
    window_focus(&f.satori, &f.oldest);
    CHECK(f.satori.mru == &f.oldest);
    CHECK(f.oldest.mru_next == &f.newest);
}

// The killer feature: from another app, the letter lands on the window of that
// app you used last -- not the newest one, not the first one in the list.
static void test_focus_app_jumps_to_the_most_recent_match(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, "vim", "foot", "foot");

    // The older of the two terminals was the more recently used one.
    f.satori.mru = &f.newest;
    f.newest.mru_next = &f.oldest;
    f.oldest.mru_next = &f.middle;
    f.middle.mru_next = NULL;

    action_focus_app(&f.satori, (union satori_arg){ .u = 'f' });

    // Creation order would have answered `middle`; recency is the whole point.
    CHECK(f.satori.focused == &f.oldest);
    CHECK(f.satori.focus_dirty);
}

// Pressing the same letter again walks that app's windows. Recency order would
// bounce between the last two forever, so this ring is creation order.
static void test_focus_app_cycles_within_an_app_in_a_stable_ring(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, "foot", "foot", "foot");

    union satori_arg f_key = { .u = 'f' };

    action_focus_app(&f.satori, f_key);
    CHECK(f.satori.focused == &f.middle);
    action_focus_app(&f.satori, f_key);
    CHECK(f.satori.focused == &f.oldest);
    action_focus_app(&f.satori, f_key);     // wraps
    CHECK(f.satori.focused == &f.newest);
}

// Windows of other apps are not part of the ring, and the wrap has to skip them
// rather than stopping at one.
static void test_focus_app_ring_skips_other_apps(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, "foot", "vim", "foot");
    f.satori.focused = &f.oldest;

    action_focus_app(&f.satori, (union satori_arg){ .u = 'f' });
    CHECK(f.satori.focused == &f.newest);       // wrapped past vim
}

// One window of that app, already focused: the ring comes back to it and
// window_focus returns early. Same shape as Mod+J/K with a single window.
static void test_focus_app_on_the_only_window_of_its_app_is_a_no_op(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, "vim", "foot", "foot");

    action_focus_app(&f.satori, (union satori_arg){ .u = 'v' });
    CHECK(f.satori.focused == &f.newest);
    CHECK(!f.satori.focus_dirty);
}

// An unmatched letter must leave focus exactly where it was. Satori owns every
// binding, so twenty-five of these twenty-six keys usually match nothing.
static void test_focus_app_with_no_match_leaves_focus_alone(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, "foot", "foot", "foot");
    f.satori.focused = &f.middle;

    action_focus_app(&f.satori, (union satori_arg){ .u = 'z' });
    CHECK(f.satori.focused == &f.middle);
    CHECK(!f.satori.focus_dirty);

    CHECK(window_find_by_app(&f.satori, 'z') == NULL);
}

// app_id case is the client's choice, and a window that has not sent one yet
// must match nothing rather than everything.
static void test_focus_app_ignores_case_and_a_missing_app_id(void) {
    struct fixture f;
    fixture_init(&f);
    fixture_app_ids(&f, NULL, "Firefox", "foot");
    f.satori.focused = NULL;

    CHECK(window_find_by_app(&f.satori, 'f') == &f.middle);
    CHECK(window_find_by_app(&f.satori, 'F') == &f.middle);

    // An empty string is not a match either; it would index off the end.
    f.middle.app_id = "";
    CHECK(window_find_by_app(&f.satori, 'f') == &f.oldest);
}

static void test_focus_app_on_an_empty_list_is_a_no_op(void) {
    struct satori satori = {0};

    action_focus_app(&satori, (union satori_arg){ .u = 'f' });
    CHECK(satori.focused == NULL);
    CHECK(!satori.focus_dirty);
}

// Every letter needs a binding: a gap is a key that silently does nothing, and
// the table is generated precisely so there cannot be one.
static void test_app_keybind_table_covers_every_letter(void) {
    app_keybinds_init();

    for (size_t i = 0; i < APP_KEYBIND_COUNT; i++) {
        const struct keybind *k = &app_keybinds[i];
        CHECK(k->action == action_focus_app);
        CHECK(k->modifiers == (MOD|ALT));
        CHECK(k->keysym == (uint32_t) (XKB_KEY_a + i));
        CHECK(k->arg.u == (uint32_t) ('a' + i));
    }

    // The reserved namespace has to stay reserved: a Mod+<letter> binding that
    // strayed into mod4|mod1 would shadow one of these.
    for (size_t i = 0; i < sizeof keybinds / sizeof keybinds[0]; i++) {
        CHECK(keybinds[i].modifiers != (MOD|ALT));
    }
}

// The XF86 block, 0x1008ff00-0x1008ffff: media and laptop function keys. They
// produce no text, which is what makes them the one safe thing to bind with no
// modifier at all.
static bool is_media_key(uint32_t keysym) {
    return (keysym & 0xffffff00) == 0x1008ff00;
}

// A typo'd table is a keybind that silently does nothing, or a spawn that
// dereferences NULL in /bin/sh. Cheap to rule out.
static void test_keybind_table_is_well_formed(void) {
    size_t n = sizeof keybinds / sizeof keybinds[0];
    CHECK(n > 0);

    bool has_exit = false;
    for (size_t i = 0; i < n; i++) {
        CHECK(keybinds[i].action != NULL);
        // An unmodified key swallows normal typing -- unless it is a media key,
        // which types nothing. Binding an unmodified letter would make that
        // letter unusable everywhere in the session.
        CHECK(keybinds[i].modifiers != 0 || is_media_key(keybinds[i].keysym));
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

static const struct keybind *find_keybind(uint32_t keysym, uint32_t modifiers) {
    for (size_t i = 0; i < sizeof keybinds / sizeof keybinds[0]; i++) {
        if (keybinds[i].keysym == keysym && keybinds[i].modifiers == modifiers) {
            return &keybinds[i];
        }
    }
    return NULL;
}

// These bindings carry more unit weight than most, because the smoke test
// deliberately does not run them: their effect is external and not idempotent,
// so executing volume or brightness in the suite would move the developer's real
// sink and real backlight. What the integration test can prove is that an
// unmodified XF86 keysym dispatches at all; the exact keysym and the exact
// command it runs are pinned here.
static void test_media_keys_are_bound_unmodified(void) {
    const struct {
        uint32_t keysym;
        const char *needle;
    } expected[] = {
        { XKB_KEY_XF86AudioMute,          "set-mute @DEFAULT_AUDIO_SINK@ toggle"   },
        { XKB_KEY_XF86AudioLowerVolume,   "set-volume @DEFAULT_AUDIO_SINK@ 5%-"    },
        { XKB_KEY_XF86AudioRaiseVolume,   "set-volume @DEFAULT_AUDIO_SINK@ 5%+"    },
        { XKB_KEY_XF86AudioMicMute,       "set-mute @DEFAULT_AUDIO_SOURCE@ toggle" },
        { XKB_KEY_XF86MonBrightnessDown,  "n=$((c - m/20))"                        },
        { XKB_KEY_XF86MonBrightnessUp,    "n=$((c + m/20))"                        },
    };

    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        // Modifier 0 is the assertion, not an accident: river matches the exact
        // modifier set, so a media key bound with any modifier never fires from
        // the keycap that is printed with it.
        const struct keybind *k = find_keybind(expected[i].keysym, 0);
        CHECK(k != NULL);
        if (!k) continue;

        CHECK(is_media_key(k->keysym));
        CHECK(k->action == action_spawn);
        CHECK(k->arg.cmd != NULL);
        CHECK(k->arg.cmd && strstr(k->arg.cmd, expected[i].needle) != NULL);
    }

    // Raising past unity clips instead of getting louder, so the cap is part of
    // the binding, not a nicety.
    const struct keybind *up = find_keybind(XKB_KEY_XF86AudioRaiseVolume, 0);
    CHECK(up && strstr(up->arg.cmd, "-l 1.0") != NULL);

    // The floor is what keeps a backlight from reaching 0. At 0 the screen is
    // black and the key that would raise it again is invisible -- an unrecoverable
    // session from a single keypress.
    const struct keybind *dim = find_keybind(XKB_KEY_XF86MonBrightnessDown, 0);
    CHECK(dim && strstr(dim->arg.cmd, "lo=$((m/100+1))") != NULL);
    CHECK(dim && strstr(dim->arg.cmd, "[ $n -lt $lo ] && n=$lo") != NULL);
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
    test_toggle_fullscreen_marks_only_the_focused_window();
    test_toggle_fullscreen_leaves_a_client_fullscreened_window();
    test_toggle_fullscreen_with_nothing_focused_is_a_no_op();
    test_toggle_floating_marks_only_the_focused_window();
    test_toggle_floating_with_nothing_focused_is_a_no_op();
    test_float_geometry_centers_inside_the_usable_area();
    test_position_follows_float_geometry();
    test_forget_output_drops_float_geometry();
    test_forget_output_drops_references_and_dirties();
    test_usable_area_falls_back_to_the_output();
    test_invalidate_layout_reproposes_everything();
    test_focus_promotes_in_the_mru_list_only();
    test_focus_app_jumps_to_the_most_recent_match();
    test_focus_app_cycles_within_an_app_in_a_stable_ring();
    test_focus_app_ring_skips_other_apps();
    test_focus_app_on_the_only_window_of_its_app_is_a_no_op();
    test_focus_app_with_no_match_leaves_focus_alone();
    test_focus_app_ignores_case_and_a_missing_app_id();
    test_focus_app_on_an_empty_list_is_a_no_op();
    test_app_keybind_table_covers_every_letter();
    test_keybind_table_is_well_formed();
    test_media_keys_are_bound_unmodified();
    test_focus_defers_while_a_layer_surface_holds_it();  // may crash if broken; keep last

    if (failures) {
        printf("FAIL  %d check(s)\n", failures);
        return 1;
    }
    printf("  ok    focus cycling, focus dirty tracking, close intent\n"
           "  ok    fullscreen toggle intent\n"
           "  ok    float toggle intent, float geometry\n"
           "  ok    output removal, usable area, layer focus deferral\n"
           "  ok    mru order, app_id lookup, within-app ring\n"
           "  ok    keybind tables, media keys\n\nPASS\n");
    return 0;
}
