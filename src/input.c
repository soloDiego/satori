// Seats, key bindings, and the actions they trigger.
//
// A binding's pressed event arrives *before* the manage sequence that follows
// it, so an action may not touch window management state. Actions record intent
// (satori->focused, win->close_pending); the manage sequence applies it.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "satori.h"

// Run cmd via the shell, detached. Double fork so the grandchild is reparented
// to init and never becomes a zombie satori has to reap.
static void spawn(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "spawn: fork failed\n");
        return;
    }
    if (pid == 0) {
        if (fork() == 0) {
            setsid();

            // satori blocks SIGINT/SIGTERM for its signalfd; the mask survives
            // exec, so clear it or the child inherits a session it can't be
            // interrupted in.
            sigset_t mask;
            sigemptyset(&mask);
            sigprocmask(SIG_SETMASK, &mask, NULL);

            execl("/bin/sh", "/bin/sh", "-c", cmd, (char *) NULL);
            _exit(127);
        }
        _exit(0);
    }
    waitpid(pid, NULL, 0);      // the intermediate child exits immediately
}

static void action_spawn(struct satori *satori, union satori_arg arg) {
    (void) satori;
    spawn(arg.cmd);
}
// The only action that is not deferred: exit_session is not window management
// state, so it carries no sequence constraint. Every client including us is
// disconnected, and the event loop falls out on the closed display.
static void action_exit_session(struct satori *satori, union satori_arg arg) {
    (void) arg;

    if (satori->wm_version < 4) {
        fprintf(stderr, "exit_session needs river_window_manager_v1 v4, have v%u\n",
                satori->wm_version);
        return;
    }
    fprintf(stderr, "action: exit session\n");
    river_window_manager_v1_exit_session(satori->wm);
}
static void action_close_focused(struct satori *satori, union satori_arg arg) {
    (void) arg;

    if (satori->focused) satori->focused->close_pending = true;
}
// The WM-initiated half of fullscreen. Until this existed only the client could
// ask (foot's own bind, a video player), so a window that offers no fullscreen
// control of its own could never be fullscreened at all.
static void action_toggle_fullscreen(struct satori *satori, union satori_arg arg) {
    (void) arg;

    struct window *win = satori->focused;
    if (!win) return;

    // Exactly the intent win_fullscreen_requested records, so both paths land in
    // windows_apply_fullscreen and there is one implementation, not two.
    win->fullscreen = !win->fullscreen;
    win->fullscreen_dirty = true;
    win->fs_output = NULL;      // no preference; apply time falls back to the first output
}
// The other half of the default layout: a floating window keeps its own size and
// position instead of filling the usable area. Same shape as the fullscreen
// toggle -- record the intent a client's unmaximize_requested records, and let
// windows_propose be the single place either one is applied.
static void action_toggle_floating(struct satori *satori, union satori_arg arg) {
    (void) arg;

    struct window *win = satori->focused;
    if (!win) return;

    win->floating = !win->floating;
    // proposed is the dirty bit here: clearing it is what makes the next manage
    // sequence re-size the window and inform it of the new state.
    win->proposed = false;
}
// The reason satori exists: one chord goes straight to the window you want, with
// no cycling and nothing to read on screen. arg.u is the letter to match.
static void action_focus_app(struct satori *satori, union satori_arg arg) {
    char letter = (char) arg.u;

    struct window *win = window_find_by_app(satori, letter);
    if (!win) {
        fprintf(stderr, "action: no window for '%c'\n", letter);
        return;
    }
    window_focus(satori, win);
    fprintf(stderr, "action: focus app '%c'\n", letter);
}
static void action_focus_next(struct satori *satori, union satori_arg arg) {
    (void) arg;

    if (!satori->windows) return;

    struct window *next = (satori->focused && satori->focused->next)
        ? satori->focused->next
        : satori->windows;      // wrap
    window_focus(satori, next);
}
static void action_focus_prev(struct satori *satori, union satori_arg arg) {
    (void) arg;

    if (!satori->windows) return;

    // The window whose next is the focused one; the tail when focus is at the
    // head or unset, which is the wrap.
    struct window *prev = satori->windows;
    for (struct window *win = satori->windows; win; win = win->next) {
        if (win->next == satori->focused || !win->next) {
            prev = win;
            break;
        }
    }
    window_focus(satori, prev);
}

#define MOD  RIVER_SEAT_V1_MODIFIERS_MOD4    // super
#define ALT  RIVER_SEAT_V1_MODIFIERS_MOD1
#define SHFT RIVER_SEAT_V1_MODIFIERS_SHIFT

// Fixed for now; the scfg config parser will build this table at startup.
//
// Satori owns every binding in the session: river 0.4 has no built-in ones and
// ships no riverctl. If it is not in this table, there is no way to do it --
// including leaving the session, hence the exit binding.
//
// mod4|mod1 belongs to the app_id lookup below and stays out of this table, so
// the whole Mod+<letter> space is free for ordinary bindings and the two never
// have to arbitrate.
static const struct keybind keybinds[] = {
    { XKB_KEY_Return, MOD, action_spawn, { .cmd = "foot"   } },
    { XKB_KEY_space,  MOD, action_spawn, { .cmd = "fuzzel" } },
    { XKB_KEY_q,      MOD, action_close_focused, {0} },
    { XKB_KEY_f,      MOD, action_toggle_fullscreen, {0} },
    { XKB_KEY_j,      MOD, action_focus_next,    {0} },
    { XKB_KEY_k,      MOD, action_focus_prev,    {0} },

    // Float/maximize toggle. Shift+space is still the space keysym: river
    // matches the unshifted one, so this does not collide with Mod+Space above.
    { XKB_KEY_space, MOD|SHFT, action_toggle_floating, {0} },

    // Ends the session with no confirmation. River matches the unshifted
    // keysym: XKB_KEY_E binds without error and never fires.
    { XKB_KEY_e, MOD|SHFT, action_exit_session, {0} },
};

