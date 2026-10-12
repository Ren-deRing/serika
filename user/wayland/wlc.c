/* a compositor on wlroots, after tinywl: the drm backend, opengl es (llvmpipe) for rendering, outputs, xdg toplevels
 * that clients put windows in, a keyboard and a pointer from libinput. a click gives a window the focus, a window
 * can ask to be moved and resized, Alt+Esc ends it. usage: wlc [seconds to run, 0 for ever]
 * WLC_ANIMATE=1 makes the background change in every frame, to see the frame loop run */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/input-event-codes.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

enum cursor_mode {
    CURSOR_PASSTHROUGH,
    CURSOR_MOVE,
    CURSOR_RESIZE,
};

struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wlr_scene_rect *background;
    struct wlr_xdg_shell *xdg_shell;

    struct wl_list toplevels;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wlr_seat *seat;
    struct wl_list keyboards;
    enum cursor_mode cursor_mode;
    struct toplevel *grabbed_toplevel;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wl_listener new_output;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;

    bool animate;
    int placed;
    unsigned frames;
    unsigned long hue;
};

struct output {
    struct server *server;
    struct wlr_output *wlr_output;
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;
};

struct toplevel {
    struct wl_list link;
    struct server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* ---- output ---- */

static void hsv(unsigned long step, float out[4]) {
    float h = (step % 360) / 60.0f, f = h - floorf(h), q = 1 - f;
    float r = 0, g = 0, b = 0;

    switch ((int)h) {
        case 0: r = 1; g = f; break;
        case 1: r = q; g = 1; break;
        case 2: g = 1; b = f; break;
        case 3: g = q; b = 1; break;
        case 4: r = f; b = 1; break;
        default: r = 1; b = q; break;
    }

    out[0] = r * 0.4f + 0.05f;
    out[1] = g * 0.4f + 0.05f;
    out[2] = b * 0.4f + 0.05f;
    out[3] = 1;
}

static void output_frame(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, frame);
    struct server *server = output->server;
    (void)data;

    if (server->animate) {
        float color[4];

        hsv(server->hue += 2, color);
        wlr_scene_rect_set_color(server->background, color);
    }

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(server->scene, output->wlr_output);

    if (wlr_scene_output_commit(scene_output, NULL)) server->frames++;

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);

    if (server->animate) wlr_output_schedule_frame(output->wlr_output);
}

static void output_request_state(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;

    wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct output *output = wl_container_of(listener, output, destroy);
    (void)data;

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_state state;

    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);

    if (mode) wlr_output_state_set_mode(&state, mode);

    bool ok = wlr_output_commit_state(wlr_output, &state);

    wlr_output_state_finish(&state);

    printf("wlc: output %s %s\n", wlr_output->name, ok ? "enabled" : "failed to enable");

    if (!ok) return;

    struct output *output = calloc(1, sizeof(*output));

    output->server = server;
    output->wlr_output = wlr_output;

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    struct wlr_output_layout_output *l_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);

    wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
}

/* ---- windows ---- */

static void focus_toplevel(struct toplevel *toplevel) {
    if (!toplevel) return;

    struct server *server = toplevel->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

    if (prev == surface) return;

    if (prev) {
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev);

        if (prev_toplevel) wlr_xdg_toplevel_set_activated(prev_toplevel, false);
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    wl_list_remove(&toplevel->link);
    wl_list_insert(&server->toplevels, &toplevel->link);
    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);

    if (keyboard)
        wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static void toplevel_map(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, map);
    (void)data;

    int n = toplevel->server->placed++;

    wlr_scene_node_set_position(&toplevel->scene_tree->node, 40 + 40 * (n % 8), 60 + 40 * (n % 8));
    wl_list_insert(&toplevel->server->toplevels, &toplevel->link);
    focus_toplevel(toplevel);

    printf("wlc: window %s\n", toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(untitled)");
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
    (void)data;

    if (toplevel == toplevel->server->grabbed_toplevel) toplevel->server->cursor_mode = CURSOR_PASSTHROUGH;

    wl_list_remove(&toplevel->link);
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, commit);
    (void)data;

    if (toplevel->xdg_toplevel->base->initial_commit) wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
    (void)data;

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    free(toplevel);
}

