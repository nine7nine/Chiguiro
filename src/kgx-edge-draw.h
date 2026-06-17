/* kgx-edge-draw.h
 *
 * Copyright 2026 jordan Johnston
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gtk/gtk.h>

#include "kgx-particle.h"

G_BEGIN_DECLS

/* Render backend (GSK). Masks are baked once per scale factor; emit stamps a
 * single batchable primitive per block (colour rect for squares, tinted
 * mask texture otherwise), replacing per-block path fills/clips. */
void kgx_particle_masks_ensure (KgxParticleMasks *masks, int scale);
void kgx_particle_masks_clear  (KgxParticleMasks *masks);
void kgx_particle_emit         (GtkSnapshot               *snapshot,
                                const KgxParticleInstance *inst,
                                const KgxParticleMasks    *masks);

/* Poxicle comparison sink: capture the emitted instance stream instead of
 * drawing via GSK, so the standalone poxicle renderer can draw the same frames.
 * See kgx-edge-draw.c. */
/* Called when a non-empty frame is promoted, so a parked overlay can restart. */
typedef void (*KgxParticleSinkWake) (gpointer user);

void kgx_particle_sink_set_active (gboolean active);
gboolean kgx_particle_sink_is_active (void);
void kgx_particle_sink_set_origin (float x, float y);
void kgx_particle_sink_set_wake   (KgxParticleSinkWake wake, gpointer user);
void kgx_particle_sink_new_frame  (void);
void kgx_particle_sink_clear      (void);
int  kgx_particle_sink_take       (KgxParticleInstance *out, int cap);

void kgx_edge_draw_segment (GtkSnapshot               *snapshot,
                            double                     head_d,
                            double                     seg_len,
                            float                      alpha,
                            float                      width,
                            float                      height,
                            double                     perim,
                            const GdkRGBA             *color,
                            int                        trail_dir,
                            const KgxParticleTunables *tune,
                            double                     phase,
                            GtkPositionType            side,
                            float                      strip_extent,
                            int                       *budget_remaining,
                            const KgxParticleMasks    *masks);

void kgx_edge_draw_overscroll (GtkSnapshot               *snapshot,
                               double                     progress,
                               GtkPositionType            edge,
                               int                        style,
                               gboolean                   reverse,
                               float                      width,
                               float                      height,
                               double                     perim,
                               const GdkRGBA             *color,
                               const KgxParticleTunables *tune,
                               GtkPositionType            side,
                               float                      strip_extent,
                               int                       *budget_remaining,
                               const KgxParticleMasks    *masks,
                               double                     segment_len);

G_END_DECLS
