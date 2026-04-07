/* kgx-edge-process.c
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

static void
kgx_edge_process_complete (KgxEdge *self,
                           gint64   now_us)
{
  int old_extent = kgx_edge_get_strip_extent (self);

  if (self->pending_change) {
    self->pending_change = FALSE;
    kgx_edge_set_process_particle (self,
                                   self->pending_preset,
                                   &self->pending_color,
                                   self->pending_reverse,
                                   &self->pending_overrides);
    return;
  }

  if (self->process_preset == KGX_PARTICLE_ROTATE ||
      self->process_preset == KGX_PARTICLE_PING_PONG ||
      self->process_reverse_mode == 2) {
    if (self->process_reverse_mode == 2)
      self->process_reverse = !self->process_reverse;

    self->process_tune_snap = *kgx_edge_resolve_tunables (self, self->process_preset);
    kgx_edge_apply_process_overrides (&self->process_tune_snap,
                                      &self->process_overrides);
    self->process_start_us = now_us;
    self->process_progress = 0.0;
    self->process_last_snapshot_progress = -1.0;
    return;
  }

  self->process_progress = -1.0;
  self->process_last_snapshot_progress = -1.0;
  self->process_start_us = 0;
  self->process_duration_s = 0.0;
  self->process_linear = FALSE;
  kgx_edge_queue_resize_views_if_needed (self, old_extent);
}

static void
kgx_edge_stop_process_particle_now (KgxEdge *self,
                                    int      old_extent)
{
  self->pending_change = FALSE;
  self->process_preset = KGX_PARTICLE_NONE;
  self->process_progress = -1.0;
  self->process_last_snapshot_progress = -1.0;
  self->process_start_us = 0;
  self->process_duration_s = 0.0;
  self->process_linear = FALSE;
  self->firework_due_us = 0;

  for (int i = 0; i < MAX_BURSTS; i++) {
    self->burst_due_us[i] = 0;
    self->burst_progress[i] = -1.0;
    self->burst_start_us[i] = 0;
    self->burst_duration_s[i] = 0.0;
  }

  kgx_edge_queue_resize_views_if_needed (self, old_extent);
  kgx_edge_mark_dirty (self);
}

gboolean
kgx_edge_advance_process_timeline (KgxEdge *self,
                                   gint64   now_us)
{
  double raw;
  double progress;

  g_return_val_if_fail (KGX_IS_EDGE (self), FALSE);

  self = kgx_edge_get_root (self);

  if (self->process_preset == KGX_PARTICLE_NONE ||
      firework_active (self) ||
      self->process_progress < 0.0 ||
      self->process_start_us <= 0 ||
      self->process_duration_s <= 0.0)
    return FALSE;

  raw = kgx_edge_timeline_progress (now_us,
                                    self->process_start_us,
                                    self->process_duration_s);
  progress = self->process_linear
           ? raw
           : kgx_edge_timeline_ease_out_cubic (raw);

  if (self->pending_change &&
      (self->process_linear || progress >= 0.5 || raw >= 1.0)) {
    kgx_edge_process_complete (self, now_us);
    return TRUE;
  }

  if (raw >= 1.0) {
    kgx_edge_process_complete (self, now_us);
    return TRUE;
  }

  if (self->process_progress == progress)
    return FALSE;

  self->process_progress = progress;
  return TRUE;
}

void
kgx_edge_stop_process_particle_immediate (KgxEdge *self)
{
  int old_extent;

  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);
  old_extent = kgx_edge_get_strip_extent (self);
  kgx_edge_stop_process_particle_now (self, old_extent);
}

void
kgx_edge_set_process_particle (KgxEdge            *self,
                               KgxParticlePreset   preset,
                               const GdkRGBA      *color,
                               int                 reverse,
                               const KgxProcessParticleOverrides *overrides)
{
  int old_extent;
  guint duration;
  double spd;

  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);
  old_extent = kgx_edge_get_strip_extent (self);

  if (self->process_progress >= 0.0 &&
      !firework_active (self) &&
      (preset != self->process_preset || preset == KGX_PARTICLE_NONE)) {
    self->pending_change = TRUE;
    self->pending_preset = preset;
    self->pending_reverse = reverse;
    self->pending_overrides = overrides
      ? *overrides
      : (KgxProcessParticleOverrides) {
          .shape = -1,
          .gap = -1,
          .speed = 0,
          .thickness = 0,
          .tail_length = 0,
          .env_attack = 0,
          .env_release = 0,
          .release_mode = -1,
          .thk_attack = 0,
          .thk_release = 0,
          .thk_release_mode = -1,
        };
    if (color)
      self->pending_color = *color;
    else
      self->pending_color = (GdkRGBA) { 0.5f, 0.5f, 0.5f, 1.0f };
    return;
  }

  self->pending_change = FALSE;
  self->process_preset = preset;
  self->process_overrides = overrides
    ? *overrides
    : (KgxProcessParticleOverrides) {
        .shape = -1,
        .gap = -1,
        .speed = 0,
        .thickness = 0,
        .tail_length = 0,
        .env_attack = 0,
        .env_release = 0,
        .release_mode = -1,
        .thk_attack = 0,
        .thk_release = 0,
        .thk_release_mode = -1,
      };

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
    kgx_edge_stop_process_particle_now (self, old_extent);
    return;
  }

  if (preset == KGX_PARTICLE_FIREWORKS || preset == KGX_PARTICLE_AMBIENT) {
    self->process_progress = -1.0;
    self->process_last_snapshot_progress = -1.0;
    self->process_start_us = 0;
    self->process_duration_s = 0.0;
    self->process_linear = FALSE;
    kgx_edge_start_process_bursts (self);
    kgx_edge_queue_resize_views_if_needed (self, old_extent);
    return;
  }

  self->process_tune_snap = *kgx_edge_resolve_tunables (self, preset);
  kgx_edge_apply_process_overrides (&self->process_tune_snap,
                                    &self->process_overrides);

  spd = self->process_tune_snap.speed;

  switch (preset) {
  case KGX_PARTICLE_ROTATE:
    duration = (guint) (3500.0 / spd);
    break;
  case KGX_PARTICLE_PING_PONG:
    duration = (guint) (1200.0 / spd);
    break;
  case KGX_PARTICLE_CORNERS:
  case KGX_PARTICLE_PULSE_OUT:
    duration = (guint) (2500.0 / spd);
    break;
  case KGX_PARTICLE_NONE:
  case KGX_PARTICLE_FIREWORKS:
  case KGX_PARTICLE_AMBIENT:
  case KGX_PARTICLE_SCROLL2:
  default:
    duration = (guint) (3000.0 / spd);
    break;
  }

  if (self->process_progress >= 0.0 &&
      self->process_start_us > 0 &&
      preset == self->process_preset)
    return;

  self->process_progress = 0.0;
  self->process_last_snapshot_progress = -1.0;
  self->process_start_us = g_get_monotonic_time ();
  self->process_duration_s = duration / 1000.0;
  self->process_linear = (preset == KGX_PARTICLE_ROTATE ||
                          preset == KGX_PARTICLE_PING_PONG);
  kgx_edge_queue_resize_views_if_needed (self, old_extent);
  kgx_edge_mark_dirty (self);
}
