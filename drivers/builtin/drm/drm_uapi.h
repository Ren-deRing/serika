#pragma once

#include <stdint.h>

#define DRM_DISPLAY_MODE_LEN 32
#define DRM_PROP_NAME_LEN    32

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    uint64_t name_len;
    uint64_t name; /* user pointer */
    uint64_t date_len;
    uint64_t date; /* user pointer */
    uint64_t desc_len;
    uint64_t desc; /* user pointer */
};

struct drm_unique {
    uint64_t unique_len;
    uint64_t unique; /* user pointer */
};

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

struct drm_prime_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t fd;
};

struct drm_gem_close {
    uint32_t handle;
    uint32_t pad;
};

struct drm_event {
    uint32_t type;
    uint32_t length;
};

struct drm_event_vblank {
    struct drm_event base;
    uint64_t user_data;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t sequence;
    uint32_t crtc_id;
};

struct drm_auth {
    uint32_t magic;
};

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[DRM_DISPLAY_MODE_LEN];
};

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    uint32_t gamma_size;
    uint32_t mode_valid;
    struct drm_mode_modeinfo mode;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint32_t count_format_types;
    uint64_t format_type_ptr;
};

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t modes_ptr;
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_modes;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

struct drm_mode_property_enum {
    uint64_t value;
    char name[DRM_PROP_NAME_LEN];
};

struct drm_mode_get_property {
    uint64_t values_ptr;
    uint64_t enum_blob_ptr;
    uint32_t prop_id;
    uint32_t flags;
    char name[DRM_PROP_NAME_LEN];
    uint32_t count_values;
    uint32_t count_enum_blobs;
};

struct drm_mode_obj_get_properties {
    uint64_t props_ptr;
    uint64_t prop_values_ptr;
    uint32_t count_props;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_obj_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t obj_id;
    uint32_t obj_type;
};

struct drm_mode_connector_set_property {
    uint64_t value;
    uint32_t prop_id;
    uint32_t connector_id;
};

struct drm_mode_get_blob {
    uint32_t blob_id;
    uint32_t length;
    uint64_t data;
};

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t reserved;
    uint64_t user_data;
};

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

struct drm_mode_set_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    int32_t crtc_x;
    int32_t crtc_y;
    uint32_t crtc_w;
    uint32_t crtc_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_h;
    uint32_t src_w;
};

struct drm_mode_cursor {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
};

struct drm_mode_cursor2 {
    uint32_t flags;
    uint32_t crtc_id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t handle;
    int32_t hot_x;
    int32_t hot_y;
};

struct drm_mode_create_blob {
    uint64_t data;
    uint32_t length;
    uint32_t blob_id;
};

struct drm_mode_destroy_blob {
    uint32_t blob_id;
};

_Static_assert(sizeof(struct drm_version) == 64, "drm_version");
_Static_assert(sizeof(struct drm_unique) == 16, "drm_unique");
_Static_assert(sizeof(struct drm_get_cap) == 16, "drm_get_cap");
_Static_assert(sizeof(struct drm_set_client_cap) == 16, "drm_set_client_cap");
_Static_assert(sizeof(struct drm_prime_handle) == 12, "drm_prime_handle");
_Static_assert(sizeof(struct drm_gem_close) == 8, "drm_gem_close");
_Static_assert(sizeof(struct drm_event) == 8, "drm_event");
_Static_assert(sizeof(struct drm_event_vblank) == 32, "drm_event_vblank");
_Static_assert(sizeof(struct drm_auth) == 4, "drm_auth");
_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "drm_mode_modeinfo");
_Static_assert(sizeof(struct drm_mode_card_res) == 64, "drm_mode_card_res");
_Static_assert(sizeof(struct drm_mode_crtc) == 104, "drm_mode_crtc");
_Static_assert(sizeof(struct drm_mode_get_plane) == 32, "drm_mode_get_plane");
_Static_assert(sizeof(struct drm_mode_get_plane_res) == 16, "drm_mode_get_plane_res");
_Static_assert(sizeof(struct drm_mode_get_encoder) == 20, "drm_mode_get_encoder");
_Static_assert(sizeof(struct drm_mode_get_connector) == 80, "drm_mode_get_connector");
_Static_assert(sizeof(struct drm_mode_property_enum) == 40, "drm_mode_property_enum");
_Static_assert(sizeof(struct drm_mode_get_property) == 64, "drm_mode_get_property");
_Static_assert(sizeof(struct drm_mode_obj_get_properties) == 32, "drm_mode_obj_get_properties");
_Static_assert(sizeof(struct drm_mode_obj_set_property) == 24, "drm_mode_obj_set_property");
_Static_assert(sizeof(struct drm_mode_connector_set_property) == 16, "drm_mode_connector_set_property");
_Static_assert(sizeof(struct drm_mode_get_blob) == 16, "drm_mode_get_blob");
_Static_assert(sizeof(struct drm_mode_fb_cmd) == 28, "drm_mode_fb_cmd");
_Static_assert(sizeof(struct drm_mode_fb_cmd2) == 104, "drm_mode_fb_cmd2");
_Static_assert(sizeof(struct drm_mode_fb_dirty_cmd) == 24, "drm_mode_fb_dirty_cmd");
_Static_assert(sizeof(struct drm_mode_crtc_page_flip) == 24, "drm_mode_crtc_page_flip");
_Static_assert(sizeof(struct drm_mode_create_dumb) == 32, "drm_mode_create_dumb");
_Static_assert(sizeof(struct drm_mode_map_dumb) == 16, "drm_mode_map_dumb");
_Static_assert(sizeof(struct drm_mode_destroy_dumb) == 4, "drm_mode_destroy_dumb");
_Static_assert(sizeof(struct drm_mode_set_plane) == 48, "drm_mode_set_plane");
_Static_assert(sizeof(struct drm_mode_cursor) == 28, "drm_mode_cursor");
_Static_assert(sizeof(struct drm_mode_cursor2) == 36, "drm_mode_cursor2");
_Static_assert(sizeof(struct drm_mode_create_blob) == 16, "drm_mode_create_blob");
_Static_assert(sizeof(struct drm_mode_destroy_blob) == 4, "drm_mode_destroy_blob");
