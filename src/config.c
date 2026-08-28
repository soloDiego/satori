// The scfg config file, and the binding table it builds.
//
// Nothing here talks to a compositor: this is the pure half of the keybind
// system, and it is meant to be exercised directly by tests. It knows about
// chords and about struct config; it reaches actions only through
// action_from_name, so the list of actions stays in input.c next to them.
//
// Parsing never mutates the running table. config_load builds a fresh one and
// returns it, or returns NULL and leaves the caller's table alone -- satori
// owns 100% of input, so a reload that cleared the bindings and then failed to
// rebuild them would be a session with no way out, including no way to fix the
// config that broke it.

#include <scfg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "satori.h"

// The names a config file may use for each modifier bit. Several spellings map
// to the same bit on purpose: "Mod" is what the docs and river's own defaults
// call super, "Super" and "Logo" are what people coming from sway type.
static const struct {
    const char  *name;
    uint32_t    bit;
} modifier_names[] = {
    { "shift",   RIVER_SEAT_V1_MODIFIERS_SHIFT },
    { "ctrl",    RIVER_SEAT_V1_MODIFIERS_CTRL  },
    { "control", RIVER_SEAT_V1_MODIFIERS_CTRL  },
    { "alt",     RIVER_SEAT_V1_MODIFIERS_MOD1  },
    { "mod1",    RIVER_SEAT_V1_MODIFIERS_MOD1  },
    { "mod",     RIVER_SEAT_V1_MODIFIERS_MOD4  },
    { "super",   RIVER_SEAT_V1_MODIFIERS_MOD4  },
    { "logo",    RIVER_SEAT_V1_MODIFIERS_MOD4  },
    { "mod4",    RIVER_SEAT_V1_MODIFIERS_MOD4  },
    { "mod3",    RIVER_SEAT_V1_MODIFIERS_MOD3  },
    { "mod5",    RIVER_SEAT_V1_MODIFIERS_MOD5  },
};

static bool modifier_from_name(const char *name, uint32_t *bit) {
    for (size_t i = 0; i < sizeof modifier_names / sizeof modifier_names[0]; i++) {
        if (strcasecmp(name, modifier_names[i].name) == 0) {
            *bit = modifier_names[i].bit;
            return true;
        }
    }
    return false;
}

