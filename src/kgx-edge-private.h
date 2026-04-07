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

  double          overscroll_progress;
  GtkPositionType overscroll_edge;
  gint64          overscroll_start_us;
  double          overscroll_duration_s;

  gboolean        ambient_enabled;
  gboolean        particle_throttle_enabled;
  int             particle_hz;

  guint           redraw_tick_id;
  guint           timeline_timeout_id;
  gint64          timeline_timeout_target_us;
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

  gint64          firework_due_us;
  double          burst_progress[MAX_BURSTS];
  double          burst_head[MAX_BURSTS];
  GdkRGBA         burst_color[MAX_BURSTS];
  gint64          burst_due_us[MAX_BURSTS];
  gint64          burst_start_us[MAX_BURSTS];
  double          burst_duration_s[MAX_BURSTS];

  gint64          ambient_due_us;
  double          ambient_progress[MAX_BURSTS];
  double          ambient_head[MAX_BURSTS];
  GdkRGBA         ambient_burst_color[MAX_BURSTS];
  gint64          ambient_burst_due_us[MAX_BURSTS];
  gint64          ambient_burst_start_us[MAX_BURSTS];
  double          ambient_burst_duration_s[MAX_BURSTS];
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
gboolean                   kgx_edge_advance_process_timeline
                                                     (KgxEdge           *self,
                                                      gint64             now_us);

void                       kgx_edge_init_bursts      (KgxEdge           *self);
void                       kgx_edge_clear_bursts     (KgxEdge           *self);
void                       kgx_edge_start_process_bursts (KgxEdge        *self);
void                       kgx_edge_resume_bursts    (KgxEdge           *self);
gboolean                   kgx_edge_advance_bursts   (KgxEdge           *self,
                                                      gint64             now_us);
gboolean                   kgx_edge_has_scheduled_bursts
                                                     (KgxEdge           *self);
gint64                     kgx_edge_get_next_burst_wakeup_us
                                                     (KgxEdge           *self);

static inline double
kgx_edge_timeline_progress (gint64 now_us,
                            gint64 start_us,
                            double duration_s)
{
  if (start_us <= 0 || duration_s <= 0.0)
    return 1.0;

  return CLAMP ((now_us - start_us) / (duration_s * G_USEC_PER_SEC), 0.0, 1.0);
}

static inline double
kgx_edge_timeline_ease_out_cubic (double progress)
{
  double inv = 1.0 - CLAMP (progress, 0.0, 1.0);

  return 1.0 - inv * inv * inv;
}
