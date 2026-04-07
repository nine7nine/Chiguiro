/* kgx-edge.h
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

#define KGX_TYPE_EDGE (kgx_edge_get_type ())
G_DECLARE_FINAL_TYPE (KgxEdge, kgx_edge, KGX, EDGE, GtkWidget)

GtkWidget *kgx_edge_new             (void);
void       kgx_edge_fire_overscroll (KgxEdge         *self,
                                     GtkPositionType  edge);
void       kgx_edge_set_ambient    (KgxEdge         *self,
                                     gboolean         ambient);
void       kgx_edge_stop_ambient_immediate (KgxEdge *self);
void       kgx_edge_reset_redraw_governor (KgxEdge *self);
void       kgx_edge_stop_process_particle_immediate (KgxEdge *self);
void       kgx_edge_set_process_particle (KgxEdge          *self,
                                          KgxParticlePreset  preset,
                                          const GdkRGBA     *color,
                                          int                reverse,
                                          int                shape_override,
                                          int                gap_override,
                                          int                speed_override,
                                          int                thk_override);

G_END_DECLS
