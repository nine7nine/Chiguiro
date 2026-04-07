/* kgx-edge.c
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

#include "kgx-config.h"

#include <adwaita.h>
#include <math.h>

#include "kgx-edge.h"


#define BASE_OVERSCROLL_MS    600
#define BASE_OVERSCROLL_SEG   200.0
#define EDGE_GOVERNOR_WARN_US 250000
#define EDGE_GOVERNOR_STRESS_US 1000000
#define EDGE_BLOCK_BUDGET_NORMAL 320
#define EDGE_BLOCK_BUDGET_WARN   224
#define EDGE_BLOCK_BUDGET_STRESS 160

typedef struct _KgxEdge KgxEdge;
typedef struct { int index; KgxEdge *self; } BurstData;


struct _KgxEdge {
  GtkWidget       parent_instance;

  KgxEdge        *master;              /* NULL = stateful master strip */
  GtkPositionType side;                /* which edge this strip renders */

  /* ── configurable properties ───────────────────────────── */
  gboolean        overscroll_enabled;
  char           *overscroll_color;     /* hex string, empty = accent */
  double          burst_spread;         /* multiplier on burst stagger */
  int             burst_count;          /* how many fire per cycle (1-MAX_BURSTS) */
  int             overscroll_style;     /* 0 = scroll1, 1 = scroll2 */
  int             overscroll_reverse;        /* 0=forward, 1=reverse, 2=alternating */
  gboolean        overscroll_reverse_toggle; /* for alternating mode */

  /* ── particle tunables ────────────────────────────────── */
  KgxParticleTunables global;           /* overscroll fallback */
  KgxParticleTunables preset[N_PRESETS]; /* indexed by KgxParticlePreset - 1 */

  /* ── overscroll state ──────────────────────────────────── */
  AdwAnimation   *overscroll_anim;
  double          overscroll_progress;   /* –1 = idle */
  GtkPositionType overscroll_edge;

  /* ── ambient (settings page) configuration ─────────────── */
  gboolean        ambient_enabled;     /* edge-settings-animation GSettings */
  gboolean        particle_throttle_enabled;
  int             particle_hz;         /* global redraw cap before governor */

  /* ── redraw governor ────────────────────────────────────── */
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

  /* ── firework / burst mode ────────────────────────────────── */
  gboolean        ambient_active;      /* settings page is currently open */
#define MAX_BURSTS 8
#define firework_active(s) ((s)->process_preset == KGX_PARTICLE_FIREWORKS || \
                            (s)->process_preset == KGX_PARTICLE_AMBIENT)

  guint           firework_timeout;
  AdwAnimation   *burst_anim[MAX_BURSTS];
  double          burst_progress[MAX_BURSTS];
  double          burst_head[MAX_BURSTS];
  GdkRGBA         burst_color[MAX_BURSTS];
  guint           burst_timeout[MAX_BURSTS];
  BurstData       burst_data[MAX_BURSTS];

  /* ── ambient (settings) burst system — fully independent ─── */
  guint           ambient_timeout;
  AdwAnimation   *ambient_anim[MAX_BURSTS];
  double          ambient_progress[MAX_BURSTS];
  double          ambient_head[MAX_BURSTS];
  GdkRGBA         ambient_burst_color[MAX_BURSTS];
  guint           ambient_burst_timeout[MAX_BURSTS];
  BurstData       ambient_burst_data[MAX_BURSTS];
  int             ambient_burst_count;   /* 1..MAX_BURSTS */
  double          ambient_burst_spread;  /* stagger multiplier */

  /* ── process-specific particle ─────────────────────────── */
  KgxParticlePreset process_preset;
  GdkRGBA           process_color;
  gboolean          process_reverse;
  int               process_shape_override;   /* -1 = use preset */
  int               process_gap_override;     /* -1 = use preset */
  int               process_speed_override;   /* 0 = use preset  */
  int               process_thk_override;     /* 0 = use preset  */
  gboolean          process_reverse_toggle;   /* for alternating mode */
  int               process_reverse_mode;    /* 0=fwd, 1=rev, 2=alternate */
  AdwAnimation     *process_anim;
  double            process_progress;   /* -1 = idle, 0..1 = active */
  double            process_last_snapshot_progress; /* stall detection */
  gint64            process_start_us;   /* monotonic µs when animation started */
  double            process_duration_s; /* animation duration in seconds */
  gboolean          process_linear;     /* TRUE for linear easing presets */

  /* ── pending preset (for graceful transitions) ──────────── */
  gboolean          pending_change;
  KgxParticlePreset pending_preset;
  GdkRGBA           pending_color;
  int               pending_reverse;
  int               pending_shape_override;
  int               pending_gap_override;
  int               pending_speed_override;
  int               pending_thk_override;

  /* ── stashed process state (saved when ambient takes over) ── */
  KgxParticlePreset stashed_preset;
  GdkRGBA           stashed_color;
  int               stashed_reverse;
  int               stashed_shape_override;
  int               stashed_gap_override;
  int               stashed_speed_override;
  int               stashed_thk_override;
  gboolean          has_stashed;

  /* ── snapshotted tunables (captured at animation start) ──── */
  KgxParticleTunables process_tune_snap;             /* for process_anim in-flight */
  KgxParticleTunables burst_tune_snap[MAX_BURSTS];   /* per firework burst slot    */
  KgxParticleTunables ambient_tune_snap[MAX_BURSTS]; /* per ambient burst slot     */

  /* ── cached pre-built paths for shaped particles ─────────── */
  GskPath            *unit_triangle;
  GskPath            *unit_diamond;

  /* ── cached parsed colors (invalidated on property change) ── */
  GdkRGBA             overscroll_rgba;
  gboolean            overscroll_rgba_valid;
};


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


/* ── tunables helpers ───────────────────────────────────── */

static inline const KgxParticleTunables *
resolve_tunables (KgxEdge *self, KgxParticlePreset preset)
{
  if (preset >= KGX_PARTICLE_FIREWORKS && preset <= KGX_PARTICLE_SCROLL2)
    return &self->preset[preset - 1];
  return &self->global;
}

static inline KgxEdge *
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

static void
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

static int
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

static inline void
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

static void
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

static inline void
set_tunable_double (KgxParticleTunables *t, int field, double v)
{
  switch (field) {
  case TUNE_SPEED:        t->speed        = v; break;
  case TUNE_TAIL_LENGTH:  t->tail_length  = v; break;
  case TUNE_PULSE_DEPTH:  t->pulse_depth  = v; break;
  case TUNE_PULSE_SPEED:  t->pulse_speed  = v; break;
  case TUNE_ENV_ATTACK:   t->env_attack   = v; break;
  case TUNE_ENV_RELEASE:  t->env_release  = v; break;
  case TUNE_THK_ATTACK:   t->thk_attack   = v; break;
  case TUNE_THK_RELEASE:  t->thk_release  = v; break;
  default: break;
  }
}

