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
                            GskPath                   *unit_triangle,
                            GskPath                   *unit_diamond);

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
                               GskPath                   *unit_triangle,
                               GskPath                   *unit_diamond,
                               double                     segment_len);

G_END_DECLS
