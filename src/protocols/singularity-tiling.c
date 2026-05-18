#include <assert.h>
#include <stdlib.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include "singularity-tiling-unstable-v1-protocol.h"
#include "labwc.h"
#include "view.h"
#include "snap.h" // Needed for enum lab_edge

struct singularity_tiling_manager {
    struct wl_global *global;
};

static void manager_handle_set_geometry(struct wl_client *client, struct wl_resource *resource, struct wl_resource *toplevel_resource, int32_t x, int32_t y, int32_t width, int32_t height) {
    struct wlr_foreign_toplevel_handle_v1 *toplevel = wl_resource_get_user_data(toplevel_resource);
    struct view *view = toplevel ? toplevel->data : NULL;

    if (!view) {
        return;
    }

    struct wlr_box geo = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };

    view_move_resize(view, geo);
}

static void manager_handle_set_tiled(struct wl_client *client, struct wl_resource *resource, struct wl_resource *toplevel_resource, uint32_t tiled) {
    struct wlr_foreign_toplevel_handle_v1 *toplevel = wl_resource_get_user_data(toplevel_resource);
    struct view *view = toplevel ? toplevel->data : NULL;

    if (!view) {
        return;
    }

    if (tiled) {
        view->tiled = LAB_EDGE_TOP | LAB_EDGE_BOTTOM | LAB_EDGE_LEFT | LAB_EDGE_RIGHT;
        view_set_decorations(view, LAB_SSD_MODE_NONE, true);
    } else {
        view->tiled = LAB_EDGE_NONE;
        view_set_decorations(view, LAB_SSD_MODE_FULL, true);
    }
}

#include <wlr/util/log.h> // Add for logging

static void manager_handle_snap_view(struct wl_client *client, struct wl_resource *resource, struct wl_resource *toplevel_resource, uint32_t direction) {
    struct wlr_foreign_toplevel_handle_v1 *toplevel = wl_resource_get_user_data(toplevel_resource);
    struct view *view = toplevel ? toplevel->data : NULL;

    wlr_log(WLR_INFO, "[TILING-PROTO] Snap Request received. Direction: %u, View: %p", direction, view);

    if (!view) {
        wlr_log(WLR_ERROR, "[TILING-PROTO] View is NULL!");
        return;
    }

    enum lab_edge edge = LAB_EDGE_NONE;
    switch (direction) {
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_LEFT: edge = LAB_EDGE_LEFT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_RIGHT: edge = LAB_EDGE_RIGHT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP: edge = LAB_EDGE_TOP; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM: edge = LAB_EDGE_BOTTOM; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP_LEFT: edge = LAB_EDGE_TOP | LAB_EDGE_LEFT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_TOP_RIGHT: edge = LAB_EDGE_TOP | LAB_EDGE_RIGHT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM_LEFT: edge = LAB_EDGE_BOTTOM | LAB_EDGE_LEFT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_BOTTOM_RIGHT: edge = LAB_EDGE_BOTTOM | LAB_EDGE_RIGHT; break;
        case ZSINGULARITY_TILING_MANAGER_V1_SNAP_DIRECTION_MAXIMIZE: 
            wlr_log(WLR_INFO, "[TILING-PROTO] Maximizing view");
            view_maximize(view, VIEW_AXIS_BOTH);
            return;
    }

    if (edge != LAB_EDGE_NONE) {
        wlr_log(WLR_INFO, "[TILING-PROTO] Snapping to edge: %d", edge);
        view_snap_to_edge(view, edge, false, false, true);
    } else {
        wlr_log(WLR_INFO, "[TILING-PROTO] No valid edge found for direction %u", direction);
    }
}

static const struct zsingularity_tiling_manager_v1_interface manager_impl = {
    .set_geometry = manager_handle_set_geometry,
    .set_tiled = manager_handle_set_tiled,
    .snap_view = manager_handle_snap_view,
};

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct singularity_tiling_manager *manager = data;
    struct wl_resource *resource = wl_resource_create(client, &zsingularity_tiling_manager_v1_interface, version, id);
    wl_resource_set_implementation(resource, &manager_impl, manager, NULL);
}

void singularity_tiling_init(void) {
    struct singularity_tiling_manager *manager = calloc(1, sizeof(*manager));
    manager->global = wl_global_create(server.wl_display, &zsingularity_tiling_manager_v1_interface, 1, manager, bind_manager);
}
