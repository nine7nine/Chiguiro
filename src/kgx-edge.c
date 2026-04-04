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

typedef struct _KgxEdge KgxEdge;
typedef struct { int index; KgxEdge *self; } BurstData;


struct _KgxEdge {
  GtkWidget       parent_instance;

  /* ── configurable properties ───────────────────────────── */
  gboolean        overscroll_enabled;
  char           *overscroll_color;     /* hex string, empty = accent */
  gboolean        privilege_enabled;
  char           *privilege_color;      /* hex string */
  double          burst_spread;         /* multiplier on burst stagger */
  int             burst_count;          /* how many fire per cycle (1-MAX_BURSTS) */
  int             overscroll_style;     /* 0 = center, 1 = corner */
  int             privilege_direction;  /* 0 = clockwise, 1 = counter-clockwise */

  /* ── particle tunables ────────────────────────────────── */
  KgxParticleTunables global;           /* overscroll / privilege fallback */
  KgxParticleTunables preset[N_PRESETS]; /* indexed by KgxParticlePreset - 1 */

  /* ── overscroll state ──────────────────────────────────── */
  AdwAnimation   *overscroll_anim;
  double          overscroll_progress;   /* –1 = idle */
  GtkPositionType overscroll_edge;

  /* ── firework / privilege mode ───────────────────────────── */
  gboolean        privileged;
  gboolean        firework_settings;   /* settings page triggered */
  gboolean        firework_privilege;  /* privilege triggered */
#define MAX_BURSTS 8
#define firework_active(s) ((s)->firework_settings || (s)->firework_privilege || \
                            (s)->process_preset == KGX_PARTICLE_FIREWORKS)

  guint           firework_timeout;
  AdwAnimation   *burst_anim[MAX_BURSTS];
  double          burst_progress[MAX_BURSTS];
  double          burst_head[MAX_BURSTS];
  GdkRGBA         burst_color[MAX_BURSTS];
  guint           burst_timeout[MAX_BURSTS];
  BurstData       burst_data[MAX_BURSTS];

  /* ── process-specific particle ─────────────────────────── */
  KgxParticlePreset process_preset;
  GdkRGBA           process_color;
  gboolean          process_reverse;
  AdwAnimation     *process_anim;
  double            process_progress;   /* -1 = idle, 0..1 = active */

  /* ── pending preset (for graceful transitions) ──────────── */
  gboolean          pending_change;
  KgxParticlePreset pending_preset;
  GdkRGBA           pending_color;
  gboolean          pending_reverse;
};


enum {
  PROP_0,
  PROP_OVERSCROLL_ENABLED,
  PROP_OVERSCROLL_COLOR,
  PROP_PRIVILEGE_ENABLED,
  PROP_PRIVILEGE_COLOR,
  PROP_OVERSCROLL_STYLE,
  PROP_BURST_SPREAD,
  PROP_BURST_COUNT,
  PROP_PRIVILEGE_DIRECTION,
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
  "env-attack", "env-release", "release-mode", "shape"
};
static const char * const preset_suffixes[N_PRESETS] = {
  "fireworks", "corners", "pulse-out", "rotate", "ping-pong"
};


G_DEFINE_FINAL_TYPE (KgxEdge, kgx_edge, GTK_TYPE_WIDGET)


/* ── tunables helpers ───────────────────────────────────── */

static inline const KgxParticleTunables *
resolve_tunables (KgxEdge *self, KgxParticlePreset preset)
{
  if (preset >= KGX_PARTICLE_FIREWORKS && preset <= KGX_PARTICLE_PING_PONG)
    return &self->preset[preset - 1];
  return &self->global;
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
  default:                return 0.0;
  }
}

/* Compute envelope value at normalized time t (0..1).
 * Returns 0..1: ramps up during attack, holds at 1, fades during release. */
static inline float
envelope (double t, double attack, double release)
{
  if (attack > 0.0 && t < attack)
    return (float)(t / attack);
  if (release > 0.0 && t > 1.0 - release)
    return (float)((1.0 - t) / release);
  return 1.0f;
}


/* ── colour helpers ──────────────────────────────────────── */

static GdkRGBA
resolve_color (KgxEdge *self, const char *hex_or_empty)
{
  GdkRGBA color = { 0, 0, 0, 1 };

  if (hex_or_empty && hex_or_empty[0] != '\0') {
    gdk_rgba_parse (&color, hex_or_empty);
    color.alpha = 1.0f;
    return color;
  }

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
                float          px,
                float          py,
                float          size,
                const GdkRGBA *color)
{
  float half = size / 2.0f;
  float inner = size * 0.707f;  /* 1/sqrt(2) — fits in same bounding box */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot,
                          &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_rotate (snapshot, 45.0f);
  gtk_snapshot_append_color (snapshot, color,
    &GRAPHENE_RECT_INIT (-inner / 2, -inner / 2, inner, inner));
  gtk_snapshot_restore (snapshot);
}

