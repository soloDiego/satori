// Seats, key bindings, and the actions they trigger.
//
// A binding's pressed event arrives *before* the manage sequence that follows
// it, so an action may not touch window management state. Actions record intent
// (satori->focused, win->close_pending); the manage sequence applies it.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
// Deferred like everything else, and for a sharper reason than usual: a reload
// destroys every river_xkb_binding_v1 and frees the keybind table, including the
// binding object whose pressed callback is running right now. The manage
// sequence that follows this keypress is a safe place to pull that out.
static void action_reload_config(struct satori *satori, union satori_arg arg) {
    (void) arg;

    satori->reload_pending = true;
}
// The escape hatch out of passthrough, and the reason passthrough is safe to
// turn on at all. This action is exempt, so it is the one chord a passthrough
// app can never swallow.
//
// Suspends passthrough for the focused window only, and until it is pressed
// again -- reach the WM keys inside a VM, then hand the keyboard back, without
// editing the config. Nothing to defer and no dirty flag: bindings_apply_enabled
// recomputes the whole enabled set in the manage sequence that follows this
// keypress, and reads passthrough_off directly.
static void action_toggle_passthrough(struct satori *satori, union satori_arg arg) {
    (void) arg;

    struct window *win = satori->focused;
    if (!win) return;

    win->passthrough_off = !win->passthrough_off;
    // The window is named because the toggle applies to whatever has focus, not
    // to the passthrough app you were looking at. Press it while a terminal is
    // focused and it flips a flag on the terminal -- harmless, but it logs the
    // same two words, and reading them as "moonlight is back" is a wrong turn
    // that costs an hour.
    fprintf(stderr, "action: passthrough %s for %s\n",
            win->passthrough_off ? "suspended" : "resumed",
            win->app_id ? win->app_id : "(no app_id)");
}

// The names a config file uses to reach an action. This table is the only
// handle it has, so an action that is not listed here cannot be bound.
//
// The last column is the passthrough exemption: an exempt binding is never
// disabled, whatever window has focus. Exactly two are exempt, and both are
// escape routes -- `passthrough` gets the keyboard back from an app, `exit`
// gets out of the session. Satori owns 100% of input and river ships no
// riverctl, so if a passthrough app could swallow both there would be no way
// out of a session at all.
static const struct action_spec action_specs[] = {
    { "spawn",       action_spawn,              SATORI_ARG_CMD,    false },
    { "close",       action_close_focused,      SATORI_ARG_NONE,   false },
    { "fullscreen",  action_toggle_fullscreen,  SATORI_ARG_NONE,   false },
    { "float",       action_toggle_floating,    SATORI_ARG_NONE,   false },
    { "focus-next",  action_focus_next,         SATORI_ARG_NONE,   false },
    { "focus-prev",  action_focus_prev,         SATORI_ARG_NONE,   false },
    { "focus-app",   action_focus_app,          SATORI_ARG_LETTER, false },
    { "reload",      action_reload_config,      SATORI_ARG_NONE,   false },
    { "passthrough", action_toggle_passthrough, SATORI_ARG_NONE,   true  },
    { "exit",        action_exit_session,       SATORI_ARG_NONE,   true  },
};

const struct action_spec *action_from_name(const char *name) {
    for (size_t i = 0; i < sizeof action_specs / sizeof action_specs[0]; i++) {
        if (strcmp(name, action_specs[i].name) == 0) return &action_specs[i];
    }
    return NULL;
}

#define MOD  RIVER_SEAT_V1_MODIFIERS_MOD4    // super
#define SHFT RIVER_SEAT_V1_MODIFIERS_SHIFT

// Brightness is written straight to sysfs rather than shelled out to
// brightnessctl or light: neither is installed, and the file is group-writable
// to `video`, which the user is in. Same rule that dropped the screenshot
// binding -- do not ship a binding to software that is not there.
//
// The floor of 1% of max is load-bearing. A backlight at 0 is a black screen,
// and the key that raises it again is one you can no longer see.
#define BACKLIGHT_DIR "/sys/class/backlight/intel_backlight"
#define BRIGHTNESS_CMD(op)                                     \
    "d=" BACKLIGHT_DIR "; "                                    \
    "m=$(cat $d/max_brightness); c=$(cat $d/brightness); "     \
    "lo=$((m/100+1)); n=$((c " op " m/20)); "                  \
    "[ $n -gt $m ] && n=$m; [ $n -lt $lo ] && n=$lo; "         \
    "echo $n > $d/brightness"

