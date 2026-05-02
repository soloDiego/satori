#include <stdio.h>
#include <wayland-client.h>

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);

    if (!display) {
        fprintf(stderr, "could not connect to wayland display "
                "(is WAYLAND_DISPLAY set and a compositor running?)\n");
        return 1;
    }
    
    wl_display_disconnect(display);
    return 0;
}
