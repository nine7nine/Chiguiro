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

static inline gint64
delay_to_due_us (gint64 now_us,
                 guint  delay_ms)
{
  return now_us + ((gint64) delay_ms * 1000);
}

static void firework_schedule (KgxEdge *self,
                               gint64   now_us);
static void ambient_schedule (KgxEdge *self,
                              gint64   now_us);

static gboolean
burst_track_active (KgxEdge *self,
                    int      index)
{
  return self->burst_start_us[index] > 0 ||
         self->burst_due_us[index] > 0 ||
         self->burst_progress[index] >= 0.0;
}

static gboolean
ambient_burst_track_active (KgxEdge *self,
                            int      index)
{
  return self->ambient_burst_start_us[index] > 0 ||
         self->ambient_burst_due_us[index] > 0 ||
         self->ambient_progress[index] >= 0.0;
}

static void
clear_pending_firework_schedule (KgxEdge *self)
{
  self->firework_due_us = 0;

  for (int i = 0; i < MAX_BURSTS; i++)
    self->burst_due_us[i] = 0;
}

static void
clear_pending_ambient_schedule (KgxEdge *self)
{
  self->ambient_due_us = 0;

  for (int i = 0; i < MAX_BURSTS; i++)
    self->ambient_burst_due_us[i] = 0;
}

static void
start_firework_burst (KgxEdge *self,
                      int      index,
                      gint64   now_us)
{
  const KgxParticleTunables *bt;
  int width;
  int height;
  double perim;

  self->burst_due_us[index] = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  kgx_edge_get_canvas_size (GTK_WIDGET (self), &width, &height);
  perim = 2.0 * (width + height);
  self->burst_head[index] = g_random_double () * perim;

  if (self->process_preset == KGX_PARTICLE_FIREWORKS)
    self->burst_color[index] = self->process_color;
  else
    self->burst_color[index] = random_muted_color ();

  bt = (self->process_preset == KGX_PARTICLE_FIREWORKS)
         ? kgx_edge_resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
         : &self->global;
  self->burst_tune_snap[index] = *bt;
  self->burst_duration_s[index] = 0.8 / bt->speed;
  self->burst_start_us[index] = now_us;
  self->burst_progress[index] = 0.0;
}

