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

// Spelled out rather than taken from SATORI_APP_KEYS_MODIFIERS: the point is to
// pin the value independently of the macro that produces it.
#define ALT  RIVER_SEAT_V1_MODIFIERS_MOD1
#define CTRL RIVER_SEAT_V1_MODIFIERS_CTRL

// The table a session with no config file runs. Every table assertion below goes
// through the real builder rather than a static array, so the built-ins are
// checked as they are actually assembled.
static struct config *defaults_config(void) {
    struct config *config = config_load(NULL, true);
    CHECK(config != NULL);
    return config;
}

static const struct keybind *find_keybind(const struct config *config,
        uint32_t keysym, uint32_t modifiers) {
    for (size_t i = 0; i < config->len; i++) {
        if (config->binds[i].keysym == keysym && config->binds[i].modifiers == modifiers) {
            return &config->binds[i];
        }
    }
    return NULL;
}

// Every letter needs a binding: a gap is a key that silently does nothing, and
// the block is generated precisely so there cannot be one.
static void test_app_keybinds_cover_every_letter(void) {
    struct config *config = defaults_config();
    if (!config) return;

    for (uint32_t i = 0; i < 26; i++) {
        const struct keybind *k = find_keybind(config, XKB_KEY_a + i, MOD|ALT);
        CHECK(k != NULL);
        if (!k) continue;
        CHECK(k->action == action_focus_app);
        CHECK(k->arg.u == (uint32_t) ('a' + i));
        CHECK(k->arg_kind == SATORI_ARG_LETTER);
    }

    // The reserved namespace has to stay reserved: any other binding that
    // strayed into mod4|mod1 would shadow one of these.
    for (size_t i = 0; i < config->len; i++) {
        if (config->binds[i].modifiers != (MOD|ALT)) continue;
        CHECK(config->binds[i].action == action_focus_app);
    }

    config_destroy(config);
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
    struct config *config = defaults_config();
    if (!config) return;

    CHECK(config->len > 0);

    bool has_exit = false, has_escape = false;
    for (size_t i = 0; i < config->len; i++) {
        const struct keybind *k = &config->binds[i];
        CHECK(k->action != NULL);
        // An unmodified key swallows normal typing -- unless it is a media key,
        // which types nothing. Binding an unmodified letter would make that
        // letter unusable everywhere in the session.
        CHECK(k->modifiers != 0 || is_media_key(k->keysym));
        if (k->arg_kind == SATORI_ARG_CMD) CHECK(k->arg.cmd != NULL);
        if (k->action == action_exit_session) has_exit = true;
        if (k->action == action_toggle_passthrough) has_escape = true;

        // Duplicate keysym+modifier pairs: which one fires is compositor policy.
        // config_set is what rules them out, by replacing in place.
        for (size_t j = i + 1; j < config->len; j++) {
            CHECK(!(k->keysym == config->binds[j].keysym
                    && k->modifiers == config->binds[j].modifiers));
        }
    }
    // Satori owns every binding in the session; without this one there is no
    // way to log out.
    CHECK(has_exit);
    // And without this one, a passthrough app takes the keyboard for good.
    CHECK(has_escape);

    config_destroy(config);
}

// These bindings carry more unit weight than most, because the smoke test
// deliberately does not run them: their effect is external and not idempotent,
// so executing volume or brightness in the suite would move the developer's real
// sink and real backlight. What the integration test can prove is that an
// unmodified XF86 keysym dispatches at all; the exact keysym and the exact
// command it runs are pinned here.
static void test_media_keys_are_bound_unmodified(void) {
    struct config *config = defaults_config();
    if (!config) return;

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
        const struct keybind *k = find_keybind(config, expected[i].keysym, 0);
        CHECK(k != NULL);
        if (!k) continue;

        CHECK(is_media_key(k->keysym));
        CHECK(k->action == action_spawn);
        CHECK(k->arg.cmd != NULL);
        CHECK(k->arg.cmd && strstr(k->arg.cmd, expected[i].needle) != NULL);
    }

    // Raising past unity clips instead of getting louder, so the cap is part of
    // the binding, not a nicety.
    const struct keybind *up = find_keybind(config, XKB_KEY_XF86AudioRaiseVolume, 0);
    CHECK(up && strstr(up->arg.cmd, "-l 1.0") != NULL);

    // The floor is what keeps a backlight from reaching 0. At 0 the screen is
    // black and the key that would raise it again is invisible -- an unrecoverable
    // session from a single keypress.
    const struct keybind *dim = find_keybind(config, XKB_KEY_XF86MonBrightnessDown, 0);
    CHECK(dim && strstr(dim->arg.cmd, "lo=$((m/100+1))") != NULL);
    CHECK(dim && strstr(dim->arg.cmd, "[ $n -lt $lo ] && n=$lo") != NULL);

    config_destroy(config);
}

