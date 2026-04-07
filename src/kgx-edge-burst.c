/* kgx-edge-burst.c
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

#include "kgx-edge-private.h"

static guint
schedule_burst_timeout (guint       interval_ms,
                        GSourceFunc function,
                        gpointer    data,
                        const char *name)
{
  guint source_id;

  source_id = g_timeout_add_full (G_PRIORITY_LOW,
                                  interval_ms,
                                  function,
                                  data,
                                  NULL);
  g_source_set_name_by_id (source_id, name);

  return source_id;
}

static GdkRGBA
random_muted_color (void)
{
  static const GdkRGBA colors[] = {
    { 0.85f, 0.40f, 0.75f, 1.0f },
    { 0.40f, 0.60f, 0.90f, 1.0f },
    { 0.40f, 0.80f, 0.50f, 1.0f },
    { 0.95f, 0.65f, 0.35f, 1.0f },
  };

  return colors[g_random_int_range (0, G_N_ELEMENTS (colors))];
}

static void firework_schedule (KgxEdge *self);
static void ambient_schedule (KgxEdge *self);

static void
burst_value_cb (double value,
                BurstData *bd)
{
  bd->self->burst_progress[bd->index] = value;
  kgx_edge_mark_dirty (bd->self);
}

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
  const KgxParticleTunables *bt;
  int i = bd->index;
  int width;
  int height;
  double perim;

  self->burst_timeout[i] = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return G_SOURCE_REMOVE;

  kgx_edge_get_canvas_size (GTK_WIDGET (self), &width, &height);
  perim = 2.0 * (width + height);
  self->burst_head[i] = g_random_double () * perim;

  if (self->process_preset == KGX_PARTICLE_FIREWORKS)
    self->burst_color[i] = self->process_color;
  else
    self->burst_color[i] = random_muted_color ();

  bt = (self->process_preset == KGX_PARTICLE_FIREWORKS)
         ? kgx_edge_resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
         : &self->global;
  self->burst_tune_snap[i] = *bt;
  adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->burst_anim[i]),
                                    (guint) (800.0 / bt->speed));
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

  self->burst_data[0].index = 0;
  self->burst_data[0].self = self;
  burst_fire (&self->burst_data[0]);

  for (int i = 1; i < self->burst_count; i++) {
    int lo;
    int hi;

    self->burst_data[i].index = i;
    self->burst_data[i].self = self;
    if (self->burst_timeout[i] != 0)
      continue;

    lo = (int) (150 * i * self->burst_spread);
    hi = (int) (600 * i * self->burst_spread);
    self->burst_timeout[i] = schedule_burst_timeout (
      g_random_int_range (MAX (lo, 50), MAX (hi, 100)),
      burst_fire,
      &self->burst_data[i],
      "[kgx] edge firework burst");
  }

  firework_schedule (self);

  return G_SOURCE_REMOVE;
}

static void
firework_schedule (KgxEdge *self)
{
  const KgxParticleTunables *ft;
  double fw_spd;
  int lo;
  int hi;
  guint delay;

  if (self->firework_timeout)
    return;

  ft = (self->process_preset == KGX_PARTICLE_FIREWORKS)
         ? kgx_edge_resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
         : &self->global;
  fw_spd = ft->speed;
  lo = (int) (600 * self->burst_spread / fw_spd);
  hi = (int) (1200 * self->burst_spread / fw_spd);
  delay = (self->burst_progress[0] < 0.0 &&
           adw_animation_get_state (self->burst_anim[0]) == ADW_ANIMATION_IDLE)
        ? 200
        : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->firework_timeout = schedule_burst_timeout (delay,
                                                   firework_fire,
                                                   self,
                                                   "[kgx] edge firework");
}

static void
ambient_burst_value_cb (double value,
                        BurstData *bd)
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

static gboolean
ambient_burst_fire (gpointer data)
{
  BurstData *bd = data;
  KgxEdge *self = bd->self;
  const KgxParticleTunables *bt;
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

  bt = &self->preset[KGX_PARTICLE_AMBIENT - 1];
  self->ambient_tune_snap[i] = *bt;
  adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->ambient_anim[i]),
                                    (guint) (800.0 / bt->speed));
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
  self->ambient_burst_data[0].self = self;
  ambient_burst_fire (&self->ambient_burst_data[0]);

  for (int i = 1; i < self->ambient_burst_count; i++) {
    int lo;
    int hi;

    self->ambient_burst_data[i].index = i;
    self->ambient_burst_data[i].self = self;
    if (self->ambient_burst_timeout[i] != 0)
      continue;

    lo = (int) (150 * i * self->ambient_burst_spread);
    hi = (int) (600 * i * self->ambient_burst_spread);
    self->ambient_burst_timeout[i] = schedule_burst_timeout (
      g_random_int_range (MAX (lo, 50), MAX (hi, 100)),
      ambient_burst_fire,
      &self->ambient_burst_data[i],
      "[kgx] edge ambient burst");
  }

  ambient_schedule (self);

  return G_SOURCE_REMOVE;
}

static void
ambient_schedule (KgxEdge *self)
{
  const KgxParticleTunables *ft;
  double spd;
  int lo;
  int hi;
  guint delay;

  if (self->ambient_timeout)
    return;

  ft = &self->preset[KGX_PARTICLE_AMBIENT - 1];
  spd = ft->speed;
  lo = (int) (600 * self->ambient_burst_spread / spd);
  hi = (int) (1200 * self->ambient_burst_spread / spd);
  delay = (self->ambient_progress[0] < 0.0 &&
           adw_animation_get_state (self->ambient_anim[0]) == ADW_ANIMATION_IDLE)
        ? 200
        : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->ambient_timeout = schedule_burst_timeout (delay,
                                                  ambient_fire,
                                                  self,
                                                  "[kgx] edge ambient");
}

void
kgx_edge_init_bursts (KgxEdge *self)
{
  for (int i = 0; i < MAX_BURSTS; i++) {
    AdwAnimationTarget *target;
    BurstData *bd = &self->burst_data[i];
    BurstData *abd = &self->ambient_burst_data[i];

    bd->index = i;
    bd->self = self;
    target = adw_callback_animation_target_new (
      (AdwAnimationTargetFunc) burst_value_cb,
      bd,
      NULL);
    self->burst_anim[i] = adw_timed_animation_new (GTK_WIDGET (self),
                                                   0.0, 1.0, 800,
                                                   target);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->burst_anim[i]),
                                    ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->burst_anim[i], "done",
                              G_CALLBACK (burst_done_cb), bd);
    self->burst_progress[i] = -1.0;

    abd->index = i;
    abd->self = self;
    target = adw_callback_animation_target_new (
      (AdwAnimationTargetFunc) ambient_burst_value_cb,
      abd,
      NULL);
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

void
kgx_edge_clear_bursts (KgxEdge *self)
{
  if (self->firework_timeout) {
    g_source_remove (self->firework_timeout);
    self->firework_timeout = 0;
  }
  if (self->ambient_timeout) {
    g_source_remove (self->ambient_timeout);
    self->ambient_timeout = 0;
  }

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_timeout[i]) {
      g_source_remove (self->burst_timeout[i]);
      self->burst_timeout[i] = 0;
    }
    if (self->ambient_burst_timeout[i]) {
      g_source_remove (self->ambient_burst_timeout[i]);
      self->ambient_burst_timeout[i] = 0;
    }
    g_clear_object (&self->burst_anim[i]);
    g_clear_object (&self->ambient_anim[i]);
  }
}

void
kgx_edge_resume_bursts (KgxEdge *self)
{
  if (firework_active (self) && self->burst_progress[0] < 0.0)
    firework_schedule (self);

  if (self->ambient_active && self->ambient_enabled &&
      self->ambient_progress[0] < 0.0)
    ambient_schedule (self);
}

void
kgx_edge_start_process_bursts (KgxEdge *self)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  if (!gtk_widget_get_mapped (GTK_WIDGET (self)) || self->firework_timeout)
    return;

  self->burst_data[0].index = 0;
  self->burst_data[0].self = self;
  burst_fire (&self->burst_data[0]);
  firework_schedule (self);
}

void
kgx_edge_set_ambient (KgxEdge *self,
                      gboolean ambient)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  if (self->ambient_active == ambient)
    return;

  self->ambient_active = ambient;

  if (ambient) {
    if (!self->ambient_enabled)
      return;

    if (!self->ambient_timeout)
      self->ambient_timeout = schedule_burst_timeout (50,
                                                      ambient_fire,
                                                      self,
                                                      "[kgx] edge ambient");
    return;
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
}

void
kgx_edge_stop_ambient_immediate (KgxEdge *self)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  if (self->ambient_timeout) {
    g_source_remove (self->ambient_timeout);
    self->ambient_timeout = 0;
  }

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->ambient_burst_timeout[i]) {
      g_source_remove (self->ambient_burst_timeout[i]);
      self->ambient_burst_timeout[i] = 0;
    }
    self->ambient_progress[i] = -1.0;
    if (self->ambient_anim[i])
      adw_animation_reset (self->ambient_anim[i]);
  }

  kgx_edge_mark_dirty (self);
}