static void
firework_fire (KgxEdge *self,
               gint64   now_us)
{
  self->firework_due_us = 0;

  if (!firework_active (self) || !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  start_firework_burst (self, 0, now_us);

  for (int i = 1; i < self->burst_count; i++) {
    int lo;
    int hi;

    if (burst_track_active (self, i))
      continue;

    lo = (int) (150 * i * self->burst_spread);
    hi = (int) (600 * i * self->burst_spread);
    self->burst_due_us[i] = delay_to_due_us (
      now_us,
      g_random_int_range (MAX (lo, 50), MAX (hi, 100)));
  }

  firework_schedule (self, now_us);
}

static void
firework_schedule (KgxEdge *self,
                   gint64   now_us)
{
  const KgxParticleTunables *ft;
  double fw_spd;
  int lo;
  int hi;
  guint delay;

  if (self->firework_due_us || !firework_active (self))
    return;

  ft = (self->process_preset == KGX_PARTICLE_FIREWORKS)
         ? kgx_edge_resolve_tunables (self, KGX_PARTICLE_FIREWORKS)
         : &self->global;
  fw_spd = ft->speed;
  lo = (int) (600 * self->burst_spread / fw_spd);
  hi = (int) (1200 * self->burst_spread / fw_spd);
  delay = (!burst_track_active (self, 0))
        ? 200
        : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->firework_due_us = delay_to_due_us (now_us, delay);
}

static void
start_ambient_burst (KgxEdge *self,
                     int      index,
                     gint64   now_us)
{
  const KgxParticleTunables *bt;
  int width;
  int height;
  double perim;

  self->ambient_burst_due_us[index] = 0;

  if (!self->ambient_active || !self->ambient_enabled ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  kgx_edge_get_canvas_size (GTK_WIDGET (self), &width, &height);
  perim = 2.0 * (width + height);
  self->ambient_head[index] = g_random_double () * perim;
  self->ambient_burst_color[index] = random_muted_color ();

  bt = &self->preset[KGX_PARTICLE_AMBIENT - 1];
  self->ambient_tune_snap[index] = *bt;
  self->ambient_burst_duration_s[index] = 0.8 / bt->speed;
  self->ambient_burst_start_us[index] = now_us;
  self->ambient_progress[index] = 0.0;
}

static void
ambient_fire (KgxEdge *self,
              gint64   now_us)
{
  self->ambient_due_us = 0;

  if (!self->ambient_active || !self->ambient_enabled ||
      !gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  start_ambient_burst (self, 0, now_us);

  for (int i = 1; i < self->ambient_burst_count; i++) {
    int lo;
    int hi;

    if (ambient_burst_track_active (self, i))
      continue;

    lo = (int) (150 * i * self->ambient_burst_spread);
    hi = (int) (600 * i * self->ambient_burst_spread);
    self->ambient_burst_due_us[i] = delay_to_due_us (
      now_us,
      g_random_int_range (MAX (lo, 50), MAX (hi, 100)));
  }

  ambient_schedule (self, now_us);
}

static void
ambient_schedule (KgxEdge *self,
                  gint64   now_us)
{
  const KgxParticleTunables *ft;
  double spd;
  int lo;
  int hi;
  guint delay;

  if (self->ambient_due_us || !self->ambient_active || !self->ambient_enabled)
    return;

  ft = &self->preset[KGX_PARTICLE_AMBIENT - 1];
  spd = ft->speed;
  lo = (int) (600 * self->ambient_burst_spread / spd);
  hi = (int) (1200 * self->ambient_burst_spread / spd);
  delay = (!ambient_burst_track_active (self, 0))
        ? 200
        : g_random_int_range (MAX (lo, 200), MAX (hi, 400));

  self->ambient_due_us = delay_to_due_us (now_us, delay);
}

static gboolean
advance_burst_progress (double *progress,
                        gint64 *start_us,
                        double *duration_s,
                        gint64  now_us)
{
  double raw;
  double eased;

  if (*start_us <= 0 || *progress < 0.0)
    return FALSE;

  raw = kgx_edge_timeline_progress (now_us, *start_us, *duration_s);
  eased = kgx_edge_timeline_ease_out_cubic (raw);

  if (raw >= 1.0) {
    *progress = -1.0;
    *start_us = 0;
    *duration_s = 0.0;
    return TRUE;
  }

  if (*progress == eased)
    return FALSE;

  *progress = eased;
  return TRUE;
}

void
kgx_edge_init_bursts (KgxEdge *self)
{
  self->firework_due_us = 0;
  self->ambient_due_us = 0;

  for (int i = 0; i < MAX_BURSTS; i++) {
    self->burst_progress[i] = -1.0;
    self->burst_due_us[i] = 0;
    self->burst_start_us[i] = 0;
    self->burst_duration_s[i] = 0.0;

    self->ambient_progress[i] = -1.0;
    self->ambient_burst_due_us[i] = 0;
    self->ambient_burst_start_us[i] = 0;
    self->ambient_burst_duration_s[i] = 0.0;
  }
}

void
kgx_edge_clear_bursts (KgxEdge *self)
{
  clear_pending_firework_schedule (self);
  clear_pending_ambient_schedule (self);

  for (int i = 0; i < MAX_BURSTS; i++) {
    self->burst_progress[i] = -1.0;
    self->burst_start_us[i] = 0;
    self->burst_duration_s[i] = 0.0;

    self->ambient_progress[i] = -1.0;
    self->ambient_burst_start_us[i] = 0;
    self->ambient_burst_duration_s[i] = 0.0;
  }
}

void
kgx_edge_resume_bursts (KgxEdge *self)
{
  gint64 now_us;
  gboolean need_driver = FALSE;

  now_us = g_get_monotonic_time ();

  if (firework_active (self) && self->firework_due_us == 0) {
    gboolean any_firework_activity = FALSE;

    for (int i = 0; i < MAX_BURSTS && !any_firework_activity; i++)
      any_firework_activity = burst_track_active (self, i);

    if (!any_firework_activity)
      firework_schedule (self, now_us);
  }

  if (self->ambient_active && self->ambient_enabled && self->ambient_due_us == 0) {
    gboolean any_ambient_activity = FALSE;

    for (int i = 0; i < MAX_BURSTS && !any_ambient_activity; i++)
      any_ambient_activity = ambient_burst_track_active (self, i);

    if (!any_ambient_activity)
      ambient_schedule (self, now_us);
  }

  need_driver = firework_active (self) ||
                self->ambient_active ||
                kgx_edge_has_scheduled_bursts (self);

  if (need_driver)
    kgx_edge_mark_dirty (self);
}

void
kgx_edge_start_process_bursts (KgxEdge *self)
{
  gint64 now_us;

  g_return_if_fail (KGX_IS_EDGE (self));

  if (!gtk_widget_get_mapped (GTK_WIDGET (self)))
    return;

  now_us = g_get_monotonic_time ();

  if (self->firework_due_us || burst_track_active (self, 0))
    return;

  start_firework_burst (self, 0, now_us);
  firework_schedule (self, now_us);
  kgx_edge_mark_dirty (self);
}

void
kgx_edge_set_ambient (KgxEdge *self,
                      gboolean ambient)
{
  gint64 now_us;

  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  if (self->ambient_active == ambient)
    return;

  self->ambient_active = ambient;
  now_us = g_get_monotonic_time ();

  if (ambient) {
    if (!self->ambient_enabled)
      return;

    if (self->ambient_due_us == 0)
      self->ambient_due_us = delay_to_due_us (now_us, 50);

    kgx_edge_mark_dirty (self);
    return;
  }

  clear_pending_ambient_schedule (self);
  kgx_edge_mark_dirty (self);
}

void
kgx_edge_stop_ambient_immediate (KgxEdge *self)
{
  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);

  clear_pending_ambient_schedule (self);

  for (int i = 0; i < MAX_BURSTS; i++) {
    self->ambient_progress[i] = -1.0;
    self->ambient_burst_start_us[i] = 0;
    self->ambient_burst_duration_s[i] = 0.0;
  }

  kgx_edge_mark_dirty (self);
}

gboolean
kgx_edge_advance_bursts (KgxEdge *self,
                         gint64   now_us)
{
  gboolean changed = FALSE;
  gboolean firework_scheduled = FALSE;
  gboolean ambient_scheduled = FALSE;

  g_return_val_if_fail (KGX_IS_EDGE (self), FALSE);

  self = kgx_edge_get_root (self);

  firework_scheduled = (self->firework_due_us != 0);
  ambient_scheduled = (self->ambient_due_us != 0);
  for (int i = 0; i < MAX_BURSTS; i++) {
    firework_scheduled = firework_scheduled || self->burst_due_us[i] != 0;
    ambient_scheduled = ambient_scheduled || self->ambient_burst_due_us[i] != 0;
  }

  if (!firework_active (self) && firework_scheduled) {
    for (int i = 0; i < MAX_BURSTS; i++) {
      if (self->burst_due_us[i] != 0) {
        self->burst_due_us[i] = 0;
        changed = TRUE;
      }
    }

    if (self->firework_due_us != 0) {
      self->firework_due_us = 0;
      changed = TRUE;
    }
  }

  if ((!self->ambient_active || !self->ambient_enabled) && ambient_scheduled) {
    for (int i = 0; i < MAX_BURSTS; i++) {
      if (self->ambient_burst_due_us[i] != 0) {
        self->ambient_burst_due_us[i] = 0;
        changed = TRUE;
      }
    }

    if (self->ambient_due_us != 0) {
      self->ambient_due_us = 0;
      changed = TRUE;
    }
  }

  if (self->firework_due_us > 0 && now_us >= self->firework_due_us) {
    firework_fire (self, now_us);
    changed = TRUE;
  }

  if (self->ambient_due_us > 0 && now_us >= self->ambient_due_us) {
    ambient_fire (self, now_us);
    changed = TRUE;
  }

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_due_us[i] > 0 && now_us >= self->burst_due_us[i]) {
      start_firework_burst (self, i, now_us);
      changed = TRUE;
    }

    if (advance_burst_progress (&self->burst_progress[i],
                                &self->burst_start_us[i],
                                &self->burst_duration_s[i],
                                now_us))
      changed = TRUE;

    if (self->ambient_burst_due_us[i] > 0 && now_us >= self->ambient_burst_due_us[i]) {
      start_ambient_burst (self, i, now_us);
      changed = TRUE;
    }

    if (advance_burst_progress (&self->ambient_progress[i],
                                &self->ambient_burst_start_us[i],
                                &self->ambient_burst_duration_s[i],
                                now_us))
      changed = TRUE;
  }

  return changed;
}