// ---- the config file ------------------------------------------------------

static void test_chord_parse_reads_modifiers_and_key(void) {
    uint32_t keysym = 0, modifiers = 0;

    CHECK(chord_parse("Mod+Return", &keysym, &modifiers));
    CHECK(keysym == XKB_KEY_Return);
    CHECK(modifiers == MOD);

    CHECK(chord_parse("Mod+Alt+f", &keysym, &modifiers));
    CHECK(keysym == XKB_KEY_f);
    CHECK(modifiers == (MOD|ALT));

    // No modifier at all is legal -- it is how the media keys are written.
    CHECK(chord_parse("XF86AudioMute", &keysym, &modifiers));
    CHECK(keysym == XKB_KEY_XF86AudioMute);
    CHECK(modifiers == 0);
}

// Aliases exist so a config can be written in whatever vocabulary its author
// already has. They must land on the same bit.
static void test_chord_parse_accepts_modifier_aliases(void) {
    uint32_t a = 0, b = 0, mods_a = 0, mods_b = 0;

    CHECK(chord_parse("Super+q", &a, &mods_a));
    CHECK(chord_parse("mod4+q", &b, &mods_b));
    CHECK(a == b);
    CHECK(mods_a == mods_b && mods_a == MOD);

    CHECK(chord_parse("CTRL+SHIFT+q", &a, &mods_a));
    CHECK(chord_parse("control+shift+q", &b, &mods_b));
    CHECK(mods_a == mods_b && mods_a == (CTRL|SHFT));
}

// The trap that has already shipped a session with no way out: river matches the
// UNSHIFTED keysym, so a chord written with a capital must still store the
// lowercase one. XKB_KEY_E binds without error and then never fires.
static void test_chord_parse_lowers_the_keysym(void) {
    uint32_t keysym = 0, modifiers = 0;

    CHECK(chord_parse("Mod+Shift+E", &keysym, &modifiers));
    CHECK(keysym == XKB_KEY_e);
    CHECK(keysym != XKB_KEY_E);
    CHECK(modifiers == (MOD|SHFT));
}

static void test_chord_parse_rejects_junk(void) {
    uint32_t keysym = 0, modifiers = 0;

    CHECK(!chord_parse("", &keysym, &modifiers));
    CHECK(!chord_parse("Nope+q", &keysym, &modifiers));         // unknown modifier
    CHECK(!chord_parse("Mod+notakey", &keysym, &modifiers));    // unknown keysym
    CHECK(!chord_parse("Mod+Shift", &keysym, &modifiers));      // modifiers only
    CHECK(!chord_parse("Mod+", &keysym, &modifiers));
}

static struct config *load_config_text(const char *text) {
    char path[] = "/tmp/satori-config-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return NULL;

    size_t len = strlen(text);
    bool written = write(fd, text, len) == (ssize_t) len;
    close(fd);

    struct config *config = written ? config_load(path, true) : NULL;
    unlink(path);
    return config;
}

// The merge rule: a config file overrides individual chords and leaves the rest
// of the built-ins alone. A one-line file must not cost you the exit binding.
static void test_config_merges_over_the_defaults(void) {
    struct config *config = load_config_text("bind Mod+Return spawn alacritty\n");
    CHECK(config != NULL);
    if (!config) return;

    const struct keybind *term = find_keybind(config, XKB_KEY_Return, MOD);
    CHECK(term && term->arg.cmd && strcmp(term->arg.cmd, "alacritty") == 0);

    // Everything the file did not mention is still bound.
    CHECK(find_keybind(config, XKB_KEY_e, MOD|SHFT) != NULL);
    CHECK(find_keybind(config, XKB_KEY_q, MOD) != NULL);
    CHECK(find_keybind(config, XKB_KEY_XF86AudioMute, 0) != NULL);
    CHECK(find_keybind(config, XKB_KEY_a, MOD|ALT) != NULL);

    config_destroy(config);
}

