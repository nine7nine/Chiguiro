/* kgx-edge-private.h
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

#include <adwaita.h>

#include "kgx-edge.h"

#define BASE_OVERSCROLL_MS       600
#define BASE_OVERSCROLL_SEG      200.0
#define EDGE_GOVERNOR_WARN_US    250000
#define EDGE_GOVERNOR_STRESS_US  1000000
#define EDGE_BLOCK_BUDGET_NORMAL 320
#define EDGE_BLOCK_BUDGET_WARN   224
#define EDGE_BLOCK_BUDGET_STRESS 160

#define MAX_BURSTS 8
#define firework_active(s) ((s)->process_preset == KGX_PARTICLE_FIREWORKS || \
                            (s)->process_preset == KGX_PARTICLE_AMBIENT)

typedef struct
{
  int index;
  KgxEdge *self;
} BurstData;

struct _KgxEdge {
  GtkWidget       parent_instance;

  KgxEdge        *master;
  GtkPositionType side;

  gboolean        overscroll_enabled;
  char           *overscroll_color;
  double          burst_spread;
  int             burst_count;
  int             overscroll_style;
  int             overscroll_reverse;
  gboolean        overscroll_reverse_toggle;

  KgxParticleTunables global;
  KgxParticleTunables preset[N_PRESETS];

  AdwAnimation   *overscroll_anim;
  double          overscroll_progress;
  GtkPositionType overscroll_edge;

  gboolean        ambient_enabled;
  gboolean        particle_throttle_enabled;
  int             particle_hz;

  guint           redraw_tick_id;
  gboolean        redraw_pending;
  gint64          last_redraw_request_us;
  gint64          last_snapshot_end_us;
  double          snapshot_cost_ewma_us;
  double          frame_snapshot_cost_us;
  double          redraw_frame_credit;
  gint64          overload_us;
  gint64          recovery_us;
  int             governor_level;
  int             frame_draw_budget_remaining;

  gboolean        ambient_active;

  guint           firework_timeout;
  AdwAnimation   *burst_anim[MAX_BURSTS];
  double          burst_progress[MAX_BURSTS];
  double          burst_head[MAX_BURSTS];
  GdkRGBA         burst_color[MAX_BURSTS];
  guint           burst_timeout[MAX_BURSTS];
  BurstData       burst_data[MAX_BURSTS];

  guint           ambient_timeout;
  AdwAnimation   *ambient_anim[MAX_BURSTS];
  double          ambient_progress[MAX_BURSTS];
  double          ambient_head[MAX_BURSTS];
  GdkRGBA         ambient_burst_color[MAX_BURSTS];
  guint           ambient_burst_timeout[MAX_BURSTS];
  BurstData       ambient_burst_data[MAX_BURSTS];
  int             ambient_burst_count;
  double          ambient_burst_spread;

  KgxParticlePreset process_preset;
  GdkRGBA           process_color;
  gboolean          process_reverse;
  int               process_shape_override;
  int               process_gap_override;
  int               process_speed_override;
  int               process_thk_override;
  gboolean          process_reverse_toggle;
  int               process_reverse_mode;
  AdwAnimation     *process_anim;
  double            process_progress;
  double            process_last_snapshot_progress;
  gint64            process_start_us;
  double            process_duration_s;
  gboolean          process_linear;

  gboolean          pending_change;
  KgxParticlePreset pending_preset;
  GdkRGBA           pending_color;
  int               pending_reverse;
  int               pending_shape_override;
  int               pending_gap_override;
  int               pending_speed_override;
  int               pending_thk_override;

  KgxParticleTunables process_tune_snap;
  KgxParticleTunables burst_tune_snap[MAX_BURSTS];
  KgxParticleTunables ambient_tune_snap[MAX_BURSTS];

  GskPath          *unit_triangle;
  GskPath          *unit_diamond;

  GdkRGBA           overscroll_rgba;
  gboolean          overscroll_rgba_valid;
};

const KgxParticleTunables *kgx_edge_resolve_tunables (KgxEdge           *self,
                                                      KgxParticlePreset  preset);
KgxEdge                   *kgx_edge_get_root         (KgxEdge           *self);
void                       kgx_edge_get_canvas_size  (GtkWidget         *widget,
                                                      int               *width,
                                                      int               *height);
int                        kgx_edge_get_strip_extent (KgxEdge           *self);
void                       kgx_edge_queue_resize_views_if_needed
                                                     (KgxEdge           *self,
                                                      int                old_extent);
void                       kgx_edge_mark_dirty       (KgxEdge           *self);

void                       kgx_edge_init_bursts      (KgxEdge           *self);
void                       kgx_edge_clear_bursts     (KgxEdge           *self);
void                       kgx_edge_start_process_bursts (KgxEdge        *self);
void                       kgx_edge_resume_bursts    (KgxEdge           *self);