static void begin_interactive(struct toplevel *toplevel, enum cursor_mode mode, uint32_t edges) {
    struct server *server = toplevel->server;
    struct wlr_surface *focused = server->seat->pointer_state.focused_surface;

    /* only the window under the pointer may be grabbed */
    if (!focused || toplevel->xdg_toplevel->base->surface != wlr_surface_get_root_surface(focused)) return;

    server->grabbed_toplevel = toplevel;
    server->cursor_mode = mode;

    if (mode == CURSOR_MOVE) {
        server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
        server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
    } else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) + ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);

        server->grab_x = server->cursor->x - border_x;
        server->grab_y = server->cursor->y - border_y;

        server->grab_geobox = *geo_box;
        server->grab_geobox.x += toplevel->scene_tree->node.x;
        server->grab_geobox.y += toplevel->scene_tree->node.y;

        server->resize_edges = edges;
    }
}

static void toplevel_request_move(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    (void)data;

    begin_interactive(toplevel, CURSOR_MOVE, 0);
}

static void toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    begin_interactive(toplevel, CURSOR_RESIZE, event->edges);
}

/* a window that asks to be bigger is told what it has: the protocol wants an answer */
static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
    (void)data;

    if (toplevel->xdg_toplevel->base->initialized) wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    (void)data;

    if (toplevel->xdg_toplevel->base->initialized) wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void server_new_toplevel(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct toplevel *toplevel = calloc(1, sizeof(*toplevel));

    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&server->scene->tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void popup_commit(struct wl_listener *listener, void *data) {
    struct popup *popup = wl_container_of(listener, popup, commit);
    (void)data;

    if (popup->xdg_popup->base->initial_commit) wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
    struct popup *popup = wl_container_of(listener, popup, destroy);
    (void)data;

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void server_new_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg_popup = data;
    struct popup *popup = calloc(1, sizeof(*popup));
    (void)listener;

    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);

    if (parent) {
        struct wlr_scene_tree *parent_tree = parent->data;

        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    }

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ---- keyboard ---- */

static bool handle_keybinding(struct server *server, xkb_keysym_t sym) {
    if (sym == XKB_KEY_Escape) {
        wl_display_terminate(server->display);
        return true;
    }

    return false;
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    (void)data;

    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8; /* libinput's codes to xkb's */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) handled = handle_keybinding(server, syms[i]) || handled;
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
    struct keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    (void)data;

    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct server *server, struct wlr_input_device *device) {
    struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);
    struct keyboard *keyboard = calloc(1, sizeof(*keyboard));

    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (!keymap) fprintf(stderr, "wlc: no keymap: is the xkb data there?\n");
    else         wlr_keyboard_set_keymap(wlr_keyboard, keymap);

    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server->seat, wlr_keyboard);
    wl_list_insert(&server->keyboards, &keyboard->link);
}

/* ---- pointer ---- */

static void server_new_pointer(struct server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            printf("wlc: keyboard %s\n", device->name);
            server_new_keyboard(server, device);
            break;

        case WLR_INPUT_DEVICE_POINTER:
            printf("wlc: pointer %s\n", device->name);
            server_new_pointer(server, device);
            break;

        default:
            break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;

    if (!wl_list_empty(&server->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;

    wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused = server->seat->pointer_state.focused_client;

    if (focused == event->seat_client)
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* the window under a point of the layout, and where in its surface */
static struct toplevel *toplevel_at(struct server *server, double lx, double ly, struct wlr_surface **surface,
                                    double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);

    if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);

    if (!scene_surface) return NULL;

    *surface = scene_surface->surface;

    struct wlr_scene_tree *tree = node->parent;

    while (tree && !tree->node.data) tree = tree->node.parent;

    return tree ? tree->node.data : NULL;
}

static void process_cursor_move(struct server *server) {
    struct toplevel *toplevel = server->grabbed_toplevel;

    wlr_scene_node_set_position(&toplevel->scene_tree->node, server->cursor->x - server->grab_x,
                                server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct server *server) {
    struct toplevel *toplevel = server->grabbed_toplevel;
    double border_x = server->cursor->x - server->grab_x;
    double border_y = server->cursor->y - server->grab_y;
    int new_left = server->grab_geobox.x, new_right = server->grab_geobox.x + server->grab_geobox.width;
    int new_top = server->grab_geobox.y, new_bottom = server->grab_geobox.y + server->grab_geobox.height;

    if (server->resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) new_top = new_bottom - 1;
    } else if (server->resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) new_bottom = new_top + 1;
    }

    if (server->resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) new_left = new_right - 1;
    } else if (server->resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) new_right = new_left + 1;
    }

    struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geo_box->x, new_top - geo_box->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
}