// An override replaces in place rather than appending: two live bindings on one
// chord makes which one fires compositor policy.
static void test_config_override_does_not_grow_the_table(void) {
    struct config *plain = defaults_config();
    struct config *over = load_config_text("bind Mod+Return spawn alacritty\n");
    CHECK(plain && over);

    if (plain && over) CHECK(over->len == plain->len);

    config_destroy(plain);
    config_destroy(over);
}

// The counterpart to merging: without `none` a built-in can only be pointed
// somewhere else, never taken away.
static void test_config_none_unbinds(void) {
    struct config *plain = defaults_config();
    struct config *config = load_config_text("bind Mod+Space none\n");
    CHECK(plain && config);
    if (!plain || !config) return;

    CHECK(find_keybind(config, XKB_KEY_space, MOD) == NULL);
    CHECK(config->len == plain->len - 1);
    // Same keysym, different modifiers: the float toggle must survive.
    CHECK(find_keybind(config, XKB_KEY_space, MOD|SHFT) != NULL);

    config_destroy(plain);
    config_destroy(config);
}

static void test_config_app_keys_can_move_or_go(void) {
    struct config *moved = load_config_text("app-keys Mod+Ctrl\n");
    CHECK(moved != NULL);
    if (moved) {
        CHECK(find_keybind(moved, XKB_KEY_a, MOD|CTRL) != NULL);
        CHECK(find_keybind(moved, XKB_KEY_z, MOD|CTRL) != NULL);
        CHECK(find_keybind(moved, XKB_KEY_a, MOD|ALT) == NULL);
        config_destroy(moved);
    }

    struct config *plain = defaults_config();
    struct config *off = load_config_text("app-keys none\n");
    CHECK(plain && off);
    if (plain && off) {
        CHECK(find_keybind(off, XKB_KEY_a, MOD|ALT) == NULL);
        CHECK(off->len == plain->len - 26);     // the whole generated block, not one letter
    }
    config_destroy(plain);
    config_destroy(off);
}

// A file that exists and is wrong returns NULL so the caller keeps the table it
// already had. Falling back to the defaults silently would be indistinguishable
// from a config that had been applied.
static void test_config_rejects_a_broken_file(void) {
    CHECK(load_config_text("bind Mod+Return nosuchaction\n") == NULL);
    CHECK(load_config_text("bind Nope+Return spawn foot\n") == NULL);
    CHECK(load_config_text("bind Mod+Return\n") == NULL);
    CHECK(load_config_text("bind Mod+Return spawn\n") == NULL);     // no command
    CHECK(load_config_text("bind Mod+Q close extra\n") == NULL);    // close takes none
    CHECK(load_config_text("bind Mod+Q none extra\n") == NULL);
    CHECK(load_config_text("nonsense foo\n") == NULL);              // unknown directive
    CHECK(load_config_text("app-keys\n") == NULL);
    CHECK(load_config_text("app-keys Nope\n") == NULL);
    CHECK(load_config_text("passthrough\n") == NULL);           // needs at least one app_id
    CHECK(load_config_text("bind Mod+Alt+F focus-app toolong\n") == NULL);

    // One bad line poisons the whole file, including the good lines above it.
    CHECK(load_config_text("bind Mod+Return spawn foot\nbind Mod+X bogus\n") == NULL);
}

// A missing file is the normal case, not an error: it means the built-ins.
static void test_config_missing_file_is_the_defaults(void) {
    struct config *plain = defaults_config();
    struct config *missing = config_load("/nonexistent/satori/config", true);
    CHECK(plain && missing);
    if (plain && missing) CHECK(missing->len == plain->len);

    config_destroy(plain);
    config_destroy(missing);
}

