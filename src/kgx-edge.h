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
  KGX_PARTICLE_AMBIENT,
} KgxParticlePreset;

#define N_PRESETS 6   /* FIREWORKS .. PING_PONG + AMBIENT */

typedef enum {
  KGX_RELEASE_UNIFORM = 0,  /* alpha fades, tail stays full length  */
  KGX_RELEASE_RETRACT,      /* tail shrinks back toward head        */
} KgxReleaseMode;

typedef enum {
  KGX_PARTICLE_SHAPE_SQUARE = 0,
  KGX_PARTICLE_SHAPE_CIRCLE,
  KGX_PARTICLE_SHAPE_DIAMOND,
  KGX_PARTICLE_SHAPE_TRIANGLE,
} KgxParticleShape;

/* Tunable field indices — keep in sync with KgxParticleTunables layout */
enum {
  TUNE_SPEED = 0,
  TUNE_THICKNESS,
  TUNE_TAIL_LENGTH,
  TUNE_PULSE_DEPTH,
  TUNE_PULSE_SPEED,
  TUNE_ENV_ATTACK,
  TUNE_ENV_RELEASE,
  TUNE_RELEASE_MODE,
  TUNE_SHAPE,
  TUNE_ENV_CURVE,
  TUNE_THK_ATTACK,
  TUNE_THK_RELEASE,
  TUNE_THK_CURVE,
  N_TUNE_FIELDS
};

typedef struct {
  double  speed;         /* animation speed multiplier   (0.1 .. 3.0) */
  int     thickness;     /* block size in pixels          (2 .. 20)   */
  double  tail_length;   /* tail length multiplier        (0.1 .. 3.0) */
  double  pulse_depth;   /* shimmer intensity             (0.0 .. 1.0) */
  double  pulse_speed;   /* shimmer wave speed            (0.1 .. 5.0) */
  double  env_attack;    /* fraction of lifetime for grow-in  (0.0 .. 0.5) */
  double  env_release;   /* fraction of lifetime for fade-out (0.0 .. 0.5) */
  int     release_mode;  /* KgxReleaseMode                             */
  int     shape;         /* KgxParticleShape                           */
  int     env_curve;     /* envelope curve: 1=concave 2=linear 3=convex */
  double  thk_attack;    /* thickness envelope attack     (0.0 .. 0.5) */
  double  thk_release;   /* thickness envelope release    (0.0 .. 0.5) */
  int     thk_curve;     /* thickness envelope curve: 1-3              */
} KgxParticleTunables;

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
