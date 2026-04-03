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
  int             thickness;            /* pixels */
  double          speed;                /* movement speed multiplier */
  double          pulse_speed;          /* tail shimmer speed multiplier */
  double          pulse_depth;          /* 0..1 shimmer intensity */
  double          tail_length;          /* multiplier on segment length */
  double          burst_spread;         /* multiplier on burst stagger */
  int             burst_count;          /* how many fire per cycle (1-MAX_BURSTS) */
  int             overscroll_style;     /* 0 = center, 1 = corner */
  int             privilege_direction;  /* 0 = clockwise, 1 = counter-clockwise */

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
};


enum {
  PROP_0,
  PROP_OVERSCROLL_ENABLED,
  PROP_OVERSCROLL_COLOR,
  PROP_PRIVILEGE_ENABLED,
  PROP_PRIVILEGE_COLOR,
  PROP_THICKNESS,
  PROP_SPEED,
  PROP_OVERSCROLL_STYLE,
  PROP_PULSE_SPEED,
  PROP_PULSE_DEPTH,
  PROP_TAIL_LENGTH,
  PROP_BURST_SPREAD,
  PROP_BURST_COUNT,
  PROP_PRIVILEGE_DIRECTION,
  LAST_PROP
};
static GParamSpec *edge_pspecs[LAST_PROP] = { NULL, };


G_DEFINE_FINAL_TYPE (KgxEdge, kgx_edge, GTK_TYPE_WIDGET)


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


/* ── overscroll drawing ──────────────────────────────────── */

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
draw_segment (GtkSnapshot   *snapshot,
              double         head_d,
              double         seg_len,
              float          alpha,
              float          width,
              float          height,
              const GdkRGBA *color,
              int            trail_dir,
              int            thick,
              double         phase,
              double         pulse_spd,
              double         pulse_depth)
{
  double blk   = (double) thick;
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
      float intensity = t * (float) pulse_depth;
      float pulse = 1.0f - intensity
                    + intensity * sinf ((float) s * 1.2f - (float)(phase * 40.0 * pulse_spd));
      a *= pulse;
    }
    GdkRGBA c = *color;
    float  px, py, bw, bh;

    c.alpha = a;

    if (d < width) {
      /* Top edge */
      px = (float) d;  py = 0;
      bw = (float) blk;  bh = (float) thick;
    } else if (d < width + height) {
      /* Right edge */
      px = width - (float) thick;  py = (float)(d - width);
      bw = (float) thick;  bh = (float) blk;
    } else if (d < 2 * width + height) {
      /* Bottom edge */
      px = width - (float)(d - width - height) - (float) blk;
      py = height - (float) thick;
      bw = (float) blk;  bh = (float) thick;
    } else {
      /* Left edge */
      px = 0;
      py = height - (float)(d - 2 * width - height) - (float) blk;
      bw = (float) thick;  bh = (float) blk;
    }

    gtk_snapshot_append_color (snapshot, &c,
                              &GRAPHENE_RECT_INIT (px, py, bw, bh));
  }
}


/* ── overscroll drawing ──────────────────────────────────── */

