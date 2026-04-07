/* kgx-edge.c
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

#include "kgx-config.h"

#include <adwaita.h>
#include <math.h>

#include "kgx-edge-draw.h"
#include "kgx-edge-private.h"


enum {
  PROP_0,
  PROP_MASTER,
  PROP_SIDE,
  PROP_OVERSCROLL_ENABLED,
  PROP_OVERSCROLL_COLOR,
  PROP_OVERSCROLL_STYLE,
  PROP_OVERSCROLL_REVERSE,
  PROP_BURST_SPREAD,
  PROP_BURST_COUNT,
  PROP_AMBIENT_ENABLED,
  PROP_PARTICLE_THROTTLE_ENABLED,
  PROP_PARTICLE_HZ,
  PROP_AMBIENT_BURST_COUNT,
  PROP_AMBIENT_BURST_SPREAD,
  /* Global tunables (N_TUNE_FIELDS values) */
  PROP_GLOBAL_BASE,
  /* Per-preset tunables (N_PRESETS * N_TUNE_FIELDS values) */
  PROP_PRESET_BASE = PROP_GLOBAL_BASE + N_TUNE_FIELDS,
  LAST_PROP = PROP_PRESET_BASE + N_PRESETS * N_TUNE_FIELDS
};
static GParamSpec *edge_pspecs[LAST_PROP] = { NULL, };

/* Property name tables — keep in sync with TUNE_* indices */
static const char * const tune_names[N_TUNE_FIELDS] = {
  "speed", "thickness", "tail-length", "pulse-depth", "pulse-speed",
  "env-attack", "env-release", "release-mode", "shape",
  "env-curve", "gap", "thk-attack", "thk-release", "thk-release-mode", "thk-curve"
};
static const char * const preset_suffixes[N_PRESETS] = {
  "fireworks", "corners", "pulse-out", "rotate", "ping-pong", "ambient", "scroll2"
};


G_DEFINE_FINAL_TYPE (KgxEdge, kgx_edge, GTK_TYPE_WIDGET)

static void     kgx_edge_queue_resize_views (KgxEdge   *self);
static gboolean kgx_edge_has_visible_content (KgxEdge *self);

/* ── tunables helpers ───────────────────────────────────── */

static inline gboolean
kgx_edge_tunable_field_is_int (int field)
{
  switch (field) {
  case TUNE_THICKNESS:
  case TUNE_RELEASE_MODE:
  case TUNE_SHAPE:
  case TUNE_ENV_CURVE:
  case TUNE_GAP:
  case TUNE_THK_RELEASE_MODE:
  case TUNE_THK_CURVE:
    return TRUE;
  default:
    return FALSE;
  }
}

static int
kgx_edge_clamp_tunable_int (int field,
                            int value)
{
  switch (field) {
  case TUNE_THICKNESS:
    return CLAMP (value, 2, 40);
  case TUNE_RELEASE_MODE:
    return CLAMP (value, 0, 3);
  case TUNE_SHAPE:
    return CLAMP (value, 0, 3);
  case TUNE_ENV_CURVE:
  case TUNE_THK_CURVE:
    return CLAMP (value, 1, 3);
  case TUNE_GAP:
    return CLAMP (value, 0, 1);
  case TUNE_THK_RELEASE_MODE:
    return CLAMP (value, 0, 4);
  default:
    return value;
  }
}

static void
kgx_edge_set_tunable_from_value (KgxParticleTunables *tune,
                                 int                  field,
                                 const GValue        *value)
{
  if (kgx_edge_tunable_field_is_int (field)) {
    kgx_particle_tunable_set_int (tune,
                                  field,
                                  kgx_edge_clamp_tunable_int (field,
                                                              g_value_get_int (value)));
  } else {
    kgx_particle_tunable_set_double (tune, field, g_value_get_double (value));
  }
}

static void
kgx_edge_get_tunable_into_value (const KgxParticleTunables *tune,
                                 int                        field,
                                 GValue                    *value)
{
  if (kgx_edge_tunable_field_is_int (field))
    g_value_set_int (value, kgx_particle_tunable_get_int (tune, field));
  else
    g_value_set_double (value, kgx_particle_tunable_get_double (tune, field));
}

static void
kgx_edge_refresh_global_animation_speed (KgxEdge *state)
{
  if (state->overscroll_anim) {
    adw_timed_animation_set_duration (
      ADW_TIMED_ANIMATION (state->overscroll_anim),
      (guint) (BASE_OVERSCROLL_MS / state->global.speed));
  }

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (state->burst_anim[i]) {
      adw_timed_animation_set_duration (
        ADW_TIMED_ANIMATION (state->burst_anim[i]),
        (guint) (800.0 / state->global.speed));
    }
  }
}

static void
kgx_edge_finish_tunable_change (KgxEdge *state,
                                int      field)
{
  if (field == TUNE_THICKNESS)
    kgx_edge_queue_resize_views (state);

  if (kgx_edge_has_visible_content (state))
    kgx_edge_mark_dirty (state);
}

const KgxParticleTunables *
kgx_edge_resolve_tunables (KgxEdge           *self,
                           KgxParticlePreset  preset)
{
  if (preset >= KGX_PARTICLE_FIREWORKS && preset <= KGX_PARTICLE_SCROLL2)
    return &self->preset[preset - 1];
  return &self->global;
}

KgxEdge *
kgx_edge_get_root (KgxEdge *self)
{
  while (self->master)
    self = self->master;

  return self;
}