gboolean
kgx_edge_has_scheduled_bursts (KgxEdge *self)
{
  g_return_val_if_fail (KGX_IS_EDGE (self), FALSE);

  self = kgx_edge_get_root (self);

  if (self->firework_due_us > 0 || self->ambient_due_us > 0)
    return TRUE;

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_due_us[i] > 0 || self->ambient_burst_due_us[i] > 0)
      return TRUE;
  }

  return FALSE;
}

gint64
kgx_edge_get_next_burst_wakeup_us (KgxEdge *self)
{
  gint64 next_us = 0;

  g_return_val_if_fail (KGX_IS_EDGE (self), 0);

  self = kgx_edge_get_root (self);

  if (self->firework_due_us > 0)
    next_us = self->firework_due_us;

  if (self->ambient_due_us > 0 &&
      (next_us == 0 || self->ambient_due_us < next_us))
    next_us = self->ambient_due_us;

  for (int i = 0; i < MAX_BURSTS; i++) {
    if (self->burst_due_us[i] > 0 &&
        (next_us == 0 || self->burst_due_us[i] < next_us))
      next_us = self->burst_due_us[i];

    if (self->ambient_burst_due_us[i] > 0 &&
        (next_us == 0 || self->ambient_burst_due_us[i] < next_us))
      next_us = self->ambient_burst_due_us[i];
  }

  return next_us;
}