static inline double
get_tunable_double (const KgxParticleTunables *t, int field)
{
  switch (field) {
  case TUNE_SPEED:        return t->speed;
  case TUNE_TAIL_LENGTH:  return t->tail_length;
  case TUNE_PULSE_DEPTH:  return t->pulse_depth;
  case TUNE_PULSE_SPEED:  return t->pulse_speed;
  case TUNE_ENV_ATTACK:   return t->env_attack;
  case TUNE_ENV_RELEASE:  return t->env_release;
  case TUNE_THK_ATTACK:   return t->thk_attack;
  case TUNE_THK_RELEASE:  return t->thk_release;
  default:                return 0.0;
  }
}

/* Compute envelope value at normalized time t (0..1).
 * Returns 0..1: ramps up during attack, holds at 1, fades during release.
 * curve: 1=concave (fast rise/fall), 2=linear, 3=convex (slow rise/fall). */
static inline float
envelope (double t, double attack, double release, int curve)
{
  float linear;
  if (attack > 0.0 && t < attack) {
    linear = (float)(t / attack);
    if (curve == 1) return sqrtf (linear);      /* concave — fast rise */
    if (curve == 3) return linear * linear;      /* convex — slow rise */
    return linear;
  }
  if (release > 0.0 && t > 1.0 - release) {
    linear = (float)((1.0 - t) / release);
    if (curve == 1) return sqrtf (linear);
    if (curve == 3) return linear * linear;
    return linear;
  }
  return 1.0f;
}

static inline float
thickness_envelope (double t, double attack, double release, int curve)
{
  return envelope (t, attack, release, curve);
}


/* ── colour helpers ──────────────────────────────────────── */