// Unquoted words are joined, so the media-key style commands can be written
// without quoting. Quotes still work for runs of whitespace.
static void test_config_spawn_joins_its_words(void) {
    struct config *config = load_config_text(
            "bind Mod+V spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+\n"
            "bind Mod+B spawn \"sh -c 'echo  spaced'\"\n");
    CHECK(config != NULL);
    if (!config) return;

    const struct keybind *joined = find_keybind(config, XKB_KEY_v, MOD);
    CHECK(joined && joined->arg.cmd
            && strcmp(joined->arg.cmd, "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+") == 0);

    const struct keybind *quoted = find_keybind(config, XKB_KEY_b, MOD);
    CHECK(quoted && quoted->arg.cmd && strstr(quoted->arg.cmd, "echo  spaced") != NULL);

    config_destroy(config);
}

// focus-app is reachable by hand as well as generated, so a single letter can be
// re-pointed without writing out all twenty-six.
static void test_config_binds_focus_app_by_hand(void) {
    struct config *config = load_config_text("bind Mod+Ctrl+G focus-app f\n");
    CHECK(config != NULL);
    if (!config) return;

    const struct keybind *k = find_keybind(config, XKB_KEY_g, MOD|CTRL);
    CHECK(k && k->action == action_focus_app);
    CHECK(k && k->arg.u == (uint32_t) 'f');
    CHECK(k && k->arg_kind == SATORI_ARG_LETTER);

    config_destroy(config);
}

// The letter block is added before the file's bind lines, so an explicit bind on
// a generated chord wins. That ordering is the only reason it is reachable.
static void test_config_bind_overrides_a_generated_letter(void) {
    struct config *config = load_config_text("bind Mod+Alt+G spawn gimp\n");
    CHECK(config != NULL);
    if (!config) return;

    const struct keybind *k = find_keybind(config, XKB_KEY_g, MOD|ALT);
    CHECK(k && k->action == action_spawn);
    CHECK(k && k->arg.cmd && strcmp(k->arg.cmd, "gimp") == 0);
    CHECK(find_keybind(config, XKB_KEY_f, MOD|ALT) != NULL);    // the rest untouched

    config_destroy(config);
}

// Replacing a binding has to release the command it replaced. Only ASan sees the
// leak, so this walks one chord through every arg kind and back out.
static void test_config_set_replaces_owned_commands(void) {
    struct config *config = defaults_config();
    if (!config) return;

    size_t len = config->len;
    union satori_arg arg = { .cmd = "first" };

    CHECK(config_set(config, XKB_KEY_F1, MOD, action_from_name("spawn"), arg));
    CHECK(config->len == len + 1);

    arg.cmd = "second";
    CHECK(config_set(config, XKB_KEY_F1, MOD, action_from_name("spawn"), arg));
    CHECK(config->len == len + 1);      // replaced, not appended

    const struct keybind *k = find_keybind(config, XKB_KEY_F1, MOD);
    CHECK(k && k->arg.cmd && strcmp(k->arg.cmd, "second") == 0);

    // Switching to an action with no argument drops the string. Freeing it here
    // is the point: arg.u would otherwise alias a live pointer.
    CHECK(config_set(config, XKB_KEY_F1, MOD, action_from_name("close"), (union satori_arg){0}));
    k = find_keybind(config, XKB_KEY_F1, MOD);
    CHECK(k && k->arg_kind == SATORI_ARG_NONE);

    config_unset(config, XKB_KEY_F1, MOD);
    CHECK(find_keybind(config, XKB_KEY_F1, MOD) == NULL);
    CHECK(config->len == len);

    config_destroy(config);
}

