#include <stdio.h>
#include <stdint.h>
#include <wayland-client.h>

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
    fprintf(stderr, "global: name:%u, interface:%s, version%u\n",
            name, interface, version);

    (void)data;
    (void)registry;
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
    fprintf(stderr, "global remove: name:%u", name);

    (void)data;
    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);

    if (!display) {
        fprintf(stderr, "could not connect to wayland display "
                "(is WAYLAND_DISPLAY set and a compositor running?)\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}
