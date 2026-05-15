// SPDX-License-Identifier: GPL-2.0-only
#ifndef SINGULARITY_BLUR_H
#define SINGULARITY_BLUR_H

struct output;
struct wlr_buffer;

/*
 * Apply background blur post-processing to the output buffer for all
 * surfaces that have registered a blur request via the
 * zsingularity_blur_manager_v1 protocol.
 *
 * Must be called after wlr_scene_output_build_state() but before
 * wlr_output_commit_state().  Only active when the environment variable
 * SINGULARITY_BLUR_ENABLED is set.
 */
void singularity_blur_render(struct output *output, struct wlr_buffer *output_buffer);

#endif /* SINGULARITY_BLUR_H */
