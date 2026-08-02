// Outputs: one per monitor. Satori currently pins everything to the first one.

#include <stdio.h>
#include <stdlib.h>

#include "satori.h"

static void output_dimensions(void *data, struct river_output_v1 *handle, int32_t width, int32_t height) {
    (void) handle;

    struct output *out = data;
    out->width = width;
    out->height = height;
    fprintf(stderr, "output: %dx%d\n", width, height);
}
static void output_position(void *data, struct river_output_v1 *handle, int32_t x, int32_t y) {
    (void) handle;

    struct output *out = data;
    out->x = x;
    out->y = y;
    fprintf(stderr, "output: x:%d,y:%d\n", x, y);
}
static void output_wl_output(void *data, struct river_output_v1 *handle, uint32_t name) {
    (void)data; (void)handle; (void)name;
}
static void output_removed(void *data, struct river_output_v1 *handle) {
    (void) handle;

    struct output *out = data;
    struct satori *satori = out->satori;

    // Windows first: they hold pointers into this struct, and the manage_start
    // that follows this event will size them against whatever output is left.
    windows_forget_output(satori, out);

    // Unlink before freeing: find the pointer slot that holds out.
    struct output **pp = &satori->outputs;
    while (*pp != out) {
        pp = &(*pp)->next;
    }
    *pp = out->next;

    river_output_v1_destroy(out->handle);
    free(out);
    fprintf(stderr, "output: removed\n");
}
static const struct river_output_v1_listener output_listener = {
    .dimensions = output_dimensions,
    .position   = output_position,
    .wl_output  = output_wl_output,
    .removed    = output_removed,
};

void output_create(struct satori *satori, struct river_output_v1 *handle) {
    struct output *out = calloc(1, sizeof *out);
    if (!out) {
        fprintf(stderr, "output_create: calloc failed\n");
        return;
    }
    out->handle = handle;
    out->satori = satori;

    out->next = satori->outputs;
    satori->outputs = out;

    river_output_v1_add_listener(handle, &output_listener, out);
    fprintf(stderr, "wm: output\n");
}

struct output *output_from_handle(struct satori *satori, struct river_output_v1 *handle) {
    for (struct output *out = satori->outputs; out; out = out->next) {
        if (out->handle == handle) return out;
    }
    return NULL;
}

void outputs_destroy_all(struct satori *satori) {
    struct output *out = satori->outputs;
    while (out) {
        struct output *next = out->next;     // save before freeing
        river_output_v1_destroy(out->handle);
        free(out);
        out = next;
    }
    satori->outputs = NULL;
}
