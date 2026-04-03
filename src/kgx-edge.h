/* kgx-edge.h
 *
 * Copyright 2026 Chigüiro contributors
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

G_BEGIN_DECLS

typedef enum {
  KGX_PARTICLE_NONE = 0,
  KGX_PARTICLE_FIREWORKS,
  KGX_PARTICLE_CORNERS,
  KGX_PARTICLE_PULSE_OUT,
  KGX_PARTICLE_ROTATE,
  KGX_PARTICLE_PING_PONG,
} KgxParticlePreset;

#define KGX_TYPE_EDGE (kgx_edge_get_type ())
G_DECLARE_FINAL_TYPE (KgxEdge, kgx_edge, KGX, EDGE, GtkWidget)

GtkWidget *kgx_edge_new             (void);
void       kgx_edge_fire_overscroll (KgxEdge         *self,
                                     GtkPositionType  edge);
void       kgx_edge_set_privileged  (KgxEdge         *self,
                                     gboolean         privileged);
void       kgx_edge_set_ambient    (KgxEdge         *self,
                                     gboolean         ambient);
void       kgx_edge_set_process_particle (KgxEdge          *self,
                                          KgxParticlePreset  preset,
                                          const GdkRGBA     *color,
                                          gboolean           reverse);

const char *kgx_particle_preset_to_string (KgxParticlePreset p);

/* Parse extended process config: "glasscolor;preset;reverse;particlecolor"
 * Backward-compatible: plain "#hex" returns just the glass color. */
void       kgx_parse_process_config     (const char        *value,
                                          char             **glass_color,
                                          KgxParticlePreset *preset,
                                          gboolean          *reverse,
                                          GdkRGBA           *particle_color);

G_END_DECLS