static void process_cursor_motion(struct server *server, uint32_t time) {
    if (server->cursor_mode == CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    } else if (server->cursor_mode == CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    if (!toplevel) wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;

    wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        server->cursor_mode = CURSOR_PASSTHROUGH;
        return;
    }

    double sx, sy;
    struct wlr_surface *surface = NULL;
    struct toplevel *toplevel = toplevel_at(server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

    focus_toplevel(toplevel);
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta,
                                 event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct server *server = wl_container_of(listener, server, cursor_frame);
    (void)data;

    wlr_seat_pointer_notify_frame(server->seat);
}

/* ---- main ---- */

struct report {
    struct wl_event_source *source;
    struct server *server;
    unsigned last;
    struct timespec t0;
};

/* every two seconds: what has been shown */
static int report_tick(void *data) {
    struct report *r = data;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    if (r->t0.tv_sec) {
        double s = (now.tv_sec - r->t0.tv_sec) + (now.tv_nsec - r->t0.tv_nsec) / 1e9;

        printf("wlc: %u frames, %.1f fps\n", r->server->frames - r->last, (r->server->frames - r->last) / s);
    }

    r->t0 = now;
    r->last = r->server->frames;

    wl_event_source_timer_update(r->source, 2000);
    return 0;
}

static int stop(void *data) {
    wl_display_terminate(data);
    return 0;
}

/* everything down. wlroots wants every listener off its signals before the thing they are on is destroyed */
static void shutdown_server(struct server *server) {
    wl_display_destroy_clients(server->display);

    wl_list_remove(&server->new_output.link);
    wl_list_remove(&server->new_input.link);
    wl_list_remove(&server->new_toplevel.link);
    wl_list_remove(&server->new_popup.link);
    wl_list_remove(&server->cursor_motion.link);
    wl_list_remove(&server->cursor_motion_absolute.link);
    wl_list_remove(&server->cursor_button.link);
    wl_list_remove(&server->cursor_axis.link);
    wl_list_remove(&server->cursor_frame.link);
    wl_list_remove(&server->request_cursor.link);
    wl_list_remove(&server->request_set_selection.link);

    wlr_scene_node_destroy(&server->scene->tree.node);
    wlr_xcursor_manager_destroy(server->cursor_mgr);
    wlr_cursor_destroy(server->cursor);
    wlr_allocator_destroy(server->allocator);
    wlr_renderer_destroy(server->renderer);
    wlr_backend_destroy(server->backend);
    wl_display_destroy(server->display);
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 0;
    struct server server = { 0 };

    server.animate = getenv("WLC_ANIMATE") != NULL;

    wlr_log_init(WLR_DEBUG, NULL);

    server.display = wl_display_create();
    server.loop = wl_display_get_event_loop(server.display);

    server.backend = wlr_backend_autocreate(server.loop, NULL);

    if (!server.backend) {
        fprintf(stderr, "wlc: no backend\n");
        return 1;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);

    if (!server.renderer) {
        fprintf(stderr, "wlc: no renderer\n");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    if (!server.allocator) {
        fprintf(stderr, "wlc: no allocator\n");
        return 1;
    }

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);

    wl_list_init(&server.toplevels);
    wl_list_init(&server.keyboards);

    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    float color[4] = { 0.1f, 0.1f, 0.3f, 1 };

    server.background = wlr_scene_rect_create(&server.scene->tree, 8192, 8192, color);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_toplevel.notify = server_new_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_toplevel);
    server.new_popup.notify = server_new_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_popup);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

    server.cursor_mode = CURSOR_PASSTHROUGH;
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = server_cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = server_cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = server_cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
    server.request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

    const char *socket = wl_display_add_socket_auto(server.display);

    if (!socket) {
        fprintf(stderr, "wlc: no socket\n");
        shutdown_server(&server);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        fprintf(stderr, "wlc: backend did not start\n");
        shutdown_server(&server);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    printf("wlc: running on %s\n", socket);

    wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");

    /* a timer for the numbers, and one for the end */
    struct report report = { .server = &server };

    report.source = wl_event_loop_add_timer(server.loop, report_tick, &report);
    wl_event_source_timer_update(report.source, 2000);

    if (seconds > 0) {
        struct wl_event_source *end = wl_event_loop_add_timer(server.loop, stop, server.display);

        wl_event_source_timer_update(end, seconds * 1000);
    }

    wl_display_run(server.display);

    unsigned frames = server.frames;

    shutdown_server(&server);

    printf("wlc: done, %u frames\n", frames);
    return 0;
}
