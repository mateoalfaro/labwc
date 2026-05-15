// SPDX-License-Identifier: GPL-2.0-only
//
// singularity-blur.c - Wayland background blur protocol for Singularity Desktop
//
// When a layer-shell surface requests blur, the compositor blurs the content
// rendered behind that surface using a multi-pass box blur (downsample +
// upsample with bilinear filtering). This creates a frosted-glass effect.
//
// The blur is applied as a post-processing step in lab_wlr_scene_output_commit(),
// after wlr_scene_output_build_state() but before wlr_output_commit_state().
//
// Enable at runtime by setting SINGULARITY_BLUR_ENABLED=1 in the environment.

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include "common/box.h"
#include "labwc.h"
#include "output.h"
#include "singularity-blur-unstable-v1-protocol.h"

struct singularity_blur_entry {
	struct wl_resource *resource;
	struct wlr_surface *surface;

	/* double-buffered parameters */
	uint32_t pending_radius;
	uint32_t pending_noise;
	uint32_t radius;
	uint32_t noise;

	struct wl_list link;               /* blur_manager.entries */
	struct wl_listener surface_destroy;
};

struct singularity_blur_manager {
	struct wl_global *global;
	struct wl_list entries; /* singularity_blur_entry.link */
};

static struct singularity_blur_manager blur_manager;

static bool
blur_runtime_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		cached = (getenv("SINGULARITY_BLUR_ENABLED") != NULL) ? 1 : 0;
	}
	return cached == 1;
}

/* Callback for wlr_scene_node_for_each_buffer: find layout coords of surface */
struct surface_find_data {
	struct wlr_surface *target;
	int lx, ly;
	bool found;
};

static void
find_surface_cb(struct wlr_scene_buffer *scene_buf, int sx, int sy, void *data)
{
	struct surface_find_data *fd = data;
	if (fd->found) {
		return;
	}
	struct wlr_scene_surface *ss =
		wlr_scene_surface_try_from_buffer(scene_buf);
	if (ss && ss->surface == fd->target) {
		fd->lx = sx;
		fd->ly = sy;
		fd->found = true;
	}
}

/*
 * Apply a single downsample->upsample box blur pass to a rectangular region of
 * the output buffer. The blur radius determines the downsample factor:
 *   radius 1-10,  1/4  scale
 *   radius 11-20, 1/8  scale
 *   radius > 20,  1/12 scale
 *
 * Two passes are performed per call (1/N, back) for a mild frosted-glass look.
 */
static void
apply_blur_to_region(struct wlr_output *wlr_output,
	struct wlr_buffer *output_buf,
	struct wlr_box region, uint32_t radius)
{
	if (region.width <= 0 || region.height <= 0) {
		return;
	}

	/* Clamp to output buffer bounds */
	struct wlr_box output_box = {
		.width = output_buf->width,
		.height = output_buf->height,
	};
	if (!wlr_box_intersection(&region, &region, &output_box)) {
		return;
	}

	/* Pick downsample factor based on radius */
	int factor;
	if (radius <= 10) {
		factor = 4;
	} else if (radius <= 20) {
		factor = 8;
	} else {
		factor = 12;
	}

	int small_w = (region.width + factor - 1) / factor;
	int small_h = (region.height + factor - 1) / factor;
	if (small_w < 1) { small_w = 1; }
	if (small_h < 1) { small_h = 1; }

	/* Allocate a small temporary buffer */
	struct wlr_buffer *tmp = wlr_allocator_create_buffer(
		server.allocator, small_w, small_h,
		&wlr_output->swapchain->format);
	if (!tmp) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to allocate temporary buffer");
		return;
	}

	/*
	 * Pass 1: downsample the region from the output buffer into tmp
	 * (bilinear filtering gives the natural blur)
	 */
	wlr_buffer_lock(output_buf);
	struct wlr_texture *output_tex =
		wlr_texture_from_buffer(server.renderer, output_buf);
	if (!output_tex) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to create output texture");
		wlr_buffer_unlock(output_buf);
		wlr_buffer_drop(tmp);
		return;
	}

	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server.renderer, tmp, NULL);
	if (!pass) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to begin downsample pass");
		wlr_texture_destroy(output_tex);
		wlr_buffer_unlock(output_buf);
		wlr_buffer_drop(tmp);
		return;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = output_tex,
		.src_box = {
			.x = region.x,
			.y = region.y,
			.width = region.width,
			.height = region.height,
		},
		.dst_box = {
			.width = small_w,
			.height = small_h,
		},
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
	if (!wlr_render_pass_submit(pass)) {
		wlr_log(WLR_ERROR, "[BLUR] Downsample pass failed");
		wlr_texture_destroy(output_tex);
		wlr_buffer_unlock(output_buf);
		wlr_buffer_drop(tmp);
		return;
	}
	wlr_texture_destroy(output_tex);

	/*
	 * Pass 2: upsample tmp back into the output buffer at the original region
	 * (bilinear upscaling produces the smooth, blurred look)
	 */
	struct wlr_texture *tmp_tex =
		wlr_texture_from_buffer(server.renderer, tmp);
	if (!tmp_tex) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to create tmp texture");
		wlr_buffer_unlock(output_buf);
		wlr_buffer_drop(tmp);
		return;
	}

	pass = wlr_renderer_begin_buffer_pass(server.renderer, output_buf, NULL);
	if (!pass) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to begin upsample pass");
		wlr_texture_destroy(tmp_tex);
		wlr_buffer_unlock(output_buf);
		wlr_buffer_drop(tmp);
		return;
	}

	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tmp_tex,
		.src_box = {
			.x = 0,
			.y = 0,
			.width = small_w,
			.height = small_h,
		},
		.dst_box = region,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
	wlr_render_pass_submit(pass);

	wlr_texture_destroy(tmp_tex);
	wlr_buffer_unlock(output_buf);
	wlr_buffer_drop(tmp);
}