// Mod+Alt+<letter>, one binding per letter. Generated rather than typed out:
// twenty-six near-identical rows are noise, and a letter missed in the middle of
// them is a key that silently does nothing.
#define APP_KEYBIND_COUNT 26
static struct keybind app_keybinds[APP_KEYBIND_COUNT];

// The letter keysyms are their own ASCII values, so the keysym doubles as the
// letter to match. Both are spelled out anyway -- they are different things that
// only happen to coincide.
_Static_assert(XKB_KEY_z - XKB_KEY_a == APP_KEYBIND_COUNT - 1,
        "letter keysyms are not contiguous");

// Idempotent: bindings borrow pointers into this table, and it is refilled with
// the same values on every seat.
static void app_keybinds_init(void) {
    for (uint32_t i = 0; i < APP_KEYBIND_COUNT; i++) {
        app_keybinds[i] = (struct keybind){
            .keysym    = XKB_KEY_a + i,
            .modifiers = MOD|ALT,
            .action    = action_focus_app,
            .arg       = { .u = (uint32_t) ('a' + i) },
        };
    }
}

static void binding_pressed(void *data, struct river_xkb_binding_v1 *handle) {
    (void) handle;

    struct binding *bind = data;
    fprintf(stderr, "binding: pressed keysym 0x%x mods 0x%x\n",
            bind->keybind->keysym, bind->keybind->modifiers);
    bind->keybind->action(bind->satori, bind->keybind->arg);
}
static void binding_released(void *data, struct river_xkb_binding_v1 *handle) {
    (void)data; (void)handle;
}
static void binding_stop_repeat(void *data, struct river_xkb_binding_v1 *handle) {
    (void)data; (void)handle;   // nothing repeats yet
}
static const struct river_xkb_binding_v1_listener binding_listener = {
    .pressed     = binding_pressed,
    .released    = binding_released,
    .stop_repeat = binding_stop_repeat,
};

static void binding_create(struct satori *satori, struct seat *seat, const struct keybind *keybind) {
    struct binding *bind = calloc(1, sizeof *bind);
    if (!bind) {
        fprintf(stderr, "binding_create: calloc failed\n");
        return;
    }
    bind->satori  = satori;
    bind->keybind = keybind;
    bind->handle  = river_xkb_bindings_v1_get_xkb_binding(satori->xkb, seat->handle,
            keybind->keysym, keybind->modifiers);

    bind->next = satori->bindings;
    satori->bindings = bind;

    river_xkb_binding_v1_add_listener(bind->handle, &binding_listener, bind);
}

void seat_create(struct satori *satori, struct river_seat_v1 *handle) {
    struct seat *seat = calloc(1, sizeof *seat);
    if (!seat) {
        fprintf(stderr, "seat_create: calloc failed\n");
        return;
    }
    seat->handle = handle;
    seat->satori = satori;

    seat->next = satori->seats;
    satori->seats = seat;

    layer_seat_create(satori, seat);

    // No key bindings without the xkb bindings global; everything else still works.
    if (satori->xkb) {
        for (size_t i = 0; i < sizeof keybinds / sizeof keybinds[0]; i++) {
            binding_create(satori, seat, &keybinds[i]);
        }
        app_keybinds_init();
        for (size_t i = 0; i < APP_KEYBIND_COUNT; i++) {
            binding_create(satori, seat, &app_keybinds[i]);
        }
    } else {
        fprintf(stderr, "seat: no river_xkb_bindings_v1, key bindings disabled\n");
    }
    fprintf(stderr, "wm: seat\n");
}

void seats_apply_focus(struct satori *satori) {
    if (!satori->focus_dirty) return;

    bool applied = false, deferred = false;
    for (struct seat *seat = satori->seats; seat; seat = seat->next) {
        // A layer surface holds this seat: focusing now is either ignored
        // (exclusive) or steals the focus the launcher is about to get
        // (non-exclusive). Stay dirty and wait for focus_none.
        if (seat->layer_focus) {
            deferred = true;
            continue;
        }
        if (satori->focused) {
            river_seat_v1_focus_window(seat->handle, satori->focused->handle);
        } else {
            river_seat_v1_clear_focus(seat->handle);
        }
        applied = true;
    }
    satori->focus_dirty = deferred;
    if (applied) {
        fprintf(stderr, "seat: focus %s\n", satori->focused ? "window" : "cleared");
    }
}

void bindings_enable_pending(struct satori *satori) {
    for (struct binding *bind = satori->bindings; bind; bind = bind->next) {
        if (bind->enabled) continue;
        river_xkb_binding_v1_enable(bind->handle);
        bind->enabled = true;
    }
}

void bindings_destroy_all(struct satori *satori) {
    struct binding *bind = satori->bindings;
    while (bind) {
        struct binding *next = bind->next;
        river_xkb_binding_v1_destroy(bind->handle);
        free(bind);
        bind = next;
    }
    satori->bindings = NULL;
}

void seats_destroy_all(struct satori *satori) {
    struct seat *seat = satori->seats;
    while (seat) {
        struct seat *next = seat->next;
        layer_seat_destroy(seat);
        river_seat_v1_destroy(seat->handle);
        free(seat);
        seat = next;
    }
    satori->seats = NULL;
}