static GdkRGBA
resolve_color_cached (KgxEdge    *self,
                      const char *hex_or_empty,
                      GdkRGBA    *cache,
                      gboolean   *valid)
{
  if (*valid)
    return *cache;

  GdkRGBA color = { 0, 0, 0, 1 };

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


/* ── shape rendering helpers ─────────────────────────────── */

static void
append_circle (GtkSnapshot   *snapshot,
               float          px,
               float          py,
               float          size,
               const GdkRGBA *color)
{
  GskRoundedRect rrect;
  gsk_rounded_rect_init_from_rect (&rrect,
    &GRAPHENE_RECT_INIT (px, py, size, size), size / 2.0f);
  gtk_snapshot_push_rounded_clip (snapshot, &rrect);
  gtk_snapshot_append_color (snapshot, color,
                             &GRAPHENE_RECT_INIT (px, py, size, size));
  gtk_snapshot_pop (snapshot);
}

static void
append_diamond (GtkSnapshot   *snapshot,
                GskPath       *unit_path,
                float          px,
                float          py,
                float          size,
                const GdkRGBA *color)
{
  float half = size / 2.0f;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_scale (snapshot, size, size);
  gtk_snapshot_append_fill (snapshot, unit_path, GSK_FILL_RULE_WINDING, color);
  gtk_snapshot_restore (snapshot);
}

/* Triangle pointing in direction of motion.
 * angle: 0=right, 90=down, 180=left, 270=up
 * Uses a pre-built unit triangle (-0.5..0.5) scaled to size — zero allocations. */
static void
append_triangle (GtkSnapshot   *snapshot,
                 GskPath       *unit_path,
                 float          px,
                 float          py,
                 float          size,
                 float          angle,
                 const GdkRGBA *color)
{
  float half = size / 2.0f;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_rotate (snapshot, angle);
  gtk_snapshot_scale (snapshot, size, size);
  gtk_snapshot_append_fill (snapshot, unit_path, GSK_FILL_RULE_WINDING, color);
  gtk_snapshot_restore (snapshot);
}


/* ── shared segment drawing ──────────────────────────────── */

/*
 * Draw a fading snake segment along the perimeter.
 *
 * head_d:   distance in pixels from the top-left corner (clockwise)
 * seg_len:  length of the segment trail in pixels
 * alpha:    peak opacity (head)
 * width/height: widget dimensions
 */
/*
 * trail_dir: -1 = trail extends counter-clockwise from head (default)
 *            +1 = trail extends clockwise from head
 */
static void
draw_segment (GtkSnapshot                *snapshot,
              double                      head_d,
              double                      seg_len,
              float                       alpha,
              float                       width,
              float                       height,
              double                      perim,
              const GdkRGBA              *color,
              int                         trail_dir,
              const KgxParticleTunables  *tune,
              double                      phase,
              GtkPositionType             side,
              float                       strip_extent,
              int                        *budget_remaining,
              GskPath                    *unit_triangle,
              GskPath                    *unit_diamond)
{
  double base_blk = (double) tune->thickness;
  double blk      = base_blk;

  /* Thickness attack (all blocks grow from 0). */
  float thk_attack_env = 1.0f;
  if (tune->thk_attack > 0.0)
    thk_attack_env = thickness_envelope (phase, tune->thk_attack, 0.0, tune->thk_curve);

  /* R = uniform shrink (all blocks together, original behavior).
   * S/G = per-block (head stays, tail changes) — computed in the loop.
   * A = all blocks shrink (head included) — uniform release factor. */
  float thk_release_factor = 0.0f;  /* per-block factor for S/G/A modes */
  if (tune->thk_release_mode == KGX_RELEASE_RETRACT) {
    /* Uniform: apply full envelope including release to all blocks. */
    blk *= thickness_envelope (phase, tune->thk_attack, tune->thk_release, tune->thk_curve);
  } else {
    blk *= thk_attack_env;
    /* Per-block release factor for S/G/A modes. */
    if ((tune->thk_release_mode == KGX_RELEASE_SPREAD ||
         tune->thk_release_mode == KGX_RELEASE_GROW ||
         tune->thk_release_mode == KGX_RELEASE_ALL) &&
        tune->thk_release > 0.0 && phase > 1.0 - tune->thk_release) {
      thk_release_factor = (float)((phase - (1.0 - tune->thk_release)) / tune->thk_release);
    }
  }
  if (blk < 1.0) blk = 1.0;

  /* Gap between blocks: 0 = gapped (default), 1 = solid (no gap). */
  double gap   = tune->gap ? 0.0 : base_blk;

  /* SPREAD release mode: increase gap between blocks during release phase,
   * pushing tail blocks away from the head. */
  if (tune->release_mode == KGX_RELEASE_SPREAD &&
      tune->env_release > 0.0 && phase > 1.0 - tune->env_release) {
    double rt = (phase - (1.0 - tune->env_release)) / tune->env_release;
    gap *= (1.0 + rt * 3.0);
  }

  double step  = blk + gap;
  int    blocks = (int)(seg_len / step);

  if (blocks < 1) blocks = 1;

  if (budget_remaining) {
    if (*budget_remaining <= 0)
      return;

    blocks = MIN (blocks, *budget_remaining);
    *budget_remaining -= blocks;
  }

  double d     = fmod (head_d + perim, perim);
  double delta = trail_dir * step;
  float  inv_blocks = 1.0f / (float)(blocks > 1 ? blocks - 1 : 1);
  float  phase_offset = (float)(phase * 40.0 * tune->pulse_speed);

  for (int s = 0; s < blocks; s++) {
    if (s > 0) {
      d += delta;
      if (d >= perim) d -= perim;
      if (d < 0.0)    d += perim;
    }
    /* Head (s=0) is bright, tail (high s) fades out. */
    float  t = (float) s * inv_blocks;

    /* Per-block thickness: head stays full, tail shrinks/grows during release.
     * thk_release_factor is 0 outside release phase, ramps 0→1 during release.
     * ALL mode: every block (head included) shrinks uniformly. */
    double block_blk = blk;
    if (thk_release_factor > 0.0f) {
      if (tune->thk_release_mode == KGX_RELEASE_ALL)
        block_blk = blk * (1.0 - (double)(thk_release_factor));
      else if (tune->thk_release_mode == KGX_RELEASE_GROW)
        block_blk = blk * (1.0 + (double)(t * thk_release_factor));
      else /* SPREAD = shrink tail only */
        block_blk = blk * (1.0 - (double)(t * thk_release_factor));
      if (block_blk < 1.0) block_blk = 1.0;
    }

    /* Base fade: head bright, tail dims. */
    float  a = alpha * (1.0f - 0.7f * t);
    /* Pulsing shimmer on tail only — skip the leading block (s=0),
     * strengthen pulse further from head, wave travels backward. */
    if (s > 0 && tune->pulse_depth > 0.0) {
      float intensity = t * (float) tune->pulse_depth;
      float pulse = 1.0f - intensity
                    + intensity * sinf ((float) s * 1.2f - phase_offset);
      a *= pulse;
    }
    GdkRGBA c = *color;
    GtkPositionType block_side;
    float  px, py, bw, bh;

    c.alpha = a;

    float bb = (float) block_blk;
    float tri_angle = 0;
    if (d < width) {
      /* Top edge — CW = right, CCW = left */
      block_side = GTK_POS_TOP;
      px = (float) d;  py = 0;
      bw = bb;  bh = bb;
      tri_angle = (trail_dir == -1) ? 0 : 180;
    } else if (d < width + height) {
      /* Right edge — CW = down, CCW = up */
      block_side = GTK_POS_RIGHT;
      px = width - bb;  py = (float)(d - width);
      bw = bb;  bh = bb;
      tri_angle = (trail_dir == -1) ? 90 : 270;
    } else if (d < 2 * width + height) {
      /* Bottom edge — CW = left, CCW = right */
      block_side = GTK_POS_BOTTOM;
      px = width - (float)(d - width - height) - bb;
      py = height - bb;
      bw = bb;  bh = bb;
      tri_angle = (trail_dir == -1) ? 180 : 0;
    } else {
      /* Left edge — CW = up, CCW = down */
      block_side = GTK_POS_LEFT;
      px = 0;
      py = height - (float)(d - 2 * width - height) - bb;
      bw = bb;  bh = bb;
      tri_angle = (trail_dir == -1) ? 270 : 90;
    }

    if (block_side != side)
      continue;

    if (side == GTK_POS_RIGHT)
      px -= width - strip_extent;
    else if (side == GTK_POS_BOTTOM)
      py -= height - strip_extent;

    switch (tune->shape) {
    case KGX_PARTICLE_SHAPE_CIRCLE:
      append_circle (snapshot, px, py, bb, &c);
      break;
    case KGX_PARTICLE_SHAPE_DIAMOND:
      append_diamond (snapshot, unit_diamond, px, py, bb, &c);
      break;
    case KGX_PARTICLE_SHAPE_TRIANGLE:
      append_triangle (snapshot, unit_triangle, px, py, bb, tri_angle, &c);
      break;
    default: /* SQUARE */
      gtk_snapshot_append_color (snapshot, &c,
                                &GRAPHENE_RECT_INIT (px, py, bw, bh));
      break;
    }
  }
}


/* ── overscroll drawing ──────────────────────────────────── */

static void
draw_overscroll (GtkSnapshot                *snapshot,
                 double                      progress,
                 GtkPositionType             edge,
                 int                         style,
                 gboolean                    reverse,
                 float                       width,
                 float                       height,
                 double                      perim,
                 const GdkRGBA              *color,
                 const KgxParticleTunables  *tune,
                 GtkPositionType             side,
                 float                       strip_extent,
                 int                        *budget_remaining,
                 GskPath                    *unit_triangle,
                 GskPath                    *unit_diamond)
{
  float  env    = envelope (progress, tune->env_attack, tune->env_release, tune->env_curve);
  float  a      = env * 0.9f;

  if (style == 1) {
    /* Scroll 2: solid bar along the active edge. */
    if (edge != side)
      return;

    float thk = (float) tune->thickness;
    float thk_env_val = 1.0f;
    if (tune->thk_attack > 0.0)
      thk_env_val = thickness_envelope (progress, tune->thk_attack, 0.0, tune->thk_curve);
    thk *= thk_env_val;
    if (thk < 1.0f) thk = 1.0f;

    float bar_len = (float) width * envelope (progress, tune->env_attack, 0.0, tune->env_curve);
    GdkRGBA c = *color;
    c.alpha = a;

    if (edge == GTK_POS_TOP) {
      float x = reverse ? width - bar_len : 0.0f;
      gtk_snapshot_append_color (snapshot, &c,
                                 &GRAPHENE_RECT_INIT (x, 0, bar_len, thk));
    } else {
      float x = reverse ? width - bar_len : 0.0f;
      gtk_snapshot_append_color (snapshot, &c,
                                 &GRAPHENE_RECT_INIT (x, strip_extent - thk, bar_len, thk));
    }
  } else {
    /* Scroll 1: corner burst along two adjacent edges. */
    double corner;
    double h_head, v_head;
    int    h_trail, v_trail;

    if (edge == GTK_POS_BOTTOM) {
      if (reverse) {
        corner = width + height + width;
        h_head = fmod (corner - width * progress + perim, perim);
        v_head = fmod (corner + height * progress, perim);
        h_trail = +1;
        v_trail = -1;
      } else {
        corner = width + height;
        h_head = fmod (corner + width * progress, perim);
        v_head = fmod (corner - height * progress + perim, perim);
        h_trail = -1;
        v_trail = +1;
      }
    } else {
      if (reverse) {
        corner = 0.0;
        h_head = fmod (width * progress, perim);
        v_head = fmod (perim - height * progress + perim, perim);
        h_trail = -1;
        v_trail = +1;
      } else {
        corner = width;
        h_head = fmod (corner - width * progress + perim, perim);
        v_head = fmod (corner + height * progress, perim);
        h_trail = +1;
        v_trail = -1;
      }
    }

    draw_segment (snapshot, h_head, BASE_OVERSCROLL_SEG, a,
                  width, height, perim, color, h_trail, tune, progress, side, strip_extent,
                  budget_remaining,
                  unit_triangle, unit_diamond);
    draw_segment (snapshot, v_head, BASE_OVERSCROLL_SEG, a,
                  width, height, perim, color, v_trail, tune, progress, side, strip_extent,
                  budget_remaining,
                  unit_triangle, unit_diamond);
  }
}


/* ── firework mode ───────────────────────────────────────── */

/* ── burst helpers (array-based for MAX_BURSTS) ────────────── */

static GdkRGBA
random_muted_color (void)
{
  static const GdkRGBA colors[] = {
    { 0.85f, 0.40f, 0.75f, 1.0f },  /* bright magenta */
    { 0.40f, 0.60f, 0.90f, 1.0f },  /* bright blue    */
    { 0.40f, 0.80f, 0.50f, 1.0f },  /* bright green   */
    { 0.95f, 0.65f, 0.35f, 1.0f },  /* bright orange  */
  };
  return colors[g_random_int_range (0, G_N_ELEMENTS (colors))];
}


static void
burst_value_cb (double value, BurstData *bd)
{
  bd->self->burst_progress[bd->index] = value;
  kgx_edge_mark_dirty (bd->self);
}


static void firework_schedule (KgxEdge *self);


static void
burst_done_cb (BurstData *bd)
{
  bd->self->burst_progress[bd->index] = -1.0;
  kgx_edge_mark_dirty (bd->self);
}


static gboolean
burst_fire (gpointer data)
{
  BurstData *bd = data;
  KgxEdge *self = bd->self;
  int i = bd->index;
  int width;
  int height;
  double perim;

  self->burst_timeout[i] = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  kgx_edge_get_canvas_size (GTK_WIDGET (self), &width, &height);
  perim  = 2.0 * (width + height);

  self->burst_head[i] = g_random_double () * perim;

  /* Process firework uses the configured particle color. */
  if (self->process_preset == KGX_PARTICLE_FIREWORKS)
    self->burst_color[i] = self->process_color;
  else
    self->burst_color[i] = random_muted_color ();

  /* Snapshot tunables at burst-fire time so in-flight bursts are
   * decoupled from live state (preset switches, tunable adjustments). */
  {
    const KgxParticleTunables *bt =
      (self->process_preset == KGX_PARTICLE_FIREWORKS)
        ? resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
        : &self->global;
    self->burst_tune_snap[i] = *bt;
    adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->burst_anim[i]),
                                      (guint)(800.0 / bt->speed));
  }

  adw_animation_reset (self->burst_anim[i]);
  adw_animation_play (self->burst_anim[i]);

  return G_SOURCE_REMOVE;
}