static void
kgx_edge_queue_draw_views (KgxEdge *self)
{
  KgxEdge *root = kgx_edge_get_root (self);
  GtkWidget *root_widget = GTK_WIDGET (root);
  GtkWidget *parent = gtk_widget_get_parent (root_widget);

  if (!parent) {
    gtk_widget_queue_draw (root_widget);
    if (root != self)
      gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  for (GtkWidget *child = gtk_widget_get_first_child (parent);
       child;
       child = gtk_widget_get_next_sibling (child)) {
    if (KGX_IS_EDGE (child) &&
        kgx_edge_get_root (KGX_EDGE (child)) == root)
      gtk_widget_queue_draw (child);
  }
}

static void
kgx_edge_queue_resize_views (KgxEdge *self)
{
  KgxEdge *root = kgx_edge_get_root (self);
  GtkWidget *root_widget = GTK_WIDGET (root);
  GtkWidget *parent = gtk_widget_get_parent (root_widget);

  if (!parent) {
    gtk_widget_queue_resize (root_widget);
    if (root != self)
      gtk_widget_queue_resize (GTK_WIDGET (self));
    return;
  }

  for (GtkWidget *child = gtk_widget_get_first_child (parent);
       child;
       child = gtk_widget_get_next_sibling (child)) {
    if (KGX_IS_EDGE (child) &&
        kgx_edge_get_root (KGX_EDGE (child)) == root)
      gtk_widget_queue_resize (child);
  }
}

void
kgx_edge_get_canvas_size (GtkWidget *widget,
                          int       *width,
                          int       *height)
{
  GtkWidget *parent = gtk_widget_get_parent (widget);

  *width = parent ? gtk_widget_get_width (parent) : gtk_widget_get_width (widget);
  *height = parent ? gtk_widget_get_height (parent) : gtk_widget_get_height (widget);

  if (*width <= 0)
    *width = gtk_widget_get_width (widget);
  if (*height <= 0)
    *height = gtk_widget_get_height (widget);
}

int
kgx_edge_get_strip_extent (KgxEdge *self)
{
  KgxEdge *root = kgx_edge_get_root (self);
  int extent = MAX (root->global.thickness, 1);
  gboolean process_segment_active =
    root->process_progress >= 0.0 &&
    root->process_preset != KGX_PARTICLE_NONE &&
    !firework_active (root);
  gboolean pending_process_segment =
    root->pending_change &&
    root->pending_preset != KGX_PARTICLE_NONE &&
    root->pending_preset != KGX_PARTICLE_FIREWORKS &&
    root->pending_preset != KGX_PARTICLE_AMBIENT;

  for (int i = 0; i < N_PRESETS; i++)
    extent = MAX (extent, root->preset[i].thickness);

  if (process_segment_active) {
    extent = MAX (extent, root->process_tune_snap.thickness);
    extent = MAX (extent, root->process_thk_override);
  }

  if (pending_process_segment)
    extent = MAX (extent, root->pending_thk_override);

  return MAX (extent + 2, 4);
}

void
kgx_edge_queue_resize_views_if_needed (KgxEdge *self,
                                       int      old_extent)
{
  self = kgx_edge_get_root (self);

  if (kgx_edge_get_strip_extent (self) != old_extent)
    kgx_edge_queue_resize_views (self);
}

static gboolean
kgx_edge_has_visible_content (KgxEdge *self)
{
  self = kgx_edge_get_root (self);

  if (self->overscroll_progress >= 0.0 || self->process_progress >= 0.0)
    return TRUE;

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_progress[i] >= 0.0 || self->ambient_progress[i] >= 0.0)
      return TRUE;
  }

  return FALSE;
}

static inline void
kgx_edge_reset_governor (KgxEdge *self)
{
  self->redraw_pending = FALSE;
  self->last_redraw_request_us = 0;
  self->last_snapshot_end_us = 0;
  self->snapshot_cost_ewma_us = 0.0;
  self->frame_snapshot_cost_us = 0.0;
  self->redraw_frame_credit = 0.0;
  self->overload_us = 0;
  self->recovery_us = 0;
  self->governor_level = 0;
  self->frame_draw_budget_remaining = 0;
}

static inline int
kgx_edge_get_effective_hz (KgxEdge *self)
{
  int hz = CLAMP (self->particle_hz, 10, 60);

  switch (self->governor_level) {
  case 1:
    hz = MAX (12, (hz * 3 + 3) / 4);
    break;
  case 2:
    hz = MAX (12, (hz + 1) / 2);
    break;
  default:
    break;
  }

  return hz;
}

static double
kgx_edge_get_refresh_hz (GdkFrameClock *frame_clock,
                         gint64         now_us)
{
  gint64 refresh_interval_us = 0;
  double fps;

  gdk_frame_clock_get_refresh_info (frame_clock,
                                    now_us,
                                    &refresh_interval_us,
                                    NULL);

  if (refresh_interval_us > 0)
    return CLAMP ((double) G_USEC_PER_SEC / (double) refresh_interval_us, 24.0, 240.0);

  fps = gdk_frame_clock_get_fps (frame_clock);
  if (fps > 0.0)
    return CLAMP (fps, 24.0, 240.0);

  return 60.0;
}

static inline int
kgx_edge_get_block_budget (KgxEdge *self)
{
  switch (self->governor_level) {
  case 1:
    return EDGE_BLOCK_BUDGET_WARN;
  case 2:
    return EDGE_BLOCK_BUDGET_STRESS;
  default:
    return EDGE_BLOCK_BUDGET_NORMAL;
  }
}

static void
kgx_edge_note_snapshot (KgxEdge *self,
                        double   snapshot_cost_us,
                        gint64   sample_end_us)
{
  gint64 nominal_interval_us = G_USEC_PER_SEC / CLAMP (self->particle_hz, 10, 60);
  gint64 sample_interval_us = nominal_interval_us;
  gboolean overloaded;

  if (self->last_snapshot_end_us > 0)
    sample_interval_us = MAX (sample_end_us - self->last_snapshot_end_us, 0);
  self->last_snapshot_end_us = sample_end_us;

  if (self->snapshot_cost_ewma_us <= 0.0)
    self->snapshot_cost_ewma_us = snapshot_cost_us;
  else
    self->snapshot_cost_ewma_us = self->snapshot_cost_ewma_us * 0.8 + snapshot_cost_us * 0.2;

  overloaded = (sample_interval_us > (gint64) (nominal_interval_us * 1.35)) ||
               (self->snapshot_cost_ewma_us > nominal_interval_us * 0.60);

  if (overloaded) {
    self->recovery_us = 0;
    self->overload_us = MIN (self->overload_us + sample_interval_us,
                             EDGE_GOVERNOR_STRESS_US * 2);
    if (self->overload_us >= EDGE_GOVERNOR_STRESS_US)
      self->governor_level = 2;
    else if (self->overload_us >= EDGE_GOVERNOR_WARN_US && self->governor_level < 1)
      self->governor_level = 1;
  } else {
    self->overload_us = MAX (self->overload_us - sample_interval_us / 2, 0);
    self->recovery_us = MIN (self->recovery_us + sample_interval_us,
                             EDGE_GOVERNOR_STRESS_US * 2);

    if (self->governor_level == 2 && self->recovery_us >= EDGE_GOVERNOR_STRESS_US) {
      self->governor_level = 1;
      self->recovery_us = 0;
    } else if (self->governor_level == 1 &&
               self->recovery_us >= EDGE_GOVERNOR_STRESS_US) {
      self->governor_level = 0;
      self->recovery_us = 0;
      self->overload_us = 0;
    }
  }
}