// A '+'-separated list of modifiers and nothing else, e.g. "Mod+Alt".
bool modifiers_parse(const char *spec, uint32_t *modifiers) {
    if (!spec || !*spec) return false;

    char *copy = strdup(spec);
    if (!copy) return false;

    uint32_t mods = 0;
    bool ok = true;
    char *save = NULL;

    for (char *tok = strtok_r(copy, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        uint32_t bit;
        if (!modifier_from_name(tok, &bit)) {
            ok = false;
            break;
        }
        mods |= bit;
    }

    free(copy);
    if (ok) *modifiers = mods;
    return ok;
}

// "Mod+Shift+E" -> keysym XKB_KEY_e, modifiers mod4|shift. Every token but the
// last is a modifier; the last one is the key.
//
// The keysym is lowered deliberately. River matches the UNSHIFTED keysym, so a
// binding stored as XKB_KEY_E is accepted by the compositor without error and
// then never fires. That failure is silent and it once shipped on the only
// binding that leaves the session, so it is handled here, once, rather than
// left to whoever writes the config.
bool chord_parse(const char *chord, uint32_t *keysym, uint32_t *modifiers) {
    if (!chord || !*chord) return false;

    char *copy = strdup(chord);
    if (!copy) return false;

    uint32_t mods = 0;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;
    bool ok = true;
    char *save = NULL;
    char *tok = strtok_r(copy, "+", &save);

    while (tok && ok) {
        char *next = strtok_r(NULL, "+", &save);
        if (next) {
            uint32_t bit;
            if (modifier_from_name(tok, &bit)) {
                mods |= bit;
            } else {
                ok = false;
            }
        } else {
            // Exact first: keysym names are case sensitive and the strict
            // lookup is the cheap one. The fallback is what makes "Space" and
            // "return" work as well as "space" and "Return".
            sym = xkb_keysym_from_name(tok, XKB_KEYSYM_NO_FLAGS);
            if (sym == XKB_KEY_NoSymbol) {
                sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
            }
            if (sym == XKB_KEY_NoSymbol) ok = false;
        }
        tok = next;
    }

    free(copy);
    if (!ok) return false;

    *keysym = xkb_keysym_to_lower(sym);
    *modifiers = mods;
    return true;
}

static struct keybind *config_find(struct config *config, uint32_t keysym, uint32_t modifiers) {
    for (size_t i = 0; i < config->len; i++) {
        if (config->binds[i].keysym == keysym && config->binds[i].modifiers == modifiers) {
            return &config->binds[i];
        }
    }
    return NULL;
}

// Release whatever the arg owns. Consulting arg_kind is the point: freeing
// unconditionally would treat a focus-app letter as a pointer.
static void keybind_finish(struct keybind *keybind) {
    if (keybind->arg_kind == SATORI_ARG_CMD) free((char *) keybind->arg.cmd);
    keybind->arg = (union satori_arg){0};
    keybind->arg_kind = SATORI_ARG_NONE;
}

// Install a binding, replacing any existing one on the same chord. arg.cmd is
// copied, so callers keep ownership of what they pass in.
bool config_set(struct config *config, uint32_t keysym, uint32_t modifiers,
        const struct action_spec *spec, union satori_arg arg) {
    if (!spec) return false;

    char *cmd = NULL;
    if (spec->arg_kind == SATORI_ARG_CMD) {
        if (!arg.cmd) return false;
        cmd = strdup(arg.cmd);
        if (!cmd) return false;
    }

    struct keybind *keybind = config_find(config, keysym, modifiers);
    if (keybind) {
        keybind_finish(keybind);    // the override drops the command it replaces
    } else {
        if (config->len == config->cap) {
            size_t cap = config->cap ? config->cap * 2 : 64;
            struct keybind *grown = realloc(config->binds, cap * sizeof *grown);
            if (!grown) {
                free(cmd);
                return false;
            }
            config->binds = grown;
            config->cap = cap;
        }
        keybind = &config->binds[config->len++];
    }

    *keybind = (struct keybind){
        .keysym    = keysym,
        .modifiers = modifiers,
        .action    = spec->action,
        .arg_kind  = spec->arg_kind,
        .arg       = cmd ? (union satori_arg){ .cmd = cmd } : arg,
    };
    return true;
}

// The counterpart to merging over the built-ins: without a way to take a
// binding away there is only a way to point it somewhere else.
void config_unset(struct config *config, uint32_t keysym, uint32_t modifiers) {
    struct keybind *keybind = config_find(config, keysym, modifiers);
    if (!keybind) return;

    keybind_finish(keybind);
    // Order means nothing to the compositor, so fill the hole with the tail.
    // Self-assignment when the match is already last, which is harmless.
    *keybind = config->binds[--config->len];
}

#define APP_KEYBIND_COUNT 26

// The letter keysyms are their own ASCII values, so the keysym doubles as the
// letter to match. Both are spelled out anyway -- they are different things
// that only happen to coincide.
_Static_assert(XKB_KEY_z - XKB_KEY_a == APP_KEYBIND_COUNT - 1,
        "letter keysyms are not contiguous");

// Generated rather than written out in the config: twenty-six near-identical
// lines are noise, and a letter missed in the middle of them is a key that
// silently does nothing.
static bool config_add_app_keys(struct config *config, uint32_t modifiers) {
    const struct action_spec *spec = action_from_name("focus-app");

    for (uint32_t i = 0; i < APP_KEYBIND_COUNT; i++) {
        union satori_arg arg = { .u = (uint32_t) ('a' + i) };
        if (!config_set(config, XKB_KEY_a + i, modifiers, spec, arg)) return false;
    }
    return true;
}

// Unquoted words are joined with single spaces, so a plain
// `spawn wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+` works without quoting.
// Quotes are only needed to protect runs of whitespace.
static char *join_params(char **params, size_t len) {
    size_t size = 1;
    for (size_t i = 0; i < len; i++) size += strlen(params[i]) + 1;

    char *out = malloc(size);
    if (!out) return NULL;

    char *p = out;
    for (size_t i = 0; i < len; i++) {
        if (i) *p++ = ' ';
        size_t n = strlen(params[i]);
        memcpy(p, params[i], n);
        p += n;
    }
    *p = '\0';
    return out;
}

static bool bind_directive(struct config *config, const struct scfg_directive *directive,
        const char *path) {
    if (directive->params_len < 2) {
        fprintf(stderr, "config: %s:%d: bind needs a chord and an action\n", path, directive->lineno);
        return false;
    }

    uint32_t keysym, modifiers;
    if (!chord_parse(directive->params[0], &keysym, &modifiers)) {
        fprintf(stderr, "config: %s:%d: bad chord '%s'\n", path, directive->lineno,
                directive->params[0]);
        return false;
    }

    const char *name = directive->params[1];

    if (strcmp(name, "none") == 0) {
        if (directive->params_len != 2) {
            fprintf(stderr, "config: %s:%d: none takes no arguments\n", path, directive->lineno);
            return false;
        }
        config_unset(config, keysym, modifiers);
        return true;
    }

    const struct action_spec *spec = action_from_name(name);
    if (!spec) {
        fprintf(stderr, "config: %s:%d: unknown action '%s'\n", path, directive->lineno, name);
        return false;
    }

    union satori_arg arg = {0};
    char *joined = NULL;

    switch (spec->arg_kind) {
    case SATORI_ARG_NONE:
        if (directive->params_len != 2) {
            fprintf(stderr, "config: %s:%d: %s takes no arguments\n", path,
                    directive->lineno, name);
            return false;
        }
        break;
    case SATORI_ARG_CMD:
        if (directive->params_len < 3) {
            fprintf(stderr, "config: %s:%d: %s needs a command\n", path, directive->lineno, name);
            return false;
        }
        joined = join_params(directive->params + 2, directive->params_len - 2);
        if (!joined) return false;
        arg.cmd = joined;
        break;
    case SATORI_ARG_LETTER:
        if (directive->params_len != 3 || strlen(directive->params[2]) != 1) {
            fprintf(stderr, "config: %s:%d: %s needs a single letter\n", path,
                    directive->lineno, name);
            return false;
        }
        arg.u = (unsigned char) directive->params[2][0];
        break;
    }

    bool ok = config_set(config, keysym, modifiers, spec, arg);
    free(joined);       // config_set copied it
    if (!ok) {
        fprintf(stderr, "config: %s:%d: could not store the binding\n", path, directive->lineno);
    }
    return ok;
}

// Resolved before the generated letter block goes in, so that an explicit bind
// line on a letter chord lands on top of the generated one rather than under it.
static bool app_keys_directive(const struct scfg_directive *directive, const char *path,
        uint32_t *modifiers, bool *enabled) {
    if (directive->params_len != 1) {
        fprintf(stderr, "config: %s:%d: app-keys takes one argument\n", path, directive->lineno);
        return false;
    }
    if (strcasecmp(directive->params[0], "none") == 0) {
        *enabled = false;
        return true;
    }
    if (!modifiers_parse(directive->params[0], modifiers)) {
        fprintf(stderr, "config: %s:%d: bad modifiers '%s'\n", path, directive->lineno,
                directive->params[0]);
        return false;
    }
    *enabled = true;
    return true;
}

// with_defaults=false parses the file on its own, with no built-ins underneath
// it. Satori always wants true; false is how a config file can be checked
// against what it claims to produce, which merged output cannot show -- a line
// missing from a file that merges is indistinguishable from a line present.
//
// A missing file is not an error -- it means the built-in table, which is a
// working session. A file that exists and does not parse IS an error: silently
// running defaults would look identical to a config that had been applied.
//
// Errors do not stop the walk. The whole file is checked so a typo on line 3
// and a typo on line 40 are reported together, and the caller throws the
// half-built table away either way.
struct config *config_load(const char *path, bool with_defaults) {
    struct config *config = calloc(1, sizeof *config);
    if (!config) return NULL;

    struct scfg_block block = {0};
    bool have_file = path && access(path, R_OK) == 0;

    if (have_file && scfg_load_file(&block, path) != 0) {
        fprintf(stderr, "config: %s: could not parse\n", path);
        config_destroy(config);
        return NULL;
    }

    bool ok = true;
    uint32_t app_modifiers = SATORI_APP_KEYS_MODIFIERS;
    bool app_keys = true;

    for (size_t i = 0; i < block.directives_len; i++) {
        const struct scfg_directive *directive = &block.directives[i];
        if (strcmp(directive->name, "app-keys") != 0) continue;
        if (!app_keys_directive(directive, path, &app_modifiers, &app_keys)) ok = false;
    }

    // The built-ins go in first: a config file overrides individual chords, it
    // does not replace the table. An unlisted default stays bound, and `bind
    // <chord> none` is how one is taken away.
    if (with_defaults && !config_apply_defaults(config)) ok = false;
    if (app_keys && !config_add_app_keys(config, app_modifiers)) ok = false;

    for (size_t i = 0; i < block.directives_len; i++) {
        const struct scfg_directive *directive = &block.directives[i];
        if (strcmp(directive->name, "bind") == 0) {
            if (!bind_directive(config, directive, path)) ok = false;
        } else if (strcmp(directive->name, "app-keys") != 0) {
            fprintf(stderr, "config: %s:%d: unknown directive '%s'\n", path,
                    directive->lineno, directive->name);
            ok = false;
        }
    }

    if (have_file) scfg_block_finish(&block);

    if (!ok) {
        config_destroy(config);
        return NULL;
    }
    return config;
}

void config_destroy(struct config *config) {
    if (!config) return;

    for (size_t i = 0; i < config->len; i++) keybind_finish(&config->binds[i]);
    free(config->binds);
    free(config);
}

// $XDG_CONFIG_HOME/satori/config, or ~/.config/satori/config.
char *config_default_path(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    const char *base, *tail;

    if (xdg && *xdg) {
        base = xdg;
        tail = "/satori/config";
    } else if (home && *home) {
        base = home;
        tail = "/.config/satori/config";
    } else {
        return NULL;
    }

    size_t size = strlen(base) + strlen(tail) + 1;
    char *path = malloc(size);
    if (path) snprintf(path, size, "%s%s", base, tail);
    return path;
}