// example/config claims to restate the built-ins exactly, and a copy of it is
// what a user starts editing. Nothing stops the two drifting except this: add a
// built-in binding and forget the example, and it goes red.
//
// Loaded WITHOUT the defaults underneath, which is the whole point. Merged, a
// line missing from the example is indistinguishable from a line present -- the
// built-in fills the hole and the comparison passes. Only the bare parse can
// tell the file actually says what it claims to.
//
// Run from the repo root, which is where `make test` runs it.
static void test_example_config_matches_the_defaults(void) {
    struct config *plain = defaults_config();
    struct config *example = config_load("example/config", false);
    CHECK(example != NULL);     // it also has to parse at all
    if (!plain || !example) {
        config_destroy(plain);
        config_destroy(example);
        return;
    }

    CHECK(example->len == plain->len);

    for (size_t i = 0; i < plain->len; i++) {
        const struct keybind *want = &plain->binds[i];
        const struct keybind *got = find_keybind(example, want->keysym, want->modifiers);
        CHECK(got != NULL);
        if (!got) continue;

        CHECK(got->action == want->action);
        CHECK(got->arg_kind == want->arg_kind);
        // An escape route that lost its exemption in the example would be a
        // passthrough session with no way back out.
        CHECK(got->exempt == want->exempt);
        if (want->arg_kind == SATORI_ARG_CMD) {
            // Catches a quoting slip too: scfg would hand back a mangled command
            // that binds fine and then does the wrong thing at the keypress.
            CHECK(got->arg.cmd && strcmp(got->arg.cmd, want->arg.cmd) == 0);
        } else if (want->arg_kind == SATORI_ARG_LETTER) {
            CHECK(got->arg.u == want->arg.u);
        }
    }

    config_destroy(plain);
    config_destroy(example);
}

// ---- passthrough ----------------------------------------------------------

// Satori never sees raw key events: river matches bindings itself and only
// sends `pressed` for chords that already matched, so there is no lookup to skip
// and no key to forward. Disabling the bindings IS the forward -- the compositor
// then stops matching them and delivers the keys to the client.
//
// These tests cover the decision, which is all of the logic. The enable/disable
// loop needs live proxies and is covered by the smoke test instead.

static void test_passthrough_matches_the_configured_app_ids(void) {
    struct config *config = load_config_text("passthrough org.qemu.qemu virt-manager\n");
    CHECK(config != NULL);
    if (!config) return;

    CHECK(config_is_passthrough(config, "org.qemu.qemu"));
    CHECK(config_is_passthrough(config, "virt-manager"));

    // Case is the client's choice; the config is written by hand.
    CHECK(config_is_passthrough(config, "Org.QEMU.qemu"));

    // Whole string, not a prefix: a window must not inherit passthrough from an
    // app_id it merely starts with.
    CHECK(!config_is_passthrough(config, "org.qemu"));
    CHECK(!config_is_passthrough(config, "org.qemu.qemu.extra"));
    CHECK(!config_is_passthrough(config, "foot"));

    // A window that has not sent an app_id yet matches nothing rather than
    // everything -- backwards, this is a window that eats every binding.
    CHECK(!config_is_passthrough(config, NULL));
    CHECK(!config_is_passthrough(config, ""));
    CHECK(!config_is_passthrough(NULL, "org.qemu.qemu"));

    config_destroy(config);

    // Repeated directives accumulate; there is no built-in set to replace, so
    // nothing needs taking away. A duplicate is not an error and not stored.
    struct config *many = load_config_text(
            "passthrough qemu\npassthrough QEMU\npassthrough vmm\n");
    CHECK(many != NULL);
    if (many) {
        CHECK(many->passthrough_len == 2);
        CHECK(config_is_passthrough(many, "qemu"));
        CHECK(config_is_passthrough(many, "vmm"));
        config_destroy(many);
    }
}

// Passthrough is opt-in: an untouched session must behave exactly as it did
// before the feature existed.
static void test_passthrough_is_empty_by_default(void) {
    struct config *config = defaults_config();
    if (!config) return;

    CHECK(config->passthrough_len == 0);
    CHECK(!config_is_passthrough(config, "org.qemu.qemu"));

    config_destroy(config);
}

static void test_passthrough_follows_the_focused_window(void) {
    struct config *config = load_config_text("passthrough org.qemu.qemu\n");
    CHECK(config != NULL);
    if (!config) return;

    struct fixture f;
    fixture_init(&f);
    f.satori.config = config;
    fixture_app_ids(&f, "foot", "org.qemu.qemu", "foot");

    CHECK(!satori_passthrough_active(&f.satori));    // focus is on a terminal

    f.satori.focused = &f.middle;
    CHECK(satori_passthrough_active(&f.satori));

    // Nothing focused is not passthrough: it would disable every binding with no
    // window to hand the keys to, and no binding left to focus one.
    f.satori.focused = NULL;
    CHECK(!satori_passthrough_active(&f.satori));

    config_destroy(config);
}