static void
draw_overscroll (GtkSnapshot     *snapshot,
                 double           progress,
                 GtkPositionType  edge,
                 int              style,
                 float            width,
                 float            height,
                 const GdkRGBA   *color,
                 int              thick,
                 double           pulse_spd,
                 double           pulse_depth)
{
  double perim  = 2.0 * (width + height);
  double fade   = progress > 0.7 ? (1.0 - progress) / 0.3 : 1.0;
  float  a      = (float)(fade * 0.9);

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
                    width, height, color, h_trail, thick, progress, pulse_spd, pulse_depth);
      draw_segment (snapshot, v_head, BASE_OVERSCROLL_SEG, a,
                    width, height, color, v_trail, thick, progress, pulse_spd, pulse_depth);
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
                  width, height, color, +1, thick, progress, pulse_spd, pulse_depth);
    draw_segment (snapshot, right_head, BASE_OVERSCROLL_SEG, a,
                  width, height, color, -1, thick, progress, pulse_spd, pulse_depth);
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

  /* Settings firework uses random colors (higher priority).
   * Privilege uses privilege color only when settings is inactive. */
  if (self->firework_settings)
    self->burst_color[i] = random_muted_color ();
  else if (self->privileged && self->privilege_enabled)
    self->burst_color[i] = resolve_color (self, self->privilege_color);
  else
    self->burst_color[i] = random_muted_color ();

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

  /* burst_spread controls cycle interval:
   * spread=0.5 → 400-600ms (bursts overlap, no gaps)
   * spread=1.0 → 600-1200ms (some overlap)
   * spread=3.0 → 1800-3600ms (sparse, visible gaps) */
  int lo = (int)(600 * self->burst_spread);
  int hi = (int)(1200 * self->burst_spread);
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
                     width, height, &color, self->thickness,
                     self->pulse_speed, self->pulse_depth);
  }

  /* Firework / privilege decoration — staggered center-bursts */
  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_progress[i] >= 0.0) {
      double perim = 2.0 * (width + height);
      double p     = self->burst_progress[i];
      double fade  = p > 0.8 ? (1.0 - p) / 0.2 : 1.0;
      float  a     = (float)(fade * 0.5);
      double seg   = BASE_OVERSCROLL_SEG * self->tail_length * 2.0;
      double spread = seg * 3.0 * p;
      double left_head  = fmod (self->burst_head[i] - spread + perim, perim);
      double right_head = fmod (self->burst_head[i] + spread, perim);

      draw_segment (snapshot, left_head, seg, a,
                    width, height, &self->burst_color[i],
                    +1, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
      draw_segment (snapshot, right_head, seg, a,
                    width, height, &self->burst_color[i],
                    -1, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
    }
  }

  /* Process-specific particle — one-shot with long fade like overscroll */
  if (self->process_progress >= 0.0 &&
      self->process_preset != KGX_PARTICLE_NONE) {
    double perim = 2.0 * (width + height);
    double p     = self->process_progress;
    double fade  = p > 0.6 ? (1.0 - p) / 0.4 : 1.0;
    float  a     = (float)(fade * 0.8);
    double seg   = BASE_OVERSCROLL_SEG * self->tail_length * 2.0;

    switch (self->process_preset) {
    case KGX_PARTICLE_CORNERS: {
      /* Burst from a corner along two adjacent edges — travel full edge length. */
      double corner = self->process_reverse
                        ? (double)(width + height)   /* bottom-right */
                        : 0.0;                       /* top-left */
      double h_head, v_head;
      int h_trail, v_trail;

      if (self->process_reverse) {
        h_head  = fmod (corner + width * p, perim);           /* CW along bottom */
        v_head  = fmod (corner - height * p + perim, perim);  /* CCW up right */
        h_trail = -1; v_trail = +1;
      } else {
        h_head  = fmod (width * p, perim);                    /* CW along top */
        v_head  = fmod (perim - height * p + perim, perim);   /* CCW up left */
        h_trail = -1; v_trail = +1;
      }

      draw_segment (snapshot, h_head, seg, a,
                    width, height, &self->process_color,
                    h_trail, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
      draw_segment (snapshot, v_head, seg, a,
                    width, height, &self->process_color,
                    v_trail, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
      break;
    }
    case KGX_PARTICLE_PULSE_OUT: {
      /* Two particles from center of edge, splitting outward — travel half perimeter. */
      double center = self->process_reverse
                        ? (double)width + height + width / 2.0  /* bottom center */
                        : width / 2.0;                          /* top center */
      double spread = (perim / 4.0) * p;
      double left_head  = fmod (center - spread + perim, perim);
      double right_head = fmod (center + spread, perim);

      draw_segment (snapshot, left_head, seg, a,
                    width, height, &self->process_color,
                    +1, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
      draw_segment (snapshot, right_head, seg, a,
                    width, height, &self->process_color,
                    -1, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
      break;
    }
    case KGX_PARTICLE_ROTATE: {
      /* Single segment travelling the full perimeter. */
      int dir = self->process_reverse ? -1 : +1;
      double head = fmod (dir * perim * p + perim, perim);
      int trail = self->process_reverse ? +1 : -1;

      draw_segment (snapshot, head, seg * 2.0, a,
                    width, height, &self->process_color,
                    trail, self->thickness, p,
                    self->pulse_speed, self->pulse_depth);
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
    case PROP_THICKNESS:
      self->thickness = g_value_get_int (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_SPEED:
      self->speed = g_value_get_double (value);
      if (self->overscroll_anim)
        adw_timed_animation_set_duration (
          ADW_TIMED_ANIMATION (self->overscroll_anim),
          (guint)(BASE_OVERSCROLL_MS / self->speed));
      for (int i = 0; i < MAX_BURSTS; i++) {
        if (self->burst_anim[i])
          adw_timed_animation_set_duration (
            ADW_TIMED_ANIMATION (self->burst_anim[i]),
            (guint)(800.0 / self->speed));
      }
      break;
    case PROP_OVERSCROLL_STYLE:
      self->overscroll_style = g_value_get_int (value);
      break;
    case PROP_PULSE_SPEED:
      self->pulse_speed = g_value_get_double (value);
      break;
    case PROP_PULSE_DEPTH:
      self->pulse_depth = g_value_get_double (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
      break;
    case PROP_TAIL_LENGTH:
      self->tail_length = g_value_get_double (value);
      gtk_widget_queue_draw (GTK_WIDGET (self));
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
    case PROP_THICKNESS:
      g_value_set_int (value, self->thickness);
      break;
    case PROP_SPEED:
      g_value_set_double (value, self->speed);
      break;
    case PROP_OVERSCROLL_STYLE:
      g_value_set_int (value, self->overscroll_style);
      break;
    case PROP_PULSE_SPEED:
      g_value_set_double (value, self->pulse_speed);
      break;
    case PROP_PULSE_DEPTH:
      g_value_set_double (value, self->pulse_depth);
      break;
    case PROP_TAIL_LENGTH:
      g_value_set_double (value, self->tail_length);
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

  edge_pspecs[PROP_THICKNESS] =
    g_param_spec_int ("thickness", NULL, NULL, 2, 20, 10,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_SPEED] =
    g_param_spec_double ("speed", NULL, NULL, 0.1, 3.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_OVERSCROLL_STYLE] =
    g_param_spec_int ("overscroll-style", NULL, NULL, 0, 1, 0,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PULSE_SPEED] =
    g_param_spec_double ("pulse-speed", NULL, NULL, 0.1, 5.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_PULSE_DEPTH] =
    g_param_spec_double ("pulse-depth", NULL, NULL, 0.0, 1.0, 0.3,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  edge_pspecs[PROP_TAIL_LENGTH] =
    g_param_spec_double ("tail-length", NULL, NULL, 0.1, 3.0, 1.0,
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
  self->thickness           = 20;
  self->speed               = 0.3;
  self->pulse_speed         = 0.8;
  self->pulse_depth         = 0.5;
  self->tail_length         = 0.9;
  self->burst_spread        = 3.1;
  self->burst_count         = 8;
  self->overscroll_progress = -1.0;
  self->process_progress    = -1.0;
  self->process_preset      = KGX_PARTICLE_NONE;

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

  parts = g_strsplit (value, ";", 4);
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
  self->process_progress = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}


static void
process_particle_done_cb (KgxEdge *self)
{
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

  self->process_preset = preset;
  self->process_reverse = reverse;

  if (color)
    self->process_color = *color;

  if (preset == KGX_PARTICLE_NONE) {
    self->process_progress = -1.0;
    if (self->process_anim)
      adw_animation_reset (self->process_anim);
    gtk_widget_queue_draw (GTK_WIDGET (self));
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

  /* Don't restart if already animating — let the process check re-trigger */
  if (self->process_progress >= 0.0 &&
      adw_animation_get_state (self->process_anim) == ADW_ANIMATION_PLAYING)
    return;

  /* Long one-shot durations with ease-out for natural fade */
  guint duration;
  switch (preset) {
  case KGX_PARTICLE_ROTATE:    duration = (guint)(4000.0 / self->speed); break;
  case KGX_PARTICLE_CORNERS:   duration = (guint)(2500.0 / self->speed); break;
  case KGX_PARTICLE_PULSE_OUT: duration = (guint)(2500.0 / self->speed); break;
  default:                     duration = (guint)(3000.0 / self->speed); break;
  }
  adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->process_anim),
                                    duration);

  adw_animation_reset (self->process_anim);
  adw_animation_play (self->process_anim);
}