static gboolean
kgx_edge_redraw_tick_cb (GtkWidget     *widget,
                         GdkFrameClock *frame_clock,
                         gpointer       user_data)
{
  KgxEdge *self = KGX_EDGE (user_data);
  gint64 now_us = gdk_frame_clock_get_frame_time (frame_clock);

  if (!self->particle_throttle_enabled) {
    self->redraw_tick_id = 0;
    kgx_edge_reset_governor (self);
    return G_SOURCE_REMOVE;
  }

  if (!kgx_edge_has_visible_content (self) && !self->redraw_pending) {
    self->redraw_tick_id = 0;
    kgx_edge_reset_governor (self);
    return G_SOURCE_REMOVE;
  }

  if (self->frame_snapshot_cost_us > 0.0) {
    kgx_edge_note_snapshot (self, self->frame_snapshot_cost_us, now_us);
    self->frame_snapshot_cost_us = 0.0;
  }

  if (self->redraw_pending) {
    double refresh_hz = kgx_edge_get_refresh_hz (frame_clock, now_us);
    double effective_hz = kgx_edge_get_effective_hz (self);
    gboolean should_draw = FALSE;

    if (self->last_redraw_request_us == 0 || effective_hz >= refresh_hz * 0.98) {
      should_draw = TRUE;
      self->redraw_frame_credit = 0.0;
    } else {
      self->redraw_frame_credit =
        MIN (self->redraw_frame_credit + effective_hz / refresh_hz, 4.0);

      if (self->redraw_frame_credit + 1e-6 >= 1.0) {
        self->redraw_frame_credit = MAX (self->redraw_frame_credit - 1.0, 0.0);
        should_draw = TRUE;
      }
    }

    if (should_draw) {
      self->last_redraw_request_us = now_us;
      self->redraw_pending = FALSE;
      self->frame_draw_budget_remaining = kgx_edge_get_block_budget (self);
      kgx_edge_queue_draw_views (self);
    }
  }

  return G_SOURCE_CONTINUE;
}

static inline void
kgx_edge_ensure_redraw_tick (KgxEdge *self)
{
  if (self->redraw_tick_id == 0 && gtk_widget_get_mapped (GTK_WIDGET (self))) {
    self->redraw_tick_id =
      gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                    kgx_edge_redraw_tick_cb,
                                    self,
                                    NULL);
  }
}

static inline void
kgx_edge_stop_redraw_tick (KgxEdge *self)
{
  if (self->redraw_tick_id != 0) {
    gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->redraw_tick_id);
    self->redraw_tick_id = 0;
  }
}

void
kgx_edge_mark_dirty (KgxEdge *self)
{
  KgxEdge *root = kgx_edge_get_root (self);
  GtkWidget *widget = GTK_WIDGET (root);

  if (!gtk_widget_get_mapped (widget))
    return;

  if (!root->particle_throttle_enabled) {
    root->redraw_pending = FALSE;
    root->last_redraw_request_us = 0;
    root->frame_draw_budget_remaining = 0;
    kgx_edge_stop_redraw_tick (root);
    kgx_edge_queue_draw_views (root);
    return;
  }

  if (!kgx_edge_has_visible_content (root)) {
    root->redraw_pending = FALSE;
    root->last_redraw_request_us = 0;
    kgx_edge_queue_draw_views (root);
    return;
  }

  root->redraw_pending = TRUE;
  kgx_edge_ensure_redraw_tick (root);
}

/* ── colour helpers ──────────────────────────────────────── */

static GdkRGBA
resolve_color_cached (KgxEdge    *self,
                      const char *hex_or_empty,
                      GdkRGBA    *cache,
                      gboolean   *valid)
{
  GdkRGBA color = { 0, 0, 0, 1 };

  if (*valid)
    return *cache;

  if (hex_or_empty && hex_or_empty[0] != '\0') {
    gdk_rgba_parse (&color, hex_or_empty);
    color.alpha = 1.0f;
    *cache = color;
    *valid = TRUE;
    return color;
  }

  /* Accent color — don't cache, it can change dynamically. */
  gtk_widget_get_color (GTK_WIDGET (self), &color);
  return color;
}


/* ── snapshot ────────────────────────────────────────────── */