static gboolean
firework_fire (gpointer data)
{
  KgxEdge *self = KGX_EDGE (data);

  self->firework_timeout = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  /* Fire first burst immediately, stagger the rest. */
  self->burst_data[0].index = 0;
  self->burst_data[0].self  = self;
  burst_fire (&self->burst_data[0]);

  for (int i = 1; i < self->burst_count; i++) {
    self->burst_data[i].index = i;
    self->burst_data[i].self  = self;
    if (self->burst_timeout[i] == 0) {
      int lo = (int)(150 * i * self->burst_spread);
      int hi = (int)(600 * i * self->burst_spread);
      self->burst_timeout[i] = g_timeout_add (
        g_random_int_range (MAX (lo, 50), MAX (hi, 100)),
        burst_fire, &self->burst_data[i]);
    }
  }

  /* Schedule next cycle immediately — overlap with current. */
  firework_schedule (self);

  return G_SOURCE_REMOVE;
}


static void
firework_schedule (KgxEdge *self)
{
  if (self->firework_timeout)
    return;

  const KgxParticleTunables *ft =
    (self->process_preset == KGX_PARTICLE_FIREWORKS)
      ? resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
      : &self->global;
  double fw_spd = ft->speed;
  int lo = (int)(600 * self->burst_spread / fw_spd);
  int hi = (int)(1200 * self->burst_spread / fw_spd);
  guint delay = (self->burst_progress[0] < 0.0 &&
                 adw_animation_get_state (self->burst_anim[0]) == ADW_ANIMATION_IDLE)
              ? 200
              : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->firework_timeout = g_timeout_add (delay, firework_fire, self);
}


/* ── ambient burst system (independent from firework) ── */

static void
ambient_burst_value_cb (double value, BurstData *bd)
{
  bd->self->ambient_progress[bd->index] = value;
  kgx_edge_mark_dirty (bd->self);
}


static void
ambient_burst_done_cb (BurstData *bd)
{
  bd->self->ambient_progress[bd->index] = -1.0;
  kgx_edge_mark_dirty (bd->self);
}


static void ambient_schedule (KgxEdge *self);


static gboolean
ambient_burst_fire (gpointer data)
{
  BurstData *bd = data;
  KgxEdge *self = bd->self;
  int i = bd->index;
  int width;
  int height;
  double perim;

  self->ambient_burst_timeout[i] = 0;

  if (!self->ambient_active || !self->ambient_enabled ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  kgx_edge_get_canvas_size (GTK_WIDGET (self), &width, &height);
  perim = 2.0 * (width + height);

  self->ambient_head[i] = g_random_double () * perim;

  self->ambient_burst_color[i] = random_muted_color ();

  /* Snapshot ambient tunables at burst-fire time. */
  {
    const KgxParticleTunables *bt = &self->preset[KGX_PARTICLE_AMBIENT - 1]; /* ambient tunables */
    self->ambient_tune_snap[i] = *bt;
    adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->ambient_anim[i]),
                                      (guint)(800.0 / bt->speed));
  }

  adw_animation_reset (self->ambient_anim[i]);
  adw_animation_play (self->ambient_anim[i]);

  return G_SOURCE_REMOVE;
}