// The built-in bindings. A config file merges over these rather than replacing
// them, so an unlisted chord stays bound and `bind <chord> none` is how one is
// taken away.
//
// Satori owns every binding in the session: river 0.4 has no built-in ones and
// ships no riverctl. If it is not reachable from this table or the config, there
// is no way to do it -- including leaving the session, hence the exit binding.
//
// mod4|mod1 belongs to the generated app_id lookup (see config_add_app_keys) and
// stays out of this table, so the whole Mod+<letter> space is free for ordinary
// bindings and the two never have to arbitrate.
//
// Written as action *names* rather than function pointers so the built-ins go in
// through config_set, exactly like a config file's lines: one code path, and a
// default that would be rejected in a config file is rejected here too.
static const struct {
    uint32_t    keysym;
    uint32_t    modifiers;
    const char  *action;
    const char  *cmd;       // NULL for actions that take no argument
} defaults[] = {
    { XKB_KEY_Return, MOD, "spawn", "foot"   },
    { XKB_KEY_space,  MOD, "spawn", "fuzzel" },
    { XKB_KEY_q,      MOD, "close",      NULL },
    { XKB_KEY_f,      MOD, "fullscreen", NULL },
    { XKB_KEY_j,      MOD, "focus-next", NULL },
    { XKB_KEY_k,      MOD, "focus-prev", NULL },

    // Float/maximize toggle. Shift+space is still the space keysym: river
    // matches the unshifted one, so this does not collide with Mod+Space above.
    { XKB_KEY_space, MOD|SHFT, "float", NULL },

    // Re-reads the config file. Bound as well as wired to SIGHUP because it
    // still works when there is no terminal open to send a signal from.
    { XKB_KEY_r, MOD|SHFT, "reload", NULL },

    // Suspends passthrough for the focused window. Exempt, so it stays live
    // while every other binding is disabled -- see action_specs.
    { XKB_KEY_p, MOD|SHFT, "passthrough", NULL },

    // Ends the session with no confirmation. River matches the unshifted
    // keysym: XKB_KEY_E binds without error and never fires.
    { XKB_KEY_e, MOD|SHFT, "exit", NULL },

    // The XF86 media keys, bound with NO modifier -- the one place in this table
    // where that is correct. These keysyms produce no text, so grabbing them
    // cannot swallow normal typing, which is the entire reason every other
    // binding needs a modifier. The keycaps are printed with these functions.
    //
    // Volume goes through wpctl: wireplumber is the session manager here. The
    // `-l 1.0` on raise is a cap at 100% -- without it wpctl boosts past unity
    // and the result is clipping, not loudness.
    { XKB_KEY_XF86AudioMute, 0, "spawn",
        "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle" },
    { XKB_KEY_XF86AudioLowerVolume, 0, "spawn",
        "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-" },
    { XKB_KEY_XF86AudioRaiseVolume, 0, "spawn",
        "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+ -l 1.0" },
    { XKB_KEY_XF86AudioMicMute, 0, "spawn",
        "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle" },

    { XKB_KEY_XF86MonBrightnessDown, 0, "spawn", BRIGHTNESS_CMD("-") },
    { XKB_KEY_XF86MonBrightnessUp,   0, "spawn", BRIGHTNESS_CMD("+") },
};

bool config_apply_defaults(struct config *config) {
    for (size_t i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
        const struct action_spec *spec = action_from_name(defaults[i].action);
        if (!spec) {
            fprintf(stderr, "config: built-in binding names unknown action '%s'\n",
                    defaults[i].action);
            return false;
        }
        union satori_arg arg = { .cmd = defaults[i].cmd };
        if (!config_set(config, defaults[i].keysym, defaults[i].modifiers, spec, arg)) {
            return false;
        }
    }
    return true;
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

// Borrows &config->binds[i] into every proxy, which is why a config is never
// grown again once bindings exist -- see the note on struct config.
static void seat_bindings_create(struct satori *satori, struct seat *seat) {
    if (!satori->xkb || !satori->config) return;

    for (size_t i = 0; i < satori->config->len; i++) {
        binding_create(satori, seat, &satori->config->binds[i]);
    }
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
        seat_bindings_create(satori, seat);
    } else {
        fprintf(stderr, "seat: no river_xkb_bindings_v1, key bindings disabled\n");
    }
    fprintf(stderr, "wm: seat\n");
}