static void
kgx_edge_snapshot (GtkWidget   *widget,
                   GtkSnapshot *snapshot)
{
  KgxEdge *self = KGX_EDGE (widget);
  KgxEdge *root = kgx_edge_get_root (self);
  gint64 snapshot_start_us;
  int width;
  int height;
  int strip_extent;
  int *draw_budget;
  double perim;
  GskPath *tri;
  GskPath *dia;

  /* Fast path: nothing animating — skip all rendering work. */
  if (!kgx_edge_has_visible_content (root))
    return;

  snapshot_start_us = g_get_monotonic_time ();
  kgx_edge_get_canvas_size (widget, &width, &height);
  if (width <= 0 || height <= 0)
    return;

  strip_extent = (self->side == GTK_POS_TOP || self->side == GTK_POS_BOTTOM)
                   ? gtk_widget_get_height (widget)
                   : gtk_widget_get_width (widget);
  if (strip_extent <= 0)
    strip_extent = kgx_edge_get_strip_extent (root);

  perim = 2.0 * (width + height);
  draw_budget = root->particle_throttle_enabled
                  ? &root->frame_draw_budget_remaining
                  : NULL;
  tri = root->unit_triangle;
  dia = root->unit_diamond;

  /* Overscroll beam */
  if (root->overscroll_progress >= 0.0 && root->overscroll_enabled) {
    GdkRGBA color;
    const KgxParticleTunables *os_tune;

    color = resolve_color_cached (root, root->overscroll_color,
                                  &root->overscroll_rgba,
                                  &root->overscroll_rgba_valid);

    os_tune = (root->overscroll_style == 1) ? kgx_edge_resolve_tunables (root, KGX_PARTICLE_SCROLL2)
                                            : &root->global;
    kgx_edge_draw_overscroll (snapshot, root->overscroll_progress,
                              root->overscroll_edge, root->overscroll_style,
                              (root->overscroll_reverse == 2)
                                ? root->overscroll_reverse_toggle
                                : (root->overscroll_reverse == 1),
                              width, height, perim, &color, os_tune,
                              self->side, strip_extent, draw_budget, tri, dia,
                              BASE_OVERSCROLL_SEG);
  }

  /* Firework decoration — staggered center-bursts.
   * When process_reverse + FIREWORKS: implode (converge to center).
   * Uses snapshotted tunables captured at burst-fire time. */
  {
    gboolean implode = (root->process_preset == KGX_PARTICLE_FIREWORKS &&
                        root->process_reverse);

    for (int i = 0; i < MAX_BURSTS; i++) {
      if (root->burst_progress[i] >= 0.0) {
        const KgxParticleTunables *bt = &root->burst_tune_snap[i];
        double p     = root->burst_progress[i];
        double bp    = implode ? 1.0 - p : p;
        float  b_env = implode
                          ? kgx_particle_envelope (bp, 0.0, 0.02, 2)
                          : kgx_particle_envelope (p, bt->env_attack, bt->env_release, bt->env_curve);
        double seg_env = kgx_particle_envelope (bp, bt->env_attack, 0.0, bt->env_curve);
        double seg   = BASE_OVERSCROLL_SEG * bt->tail_length * 2.0 * seg_env;
        float  a     = b_env * 0.5f;
        double spread = seg * 3.0 * bp;
        double left_head  = fmod (root->burst_head[i] - spread + perim, perim);
        double right_head = fmod (root->burst_head[i] + spread, perim);
        int l_trail = implode ? -1 : +1;
        int r_trail = implode ? +1 : -1;

        kgx_edge_draw_segment (snapshot, left_head, seg, a,
                               width, height, perim, &root->burst_color[i],
                               l_trail, bt, p, self->side, strip_extent, draw_budget, tri, dia);
        kgx_edge_draw_segment (snapshot, right_head, seg, a,
                               width, height, perim, &root->burst_color[i],
                               r_trail, bt, p, self->side, strip_extent, draw_budget, tri, dia);
      }
    }
  }

  /* Ambient burst decoration — independent from process fireworks.
   * Uses snapshotted tunables captured at ambient-burst-fire time.
   * Keep rendering if any burst is still in-flight even after ambient_active
   * is cleared — this gives a graceful fade-out when leaving settings. */
  {
    gboolean ambient_render = (root->ambient_active && root->ambient_enabled);

    if (!ambient_render) {
      for (int i = 0; i < MAX_BURSTS && !ambient_render; i++)
        ambient_render = (root->ambient_progress[i] >= 0.0);
    }

    if (ambient_render) {
      for (int i = 0; i < MAX_BURSTS; i++) {
        if (root->ambient_progress[i] >= 0.0) {
          const KgxParticleTunables *abt = &root->ambient_tune_snap[i];
          double p     = root->ambient_progress[i];
          float  b_env = kgx_particle_envelope (p, abt->env_attack, abt->env_release, abt->env_curve);
          double seg_env = kgx_particle_envelope (p, abt->env_attack, 0.0, abt->env_curve);
          double seg   = BASE_OVERSCROLL_SEG * abt->tail_length * 2.0 * seg_env;
          float  a     = b_env * 0.5f;
          double spread = seg * 3.0 * p;
          double left_head  = fmod (root->ambient_head[i] - spread + perim, perim);
          double right_head = fmod (root->ambient_head[i] + spread, perim);

          kgx_edge_draw_segment (snapshot, left_head, seg, a,
                                 width, height, perim, &root->ambient_burst_color[i],
                                 +1, abt, p, self->side, strip_extent, draw_budget, tri, dia);
          kgx_edge_draw_segment (snapshot, right_head, seg, a,
                                 width, height, perim, &root->ambient_burst_color[i],
                                 -1, abt, p, self->side, strip_extent, draw_budget, tri, dia);
        }
      }
    }
  }

  /* Process-specific particle.
   * Ambient/settings visuals are allowed to run alongside it so presets can
   * be edited live; the dedicated ambient/settings toggle is what disables
   * that path when desired. Uses snapshotted tunables captured at animation
   * start. */
  if (root->process_progress >= 0.0 &&
      root->process_preset != KGX_PARTICLE_NONE) {
    const KgxParticleTunables *pt = &root->process_tune_snap;
    double p;
    float env;
    float a;
    double seg_full;
    double tail_env;
    double seg;

    /* Use AdwAnimation-driven progress when it's advancing normally.
     * Fall back to wall clock only when progress hasn't changed between
     * snapshots (frame clock stall during tab-switch layout). */
    if (root->process_progress != root->process_last_snapshot_progress) {
      p = root->process_progress;
    } else if (root->process_start_us > 0 && root->process_duration_s > 0) {
      double elapsed = (g_get_monotonic_time () - root->process_start_us) / 1000000.0;
      p = CLAMP (elapsed / root->process_duration_s, 0.0, 1.0);
      if (!root->process_linear) {
        double inv = 1.0 - p;
        p = 1.0 - inv * inv * inv;
      }
    } else {
      p = root->process_progress;
    }
    root->process_last_snapshot_progress = root->process_progress;
    env = kgx_particle_envelope (p, pt->env_attack, pt->env_release, pt->env_curve);
    a = env * 0.8f;

    seg_full = BASE_OVERSCROLL_SEG * pt->tail_length * 2.0;
    /* Tail envelope: grows during attack. In retract mode, shrinks during release.
     * Rotate keeps full tail -- it's always in motion. */
    if (root->process_preset == KGX_PARTICLE_ROTATE)
      tail_env = 1.0;
    else if (pt->release_mode == KGX_RELEASE_RETRACT) {
      /* Retract faster than alpha fades so the pull-back is visible.
       * Use concave curve (fast drop) regardless of user curve setting. */
      float raw = kgx_particle_envelope (p, pt->env_attack, pt->env_release, 1);
      tail_env = (double) (raw * raw);  /* squared for aggressive retraction */
    } else
      tail_env = kgx_particle_envelope (p, pt->env_attack, 0.0, pt->env_curve);  /* uniform/spread/grow: attack only */
    seg = seg_full * tail_env;

    switch (root->process_preset) {
    case KGX_PARTICLE_CORNERS: {
      double corner_a;
      double corner_b;
      double travel;
      double clamped_seg;
      double a_cw;
      double a_ccw;
      double b_cw;
      double b_ccw;

      if (root->process_reverse) {
        corner_a = (double) width;
        corner_b = (double)(2 * width + height);
      } else {
        corner_a = 0.0;
        corner_b = (double)(width + height);
      }

      travel = (width + height) / 2.0 * p;
      clamped_seg = MIN (seg, travel);
      a_cw  = fmod (corner_a + travel, perim);
      a_ccw = fmod (corner_a - travel + perim * 2, perim);
      b_cw  = fmod (corner_b + travel, perim);
      b_ccw = fmod (corner_b - travel + perim * 2, perim);

      kgx_edge_draw_segment (snapshot, a_cw,  clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      kgx_edge_draw_segment (snapshot, a_ccw, clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      kgx_edge_draw_segment (snapshot, b_cw,  clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      kgx_edge_draw_segment (snapshot, b_ccw, clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_PULSE_OUT: {
      double center;
      double spread;
      double clamped_seg;
      double left_head;
      double right_head;

      center = root->process_reverse
                 ? (double)width + height + width / 2.0
                 : width / 2.0;
      spread = (perim / 4.0) * p;
      clamped_seg = MIN (seg, spread);
      left_head  = fmod (center - spread + perim, perim);
      right_head = fmod (center + spread, perim);

      kgx_edge_draw_segment (snapshot, left_head,  clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      kgx_edge_draw_segment (snapshot, right_head, clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_ROTATE: {
      int dir = root->process_reverse ? -1 : +1;
      int trail = root->process_reverse ? +1 : -1;
      double half_p = fmod (p * 2.0, 1.0);
      double eased = 1.0 - (1.0 - half_p) * (1.0 - half_p) * (1.0 - half_p);
      float lap_env = kgx_particle_envelope (half_p, pt->env_attack, pt->env_release, pt->env_curve);
      float lap_a = lap_env * 0.8f;
      double lap_tail;
      double lap_seg;
      double travel;
      double clamped_seg;
      double offset;
      double head;

      if (pt->release_mode == KGX_RELEASE_RETRACT) {
        float raw = kgx_particle_envelope (half_p, pt->env_attack, pt->env_release, 1);
        lap_tail = (double)(raw * raw);
      } else
        lap_tail = kgx_particle_envelope (half_p, pt->env_attack, 0.0, pt->env_curve);
      lap_seg = seg_full * lap_tail;

      travel = (perim / 2.0) * eased;
      clamped_seg = MIN (lap_seg * 2.0, travel);
      offset = (p >= 0.5) ? perim / 2.0 : 0.0;
      head = fmod (dir * travel + offset + perim, perim);

      kgx_edge_draw_segment (snapshot, head, clamped_seg, lap_a,
                             width, height, perim, &root->process_color, trail, pt, half_p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_PING_PONG: {
      double edge_start;
      double half_p = fmod (p * 2.0, 1.0);
      gboolean returning = (p >= 0.5);
      float pp_env = kgx_particle_envelope (half_p, pt->env_attack, pt->env_release, pt->env_curve);
      float pp_a = pp_env * 0.9f;
      double pp_tail;
      double pp_seg;
      double travel;
      double clamped_seg;
      double pos;
      int trail;

      edge_start = root->process_reverse
                     ? (double)(width + height) : 0.0;
      if (pt->release_mode == KGX_RELEASE_RETRACT) {
        float raw = kgx_particle_envelope (half_p, pt->env_attack, pt->env_release, 1);
        pp_tail = (double)(raw * raw);
      } else
        pp_tail = kgx_particle_envelope (half_p, pt->env_attack, 0.0, pt->env_curve);
      pp_seg = BASE_OVERSCROLL_SEG * pp_tail;

      travel = (double) width * half_p;
      clamped_seg = MIN (pp_seg, travel);
      if (!returning) {
        pos   = fmod (edge_start + travel + perim, perim);
        trail = -1;
      } else {
        pos   = fmod (edge_start + (double) width * (1.0 - half_p) + perim, perim);
        trail = +1;
      }

      kgx_edge_draw_segment (snapshot, pos, clamped_seg, pp_a,
                             width, height, perim, &root->process_color, trail, pt, half_p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_NONE:
    case KGX_PARTICLE_FIREWORKS:
    case KGX_PARTICLE_AMBIENT:
    case KGX_PARTICLE_SCROLL2:
    default:
      break;
    }
  }

  root->frame_snapshot_cost_us += MAX ((double) (g_get_monotonic_time () - snapshot_start_us), 0.0);
}


/* ── measure (zero natural size — overlay fills parent) ─── */

static void
kgx_edge_measure (GtkWidget      *widget,
                  GtkOrientation  orientation,
                  int             for_size,
                  int            *minimum,
                  int            *natural,
                  int            *minimum_baseline,
                  int            *natural_baseline)
{
  KgxEdge *self = KGX_EDGE (widget);
  int extent = kgx_edge_get_strip_extent (self);
  gboolean horizontal_strip = (self->side == GTK_POS_TOP || self->side == GTK_POS_BOTTOM);

  if ((horizontal_strip && orientation == GTK_ORIENTATION_VERTICAL) ||
      (!horizontal_strip && orientation == GTK_ORIENTATION_HORIZONTAL)) {
    *minimum = extent;
    *natural = extent;
  } else {
    *minimum = 0;
    *natural = 0;
  }

  if (minimum_baseline)
    *minimum_baseline = -1;
  if (natural_baseline)
    *natural_baseline = -1;
}


/* ── animation callbacks ────────────────────────────────── */

static void
overscroll_value_cb (double   value,
                     KgxEdge *self)
{
  self->overscroll_progress = value;
  kgx_edge_mark_dirty (self);
}


static void
overscroll_done_cb (KgxEdge *self)
{
  self->overscroll_progress = -1.0;
  kgx_edge_mark_dirty (self);
}


/* ── map: resume firework if needed ───────────────────────── */

static void
kgx_edge_map (GtkWidget *widget)
{
  KgxEdge *self = KGX_EDGE (widget);
  KgxEdge *root = kgx_edge_get_root (self);

  GTK_WIDGET_CLASS (kgx_edge_parent_class)->map (widget);

  if (self != root) {
    if (kgx_edge_has_visible_content (root))
      gtk_widget_queue_draw (widget);
    return;
  }

  kgx_edge_resume_bursts (self);

  if (kgx_edge_has_visible_content (self)) {
    if (self->particle_throttle_enabled) {
      self->redraw_pending = TRUE;
      kgx_edge_ensure_redraw_tick (self);
    } else {
      kgx_edge_queue_draw_views (self);
    }
  }
}


static void
kgx_edge_unmap (GtkWidget *widget)
{
  KgxEdge *self = KGX_EDGE (widget);

  if (self == kgx_edge_get_root (self))
    kgx_edge_stop_redraw_tick (self);

  GTK_WIDGET_CLASS (kgx_edge_parent_class)->unmap (widget);
}


/* ── GObject property handlers ───────────────────────────── */

static void
kgx_edge_set_property (GObject      *object,
                       guint         property_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  KgxEdge *self = KGX_EDGE (object);
  KgxEdge *state = self;

  switch (property_id) {
    case PROP_MASTER:
      g_set_object (&self->master, g_value_get_object (value));
      gtk_widget_queue_resize (GTK_WIDGET (self));
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    case PROP_SIDE:
      self->side = g_value_get_enum (value);
      gtk_widget_queue_resize (GTK_WIDGET (self));
      gtk_widget_queue_draw (GTK_WIDGET (self));
      return;
    default:
      break;
  }

  state = kgx_edge_get_root (self);

  /* Handle per-preset tunables via indexed arithmetic */
  if (property_id >= PROP_PRESET_BASE &&
      property_id < (guint) LAST_PROP) {
    int idx   = property_id - PROP_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    kgx_edge_set_tunable_from_value (&state->preset[pi], field, value);
    kgx_edge_finish_tunable_change (state, field);
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    kgx_edge_set_tunable_from_value (&state->global, field, value);
    if (field == TUNE_SPEED)
      kgx_edge_refresh_global_animation_speed (state);
    kgx_edge_finish_tunable_change (state, field);
    return;
  }

  switch (property_id) {
    case PROP_OVERSCROLL_ENABLED:
      state->overscroll_enabled = g_value_get_boolean (value);
      kgx_edge_mark_dirty (state);
      break;
    case PROP_OVERSCROLL_COLOR:
      g_free (state->overscroll_color);
      state->overscroll_color = g_value_dup_string (value);
      state->overscroll_rgba_valid = FALSE;
      kgx_edge_mark_dirty (state);
      break;
    case PROP_OVERSCROLL_STYLE:
      state->overscroll_style = g_value_get_int (value);
      break;
    case PROP_OVERSCROLL_REVERSE:
      state->overscroll_reverse = g_value_get_int (value);
      break;
    case PROP_BURST_SPREAD:
      state->burst_spread = g_value_get_double (value);
      break;
    case PROP_BURST_COUNT:
      state->burst_count = CLAMP (g_value_get_int (value), 1, MAX_BURSTS);
      break;
    case PROP_AMBIENT_ENABLED:
      {
        gboolean was = state->ambient_enabled;
        state->ambient_enabled = g_value_get_boolean (value);
        /* Restart or stop ambient if settings page is currently open. */
        if (state->ambient_active && was != state->ambient_enabled) {
          state->ambient_active = FALSE;
          kgx_edge_set_ambient (state, TRUE);
        }
      }
      break;
    case PROP_PARTICLE_THROTTLE_ENABLED:
      state->particle_throttle_enabled = g_value_get_boolean (value);
      kgx_edge_reset_governor (state);
      if (!state->particle_throttle_enabled)
        kgx_edge_stop_redraw_tick (state);
      if (kgx_edge_has_visible_content (state))
        kgx_edge_mark_dirty (state);
      break;
    case PROP_PARTICLE_HZ:
      state->particle_hz = CLAMP (g_value_get_int (value), 10, 60);
      kgx_edge_reset_governor (state);
      if (kgx_edge_has_visible_content (state))
        kgx_edge_mark_dirty (state);
      break;
    case PROP_AMBIENT_BURST_COUNT:
      state->ambient_burst_count = CLAMP (g_value_get_int (value), 1, MAX_BURSTS);
      break;
    case PROP_AMBIENT_BURST_SPREAD:
      state->ambient_burst_spread = g_value_get_double (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_edge_get_property (GObject    *object,
                       guint       property_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
  KgxEdge *self = KGX_EDGE (object);
  KgxEdge *state = kgx_edge_get_root (self);

  switch (property_id) {
    case PROP_MASTER:
      g_value_set_object (value, self->master);
      return;
    case PROP_SIDE:
      g_value_set_enum (value, self->side);
      return;
    default:
      break;
  }

  /* Handle per-preset tunables */
  if (property_id >= PROP_PRESET_BASE &&
      property_id < (guint) LAST_PROP) {
    int idx   = property_id - PROP_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    kgx_edge_get_tunable_into_value (&state->preset[pi], field, value);
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    kgx_edge_get_tunable_into_value (&state->global, field, value);
    return;
  }

  switch (property_id) {
    case PROP_OVERSCROLL_ENABLED:
      g_value_set_boolean (value, state->overscroll_enabled);
      break;
    case PROP_OVERSCROLL_COLOR:
      g_value_set_string (value, state->overscroll_color ? state->overscroll_color : "");
      break;
    case PROP_OVERSCROLL_STYLE:
      g_value_set_int (value, state->overscroll_style);
      break;
    case PROP_OVERSCROLL_REVERSE:
      g_value_set_int (value, state->overscroll_reverse);
      break;
    case PROP_BURST_SPREAD:
      g_value_set_double (value, state->burst_spread);
      break;
    case PROP_BURST_COUNT:
      g_value_set_int (value, state->burst_count);
      break;
    case PROP_AMBIENT_ENABLED:
      g_value_set_boolean (value, state->ambient_enabled);
      break;
    case PROP_PARTICLE_THROTTLE_ENABLED:
      g_value_set_boolean (value, state->particle_throttle_enabled);
      break;
    case PROP_PARTICLE_HZ:
      g_value_set_int (value, state->particle_hz);
      break;
    case PROP_AMBIENT_BURST_COUNT:
      g_value_set_int (value, state->ambient_burst_count);
      break;
    case PROP_AMBIENT_BURST_SPREAD:
      g_value_set_double (value, state->ambient_burst_spread);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


/* ── lifecycle ───────────────────────────────────────────── */

static void
kgx_edge_dispose (GObject *object)
{
  KgxEdge *self = KGX_EDGE (object);

  kgx_edge_stop_redraw_tick (self);
  g_clear_object (&self->overscroll_anim);
  self->process_preset = KGX_PARTICLE_NONE;
  self->process_progress = -1.0;
  if (self->process_anim) {
    adw_animation_reset (self->process_anim);
    g_clear_object (&self->process_anim);
  }
  g_clear_object (&self->master);
  g_clear_pointer (&self->overscroll_color, g_free);
  g_clear_pointer (&self->unit_triangle, gsk_path_unref);
  g_clear_pointer (&self->unit_diamond, gsk_path_unref);
  kgx_edge_clear_bursts (self);

  G_OBJECT_CLASS (kgx_edge_parent_class)->dispose (object);
}


static void
kgx_edge_class_init (KgxEdgeClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose      = kgx_edge_dispose;
  object_class->set_property = kgx_edge_set_property;
  object_class->get_property = kgx_edge_get_property;

  widget_class->snapshot = kgx_edge_snapshot;
  widget_class->measure  = kgx_edge_measure;
  widget_class->map      = kgx_edge_map;
  widget_class->unmap    = kgx_edge_unmap;

  edge_pspecs[PROP_MASTER] =
    g_param_spec_object ("master", NULL, NULL,
                         KGX_TYPE_EDGE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_SIDE] =
    g_param_spec_enum ("side", NULL, NULL,
                       GTK_TYPE_POSITION_TYPE,
                       GTK_POS_TOP,
                       G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_ENABLED] =
    g_param_spec_boolean ("overscroll-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_COLOR] =
    g_param_spec_string ("overscroll-color", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_STYLE] =
    g_param_spec_int ("overscroll-style", NULL, NULL, 0, 1, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_REVERSE] =
    g_param_spec_int ("overscroll-reverse", NULL, NULL, 0, 2, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_BURST_SPREAD] =
    g_param_spec_double ("burst-spread", NULL, NULL, 0.5, 5.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_BURST_COUNT] =
    g_param_spec_int ("burst-count", NULL, NULL, 1, MAX_BURSTS, 4,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_AMBIENT_ENABLED] =
    g_param_spec_boolean ("ambient-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PARTICLE_THROTTLE_ENABLED] =
    g_param_spec_boolean ("particle-throttle-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PARTICLE_HZ] =
    g_param_spec_int ("particle-hz", NULL, NULL, 10, 60, 30,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);


  edge_pspecs[PROP_AMBIENT_BURST_COUNT] =
    g_param_spec_int ("ambient-burst-count", NULL, NULL, 1, 8, 8,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_AMBIENT_BURST_SPREAD] =
    g_param_spec_double ("ambient-burst-spread", NULL, NULL, 0.5, 5.0, 3.1,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /* ── global tunable property specs ─────────────────────── */
  /*              speed  thick  tail   pdep   pspd   atk    rel    rmode  shape  ecurve thkatk thkrel gap  thkrmode thkcurve */
  {
    /*              spd  thk  tail pdep pspd atk  rel  rm sh crv gap  thka thkr thkrm thkc */
    static const double dbl_min[] = { 0.1, 0, 0.1, 0.0, 0.1, 0.0, 0.0, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0 };
    static const double dbl_max[] = { 3.0, 0, 3.0, 1.0, 5.0, 0.5, 0.5, 0, 0, 0, 2.0, 0.5, 0.5, 0, 0 };
    static const double dbl_def[] = { 1.0, 0, 1.0, 0.3, 1.0, 0.2, 0.3, 0, 0, 0, 1.0, 0.0, 0.0, 0, 0 };

    for (int f = 0; f < N_TUNE_FIELDS; f++) {
      int id = PROP_GLOBAL_BASE + f;
      if (f == TUNE_THICKNESS) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 2, 40, 10,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_GAP) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 1, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_RELEASE_MODE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 3, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_THK_RELEASE_MODE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 4, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_SHAPE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 3, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_ENV_CURVE || f == TUNE_THK_CURVE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 1, 3, 2,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else {
        edge_pspecs[id] =
          g_param_spec_double (tune_names[f], NULL, NULL,
                               dbl_min[f], dbl_max[f], dbl_def[f],
                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      }
    }
  }

  /* ── per-preset tunable property specs ─────────────────── */
  /*              speed  thick  tail   pdep   pspd   atk    rel    rmode  shape  ecurve thkatk thkrel gap  thkrmode thkcurve */
  {
    /*              spd  thk  tail pdep pspd atk  rel  rm sh crv gap  thka thkr thkrm thkc */
    static const double dbl_min[] = { 0.1, 0, 0.1, 0.0, 0.1, 0.0, 0.0, 0, 0, 0, 0.0, 0.0, 0.0, 0, 0 };
    static const double dbl_max[] = { 3.0, 0, 3.0, 1.0, 5.0, 0.5, 0.5, 0, 0, 0, 2.0, 0.5, 0.5, 0, 0 };
    static const double dbl_def[] = { 1.0, 0, 0.9, 0.5, 0.8, 0.2, 0.3, 0, 0, 0, 1.0, 0.0, 0.0, 0, 0 };

    for (int p = 0; p < N_PRESETS; p++) {
      for (int f = 0; f < N_TUNE_FIELDS; f++) {
        int id = PROP_PRESET_BASE + p * N_TUNE_FIELDS + f;
        char *name = g_strdup_printf ("%s-%s", tune_names[f],
                                      preset_suffixes[p]);
        if (f == TUNE_THICKNESS) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 2, 40, 20,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_GAP) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 1, 0,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_RELEASE_MODE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 3, 0,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_THK_RELEASE_MODE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 4, 0,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_SHAPE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 3, 0,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_ENV_CURVE || f == TUNE_THK_CURVE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 1, 3, 2,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else {
          edge_pspecs[id] =
            g_param_spec_double (name, NULL, NULL,
                                 dbl_min[f], dbl_max[f], dbl_def[f],
                                 G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        }
        g_free (name);
      }
    }
  }

  g_object_class_install_properties (object_class, LAST_PROP, edge_pspecs);

  gtk_widget_class_set_css_name (widget_class, "kgx-edge");
}


static void
kgx_edge_init (KgxEdge *self)
{
  AdwAnimationTarget *target;

  /* Defaults */
  self->overscroll_enabled  = TRUE;
  self->overscroll_color    = g_strdup ("");
  self->side                = GTK_POS_TOP;
  self->burst_spread        = 3.1;
  self->burst_count         = 8;
  self->ambient_enabled     = TRUE;
  self->particle_throttle_enabled = TRUE;
  self->particle_hz         = 30;
  self->ambient_burst_count = 8;
  self->ambient_burst_spread = 3.1;
  self->overscroll_progress = -1.0;
  self->process_progress    = -1.0;
  self->process_preset      = KGX_PARTICLE_NONE;
  kgx_edge_reset_governor (self);

  /* Global tunables (used for overscroll fallback) */
  self->global = (KgxParticleTunables) {
    .speed = 0.3, .thickness = 20, .tail_length = 0.9,
    .pulse_depth = 0.5, .pulse_speed = 0.8,
    .env_attack = 0.2, .env_release = 0.3,
    .release_mode = KGX_RELEASE_UNIFORM,
    .gap = 0,
    .env_curve = 2, .thk_attack = 0.0, .thk_release = 0.0, .thk_curve = 2,
  };

  /* Per-preset tunables */
  for (int i = 0; i < N_PRESETS; i++) {
    self->preset[i] = (KgxParticleTunables) {
      .speed = 1.0, .thickness = 20, .tail_length = 0.9,
      .pulse_depth = 0.5, .pulse_speed = 0.8,
      .env_attack = 0.2, .env_release = 0.3, .gap = 0,
      .release_mode = KGX_RELEASE_UNIFORM,
      .env_curve = 2, .thk_attack = 0.0, .thk_release = 0.0, .thk_curve = 2,
    };
  }

  /* Pre-build unit-size paths for triangle and diamond shapes
   * (no per-frame allocation — just translate+scale at draw time). */
  {
    GskPathBuilder *builder = gsk_path_builder_new ();
    gsk_path_builder_move_to (builder, -0.5f, -0.5f);
    gsk_path_builder_line_to (builder,  0.5f,  0.0f);
    gsk_path_builder_line_to (builder, -0.5f,  0.5f);
    gsk_path_builder_close (builder);
    self->unit_triangle = gsk_path_builder_free_to_path (builder);
  }
  {
    GskPathBuilder *builder = gsk_path_builder_new ();
    gsk_path_builder_move_to (builder,  0.0f, -0.5f);
    gsk_path_builder_line_to (builder,  0.5f,  0.0f);
    gsk_path_builder_line_to (builder,  0.0f,  0.5f);
    gsk_path_builder_line_to (builder, -0.5f,  0.0f);
    gsk_path_builder_close (builder);
    self->unit_diamond = gsk_path_builder_free_to_path (builder);
  }

  gtk_widget_set_can_target (GTK_WIDGET (self), FALSE);
  gtk_widget_set_can_focus (GTK_WIDGET (self), FALSE);

  /* Overscroll animation */
  target = adw_callback_animation_target_new (
      (AdwAnimationTargetFunc) overscroll_value_cb, self, NULL);

  self->overscroll_anim = adw_timed_animation_new (GTK_WIDGET (self),
                                                   0.0, 1.0,
                                                   BASE_OVERSCROLL_MS,
                                                   target);
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->overscroll_anim),
                                  ADW_EASE_OUT_CUBIC);
  g_signal_connect_swapped (self->overscroll_anim, "done",
                            G_CALLBACK (overscroll_done_cb), self);
  kgx_edge_init_bursts (self);
}


/* ── public API ──────────────────────────────────────────── */

GtkWidget *
kgx_edge_new (void)
{
  return g_object_new (KGX_TYPE_EDGE, NULL);
}


void
kgx_edge_fire_overscroll (KgxEdge         *self,
                          GtkPositionType  edge)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  if (!self->overscroll_enabled)
    return;

  self->overscroll_edge = edge;

  /* Flip the alternating toggle before playing so this fire uses the new value */
  if (self->overscroll_reverse == 2)
    self->overscroll_reverse_toggle = !self->overscroll_reverse_toggle;

  self->overscroll_progress = 0.0;
  adw_animation_reset (self->overscroll_anim);
  adw_animation_play (self->overscroll_anim);
  kgx_edge_mark_dirty (self);
}

void
kgx_edge_reset_redraw_governor (KgxEdge *self)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);
  kgx_edge_reset_governor (self);
}