/* Triangle pointing in direction of motion.
 * angle: 0=right, 90=down, 180=left, 270=up */
static void
append_triangle (GtkSnapshot   *snapshot,
                 float          px,
                 float          py,
                 float          size,
                 float          angle,
                 const GdkRGBA *color)
{
  float half = size / 2.0f;
  /* Build a right-pointing triangle centered at origin, then rotate */
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_rotate (snapshot, angle);

  GskPathBuilder *builder = gsk_path_builder_new ();
  gsk_path_builder_move_to (builder, -half, -half);        /* top-left */
  gsk_path_builder_line_to (builder,  half,  0);           /* right-center (tip) */
  gsk_path_builder_line_to (builder, -half,  half);        /* bottom-left */
  gsk_path_builder_close (builder);

  GskPath *path = gsk_path_builder_free_to_path (builder);
  gtk_snapshot_append_fill (snapshot, path, GSK_FILL_RULE_WINDING, color);
  gsk_path_unref (path);
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
              const GdkRGBA              *color,
              int                         trail_dir,
              const KgxParticleTunables  *tune,
              double                      phase)
{
  double blk   = (double) tune->thickness;
  double gap   = blk;
  double perim = 2.0 * (width + height);
  double step  = blk + gap;
  int    blocks = (int)(seg_len / step);

  if (blocks < 1) blocks = 1;

  for (int s = 0; s < blocks; s++) {
    double d = fmod (head_d + trail_dir * s * step + perim * 2, perim);
    /* Head (s=0) is bright, tail (high s) fades out. */
    float  t = (float) s / (blocks - 1 > 0 ? blocks - 1 : 1);
    /* Base fade: head bright, tail dims. */
    float  a = alpha * (1.0f - 0.7f * t);
    /* Pulsing shimmer on tail only — skip the leading block (s=0),
     * strengthen pulse further from head, wave travels backward. */
    if (s > 0) {
      float intensity = t * (float) tune->pulse_depth;
      float pulse = 1.0f - intensity
                    + intensity * sinf ((float) s * 1.2f - (float)(phase * 40.0 * tune->pulse_speed));
      a *= pulse;
    }
    GdkRGBA c = *color;
    float  px, py, bw, bh;

    c.alpha = a;

    float tri_angle = 0;
    if (d < width) {
      /* Top edge — CW = right, CCW = left */
      px = (float) d;  py = 0;
      bw = (float) blk;  bh = (float) blk;
      tri_angle = (trail_dir == -1) ? 0 : 180;
    } else if (d < width + height) {
      /* Right edge — CW = down, CCW = up */
      px = width - (float) blk;  py = (float)(d - width);
      bw = (float) blk;  bh = (float) blk;
      tri_angle = (trail_dir == -1) ? 90 : 270;
    } else if (d < 2 * width + height) {
      /* Bottom edge — CW = left, CCW = right */
      px = width - (float)(d - width - height) - (float) blk;
      py = height - (float) blk;
      bw = (float) blk;  bh = (float) blk;
      tri_angle = (trail_dir == -1) ? 180 : 0;
    } else {
      /* Left edge — CW = up, CCW = down */
      px = 0;
      py = height - (float)(d - 2 * width - height) - (float) blk;
      bw = (float) blk;  bh = (float) blk;
      tri_angle = (trail_dir == -1) ? 270 : 90;
    }

    switch (tune->shape) {
    case KGX_PARTICLE_SHAPE_CIRCLE:
      append_circle (snapshot, px, py, (float) blk, &c);
      break;
    case KGX_PARTICLE_SHAPE_DIAMOND:
      append_diamond (snapshot, px, py, (float) blk, &c);
      break;
    case KGX_PARTICLE_SHAPE_TRIANGLE:
      append_triangle (snapshot, px, py, (float) blk, tri_angle, &c);
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
                 float                       width,
                 float                       height,
                 const GdkRGBA              *color,
                 const KgxParticleTunables  *tune)
{
  double perim  = 2.0 * (width + height);
  float  env    = envelope (progress, tune->env_attack, tune->env_release);
  float  a      = env * 0.9f;

  if (style == 1) {
    /* Corner mode: burst from corner along two adjacent edges. */
    double corner;
    double h_head, v_head;

    if (edge == GTK_POS_BOTTOM) {
      /* Bottom-right corner → left along bottom, up along right. */
      corner = width + height;
      h_head = fmod (corner + width * progress, perim);   /* left along bottom */
      v_head = fmod (corner - height * progress + perim, perim); /* up along right */
    } else {
      /* Top-right corner → left along top, down along right. */
      corner = width;
      h_head = fmod (corner - width * progress + perim, perim);  /* left along top */
      v_head = fmod (corner + height * progress, perim);  /* down along right */
    }

    /* Trail must oppose the head's travel direction:
     * bottom: h_head moves CW (trail -1), v_head moves CCW (trail +1)
     * top:    h_head moves CCW (trail +1), v_head moves CW (trail -1) */
    {
      int h_trail = (edge == GTK_POS_BOTTOM) ? -1 : +1;
      int v_trail = (edge == GTK_POS_BOTTOM) ? +1 : -1;
      draw_segment (snapshot, h_head, BASE_OVERSCROLL_SEG, a,
                    width, height, color, h_trail, tune, progress);
      draw_segment (snapshot, v_head, BASE_OVERSCROLL_SEG, a,
                    width, height, color, v_trail, tune, progress);
    }
  } else {
    /* Center mode: burst from center of edge, two snakes split outward. */
    double center;
    double left_head, right_head;

    if (edge == GTK_POS_TOP) {
      center = width / 2.0;
    } else {
      center = width + height + width / 2.0;
    }

    left_head  = fmod (center - (width / 2.0) * progress + perim, perim);
    right_head = fmod (center + (width / 2.0) * progress, perim);

    draw_segment (snapshot, left_head, BASE_OVERSCROLL_SEG, a,
                  width, height, color, +1, tune, progress);
    draw_segment (snapshot, right_head, BASE_OVERSCROLL_SEG, a,
                  width, height, color, -1, tune, progress);
  }
}


/* ── privilege snake drawing ─────────────────────────────── */

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
  gtk_widget_queue_draw (GTK_WIDGET (bd->self));
}


static void firework_schedule (KgxEdge *self);


static void
burst_done_cb (BurstData *bd)
{
  bd->self->burst_progress[bd->index] = -1.0;
  gtk_widget_queue_draw (GTK_WIDGET (bd->self));
}


static gboolean
burst_fire (gpointer data)
{
  BurstData *bd = data;
  KgxEdge *self = bd->self;
  int i = bd->index;
  int width, height;
  double perim;

  self->burst_timeout[i] = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  width  = gtk_widget_get_width (GTK_WIDGET (self));
  height = gtk_widget_get_height (GTK_WIDGET (self));
  perim  = 2.0 * (width + height);

  self->burst_head[i] = g_random_double () * perim;

  /* Settings firework uses random colors (highest priority).
   * Process firework uses the configured particle color.
   * Privilege uses privilege color when settings is inactive. */
  if (self->firework_settings)
    self->burst_color[i] = random_muted_color ();
  else if (self->process_preset == KGX_PARTICLE_FIREWORKS)
    self->burst_color[i] = self->process_color;
  else if (self->privileged && self->privilege_enabled)
    self->burst_color[i] = resolve_color (self, self->privilege_color);
  else
    self->burst_color[i] = random_muted_color ();

  /* Apply per-preset speed to firework bursts when active */
  {
    const KgxParticleTunables *bt =
      (self->process_preset == KGX_PARTICLE_FIREWORKS)
        ? resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
        : &self->global;
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


/* ── snapshot ────────────────────────────────────────────── */

static void
kgx_edge_snapshot (GtkWidget   *widget,
                   GtkSnapshot *snapshot)
{
  KgxEdge *self   = KGX_EDGE (widget);
  int      width  = gtk_widget_get_width (widget);
  int      height = gtk_widget_get_height (widget);

  /* Overscroll beam */
  if (self->overscroll_progress >= 0.0 && self->overscroll_enabled) {
    GdkRGBA color = resolve_color (self, self->overscroll_color);

    draw_overscroll (snapshot, self->overscroll_progress,
                     self->overscroll_edge, self->overscroll_style,
                     width, height, &color, &self->global);
  }

  /* Firework / privilege decoration — staggered center-bursts.
   * When process_reverse + FIREWORKS: implode (converge to center). */
  {
    gboolean implode = (self->process_preset == KGX_PARTICLE_FIREWORKS &&
                        self->process_reverse &&
                        !self->firework_settings);
    const KgxParticleTunables *bt =
      (self->process_preset == KGX_PARTICLE_FIREWORKS && !self->firework_settings)
        ? resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
        : &self->global;

    for (int i = 0; i < MAX_BURSTS; i++) {
      if (self->burst_progress[i] >= 0.0) {
        double perim = 2.0 * (width + height);
        double p     = self->burst_progress[i];
        double bp    = implode ? 1.0 - p : p;
        float  b_env = implode
                          ? envelope (bp, 0.0, 0.02)
                          : envelope (p, bt->env_attack, bt->env_release);
        double seg_env = envelope (bp, bt->env_attack, 0.0);
        double seg   = BASE_OVERSCROLL_SEG * bt->tail_length * 2.0 * seg_env;
        float  a     = b_env * 0.5f;
        double spread = seg * 3.0 * bp;
        double left_head  = fmod (self->burst_head[i] - spread + perim, perim);
        double right_head = fmod (self->burst_head[i] + spread, perim);
        int l_trail = implode ? -1 : +1;
        int r_trail = implode ? +1 : -1;

        draw_segment (snapshot, left_head, seg, a,
                      width, height, &self->burst_color[i],
                      l_trail, bt, p);
        draw_segment (snapshot, right_head, seg, a,
                      width, height, &self->burst_color[i],
                      r_trail, bt, p);
      }
    }
  }

  /* Process-specific particle — suppressed when settings page is open
   * (the pending_change mechanism ensures the loop stops gracefully). */
  if (self->process_progress >= 0.0 &&
      self->process_preset != KGX_PARTICLE_NONE &&
      !self->firework_settings) {
    const KgxParticleTunables *pt = resolve_tunables (self, self->process_preset);
    double perim = 2.0 * (width + height);
    double p     = self->process_progress;
    float  env   = envelope (p, pt->env_attack, pt->env_release);
    float  a     = env * 0.8f;

    double seg_full = BASE_OVERSCROLL_SEG * pt->tail_length * 2.0;
    /* Tail envelope: grows during attack. In retract mode, shrinks during release.
     * Rotate keeps full tail -- it's always in motion. */
    double tail_env;
    if (self->process_preset == KGX_PARTICLE_ROTATE)
      tail_env = 1.0;
    else if (pt->release_mode == KGX_RELEASE_RETRACT)
      tail_env = envelope (p, pt->env_attack, pt->env_release);
    else
      tail_env = envelope (p, pt->env_attack, 0.0);  /* uniform: attack only */
    double seg = seg_full * tail_env;

    switch (self->process_preset) {
    case KGX_PARTICLE_CORNERS: {
      double corner_a, corner_b;
      if (self->process_reverse) {
        corner_a = (double) width;
        corner_b = (double)(2 * width + height);
      } else {
        corner_a = 0.0;
        corner_b = (double)(width + height);
      }

      double a_cw  = fmod (corner_a + (width + height) / 2.0 * p, perim);
      double a_ccw = fmod (corner_a - (width + height) / 2.0 * p + perim * 2, perim);
      double b_cw  = fmod (corner_b + (width + height) / 2.0 * p, perim);
      double b_ccw = fmod (corner_b - (width + height) / 2.0 * p + perim * 2, perim);

      draw_segment (snapshot, a_cw,  seg, a, width, height, &self->process_color, -1, pt, p);
      draw_segment (snapshot, a_ccw, seg, a, width, height, &self->process_color, +1, pt, p);
      draw_segment (snapshot, b_cw,  seg, a, width, height, &self->process_color, -1, pt, p);
      draw_segment (snapshot, b_ccw, seg, a, width, height, &self->process_color, +1, pt, p);
      break;
    }
    case KGX_PARTICLE_PULSE_OUT: {
      double center = self->process_reverse
                        ? (double)width + height + width / 2.0
                        : width / 2.0;
      double spread = (perim / 4.0) * p;
      double left_head  = fmod (center - spread + perim, perim);
      double right_head = fmod (center + spread, perim);

      draw_segment (snapshot, left_head,  seg, a, width, height, &self->process_color, +1, pt, p);
      draw_segment (snapshot, right_head, seg, a, width, height, &self->process_color, -1, pt, p);
      break;
    }
    case KGX_PARTICLE_ROTATE: {
      int dir   = self->process_reverse ? -1 : +1;
      int trail = self->process_reverse ? +1 : -1;

      double half_p   = fmod (p * 2.0, 1.0);
      double eased    = 1.0 - (1.0 - half_p) * (1.0 - half_p) * (1.0 - half_p);
      float  lap_env  = envelope (half_p, pt->env_attack, pt->env_release);
      float  lap_a    = lap_env * 0.8f;
      double lap_tail = (pt->release_mode == KGX_RELEASE_RETRACT)
                          ? envelope (half_p, pt->env_attack, pt->env_release)
                          : envelope (half_p, pt->env_attack, 0.0);
      double lap_seg  = seg_full * lap_tail;

      double offset = (p >= 0.5) ? perim / 2.0 : 0.0;
      double head   = fmod (dir * (perim / 2.0) * eased + offset + perim, perim);

      draw_segment (snapshot, head, lap_seg * 2.0, lap_a,
                    width, height, &self->process_color, trail, pt, half_p);
      break;
    }
    case KGX_PARTICLE_PING_PONG: {
      double edge_start = self->process_reverse
                            ? (double)(width + height) : 0.0;

      double half_p = fmod (p * 2.0, 1.0);
      gboolean returning = (p >= 0.5);
      float  pp_env = envelope (half_p, pt->env_attack, pt->env_release);
      float  pp_a   = pp_env * 0.9f;
      double pp_tail = (pt->release_mode == KGX_RELEASE_RETRACT)
                         ? envelope (half_p, pt->env_attack, pt->env_release)
                         : envelope (half_p, pt->env_attack, 0.0);
      double pp_seg = BASE_OVERSCROLL_SEG * pp_tail;

      double pos;
      int trail;
      if (!returning) {
        pos   = fmod (edge_start + (double) width * half_p + perim, perim);
        trail = -1;
      } else {
        pos   = fmod (edge_start + (double) width * (1.0 - half_p) + perim, perim);
        trail = +1;
      }

      draw_segment (snapshot, pos, pp_seg, pp_a,
                    width, height, &self->process_color, trail, pt, half_p);
      break;
    }
    default:
      break;
    }
  }
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
  *minimum = 0;
  *natural = 0;
}


/* ── animation callbacks ────────────────────────────────── */

static void
overscroll_value_cb (double   value,
                     KgxEdge *self)
{
  self->overscroll_progress = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


static void
overscroll_done_cb (KgxEdge *self)
{
  self->overscroll_progress = -1.0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


/* ── map: resume firework if needed ───────────────────────── */

static void
kgx_edge_map (GtkWidget *widget)
{
  KgxEdge *self = KGX_EDGE (widget);

  GTK_WIDGET_CLASS (kgx_edge_parent_class)->map (widget);

  if (firework_active (self) && self->burst_progress[0] < 0.0)
    firework_schedule (self);
}


/* ── GObject property handlers ───────────────────────────── */

static void
kgx_edge_set_property (GObject      *object,
                       guint         property_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
  KgxEdge *self = KGX_EDGE (object);

  /* Handle per-preset tunables via indexed arithmetic */
  if (property_id >= PROP_PRESET_BASE &&
      property_id < (guint) LAST_PROP) {
    int idx   = property_id - PROP_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    if (field == TUNE_THICKNESS)
      self->preset[pi].thickness = CLAMP (g_value_get_int (value), 2, 40);
    else if (field == TUNE_RELEASE_MODE)
      self->preset[pi].release_mode = CLAMP (g_value_get_int (value), 0, 1);
    else if (field == TUNE_SHAPE)
      self->preset[pi].shape = CLAMP (g_value_get_int (value), 0, 3);
    else
      set_tunable_double (&self->preset[pi], field, g_value_get_double (value));
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    if (field == TUNE_THICKNESS) {
      self->global.thickness = g_value_get_int (value);
    } else if (field == TUNE_RELEASE_MODE) {
      self->global.release_mode = CLAMP (g_value_get_int (value), 0, 1);
    } else if (field == TUNE_SHAPE) {
      self->global.shape = CLAMP (g_value_get_int (value), 0, 3);
    } else if (field == TUNE_SPEED) {
      self->global.speed = g_value_get_double (value);
      /* Update overscroll / burst animation durations */
      if (self->overscroll_anim)
        adw_timed_animation_set_duration (
          ADW_TIMED_ANIMATION (self->overscroll_anim),
          (guint)(BASE_OVERSCROLL_MS / self->global.speed));
      for (int i = 0; i < MAX_BURSTS; i++) {
        if (self->burst_anim[i])
          adw_timed_animation_set_duration (
            ADW_TIMED_ANIMATION (self->burst_anim[i]),
            (guint)(800.0 / self->global.speed));
      }
    } else {
      set_tunable_double (&self->global, field, g_value_get_double (value));
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  switch (property_id) {
    case PROP_OVERSCROLL_ENABLED:
      self->overscroll_enabled = g_value_get_boolean (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_OVERSCROLL_COLOR:
      g_free (self->overscroll_color);
      self->overscroll_color = g_value_dup_string (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_PRIVILEGE_ENABLED:
      self->privilege_enabled = g_value_get_boolean (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_PRIVILEGE_COLOR:
      g_free (self->privilege_color);
      self->privilege_color = g_value_dup_string (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_OVERSCROLL_STYLE:
      self->overscroll_style = g_value_get_int (value);
      break;
    case PROP_BURST_SPREAD:
      self->burst_spread = g_value_get_double (value);
      break;
    case PROP_BURST_COUNT:
      self->burst_count = CLAMP (g_value_get_int (value), 1, MAX_BURSTS);
      break;
    case PROP_PRIVILEGE_DIRECTION:
      self->privilege_direction = g_value_get_int (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
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

  /* Handle per-preset tunables */
  if (property_id >= PROP_PRESET_BASE &&
      property_id < (guint) LAST_PROP) {
    int idx   = property_id - PROP_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    if (field == TUNE_THICKNESS)
      g_value_set_int (value, self->preset[pi].thickness);
    else if (field == TUNE_RELEASE_MODE)
      g_value_set_int (value, self->preset[pi].release_mode);
    else if (field == TUNE_SHAPE)
      g_value_set_int (value, self->preset[pi].shape);
    else
      g_value_set_double (value, get_tunable_double (&self->preset[pi], field));
    return;
  }

  /* Handle global tunables */
  if (property_id >= PROP_GLOBAL_BASE &&
      property_id < PROP_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_GLOBAL_BASE;
    if (field == TUNE_THICKNESS)
      g_value_set_int (value, self->global.thickness);
    else if (field == TUNE_RELEASE_MODE)
      g_value_set_int (value, self->global.release_mode);
    else if (field == TUNE_SHAPE)
      g_value_set_int (value, self->global.shape);
    else
      g_value_set_double (value, get_tunable_double (&self->global, field));
    return;
  }

  switch (property_id) {
    case PROP_OVERSCROLL_ENABLED:
      g_value_set_boolean (value, self->overscroll_enabled);
      break;
    case PROP_OVERSCROLL_COLOR:
      g_value_set_string (value, self->overscroll_color ? self->overscroll_color : "");
      break;
    case PROP_PRIVILEGE_ENABLED:
      g_value_set_boolean (value, self->privilege_enabled);
      break;
    case PROP_PRIVILEGE_COLOR:
      g_value_set_string (value, self->privilege_color ? self->privilege_color : "#d940a6");
      break;
    case PROP_OVERSCROLL_STYLE:
      g_value_set_int (value, self->overscroll_style);
      break;
    case PROP_BURST_SPREAD:
      g_value_set_double (value, self->burst_spread);
      break;
    case PROP_BURST_COUNT:
      g_value_set_int (value, self->burst_count);
      break;
    case PROP_PRIVILEGE_DIRECTION:
      g_value_set_int (value, self->privilege_direction);
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

  g_clear_object (&self->overscroll_anim);
  self->process_preset = KGX_PARTICLE_NONE;
  self->process_progress = -1.0;
  if (self->process_anim) {
    adw_animation_reset (self->process_anim);
    g_clear_object (&self->process_anim);
  }
  for (int i = 0; i < MAX_BURSTS; i++)
    g_clear_object (&self->burst_anim[i]);
  g_clear_pointer (&self->overscroll_color, g_free);
  g_clear_pointer (&self->privilege_color, g_free);

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

  edge_pspecs[PROP_OVERSCROLL_ENABLED] =
    g_param_spec_boolean ("overscroll-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_COLOR] =
    g_param_spec_string ("overscroll-color", NULL, NULL, "",
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PRIVILEGE_ENABLED] =
    g_param_spec_boolean ("privilege-enabled", NULL, NULL, TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PRIVILEGE_COLOR] =
    g_param_spec_string ("privilege-color", NULL, NULL, "#d940a6",
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_STYLE] =
    g_param_spec_int ("overscroll-style", NULL, NULL, 0, 1, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_BURST_SPREAD] =
    g_param_spec_double ("burst-spread", NULL, NULL, 0.5, 5.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_BURST_COUNT] =
    g_param_spec_int ("burst-count", NULL, NULL, 1, MAX_BURSTS, 4,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PRIVILEGE_DIRECTION] =
    g_param_spec_int ("privilege-direction", NULL, NULL, 0, 1, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  /* ── global tunable property specs ─────────────────────── */
  /*              speed  thick  tail   pdep   pspd   atk    rel    rmode  shape */
  {
    static const double dbl_min[] = { 0.1, 0, 0.1, 0.0, 0.1, 0.0, 0.0, 0, 0 };
    static const double dbl_max[] = { 3.0, 0, 3.0, 1.0, 5.0, 0.5, 0.5, 0, 0 };
    static const double dbl_def[] = { 1.0, 0, 1.0, 0.3, 1.0, 0.2, 0.3, 0, 0 };

    for (int f = 0; f < N_TUNE_FIELDS; f++) {
      int id = PROP_GLOBAL_BASE + f;
      if (f == TUNE_THICKNESS) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 2, 40, 10,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_RELEASE_MODE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 1, 0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
      } else if (f == TUNE_SHAPE) {
        edge_pspecs[id] =
          g_param_spec_int (tune_names[f], NULL, NULL, 0, 3, 0,
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
  /*              speed  thick  tail   pdep   pspd   atk    rel    rmode  shape */
  {
    static const double dbl_min[] = { 0.1, 0, 0.1, 0.0, 0.1, 0.0, 0.0, 0, 0 };
    static const double dbl_max[] = { 3.0, 0, 3.0, 1.0, 5.0, 0.5, 0.5, 0, 0 };
    static const double dbl_def[] = { 1.0, 0, 0.9, 0.5, 0.8, 0.2, 0.3, 0, 0 };

    for (int p = 0; p < N_PRESETS; p++) {
      for (int f = 0; f < N_TUNE_FIELDS; f++) {
        int id = PROP_PRESET_BASE + p * N_TUNE_FIELDS + f;
        char *name = g_strdup_printf ("%s-%s", tune_names[f],
                                      preset_suffixes[p]);
        if (f == TUNE_THICKNESS) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 2, 40, 20,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_RELEASE_MODE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 1, 0,
                              G_PARAM_READWRITE | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
        } else if (f == TUNE_SHAPE) {
          edge_pspecs[id] =
            g_param_spec_int (name, NULL, NULL, 0, 3, 0,
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
  self->privilege_enabled   = TRUE;
  self->privilege_color     = g_strdup ("#d940a6");
  self->overscroll_color    = g_strdup ("");
  self->burst_spread        = 3.1;
  self->burst_count         = 8;
  self->overscroll_progress = -1.0;
  self->process_progress    = -1.0;
  self->process_preset      = KGX_PARTICLE_NONE;

  /* Global tunables (used for overscroll / privilege fallback) */
  self->global = (KgxParticleTunables) {
    .speed = 0.3, .thickness = 20, .tail_length = 0.9,
    .pulse_depth = 0.5, .pulse_speed = 0.8,
    .env_attack = 0.2, .env_release = 0.3,
    .release_mode = KGX_RELEASE_UNIFORM,
  };

  /* Per-preset tunables */
  for (int i = 0; i < N_PRESETS; i++) {
    self->preset[i] = (KgxParticleTunables) {
      .speed = 1.0, .thickness = 20, .tail_length = 0.9,
      .pulse_depth = 0.5, .pulse_speed = 0.8,
      .env_attack = 0.2, .env_release = 0.3,
      .release_mode = KGX_RELEASE_UNIFORM,
    };
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

  /* Burst animations for firework/privilege effect. */
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

  if (!self->overscroll_enabled)
    return;

  self->overscroll_edge = edge;

  adw_animation_reset (self->overscroll_anim);
  adw_animation_play (self->overscroll_anim);
}


void
kgx_edge_set_privileged (KgxEdge  *self,
                         gboolean  privileged)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  if (self->privileged == privileged)
    return;

  self->privileged = privileged;

  if (!self->privilege_enabled)
    return;

  self->firework_privilege = privileged;

  if (privileged && !self->firework_settings) {
    /* Fire immediately on privilege activation. */
    if (!self->firework_timeout)
      self->firework_timeout = g_timeout_add (50, firework_fire, self);
  } else if (!privileged && !self->firework_settings) {
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
  }
}


void
kgx_edge_set_ambient (KgxEdge  *self,
                      gboolean  ambient)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  if (self->firework_settings == ambient)
    return;

  self->firework_settings = ambient;

  if (ambient) {
    /* Stop the process particle loop gracefully — it will fade out
     * via its envelope while the settings fireworks play over it. */
    if (self->process_progress >= 0.0 &&
        self->process_anim &&
        adw_animation_get_state (self->process_anim) == ADW_ANIMATION_PLAYING) {
      self->pending_change = TRUE;
      self->pending_preset = KGX_PARTICLE_NONE;
      self->pending_color = (GdkRGBA) { 0, 0, 0, 0 };
      self->pending_reverse = FALSE;
    }
    if (!self->firework_timeout)
      self->firework_timeout = g_timeout_add (50, firework_fire, self);
  } else if (!self->firework_privilege) {
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
  default:                     return "none";
  }
}


void
kgx_parse_process_config (const char        *value,
                           char             **glass_color,
                           KgxParticlePreset *preset,
                           gboolean          *reverse,
                           GdkRGBA           *particle_color)
{
  g_auto (GStrv) parts = NULL;

  if (glass_color)    *glass_color = NULL;
  if (preset)         *preset = KGX_PARTICLE_NONE;
  if (reverse)        *reverse = FALSE;
  if (particle_color) *particle_color = (GdkRGBA) { 0.5f, 0.5f, 0.5f, 1.0f };

  if (!value || !value[0])
    return;

  parts = g_strsplit (value, ";", 5);
  int n = g_strv_length (parts);

  if (n >= 1 && glass_color)
    *glass_color = g_strdup (parts[0]);

  if (n >= 2 && preset)
    *preset = preset_from_string (parts[1]);

  if (n >= 3 && reverse)
    *reverse = g_str_equal (parts[2], "1");

  if (n >= 4 && particle_color)
    gdk_rgba_parse (particle_color, parts[3]);
}


/* ── process particle callbacks ──────────────────────────── */

static void
process_particle_value_cb (double value, KgxEdge *self)
{
  /* For multi-step presets (ROTATE, PING_PONG), check if we've crossed
   * the half-cycle boundary (0.5) while a pending change is queued.
   * If so, stop the animation early so the transition happens at the
   * step boundary rather than waiting for the full cycle. */
  if (self->pending_change &&
      (self->process_preset == KGX_PARTICLE_ROTATE ||
       self->process_preset == KGX_PARTICLE_PING_PONG) &&
      self->process_progress < 0.5 && value >= 0.5) {
    adw_animation_skip (self->process_anim);
    return;
  }

  self->process_progress = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


static void
process_particle_done_cb (KgxEdge *self)
{
  /* If a preset change is pending, apply it now that the current
   * animation has finished gracefully. */
  if (self->pending_change) {
    self->pending_change = FALSE;
    self->process_progress = -1.0;
    gtk_widget_queue_draw (GTK_WIDGET (self));
    /* Apply the pending preset (may be NONE, which just stops). */
    kgx_edge_set_process_particle (self,
                                   self->pending_preset,
                                   &self->pending_color,
                                   self->pending_reverse);
    return;
  }

  /* Rotate and ping-pong loop continuously while active */
  if (self->process_preset == KGX_PARTICLE_ROTATE ||
      self->process_preset == KGX_PARTICLE_PING_PONG) {
    adw_animation_reset (self->process_anim);
    adw_animation_play (self->process_anim);
    return;
  }

  self->process_progress = -1.0;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


void
kgx_edge_set_process_particle (KgxEdge          *self,
                               KgxParticlePreset  preset,
                               const GdkRGBA     *color,
                               gboolean           reverse)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  /* If an animation is currently playing and the preset is changing,
   * queue the change as pending so the current cycle finishes gracefully. */
  if (self->process_progress >= 0.0 &&
      self->process_anim &&
      adw_animation_get_state (self->process_anim) == ADW_ANIMATION_PLAYING &&
      (preset != self->process_preset || preset == KGX_PARTICLE_NONE)) {
    self->pending_change = TRUE;
    self->pending_preset = preset;
    self->pending_reverse = reverse;
    if (color)
      self->pending_color = *color;
    else
      self->pending_color = (GdkRGBA) { 0.5f, 0.5f, 0.5f, 1.0f };

    return;
  }

  self->pending_change = FALSE;
  self->process_preset = preset;
  self->process_reverse = reverse;

  if (color)
    self->process_color = *color;

  if (preset == KGX_PARTICLE_NONE) {
    self->process_progress = -1.0;
    if (self->process_anim)
      adw_animation_reset (self->process_anim);
    /* Stop firework cycle if it was running for a process FIREWORKS preset */
    if (self->firework_timeout &&
        !self->firework_settings && !self->firework_privilege) {
      g_source_remove (self->firework_timeout);
      self->firework_timeout = 0;
    }
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return;
  }

  /* Fireworks reuses the burst/privilege animation system */
  if (preset == KGX_PARTICLE_FIREWORKS) {
    if (gtk_widget_get_mapped (GTK_WIDGET (self)) && !self->firework_timeout) {
      self->burst_data[0].index = 0;
      self->burst_data[0].self  = self;
      burst_fire (&self->burst_data[0]);
      firework_schedule (self);
    }
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

  /* Always update easing and duration — speed may have changed */
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->process_anim),
                                  (preset == KGX_PARTICLE_ROTATE ||
                                   preset == KGX_PARTICLE_PING_PONG)
                                    ? ADW_LINEAR
                                    : ADW_EASE_OUT_CUBIC);

  double spd = resolve_tunables (self, preset)->speed;
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

  adw_animation_reset (self->process_anim);
  adw_animation_play (self->process_anim);
}