// The escape binding suspends passthrough per app_id, so every window of that
// app is covered -- including ones that do not exist yet.
//
// This is THE bug the app_id key exists to fix. Moonlight destroys its window
// and creates a new one when a stream starts, and with the flag on struct window
// the suspension died with it: passthrough re-armed mid-stream, the keyboard
// vanished again, and nothing in the log said why. The fixture models that by
// suspending one window and then asking about a different struct with the same
// app_id.
static void test_passthrough_escape_survives_a_recreated_window(void) {
    struct config *config = load_config_text("passthrough org.qemu.qemu\n");
    CHECK(config != NULL);
    if (!config) return;

    struct fixture f;
    fixture_init(&f);
    f.satori.config = config;
    fixture_app_ids(&f, "org.qemu.qemu", "org.qemu.qemu", "foot");

    CHECK(satori_passthrough_active(&f.satori));

    action_toggle_passthrough(&f.satori, NOARG);
    CHECK(!satori_passthrough_active(&f.satori));

    // A different struct window entirely -- the app's replacement window -- and
    // the suspension still holds. On the old per-window flag this was the check
    // that came back true and took the keyboard away.
    f.satori.focused = &f.middle;
    CHECK(!satori_passthrough_active(&f.satori));

    // Another app is unaffected: suspending one must not disarm the rest.
    CHECK(!config_is_passthrough(config, "foot"));

    // And it comes back: a suspension that could not be undone would make the
    // config line stop meaning anything after one press. Resuming from the
    // replacement window works, which is the only window still on screen.
    action_toggle_passthrough(&f.satori, NOARG);
    CHECK(satori_passthrough_active(&f.satori));
    f.satori.focused = &f.newest;
    CHECK(satori_passthrough_active(&f.satori));

    passthrough_suspend_free(&f.satori);
    config_destroy(config);
}

// Case-insensitive, matching config_is_passthrough. Load-bearing rather than
// defensive: the suspend is stored from one window's app_id and then tested
// against a *different* window's, and nothing promises an app spells its app_id
// identically across a window it destroyed and one it recreated. Suspending from
// the same string it was stored under would pass under strcmp too, so the two
// windows here deliberately disagree on case.
static void test_passthrough_escape_matches_app_id_case_insensitively(void) {
    struct config *config = load_config_text("passthrough org.QEMU.qemu\n");
    CHECK(config != NULL);
    if (!config) return;

    struct fixture f;
    fixture_init(&f);
    f.satori.config = config;
    fixture_app_ids(&f, "ORG.qemu.QEMU", "org.qemu.qemu", "foot");

    CHECK(satori_passthrough_active(&f.satori));        // config match is case-blind
    action_toggle_passthrough(&f.satori, NOARG);
    CHECK(!satori_passthrough_active(&f.satori));

    // The replacement window, spelled differently. The suspend must still hold.
    f.satori.focused = &f.middle;
    CHECK(!satori_passthrough_active(&f.satori));

    // And resuming from that spelling clears the entry stored under the other.
    action_toggle_passthrough(&f.satori, NOARG);
    CHECK(f.satori.suspended_len == 0);
    CHECK(satori_passthrough_active(&f.satori));

    passthrough_suspend_free(&f.satori);
    config_destroy(config);
}

static void test_passthrough_escape_with_nothing_focused_is_a_no_op(void) {
    struct satori satori = {0};
    action_toggle_passthrough(&satori, NOARG);
    CHECK(satori.focused == NULL);
    CHECK(satori.suspended_len == 0);
}

// A window that has not reported an app_id yet can never be in passthrough, so
// there is nothing to suspend. Recording one would put a NULL in the set.
static void test_passthrough_escape_with_no_app_id_is_a_no_op(void) {
    struct fixture f;
    fixture_init(&f);           // app_ids are all NULL

    action_toggle_passthrough(&f.satori, NOARG);
    CHECK(f.satori.suspended_len == 0);
}