void
singularity_blur_render(struct output *output, struct wlr_buffer *output_buffer)
{
	if (!blur_runtime_enabled()) {
		return;
	}

	struct wlr_scene_output *scene_output = output->scene_output;
	struct wlr_output *wlr_output = output->wlr_output;

	struct singularity_blur_entry *entry;
	wl_list_for_each(entry, &blur_manager.entries, link) {
		if (entry->radius == 0) {
			continue;
		}
		if (!entry->surface) {
			continue;
		}

		struct surface_find_data fd = { .target = entry->surface };
		wlr_scene_node_for_each_buffer(
			&server.scene->tree.node,
			find_surface_cb, &fd);
		if (!fd.found) {
			continue;
		}

		/*
		 * Convert scene layout coordinates to output-local physical pixels.
		 * scene_output->x/y is the output's origin in scene layout coords.
		 */
		float scale = wlr_output->scale;
		int local_x = (int)((fd.lx - scene_output->x) * scale);
		int local_y = (int)((fd.ly - scene_output->y) * scale);
		int phys_w = (int)(entry->surface->current.width * scale);
		int phys_h = (int)(entry->surface->current.height * scale);

		if (phys_w <= 0 || phys_h <= 0) {
			continue;
		}

		struct wlr_box region = {
			.x = local_x,
			.y = local_y,
			.width = phys_w,
			.height = phys_h,
		};

		apply_blur_to_region(wlr_output,
			output_buffer, region, entry->radius);
	}
}

static void
handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct singularity_blur_entry *entry =
		wl_container_of(listener, entry, surface_destroy);
	entry->surface = NULL;
	wl_list_remove(&entry->surface_destroy.link);
	wl_list_init(&entry->surface_destroy.link);
}

static void
blur_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
blur_handle_set_radius(struct wl_client *client, struct wl_resource *resource,
	uint32_t radius)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_radius = radius;
	}
}

static void
blur_handle_set_noise(struct wl_client *client, struct wl_resource *resource,
	uint32_t noise)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->pending_noise = noise;
	}
}

static void
blur_handle_commit(struct wl_client *client, struct wl_resource *resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (entry) {
		entry->radius = entry->pending_radius;
		entry->noise = entry->pending_noise;
		wlr_log(WLR_DEBUG, "[BLUR] Surface %p committed blur radius=%u noise=%u",
			(void *)entry->surface, entry->radius, entry->noise);
	}
}

static const struct zsingularity_blur_v1_interface blur_impl = {
	.destroy = blur_handle_destroy,
	.set_radius = blur_handle_set_radius,
	.set_noise = blur_handle_set_noise,
	.commit = blur_handle_commit,
};

static void
blur_resource_destroy(struct wl_resource *resource)
{
	struct singularity_blur_entry *entry = wl_resource_get_user_data(resource);
	if (!entry) {
		return;
	}
	wl_list_remove(&entry->link);
	if (entry->surface) {
		wl_list_remove(&entry->surface_destroy.link);
	}
	free(entry);
}

static void
manager_handle_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
manager_handle_get_blur(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, struct wl_resource *surface_resource)
{
	struct wlr_surface *surface =
		wlr_surface_from_resource(surface_resource);
	if (!surface) {
		wl_resource_post_error(resource,
			ZSINGULARITY_BLUR_MANAGER_V1_ERROR_INVALID_SURFACE,
			"invalid surface");
		return;
	}

	struct singularity_blur_entry *entry =
		calloc(1, sizeof(*entry));
	if (!entry) {
		wl_client_post_no_memory(client);
		return;
	}

	entry->resource = wl_resource_create(client,
		&zsingularity_blur_v1_interface,
		wl_resource_get_version(resource), id);
	if (!entry->resource) {
		free(entry);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(entry->resource, &blur_impl, entry,
		blur_resource_destroy);

	entry->surface = surface;
	entry->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &entry->surface_destroy);

	wl_list_insert(&blur_manager.entries, &entry->link);
	wlr_log(WLR_DEBUG, "[BLUR] New blur entry for surface %p", (void *)surface);
}

static const struct zsingularity_blur_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_blur = manager_handle_get_blur,
};

static void
bind_manager(struct wl_client *client, void *data, uint32_t version,
	uint32_t id)
{
	struct wl_resource *resource = wl_resource_create(client,
		&zsingularity_blur_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

void
singularity_blur_init(void)
{
	wl_list_init(&blur_manager.entries);

	blur_manager.global = wl_global_create(server.wl_display,
		&zsingularity_blur_manager_v1_interface, 1,
		&blur_manager, bind_manager);
	if (!blur_manager.global) {
		wlr_log(WLR_ERROR, "[BLUR] Failed to create blur manager global");
		return;
	}

	wlr_log(WLR_INFO, "[BLUR] Singularity blur protocol initialized%s",
		blur_runtime_enabled() ? " (active)" : " (set SINGULARITY_BLUR_ENABLED=1 to activate)");
}