static gboolean
ambient_fire (gpointer data)
{
  KgxEdge *self = KGX_EDGE (data);

  self->ambient_timeout = 0;

  if (!self->ambient_active || !self->ambient_enabled ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  self->ambient_burst_data[0].index = 0;
  self->ambient_burst_data[0].self  = self;
  ambient_burst_fire (&self->ambient_burst_data[0]);

  for (int i = 1; i < self->ambient_burst_count; i++) {
    self->ambient_burst_data[i].index = i;
    self->ambient_burst_data[i].self  = self;
    if (self->ambient_burst_timeout[i] == 0) {
      int lo = (int)(150 * i * self->ambient_burst_spread);
      int hi = (int)(600 * i * self->ambient_burst_spread);
      self->ambient_burst_timeout[i] = g_timeout_add (
        g_random_int_range (MAX (lo, 50), MAX (hi, 100)),
        ambient_burst_fire, &self->ambient_burst_data[i]);
    }
  }

  ambient_schedule (self);
  return G_SOURCE_REMOVE;
}


static void
ambient_schedule (KgxEdge *self)
{
  if (self->ambient_timeout)
    return;

  const KgxParticleTunables *ft = &self->preset[KGX_PARTICLE_AMBIENT - 1]; /* ambient tunables */
  double spd = ft->speed;
  int lo = (int)(600 * self->ambient_burst_spread / spd);
  int hi = (int)(1200 * self->ambient_burst_spread / spd);
  guint delay = (self->ambient_progress[0] < 0.0 &&
                 adw_animation_get_state (self->ambient_anim[0]) == ADW_ANIMATION_IDLE)
              ? 200
              : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->ambient_timeout = g_timeout_add (delay, ambient_fire, self);
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
    GdkRGBA color = resolve_color_cached (root, root->overscroll_color,
                                          &root->overscroll_rgba,
                                          &root->overscroll_rgba_valid);

    const KgxParticleTunables *os_tune =
      (root->overscroll_style == 1) ? resolve_tunables (root, KGX_PARTICLE_SCROLL2)
                                    : &root->global;
    draw_overscroll (snapshot, root->overscroll_progress,
                     root->overscroll_edge, root->overscroll_style,
                     (root->overscroll_reverse == 2)
                       ? root->overscroll_reverse_toggle
                       : (root->overscroll_reverse == 1),
                     width, height, perim, &color, os_tune,
                     self->side, strip_extent, draw_budget, tri, dia);
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
                          ? envelope (bp, 0.0, 0.02, 2)
                          : envelope (p, bt->env_attack, bt->env_release, bt->env_curve);
        double seg_env = envelope (bp, bt->env_attack, 0.0, bt->env_curve);
        double seg   = BASE_OVERSCROLL_SEG * bt->tail_length * 2.0 * seg_env;
        float  a     = b_env * 0.5f;
        double spread = seg * 3.0 * bp;
        double left_head  = fmod (root->burst_head[i] - spread + perim, perim);
        double right_head = fmod (root->burst_head[i] + spread, perim);
        int l_trail = implode ? -1 : +1;
        int r_trail = implode ? +1 : -1;

        draw_segment (snapshot, left_head, seg, a,
                      width, height, perim, &root->burst_color[i],
                      l_trail, bt, p, self->side, strip_extent, draw_budget, tri, dia);
        draw_segment (snapshot, right_head, seg, a,
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
        float  b_env = envelope (p, abt->env_attack, abt->env_release, abt->env_curve);
        double seg_env = envelope (p, abt->env_attack, 0.0, abt->env_curve);
        double seg   = BASE_OVERSCROLL_SEG * abt->tail_length * 2.0 * seg_env;
        float  a     = b_env * 0.5f;
        double spread = seg * 3.0 * p;
        double left_head  = fmod (root->ambient_head[i] - spread + perim, perim);
        double right_head = fmod (root->ambient_head[i] + spread, perim);

        draw_segment (snapshot, left_head, seg, a,
                      width, height, perim, &root->ambient_burst_color[i],
                      +1, abt, p, self->side, strip_extent, draw_budget, tri, dia);
        draw_segment (snapshot, right_head, seg, a,
                      width, height, perim, &root->ambient_burst_color[i],
                      -1, abt, p, self->side, strip_extent, draw_budget, tri, dia);
      }
    }
    }
  }

  /* Process-specific particle — suppressed when settings page is open,
   * EXCEPT during the brief graceful falloff (pending_change in progress).
   * Once the pending fires and stops the animation, suppression kicks in.
   * Uses snapshotted tunables captured at animation start. */
  if (root->process_progress >= 0.0 &&
      root->process_preset != KGX_PARTICLE_NONE) {
    const KgxParticleTunables *pt = &root->process_tune_snap;

    /* Use AdwAnimation-driven progress when it's advancing normally.
     * Fall back to wall clock only when progress hasn't changed between
     * snapshots (frame clock stall during tab-switch layout). */
    double p;
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
    float  env   = envelope (p, pt->env_attack, pt->env_release, pt->env_curve);
    float  a     = env * 0.8f;

    double seg_full = BASE_OVERSCROLL_SEG * pt->tail_length * 2.0;
    /* Tail envelope: grows during attack. In retract mode, shrinks during release.
     * Rotate keeps full tail -- it's always in motion. */
    double tail_env;
    if (root->process_preset == KGX_PARTICLE_ROTATE)
      tail_env = 1.0;
    else if (pt->release_mode == KGX_RELEASE_RETRACT) {
      /* Retract faster than alpha fades so the pull-back is visible.
       * Use concave curve (fast drop) regardless of user curve setting. */
      float raw = envelope (p, pt->env_attack, pt->env_release, 1);
      tail_env = (double)(raw * raw);  /* squared for aggressive retraction */
    } else
      tail_env = envelope (p, pt->env_attack, 0.0, pt->env_curve);  /* uniform/spread/grow: attack only */
    double seg = seg_full * tail_env;

    switch (root->process_preset) {
    case KGX_PARTICLE_CORNERS: {
      double corner_a, corner_b;
      if (root->process_reverse) {
        corner_a = (double) width;
        corner_b = (double)(2 * width + height);
      } else {
        corner_a = 0.0;
        corner_b = (double)(width + height);
      }

      double travel = (width + height) / 2.0 * p;
      double clamped_seg = MIN (seg, travel);
      double a_cw  = fmod (corner_a + travel, perim);
      double a_ccw = fmod (corner_a - travel + perim * 2, perim);
      double b_cw  = fmod (corner_b + travel, perim);
      double b_ccw = fmod (corner_b - travel + perim * 2, perim);

      draw_segment (snapshot, a_cw,  clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      draw_segment (snapshot, a_ccw, clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      draw_segment (snapshot, b_cw,  clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      draw_segment (snapshot, b_ccw, clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_PULSE_OUT: {
      double center = root->process_reverse
                        ? (double)width + height + width / 2.0
                        : width / 2.0;
      double spread = (perim / 4.0) * p;
      double clamped_seg = MIN (seg, spread);
      double left_head  = fmod (center - spread + perim, perim);
      double right_head = fmod (center + spread, perim);

      draw_segment (snapshot, left_head,  clamped_seg, a, width, height, perim, &root->process_color, +1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      draw_segment (snapshot, right_head, clamped_seg, a, width, height, perim, &root->process_color, -1, pt, p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_ROTATE: {
      int dir   = root->process_reverse ? -1 : +1;
      int trail = root->process_reverse ? +1 : -1;

      double half_p   = fmod (p * 2.0, 1.0);
      double eased    = 1.0 - (1.0 - half_p) * (1.0 - half_p) * (1.0 - half_p);
      float  lap_env  = envelope (half_p, pt->env_attack, pt->env_release, pt->env_curve);
      float  lap_a    = lap_env * 0.8f;
      double lap_tail;
      if (pt->release_mode == KGX_RELEASE_RETRACT) {
        float raw = envelope (half_p, pt->env_attack, pt->env_release, 1);
        lap_tail = (double)(raw * raw);
      } else
        lap_tail = envelope (half_p, pt->env_attack, 0.0, pt->env_curve);
      double lap_seg  = seg_full * lap_tail;

      double travel = (perim / 2.0) * eased;
      double clamped_seg = MIN (lap_seg * 2.0, travel);
      double offset = (p >= 0.5) ? perim / 2.0 : 0.0;
      double head   = fmod (dir * travel + offset + perim, perim);

      draw_segment (snapshot, head, clamped_seg, lap_a,
                    width, height, perim, &root->process_color, trail, pt, half_p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
    case KGX_PARTICLE_PING_PONG: {
      double edge_start = root->process_reverse
                            ? (double)(width + height) : 0.0;

      double half_p = fmod (p * 2.0, 1.0);
      gboolean returning = (p >= 0.5);
      float  pp_env = envelope (half_p, pt->env_attack, pt->env_release, pt->env_curve);
      float  pp_a   = pp_env * 0.9f;
      double pp_tail;
      if (pt->release_mode == KGX_RELEASE_RETRACT) {
        float raw = envelope (half_p, pt->env_attack, pt->env_release, 1);
        pp_tail = (double)(raw * raw);
      } else
        pp_tail = envelope (half_p, pt->env_attack, 0.0, pt->env_curve);
      double pp_seg = BASE_OVERSCROLL_SEG * pp_tail;

      double travel = (double) width * half_p;
      double clamped_seg = MIN (pp_seg, travel);
      double pos;
      int trail;
      if (!returning) {
        pos   = fmod (edge_start + travel + perim, perim);
        trail = -1;
      } else {
        pos   = fmod (edge_start + (double) width * (1.0 - half_p) + perim, perim);
        trail = +1;
      }

      draw_segment (snapshot, pos, clamped_seg, pp_a,
                    width, height, perim, &root->process_color, trail, pt, half_p, self->side, strip_extent, draw_budget, tri, dia);
      break;
    }
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

  if (firework_active (self) && self->burst_progress[0] < 0.0)
    firework_schedule (self);

  if (self->ambient_active && self->ambient_enabled &&
      self->ambient_progress[0] < 0.0)
    ambient_schedule (self);

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
    if (field == TUNE_THICKNESS)
      state->preset[pi].thickness = CLAMP (g_value_get_int (value), 2, 40);
    else if (field == TUNE_RELEASE_MODE)
      state->preset[pi].release_mode = CLAMP (g_value_get_int (value), 0, 3);
    else if (field == TUNE_SHAPE)
      state->preset[pi].shape = CLAMP (g_value_get_int (value), 0, 3);
    else if (field == TUNE_ENV_CURVE)
      state->preset[pi].env_curve = CLAMP (g_value_get_int (value), 1, 3);
    else if (field == TUNE_GAP)
      state->preset[pi].gap = CLAMP (g_value_get_int (value), 0, 1);
    else if (field == TUNE_THK_RELEASE_MODE)
      state->preset[pi].thk_release_mode = CLAMP (g_value_get_int (value), 0, 4);
    else if (field == TUNE_THK_CURVE)
      state->preset[pi].thk_curve = CLAMP (g_value_get_int (value), 1, 3);
    else
      set_tunable_double (&state->preset[pi], field, g_value_get_double (value));
    if (field == TUNE_THICKNESS)
      kgx_edge_queue_resize_views (state);
    if (kgx_edge_has_visible_content (state))
      kgx_edge_mark_dirty (state);
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    if (field == TUNE_THICKNESS) {
      state->global.thickness = g_value_get_int (value);
    } else if (field == TUNE_RELEASE_MODE) {
      state->global.release_mode = CLAMP (g_value_get_int (value), 0, 3);
    } else if (field == TUNE_SHAPE) {
      state->global.shape = CLAMP (g_value_get_int (value), 0, 3);
    } else if (field == TUNE_ENV_CURVE) {
      state->global.env_curve = CLAMP (g_value_get_int (value), 1, 3);
    } else if (field == TUNE_GAP) {
      state->global.gap = CLAMP (g_value_get_int (value), 0, 1);
    } else if (field == TUNE_THK_RELEASE_MODE) {
      state->global.thk_release_mode = CLAMP (g_value_get_int (value), 0, 4);
    } else if (field == TUNE_THK_CURVE) {
      state->global.thk_curve = CLAMP (g_value_get_int (value), 1, 3);
    } else if (field == TUNE_SPEED) {
      state->global.speed = g_value_get_double (value);
      /* Update overscroll / burst animation durations */
      if (state->overscroll_anim)
        adw_timed_animation_set_duration (
          ADW_TIMED_ANIMATION (state->overscroll_anim),
          (guint)(BASE_OVERSCROLL_MS / state->global.speed));
      for (int i = 0; i < MAX_BURSTS; i++) {
        if (state->burst_anim[i])
          adw_timed_animation_set_duration (
            ADW_TIMED_ANIMATION (state->burst_anim[i]),
            (guint)(800.0 / state->global.speed));
      }
    } else {
      set_tunable_double (&state->global, field, g_value_get_double (value));
    }
    if (field == TUNE_THICKNESS)
      kgx_edge_queue_resize_views (state);
    if (kgx_edge_has_visible_content (state))
      kgx_edge_mark_dirty (state);
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
    if (field == TUNE_THICKNESS)
      g_value_set_int (value, state->preset[pi].thickness);
    else if (field == TUNE_RELEASE_MODE)
      g_value_set_int (value, state->preset[pi].release_mode);
    else if (field == TUNE_SHAPE)
      g_value_set_int (value, state->preset[pi].shape);
    else if (field == TUNE_ENV_CURVE)
      g_value_set_int (value, state->preset[pi].env_curve);
    else if (field == TUNE_THK_CURVE)
      g_value_set_int (value, state->preset[pi].thk_curve);
    else
      g_value_set_double (value, get_tunable_double (&state->preset[pi], field));
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    if (field == TUNE_THICKNESS)
      g_value_set_int (value, state->global.thickness);
    else if (field == TUNE_RELEASE_MODE)
      g_value_set_int (value, state->global.release_mode);
    else if (field == TUNE_SHAPE)
      g_value_set_int (value, state->global.shape);
    else if (field == TUNE_ENV_CURVE)
      g_value_set_int (value, state->global.env_curve);
    else if (field == TUNE_THK_CURVE)
      g_value_set_int (value, state->global.thk_curve);
    else
      g_value_set_double (value, get_tunable_double (&state->global, field));
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
  for (int i = 0; i < MAX_BURSTS; i++)
    g_clear_object (&self->burst_anim[i]);
  for (int i = 0; i < MAX_BURSTS; i++)
    g_clear_object (&self->ambient_anim[i]);
  g_clear_object (&self->master);
  g_clear_pointer (&self->overscroll_color, g_free);
  g_clear_pointer (&self->unit_triangle, gsk_path_unref);
  g_clear_pointer (&self->unit_diamond, gsk_path_unref);

  if (self->firework_timeout) {
    g_source_remove (self->firework_timeout);
    self->firework_timeout = 0;
  }
  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_timeout[i]) {
      g_source_remove (self->burst_timeout[i]);
      self->burst_timeout[i] = 0;
    }
  }

  if (self->ambient_timeout) {
    g_source_remove (self->ambient_timeout);
    self->ambient_timeout = 0;
  }
  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->ambient_burst_timeout[i]) {
      g_source_remove (self->ambient_burst_timeout[i]);
      self->ambient_burst_timeout[i] = 0;
    }
  }

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

  /* Burst animations for firework effect. */
  for (int i = 0; i < MAX_BURSTS; i++) {
    BurstData *bd = &self->burst_data[i];
    bd->index = i;
    bd->self  = self;

    target = adw_callback_animation_target_new (
        (AdwAnimationTargetFunc) burst_value_cb, bd, NULL);

    self->burst_anim[i] = adw_timed_animation_new (GTK_WIDGET (self),
                                                    0.0, 1.0, 800,
                                                    target);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->burst_anim[i]),
                                    ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->burst_anim[i], "done",
                              G_CALLBACK (burst_done_cb), bd);

    self->burst_progress[i] = -1.0;
  }

  /* Ambient burst animations — independent from firework. */
  for (int i = 0; i < MAX_BURSTS; i++) {
    BurstData *abd = &self->ambient_burst_data[i];
    abd->index = i;
    abd->self  = self;

    target = adw_callback_animation_target_new (
        (AdwAnimationTargetFunc) ambient_burst_value_cb, abd, NULL);

    self->ambient_anim[i] = adw_timed_animation_new (GTK_WIDGET (self),
                                                      0.0, 1.0, 800,
                                                      target);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->ambient_anim[i]),
                                    ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->ambient_anim[i], "done",
                              G_CALLBACK (ambient_burst_done_cb), abd);

    self->ambient_progress[i] = -1.0;
  }
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
kgx_edge_set_ambient (KgxEdge  *self,
                      gboolean  ambient)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  if (self->ambient_active == ambient)
    return;

  self->ambient_active = ambient;

  if (ambient) {
    if (!self->ambient_enabled)
      return;

    /* Start ambient bursts alongside any running process particle. */
    if (!self->ambient_timeout)
      self->ambient_timeout = g_timeout_add (50, ambient_fire, self);
  } else {
    /* Stop ambient burst timers. */
    if (self->ambient_timeout) {
      g_source_remove (self->ambient_timeout);
      self->ambient_timeout = 0;
    }
    for (int i = 0; i < MAX_BURSTS; i++) {
      if (self->ambient_burst_timeout[i]) {
        g_source_remove (self->ambient_burst_timeout[i]);
        self->ambient_burst_timeout[i] = 0;
      }
    }
    /* In-flight ambient_anim[] play out naturally (graceful finish). */
  }
}


/* ── process config parser ───────────────────────────────── */

static KgxParticlePreset
preset_from_string (const char *s)
{
  if (!s || !s[0])            return KGX_PARTICLE_NONE;
  if (g_str_equal (s, "fireworks"))  return KGX_PARTICLE_FIREWORKS;
  if (g_str_equal (s, "corners"))    return KGX_PARTICLE_CORNERS;
  if (g_str_equal (s, "pulse-out"))  return KGX_PARTICLE_PULSE_OUT;
  if (g_str_equal (s, "rotate"))     return KGX_PARTICLE_ROTATE;
  if (g_str_equal (s, "ping-pong"))  return KGX_PARTICLE_PING_PONG;
  if (g_str_equal (s, "ambient"))    return KGX_PARTICLE_AMBIENT;
  if (g_str_equal (s, "none"))       return KGX_PARTICLE_NONE;
  return KGX_PARTICLE_NONE;
}


const char *
kgx_particle_preset_to_string (KgxParticlePreset p)
{
  switch (p) {
  case KGX_PARTICLE_FIREWORKS: return "fireworks";
  case KGX_PARTICLE_CORNERS:   return "corners";
  case KGX_PARTICLE_PULSE_OUT: return "pulse-out";
  case KGX_PARTICLE_ROTATE:    return "rotate";
  case KGX_PARTICLE_PING_PONG: return "ping-pong";
  case KGX_PARTICLE_AMBIENT:   return "ambient";
  default:                     return "none";
  }
}


void
kgx_parse_process_config (const char        *value,
                           char             **glass_color,
                           KgxParticlePreset *preset,
                           int               *reverse,
                           GdkRGBA           *particle_color,
                           int               *shape_override,
                           int               *gap_override,
                           int               *speed_override,
                           int               *thk_override)
{
  g_auto (GStrv) parts = NULL;

  if (glass_color)    *glass_color = NULL;
  if (preset)         *preset = KGX_PARTICLE_NONE;
  if (reverse)        *reverse = 0;
  if (particle_color) *particle_color = (GdkRGBA) { 0.5f, 0.5f, 0.5f, 1.0f };
  if (shape_override) *shape_override = -1;
  if (gap_override)   *gap_override = -1;
  if (speed_override) *speed_override = 0;
  if (thk_override)   *thk_override = 0;

  if (!value || !value[0])
    return;

  parts = g_strsplit (value, ";", 9);
  int n = g_strv_length (parts);

  if (n >= 1 && glass_color)
    *glass_color = g_strdup (parts[0]);

  if (n >= 2 && preset)
    *preset = preset_from_string (parts[1]);

  if (n >= 3 && reverse)
    *reverse = atoi (parts[2]);

  if (n >= 4 && particle_color)
    gdk_rgba_parse (particle_color, parts[3]);

  if (n >= 5 && shape_override)
    *shape_override = atoi (parts[4]);

  if (n >= 6 && gap_override)
    *gap_override = atoi (parts[5]);

  if (n >= 7 && speed_override)
    *speed_override = atoi (parts[6]);

  if (n >= 8 && thk_override)
    *thk_override = atoi (parts[7]);
}


/* ── process particle callbacks ──────────────────────────── */

static void
process_particle_value_cb (double value, KgxEdge *self)
{
  /* When a pending change is queued, skip to end so the done callback
   * can apply the new preset.  Looping presets (LINEAR easing) have no
   * meaningful deceleration — skip immediately.  One-shot presets use
   * ease-out-cubic where 0.5 marks the start of the deceleration tail,
   * so wait until then for a graceful handoff. */
  if (self->pending_change && (self->process_linear || value >= 0.5)) {
    adw_animation_skip (self->process_anim);
    return;
  }

  self->process_progress = value;
  kgx_edge_mark_dirty (self);
}


static void
process_particle_done_cb (KgxEdge *self)
{
  int old_extent = kgx_edge_get_strip_extent (self);

  /* If a preset change is pending, apply it now that the current
   * animation has finished gracefully.  Don't set progress to -1
   * here — that creates a one-frame gap where no particle is drawn.
   * The recursive set_process_particle seeds progress = 0 so the
   * new preset is visible immediately. */
  if (self->pending_change) {
    self->pending_change = FALSE;
    kgx_edge_set_process_particle (self,
                                   self->pending_preset,
                                   &self->pending_color,
                                   self->pending_reverse,
                                   self->pending_shape_override,
                                   self->pending_gap_override,
                                   self->pending_speed_override,
                                   self->pending_thk_override);
    return;
  }

  /* Loop when: preset naturally loops (Rotate/Ping-Pong), OR alternating
   * mode is active (any preset fires forward then reverse repeatedly). */
  if (self->process_preset == KGX_PARTICLE_ROTATE ||
      self->process_preset == KGX_PARTICLE_PING_PONG ||
      self->process_reverse_mode == 2) {
    /* Toggle direction each cycle in alternating mode */
    if (self->process_reverse_mode == 2)
      self->process_reverse = !self->process_reverse;

    self->process_tune_snap = *resolve_tunables (self, self->process_preset);
    /* Re-apply per-app overrides after re-snapshot */
    if (self->process_shape_override >= 0)
      self->process_tune_snap.shape = self->process_shape_override;
    if (self->process_gap_override >= 0)
      self->process_tune_snap.gap = self->process_gap_override;
    if (self->process_speed_override > 0)
      self->process_tune_snap.speed = self->process_speed_override / 100.0;
    if (self->process_thk_override > 0)
      self->process_tune_snap.thickness = self->process_thk_override;
    self->process_start_us = g_get_monotonic_time ();
    adw_animation_reset (self->process_anim);
    adw_animation_play (self->process_anim);
    return;
  }

  self->process_progress = -1.0;
  self->process_start_us = 0;
  kgx_edge_queue_resize_views_if_needed (self, old_extent);
  kgx_edge_mark_dirty (self);
}


void
kgx_edge_set_process_particle (KgxEdge          *self,
                               KgxParticlePreset  preset,
                               const GdkRGBA     *color,
                               int                reverse,
                               int                shape_override,
                               int                gap_override,
                               int                speed_override,
                               int                thk_override)
{
  int old_extent;

  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);
  old_extent = kgx_edge_get_strip_extent (self);

  /* If an animation is currently playing and the preset is changing,
   * queue the change as pending so the current cycle finishes gracefully.
   * Keep the original easing — switching to LINEAR mid-animation causes a
   * visible position jump because the time→value mapping changes abruptly
   * (e.g. ease-out-cubic at t=0.5 ≈ 0.875, LINEAR at t=0.5 = 0.5).
   * The natural deceleration is fine now that tunables are snapshotted. */
  if (self->process_progress >= 0.0 &&
      self->process_anim &&
      adw_animation_get_state (self->process_anim) == ADW_ANIMATION_PLAYING &&
      (preset != self->process_preset || preset == KGX_PARTICLE_NONE)) {
    self->pending_change = TRUE;
    self->pending_preset = preset;
    self->pending_reverse = reverse;
    self->pending_shape_override = shape_override;
    self->pending_gap_override = gap_override;
    self->pending_speed_override = speed_override;
    self->pending_thk_override = thk_override;
    if (color)
      self->pending_color = *color;
    else
      self->pending_color = (GdkRGBA) { 0.5f, 0.5f, 0.5f, 1.0f };
    return;
  }

  self->pending_change = FALSE;
  self->process_preset = preset;
  self->process_shape_override = shape_override;
  self->process_gap_override = gap_override;
  self->process_speed_override = speed_override;
  self->process_thk_override = thk_override;

  /* Resolve reverse mode: 0=forward, 1=reverse, 2=alternating */
  self->process_reverse_mode = reverse;
  if (reverse == 2) {
    self->process_reverse = self->process_reverse_toggle;
    self->process_reverse_toggle = !self->process_reverse_toggle;
  } else {
    self->process_reverse = (reverse == 1);
  }

  if (color)
    self->process_color = *color;

  if (preset == KGX_PARTICLE_NONE) {
    self->process_progress = -1.0;
    self->process_start_us = 0;
    if (self->process_anim)
      adw_animation_reset (self->process_anim);
    /* Stop firework cycle and all in-flight bursts */
    if (self->firework_timeout) {
      g_source_remove (self->firework_timeout);
      self->firework_timeout = 0;
    }
    for (int i = 0; i < MAX_BURSTS; i++) {
      if (self->burst_timeout[i]) {
        g_source_remove (self->burst_timeout[i]);
        self->burst_timeout[i] = 0;
      }
      self->burst_progress[i] = -1.0;
      if (self->burst_anim[i])
        adw_animation_reset (self->burst_anim[i]);
    }
    kgx_edge_queue_resize_views_if_needed (self, old_extent);
    kgx_edge_mark_dirty (self);
    return;
  }

  /* Fireworks and Ambient reuse the burst animation system */
  if (preset == KGX_PARTICLE_FIREWORKS || preset == KGX_PARTICLE_AMBIENT) {
    if (gtk_widget_get_mapped (GTK_WIDGET (self)) && !self->firework_timeout) {
      self->burst_data[0].index = 0;
      self->burst_data[0].self  = self;
      burst_fire (&self->burst_data[0]);
      firework_schedule (self);
    }
    kgx_edge_queue_resize_views_if_needed (self, old_extent);
    return;
  }

  /* Create animation on first use */
  if (!self->process_anim) {
    AdwAnimationTarget *t = adw_callback_animation_target_new (
        (AdwAnimationTargetFunc) process_particle_value_cb, self, NULL);
    self->process_anim = adw_timed_animation_new (GTK_WIDGET (self),
                                                   0.0, 1.0, 2000, t);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->process_anim),
                                    ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->process_anim, "done",
                              G_CALLBACK (process_particle_done_cb), self);
  }

  /* Snapshot tunables at animation start so in-flight rendering is
   * decoupled from live state.  Re-snapshotted on loop restart. */
  self->process_tune_snap = *resolve_tunables (self, preset);

  /* Apply per-app overrides on top of the snapshot */
  if (shape_override >= 0)
    self->process_tune_snap.shape = shape_override;
  if (gap_override >= 0)
    self->process_tune_snap.gap = gap_override;
  if (speed_override > 0)
    self->process_tune_snap.speed = speed_override / 100.0;
  if (thk_override > 0)
    self->process_tune_snap.thickness = thk_override;

  /* Always update easing and duration — speed may have changed */
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->process_anim),
                                  (preset == KGX_PARTICLE_ROTATE ||
                                   preset == KGX_PARTICLE_PING_PONG)
                                    ? ADW_LINEAR
                                    : ADW_EASE_OUT_CUBIC);

  double spd = self->process_tune_snap.speed;
  guint duration;
  switch (preset) {
  case KGX_PARTICLE_ROTATE:    duration = (guint)(3500.0 / spd); break;
  case KGX_PARTICLE_PING_PONG: duration = (guint)(1200.0 / spd); break;
  case KGX_PARTICLE_CORNERS:   duration = (guint)(2500.0 / spd); break;
  case KGX_PARTICLE_PULSE_OUT: duration = (guint)(2500.0 / spd); break;
  default:                     duration = (guint)(3000.0 / spd); break;
  }
  adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->process_anim),
                                    duration);

  /* Don't restart if already animating — duration update above takes
   * effect on the next loop cycle for looping presets. */
  if (self->process_progress >= 0.0 &&
      adw_animation_get_state (self->process_anim) == ADW_ANIMATION_PLAYING)
    return;

  self->process_progress = 0.0;
  self->process_start_us = g_get_monotonic_time ();
  self->process_duration_s = duration / 1000.0;
  self->process_linear = (preset == KGX_PARTICLE_ROTATE ||
                          preset == KGX_PARTICLE_PING_PONG);
  adw_animation_reset (self->process_anim);
  adw_animation_play (self->process_anim);
  kgx_edge_queue_resize_views_if_needed (self, old_extent);
  kgx_edge_mark_dirty (self);
}