// The exemption is what makes passthrough safe to turn on. It is not an ordering
// check that could be got wrong -- exempt bindings are simply never disabled.
// Satori owns 100% of input and river ships no riverctl, so losing both of these
// is a session with no way out and no way to fix it.
static void test_passthrough_exempts_the_escape_routes(void) {
    struct config *config = defaults_config();
    if (!config) return;

    const struct keybind *escape = find_keybind(config, XKB_KEY_p, MOD|SHFT);
    const struct keybind *quit = find_keybind(config, XKB_KEY_e, MOD|SHFT);
    CHECK(escape && escape->action == action_toggle_passthrough);
    CHECK(quit && quit->action == action_exit_session);
    CHECK(escape && escape->exempt);
    CHECK(quit && quit->exempt);

    // Exactly those two and nothing else. An exempt binding is one a passthrough
    // app can never receive, so the set has to stay deliberate.
    size_t exempt = 0;
    for (size_t i = 0; i < config->len; i++) {
        if (!config->binds[i].exempt) continue;
        exempt++;
        CHECK(config->binds[i].action == action_toggle_passthrough
                || config->binds[i].action == action_exit_session);
    }
    CHECK(exempt == 2);

    // What bindings_apply_enabled asks of each binding, in both states.
    for (size_t i = 0; i < config->len; i++) {
        const struct keybind *k = &config->binds[i];
        CHECK(binding_stays_enabled(k, false));      // nothing is disabled with passthrough off
        CHECK(binding_stays_enabled(k, true) == k->exempt);
    }

    config_destroy(config);
}

// The exemption belongs to the action, not the chord, so moving the escape hatch
// to a comfortable key carries it. A chord-level flag would let a config file
// relocate the binding and silently lose the only way back out.
static void test_passthrough_exemption_follows_a_rebound_action(void) {
    struct config *config = load_config_text(
            "bind Mod+Shift+P none\n"
            "bind Mod+Ctrl+Escape passthrough\n");
    CHECK(config != NULL);
    if (!config) return;

    CHECK(find_keybind(config, XKB_KEY_p, MOD|SHFT) == NULL);

    const struct keybind *moved = find_keybind(config, XKB_KEY_Escape, MOD|CTRL);
    CHECK(moved && moved->action == action_toggle_passthrough);
    CHECK(moved && moved->exempt);

    config_destroy(config);
}

// Deferred like every other action, for a sharper reason: reloading frees the
// keybind table the running binding callback is reading out of.
static void test_reload_only_records_intent(void) {
    struct satori satori = {0};

    action_reload_config(&satori, NOARG);
    CHECK(satori.reload_pending);
    CHECK(satori.config == NULL);       // nothing was rebuilt at the keypress
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
    test_app_keybinds_cover_every_letter();
    test_keybind_table_is_well_formed();
    test_media_keys_are_bound_unmodified();

    test_chord_parse_reads_modifiers_and_key();
    test_chord_parse_accepts_modifier_aliases();
    test_chord_parse_lowers_the_keysym();
    test_chord_parse_rejects_junk();
    test_config_merges_over_the_defaults();
    test_config_override_does_not_grow_the_table();
    test_config_none_unbinds();
    test_config_app_keys_can_move_or_go();
    test_config_rejects_a_broken_file();
    test_config_missing_file_is_the_defaults();
    test_config_spawn_joins_its_words();
    test_config_binds_focus_app_by_hand();
    test_config_bind_overrides_a_generated_letter();
    test_config_set_replaces_owned_commands();
    test_example_config_matches_the_defaults();
    test_reload_only_records_intent();

    test_passthrough_matches_the_configured_app_ids();
    test_passthrough_is_empty_by_default();
    test_passthrough_follows_the_focused_window();
    test_passthrough_escape_survives_a_recreated_window();
    test_passthrough_escape_matches_app_id_case_insensitively();
    test_passthrough_escape_with_no_app_id_is_a_no_op();
    test_passthrough_escape_with_nothing_focused_is_a_no_op();
    test_passthrough_exempts_the_escape_routes();
    test_passthrough_exemption_follows_a_rebound_action();

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
           "  ok    keybind tables, media keys\n"
           "  ok    chord parsing, keysym lowering\n"
           "  ok    config merge, unbind, app-keys, rejection\n"
           "  ok    binding table ownership, reload intent\n"
           "  ok    passthrough matching, escape toggle, exempt bindings\n\nPASS\n");
    return 0;
}
