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
process_particle_value_cb (double value,
                           KgxEdge *self)
{
  if (self->pending_change && (self->process_linear || value >= 0.5)) {
    adw_animation_skip (self->process_anim);
    return;
  }

  self->process_progress = value;
  kgx_edge_mark_dirty (self);
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

  if (self->process_anim)
    adw_animation_reset (self->process_anim);

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
}

static void
process_particle_done_cb (KgxEdge *self)
{
  int old_extent = kgx_edge_get_strip_extent (self);

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

  if (self->process_preset == KGX_PARTICLE_ROTATE ||
      self->process_preset == KGX_PARTICLE_PING_PONG ||
      self->process_reverse_mode == 2) {
    if (self->process_reverse_mode == 2)
      self->process_reverse = !self->process_reverse;

    self->process_tune_snap = *kgx_edge_resolve_tunables (self, self->process_preset);
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
                               int                 shape_override,
                               int                 gap_override,
                               int                 speed_override,
                               int                 thk_override)
{
  int old_extent;
  guint duration;
  double spd;

  g_return_if_fail (KGX_IS_EDGE (self));

  self = kgx_edge_get_root (self);
  old_extent = kgx_edge_get_strip_extent (self);

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
    kgx_edge_start_process_bursts (self);
    kgx_edge_queue_resize_views_if_needed (self, old_extent);
    return;
  }

  if (!self->process_anim) {
    AdwAnimationTarget *target = adw_callback_animation_target_new (
      (AdwAnimationTargetFunc) process_particle_value_cb,
      self,
      NULL);

    self->process_anim = adw_timed_animation_new (GTK_WIDGET (self),
                                                  0.0, 1.0, 2000, target);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->process_anim),
                                    ADW_EASE_OUT_CUBIC);
    g_signal_connect_swapped (self->process_anim, "done",
                              G_CALLBACK (process_particle_done_cb), self);
  }

  self->process_tune_snap = *kgx_edge_resolve_tunables (self, preset);
  if (shape_override >= 0)
    self->process_tune_snap.shape = shape_override;
  if (gap_override >= 0)
    self->process_tune_snap.gap = gap_override;
  if (speed_override > 0)
    self->process_tune_snap.speed = speed_override / 100.0;
  if (thk_override > 0)
    self->process_tune_snap.thickness = thk_override;

  spd = self->process_tune_snap.speed;
  adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->process_anim),
                                  (preset == KGX_PARTICLE_ROTATE ||
                                   preset == KGX_PARTICLE_PING_PONG)
                                    ? ADW_LINEAR
                                    : ADW_EASE_OUT_CUBIC);

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

  adw_timed_animation_set_duration (ADW_TIMED_ANIMATION (self->process_anim),
                                    duration);
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