void bindings_create_all(struct satori *satori) {
    if (!satori->xkb) return;

    for (struct seat *seat = satori->seats; seat; seat = seat->next) {
        seat_bindings_create(satori, seat);
    }
}

// Parse into a new table and swap only on success. A reload that cleared the
// bindings and then failed to rebuild them would be a session with no way out --
// no terminal, no launcher, and no way to fix the config that broke it.
//
// The teardown order is the load-bearing part: every proxy borrows a
// &config->binds[i], so all of them have to go before the table they point into
// is freed. Running inside the manage sequence means the fresh bindings are
// enabled by bindings_apply_enabled a few lines later, in this same sequence.
void config_reload(struct satori *satori) {
    if (!satori->reload_pending) return;
    satori->reload_pending = false;

    struct config *fresh = config_load(satori->config_path, true);
    if (!fresh) {
        fprintf(stderr, "config: reload failed, keeping the running bindings\n");
        return;
    }

    bindings_destroy_all(satori);
    config_destroy(satori->config);
    satori->config = fresh;
    bindings_create_all(satori);

    fprintf(stderr, "config: reloaded %zu bindings\n", fresh->len);
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
    // Naming the window is not decoration: passthrough is keyed on app_id, and
    // an unnamed "focus window" makes "which window has the keyboard" -- the
    // only question that matters when a chord stops working -- unanswerable
    // from the log. Same gap, and same fix, as `window: app_id`.
    if (applied) {
        if (satori->focused) {
            fprintf(stderr, "seat: focus window (%s)\n",
                    satori->focused->app_id ? satori->focused->app_id : "none");
        } else {
            fprintf(stderr, "seat: focus cleared\n");
        }
    }
}

// Passthrough: hand the keyboard to the focused window's client instead of
// binding it.
//
// Satori never sees raw key events -- river matches bindings itself and only
// sends us `pressed` for chords that already matched, and the XML is explicit
// that everything else goes straight to the focused surface. So there is no
// lookup to skip and no key to forward. Disabling the bindings IS the forward:
// with them off, the compositor stops matching those chords and delivers them
// to the client like any other key.
//
// The escape binding is not "checked first", it is never disabled at all -- the
// exemption is structural, so no ordering bug can shadow it. See action_specs.
bool satori_passthrough_active(const struct satori *satori) {
    const struct window *win = satori->focused;
    if (!win || win->passthrough_off) return false;

    return config_is_passthrough(satori->config, win->app_id);
}

// Its own function for the same reason window_position is: the loop below sends
// requests on live proxies and cannot run in a unit test, and "the escape
// binding survives passthrough" is the part most worth pinning.
bool binding_stays_enabled(const struct keybind *keybind, bool passthrough) {
    return !passthrough || keybind->exempt;
}

// Recomputed from scratch every manage sequence rather than driven by a dirty
// flag. The inputs are focus, the focused window's app_id, the escape toggle and
// the config, and app_id in particular arrives on its own event some time AFTER
// the window does -- a flag set at focus time would miss it and leave a
// passthrough app bound until something else moved. The walk is a bool compare
// over a few dozen bindings and only sends a request when one actually changes.
void bindings_apply_enabled(struct satori *satori) {
    bool passthrough = satori_passthrough_active(satori);

    for (struct binding *bind = satori->bindings; bind; bind = bind->next) {
        bool want = binding_stays_enabled(bind->keybind, passthrough);
        if (bind->enabled == want) continue;

        if (want) {
            river_xkb_binding_v1_enable(bind->handle);
        } else {
            river_xkb_binding_v1_disable(bind->handle);
        }
        bind->enabled = want;
    }

    if (passthrough != satori->passthrough) {
        satori->passthrough = passthrough;
        fprintf(stderr, "passthrough: %s\n", passthrough ? "on" : "off");
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
