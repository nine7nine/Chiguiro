/* kgx-particle.c
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

#include <math.h>

#include "kgx-particle.h"

void
kgx_particle_tunable_set_double (KgxParticleTunables *t,
                                 int                  field,
                                 double               value)
{
  switch (field) {
  case TUNE_SPEED:
    t->speed = value;
    break;
  case TUNE_TAIL_LENGTH:
    t->tail_length = value;
    break;
  case TUNE_PULSE_DEPTH:
    t->pulse_depth = value;
    break;
  case TUNE_PULSE_SPEED:
    t->pulse_speed = value;
    break;
  case TUNE_ENV_ATTACK:
    t->env_attack = value;
    break;
  case TUNE_ENV_RELEASE:
    t->env_release = value;
    break;
  case TUNE_THK_ATTACK:
    t->thk_attack = value;
    break;
  case TUNE_THK_RELEASE:
    t->thk_release = value;
    break;
  default:
    break;
  }
}

double
kgx_particle_tunable_get_double (const KgxParticleTunables *t,
                                 int                        field)
{
  switch (field) {
  case TUNE_SPEED:
    return t->speed;
  case TUNE_TAIL_LENGTH:
    return t->tail_length;
  case TUNE_PULSE_DEPTH:
    return t->pulse_depth;
  case TUNE_PULSE_SPEED:
    return t->pulse_speed;
  case TUNE_ENV_ATTACK:
    return t->env_attack;
  case TUNE_ENV_RELEASE:
    return t->env_release;
  case TUNE_THK_ATTACK:
    return t->thk_attack;
  case TUNE_THK_RELEASE:
    return t->thk_release;
  default:
    return 0.0;
  }
}

void
kgx_particle_tunable_set_int (KgxParticleTunables *t,
                              int                  field,
                              int                  value)
{
  switch (field) {
  case TUNE_THICKNESS:
    t->thickness = value;
    break;
  case TUNE_RELEASE_MODE:
    t->release_mode = value;
    break;
  case TUNE_SHAPE:
    t->shape = value;
    break;
  case TUNE_ENV_CURVE:
    t->env_curve = value;
    break;
  case TUNE_GAP:
    t->gap = value;
    break;
  case TUNE_THK_RELEASE_MODE:
    t->thk_release_mode = value;
    break;
  case TUNE_THK_CURVE:
    t->thk_curve = value;
    break;
  default:
    break;
  }
}

int
kgx_particle_tunable_get_int (const KgxParticleTunables *t,
                              int                        field)
{
  switch (field) {
  case TUNE_THICKNESS:
    return t->thickness;
  case TUNE_RELEASE_MODE:
    return t->release_mode;
  case TUNE_SHAPE:
    return t->shape;
  case TUNE_ENV_CURVE:
    return t->env_curve;
  case TUNE_GAP:
    return t->gap;
  case TUNE_THK_RELEASE_MODE:
    return t->thk_release_mode;
  case TUNE_THK_CURVE:
    return t->thk_curve;
  default:
    return 0;
  }
}

float
kgx_particle_envelope (double t,
                       double attack,
                       double release,
                       int    curve)
{
  float linear;

  if (attack > 0.0 && t < attack) {
    linear = (float) (t / attack);
    if (curve == 1)
      return sqrtf (linear);
    if (curve == 3)
      return linear * linear;
    return linear;
  }

  if (release > 0.0 && t > 1.0 - release) {
    linear = (float) ((1.0 - t) / release);
    if (curve == 1)
      return sqrtf (linear);
    if (curve == 3)
      return linear * linear;
    return linear;
  }

  return 1.0f;
}

float
kgx_particle_thickness_envelope (double t,
                                 double attack,
                                 double release,
                                 int    curve)
{
  return kgx_particle_envelope (t, attack, release, curve);
}

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
  case KGX_PARTICLE_NONE:
    return "none";
  case KGX_PARTICLE_FIREWORKS:
    return "fireworks";
  case KGX_PARTICLE_CORNERS:
    return "corners";
  case KGX_PARTICLE_PULSE_OUT:
    return "pulse-out";
  case KGX_PARTICLE_ROTATE:
    return "rotate";
  case KGX_PARTICLE_PING_PONG:
    return "ping-pong";
  case KGX_PARTICLE_AMBIENT:
    return "ambient";
  case KGX_PARTICLE_SCROLL2:
    return "none";
  default:
    return "none";
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
  int n;

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
  n = g_strv_length (parts);

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
