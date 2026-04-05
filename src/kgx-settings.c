/* kgx-settings.c
 *
 * Copyright 2022-2023 Zander Brown
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

#include <gio/gio.h>
#include <vte/vte.h>

#include "kgx-edge.h"
#include "kgx-livery-manager.h"
#include "kgx-livery.h"
#include "kgx-marshals.h"
#include "kgx-system-info.h"
#include "kgx-templated.h"
#include "kgx-utils.h"

#include "kgx-settings.h"

#define RESTORE_SIZE_KEY "restore-window-size"
#define LAST_SIZE_KEY "last-window-size"
#define LAST_MAXIMISED_KEY "last-window-maximised"

#define AUDIBLE_BELL "audible-bell"

#define CUSTOM_FONT "custom-font"

struct _KgxSettings {
  GObject               parent_instance;

  PangoFontDescription *font;
  double                scale;
  int                   scrollback_lines;
  gboolean              audible_bell;
  gboolean              visual_bell;
  gboolean              use_system_font;
  PangoFontDescription *custom_font;
  int64_t               scrollback_limit;
  gboolean              ignore_scrollback_limit;
  gboolean              software_flow_control;
  KgxLivery            *livery;
  gboolean              transparency;
  double                transparency_level;
  double                glass_opacity;
  gboolean              use_glass_bg;
  char                 *glass_color;
  char                 *accent_color;

  gboolean              edge_overscroll;
  char                 *edge_overscroll_color;
  int                   edge_overscroll_style;
  int                   edge_overscroll_reverse;
  gboolean              edge_settings_animation;
  int                   edge_burst_count_ambient;
  double                edge_burst_spread_ambient;
  KgxParticleTunables   edge_global;
  KgxParticleTunables   edge_preset[N_PRESETS];
  int                   edge_burst_count;
  double                edge_burst_spread;
  GHashTable           *process_glass_colors;  /* string→string */

  KgxLiveryManager     *livery_manager;

  GSettings            *settings;
  KgxSystemInfo        *system_info;
};


G_DEFINE_FINAL_TYPE_WITH_CODE (KgxSettings, kgx_settings, G_TYPE_OBJECT,
                               G_IMPLEMENT_INTERFACE (KGX_TYPE_TEMPLATED, NULL))


enum {
  PROP_0,
  PROP_FONT,
  PROP_FONT_SCALE,
  PROP_SCALE_CAN_INCREASE,
  PROP_SCALE_CAN_DECREASE,
  PROP_SCROLLBACK_LINES,
  PROP_AUDIBLE_BELL,
  PROP_VISUAL_BELL,
  PROP_USE_SYSTEM_FONT,
  PROP_CUSTOM_FONT,
  PROP_SCROLLBACK_LIMIT,
  PROP_IGNORE_SCROLLBACK_LIMIT,
  PROP_SOFTWARE_FLOW_CONTROL,
  PROP_LIVERY,
  PROP_TRANSPARENCY,
  PROP_TRANSPARENCY_LEVEL,
  PROP_GLASS_OPACITY,
  PROP_GLASS_COLOR,
  PROP_USE_GLASS_BG,
  PROP_ACCENT_COLOR,
  PROP_PROCESS_GLASS_COLORS,
  PROP_EDGE_OVERSCROLL,
  PROP_EDGE_OVERSCROLL_COLOR,
  PROP_EDGE_OVERSCROLL_STYLE,
  PROP_EDGE_OVERSCROLL_REVERSE,
  PROP_EDGE_SETTINGS_ANIMATION,
  PROP_EDGE_BURST_COUNT_AMBIENT,
  PROP_EDGE_BURST_SPREAD_AMBIENT,
  /* Indexed tunables: global (N_TUNE_FIELDS values) */
  PROP_EDGE_GLOBAL_BASE,
  /* Per-preset tunables (N_PRESETS * N_TUNE_FIELDS values) */
  PROP_EDGE_PRESET_BASE = PROP_EDGE_GLOBAL_BASE + N_TUNE_FIELDS,
  /* Non-tunable edge props that remain explicit */
  PROP_EDGE_BURST_COUNT = PROP_EDGE_PRESET_BASE + N_PRESETS * N_TUNE_FIELDS,
  PROP_EDGE_BURST_SPREAD,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };

/* Property name tables — keep in sync with TUNE_* indices and kgx-edge.c */
static const char * const tune_names[N_TUNE_FIELDS] = {
  "speed", "thickness", "tail-length", "pulse-depth", "pulse-speed",
  "env-attack", "env-release", "release-mode", "shape",
  "env-curve", "gap", "thk-attack", "thk-release", "thk-release-mode", "thk-curve"
};
static const char * const preset_suffixes[N_PRESETS] = {
  "fireworks", "corners", "pulse-out", "rotate", "ping-pong", "ambient", "scroll2"
};

/* Tunable field metadata for table-driven property installation */
static const struct {
  double dmin, dmax, global_default, preset_default;
  int    imin, imax, iglobal_default, ipreset_default;
  gboolean is_int;
} tune_meta[N_TUNE_FIELDS] = {
  [TUNE_SPEED]        = { 0.1, 3.0, 1.0, 1.0,  0, 0, 0, 0, FALSE },
  [TUNE_THICKNESS]    = { 0,   0,   0,   0,    2, 40, 6, 20, TRUE },
  [TUNE_TAIL_LENGTH]  = { 0.1, 3.0, 1.0, 0.9, 0, 0, 0, 0, FALSE },
  [TUNE_PULSE_DEPTH]  = { 0.0, 1.0, 0.3, 0.5, 0, 0, 0, 0, FALSE },
  [TUNE_PULSE_SPEED]  = { 0.1, 5.0, 1.0, 0.8, 0, 0, 0, 0, FALSE },
  [TUNE_ENV_ATTACK]   = { 0.0, 0.5, 0.2, 0.2, 0, 0, 0, 0, FALSE },
  [TUNE_ENV_RELEASE]  = { 0.0, 0.5, 0.3, 0.3, 0, 0, 0, 0, FALSE },
  [TUNE_RELEASE_MODE] = { 0,   0,   0,   0,   0, 3, 0, 0, TRUE },
  [TUNE_SHAPE]        = { 0,   0,   0,   0,   0, 3, 0, 0, TRUE },
  [TUNE_ENV_CURVE]    = { 0,   0,   0,   0,   1, 3, 2, 2, TRUE },
  [TUNE_GAP]          = { 0,   0,   0,   0,   0, 1, 0, 0, TRUE },
  [TUNE_THK_ATTACK]   = { 0.0, 0.5, 0.0, 0.0, 0, 0, 0, 0, FALSE },
  [TUNE_THK_RELEASE]  = { 0.0, 0.5, 0.0, 0.0, 0, 0, 0, 0, FALSE },
  [TUNE_THK_RELEASE_MODE] = { 0, 0, 0, 0,    0, 4, 0, 0, TRUE },
  [TUNE_THK_CURVE]    = { 0,   0,   0,   0,   1, 3, 2, 2, TRUE },
};

/* ── Tunable field accessors ──────────────────────────────── */

static inline double
get_tunable_double_field (const KgxParticleTunables *t, int field)
{
  switch (field) {
  case TUNE_SPEED:       return t->speed;
  case TUNE_TAIL_LENGTH: return t->tail_length;
  case TUNE_PULSE_DEPTH: return t->pulse_depth;
  case TUNE_PULSE_SPEED: return t->pulse_speed;
  case TUNE_ENV_ATTACK:  return t->env_attack;
  case TUNE_ENV_RELEASE: return t->env_release;
  case TUNE_THK_ATTACK:  return t->thk_attack;
  case TUNE_THK_RELEASE: return t->thk_release;
  default:               return 0.0;
  }
}

static inline void
set_tunable_double_field (KgxParticleTunables *t, int field, double v)
{
  switch (field) {
  case TUNE_SPEED:       t->speed       = v; break;
  case TUNE_TAIL_LENGTH: t->tail_length = v; break;
  case TUNE_PULSE_DEPTH: t->pulse_depth = v; break;
  case TUNE_PULSE_SPEED: t->pulse_speed = v; break;
  case TUNE_ENV_ATTACK:  t->env_attack  = v; break;
  case TUNE_ENV_RELEASE: t->env_release = v; break;
  case TUNE_THK_ATTACK:  t->thk_attack  = v; break;
  case TUNE_THK_RELEASE: t->thk_release = v; break;
  default: break;
  }
}

static inline int
get_tunable_int_field (const KgxParticleTunables *t, int field)
{
  switch (field) {
  case TUNE_THICKNESS:        return t->thickness;
  case TUNE_RELEASE_MODE:     return t->release_mode;
  case TUNE_SHAPE:            return t->shape;
  case TUNE_ENV_CURVE:        return t->env_curve;
  case TUNE_GAP:              return t->gap;
  case TUNE_THK_RELEASE_MODE: return t->thk_release_mode;
  case TUNE_THK_CURVE:        return t->thk_curve;
  default:                return 0;
  }
}

static inline void
set_tunable_int_field (KgxParticleTunables *t, int field, int v)
{
  switch (field) {
  case TUNE_THICKNESS:        t->thickness        = v; break;
  case TUNE_RELEASE_MODE:     t->release_mode     = v; break;
  case TUNE_SHAPE:            t->shape            = v; break;
  case TUNE_ENV_CURVE:        t->env_curve        = v; break;
  case TUNE_GAP:              t->gap              = v; break;
  case TUNE_THK_RELEASE_MODE: t->thk_release_mode = v; break;
  case TUNE_THK_CURVE:        t->thk_curve        = v; break;
  default: break;
  }
}


static void
kgx_settings_dispose (GObject *object)
{
  KgxSettings *self = KGX_SETTINGS (object);

  kgx_templated_dispose_template (KGX_TEMPLATED (object), KGX_TYPE_SETTINGS);

  g_clear_pointer (&self->font, pango_font_description_free);
  g_clear_pointer (&self->custom_font, pango_font_description_free);
  g_clear_pointer (&self->livery, kgx_livery_unref);
  g_clear_pointer (&self->process_glass_colors, g_hash_table_unref);
  g_clear_pointer (&self->glass_color, g_free);
  g_clear_pointer (&self->accent_color, g_free);
  g_clear_pointer (&self->edge_overscroll_color, g_free);

  G_OBJECT_CLASS (kgx_settings_parent_class)->dispose (object);
}


static void
update_scale (KgxSettings *self, double value)
{
  double clamped = CLAMP (value, KGX_FONT_SCALE_MIN, KGX_FONT_SCALE_MAX);

  if (self->scale == clamped) {
    return;
  }

  self->scale = clamped;

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_FONT_SCALE]);
  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_SCALE_CAN_INCREASE]);
  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_SCALE_CAN_DECREASE]);
}


static void
kgx_settings_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  KgxSettings *self = KGX_SETTINGS (object);

  /* ── Indexed tunable ranges ─────────────────────────────── */
  if (property_id >= PROP_EDGE_PRESET_BASE &&
      property_id < PROP_EDGE_PRESET_BASE + N_PRESETS * N_TUNE_FIELDS) {
    int idx   = property_id - PROP_EDGE_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    KgxParticleTunables *t = &self->edge_preset[pi];
    if (tune_meta[field].is_int) {
      int nv = CLAMP (g_value_get_int (value),
                       tune_meta[field].imin, tune_meta[field].imax);
      if (nv != get_tunable_int_field (t, field)) {
        set_tunable_int_field (t, field, nv);
        g_object_notify_by_pspec (object, pspec);
      }
    } else {
      double nv = CLAMP (g_value_get_double (value),
                          tune_meta[field].dmin, tune_meta[field].dmax);
      if (!G_APPROX_VALUE (get_tunable_double_field (t, field), nv, DBL_EPSILON)) {
        set_tunable_double_field (t, field, nv);
        g_object_notify_by_pspec (object, pspec);
      }
    }
    return;
  }

  if (property_id >= PROP_EDGE_GLOBAL_BASE &&
      property_id < PROP_EDGE_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_EDGE_GLOBAL_BASE;
    KgxParticleTunables *t = &self->edge_global;
    if (tune_meta[field].is_int) {
      int nv = CLAMP (g_value_get_int (value),
                       tune_meta[field].imin, tune_meta[field].imax);
      if (nv != get_tunable_int_field (t, field)) {
        set_tunable_int_field (t, field, nv);
        g_object_notify_by_pspec (object, pspec);
      }
    } else {
      double nv = CLAMP (g_value_get_double (value),
                          tune_meta[field].dmin, tune_meta[field].dmax);
      if (!G_APPROX_VALUE (get_tunable_double_field (t, field), nv, DBL_EPSILON)) {
        set_tunable_double_field (t, field, nv);
        g_object_notify_by_pspec (object, pspec);
      }
    }
    return;
  }

  switch (property_id) {
    case PROP_FONT:
      g_clear_pointer (&self->font, pango_font_description_free);
      self->font = g_value_dup_boxed (value);
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_FONT_SCALE:
      update_scale (self, g_value_get_double (value));
      break;
    case PROP_SCROLLBACK_LINES:
      {
        int new_value = g_value_get_int (value);

        if (new_value != self->scrollback_lines) {
          self->scrollback_lines = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_AUDIBLE_BELL:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->audible_bell,
                            value);
      break;
    case PROP_VISUAL_BELL:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->visual_bell,
                            value);
      break;
    case PROP_USE_SYSTEM_FONT:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->use_system_font,
                            value);
      break;
    case PROP_CUSTOM_FONT:
      kgx_settings_set_custom_font (self, g_value_get_boxed (value));
      break;
    case PROP_SCROLLBACK_LIMIT:
      kgx_settings_set_scrollback_limit (self, g_value_get_int64 (value));
      break;
    case PROP_IGNORE_SCROLLBACK_LIMIT:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->ignore_scrollback_limit,
                            value);
      break;
    case PROP_SOFTWARE_FLOW_CONTROL:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->software_flow_control,
                            value);
      break;
    case PROP_TRANSPARENCY:
      kgx_set_boolean_prop (object,
                            pspec,
                            &self->transparency,
                            value);
      break;
    case PROP_TRANSPARENCY_LEVEL:
      {
        double new_value = CLAMP (g_value_get_double (value), 0.0, 1.0);

        if (!G_APPROX_VALUE (self->transparency_level, new_value, DBL_EPSILON)) {
          self->transparency_level = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_GLASS_OPACITY:
      {
        double new_value = CLAMP (g_value_get_double (value), 0.0, 1.0);

        if (!G_APPROX_VALUE (self->glass_opacity, new_value, DBL_EPSILON)) {
          self->glass_opacity = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_USE_GLASS_BG:
      kgx_set_boolean_prop (object, pspec, &self->use_glass_bg, value);
      break;
    case PROP_GLASS_COLOR:
      g_free (self->glass_color);
      self->glass_color = g_value_dup_string (value);
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_ACCENT_COLOR:
      g_free (self->accent_color);
      self->accent_color = g_value_dup_string (value);
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_PROCESS_GLASS_COLORS:
      g_clear_pointer (&self->process_glass_colors, g_hash_table_unref);
      self->process_glass_colors = g_value_dup_boxed (value);
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_EDGE_OVERSCROLL:
      kgx_set_boolean_prop (object, pspec, &self->edge_overscroll, value);
      break;
    case PROP_EDGE_OVERSCROLL_COLOR:
      g_free (self->edge_overscroll_color);
      self->edge_overscroll_color = g_value_dup_string (value);
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_EDGE_OVERSCROLL_STYLE:
      {
        int new_value = CLAMP (g_value_get_int (value), 0, 1);
        if (new_value != self->edge_overscroll_style) {
          self->edge_overscroll_style = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_EDGE_OVERSCROLL_REVERSE:
      {
        int new_value = CLAMP (g_value_get_int (value), 0, 2);
        if (new_value != self->edge_overscroll_reverse) {
          self->edge_overscroll_reverse = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_EDGE_SETTINGS_ANIMATION:
      kgx_set_boolean_prop (object, pspec, &self->edge_settings_animation, value);
      break;
    case PROP_EDGE_BURST_COUNT_AMBIENT:
      {
        int new_value = CLAMP (g_value_get_int (value), 1, 8);
        if (new_value != self->edge_burst_count_ambient) {
          self->edge_burst_count_ambient = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_EDGE_BURST_SPREAD_AMBIENT:
      {
        double new_value = CLAMP (g_value_get_double (value), 0.5, 5.0);
        if (!G_APPROX_VALUE (self->edge_burst_spread_ambient, new_value, DBL_EPSILON)) {
          self->edge_burst_spread_ambient = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_EDGE_BURST_COUNT:
      {
        int new_value = CLAMP (g_value_get_int (value), 1, 8);
        if (new_value != self->edge_burst_count) {
          self->edge_burst_count = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_EDGE_BURST_SPREAD:
      {
        double new_value = CLAMP (g_value_get_double (value), 0.5, 5.0);
        if (!G_APPROX_VALUE (self->edge_burst_spread, new_value, DBL_EPSILON)) {
          self->edge_burst_spread = new_value;
          g_object_notify_by_pspec (object, pspec);
        }
      }
      break;
    case PROP_LIVERY:
      kgx_settings_set_livery (self, g_value_get_boxed (value));
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static void
kgx_settings_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  KgxSettings *self = KGX_SETTINGS (object);

  /* ── Indexed tunable ranges ─────────────────────────────── */
  if (property_id >= PROP_EDGE_PRESET_BASE &&
      property_id < PROP_EDGE_PRESET_BASE + N_PRESETS * N_TUNE_FIELDS) {
    int idx   = property_id - PROP_EDGE_PRESET_BASE;
    int pi    = idx / N_TUNE_FIELDS;
    int field = idx % N_TUNE_FIELDS;
    const KgxParticleTunables *t = &self->edge_preset[pi];
    if (tune_meta[field].is_int)
      g_value_set_int (value, get_tunable_int_field (t, field));
    else
      g_value_set_double (value, get_tunable_double_field (t, field));
    return;
  }

  if (property_id >= PROP_EDGE_GLOBAL_BASE &&
      property_id < PROP_EDGE_GLOBAL_BASE + N_TUNE_FIELDS) {
    int field = property_id - PROP_EDGE_GLOBAL_BASE;
    const KgxParticleTunables *t = &self->edge_global;
    if (tune_meta[field].is_int)
      g_value_set_int (value, get_tunable_int_field (t, field));
    else
      g_value_set_double (value, get_tunable_double_field (t, field));
    return;
  }

  switch (property_id) {
    case PROP_FONT:
      g_value_set_boxed (value, self->font);
      break;
    case PROP_FONT_SCALE:
      g_value_set_double (value, self->scale);
      break;
    case PROP_SCALE_CAN_INCREASE:
      g_value_set_boolean (value, self->scale < KGX_FONT_SCALE_MAX);
      break;
    case PROP_SCALE_CAN_DECREASE:
      g_value_set_boolean (value, self->scale > KGX_FONT_SCALE_MIN);
      break;
    case PROP_SCROLLBACK_LINES:
      g_value_set_int (value, self->scrollback_lines);
      break;
    case PROP_AUDIBLE_BELL:
      g_value_set_boolean (value, kgx_settings_get_audible_bell (self));
      break;
    case PROP_VISUAL_BELL:
      g_value_set_boolean (value, kgx_settings_get_visual_bell (self));
      break;
    case PROP_USE_SYSTEM_FONT:
      g_value_set_boolean (value, self->use_system_font);
      break;
    case PROP_CUSTOM_FONT:
      g_value_take_boxed (value, kgx_settings_dup_custom_font (self));
      break;
    case PROP_SCROLLBACK_LIMIT:
      g_value_set_int64 (value, self->scrollback_limit);
      break;
    case PROP_IGNORE_SCROLLBACK_LIMIT:
      g_value_set_boolean (value, self->ignore_scrollback_limit);
      break;
    case PROP_SOFTWARE_FLOW_CONTROL:
      g_value_set_boolean (value, kgx_settings_get_software_flow_control (self));
      break;
    case PROP_LIVERY:
      g_value_set_boxed (value, kgx_settings_get_livery (self));
      break;
    case PROP_TRANSPARENCY:
      g_value_set_boolean (value, self->transparency);
      break;
    case PROP_TRANSPARENCY_LEVEL:
      g_value_set_double (value, self->transparency_level);
      break;
    case PROP_GLASS_OPACITY:
      g_value_set_double (value, self->glass_opacity);
      break;
    case PROP_USE_GLASS_BG:
      g_value_set_boolean (value, self->use_glass_bg);
      break;
    case PROP_GLASS_COLOR:
      g_value_set_string (value, self->glass_color ? self->glass_color : "#000000");
      break;
    case PROP_ACCENT_COLOR:
      g_value_set_string (value, self->accent_color ? self->accent_color : "");
      break;
    case PROP_PROCESS_GLASS_COLORS:
      g_value_set_boxed (value, self->process_glass_colors);
      break;
    case PROP_EDGE_OVERSCROLL:
      g_value_set_boolean (value, self->edge_overscroll);
      break;
    case PROP_EDGE_OVERSCROLL_COLOR:
      g_value_set_string (value, self->edge_overscroll_color ? self->edge_overscroll_color : "");
      break;
    case PROP_EDGE_OVERSCROLL_STYLE:
      g_value_set_int (value, self->edge_overscroll_style);
      break;
    case PROP_EDGE_OVERSCROLL_REVERSE:
      g_value_set_int (value, self->edge_overscroll_reverse);
      break;
    case PROP_EDGE_SETTINGS_ANIMATION:
      g_value_set_boolean (value, self->edge_settings_animation);
      break;
    case PROP_EDGE_BURST_COUNT_AMBIENT:
      g_value_set_int (value, self->edge_burst_count_ambient);
      break;
    case PROP_EDGE_BURST_SPREAD_AMBIENT:
      g_value_set_double (value, self->edge_burst_spread_ambient);
      break;
    case PROP_EDGE_BURST_COUNT:
      g_value_set_int (value, self->edge_burst_count);
      break;
    case PROP_EDGE_BURST_SPREAD:
      g_value_set_double (value, self->edge_burst_spread);
      break;
    KGX_INVALID_PROP (object, property_id, pspec);
  }
}


static int
resolve_lines (GObject  *object,
               gboolean  ignore_limit,
               int64_t   limit)
{
  return CLAMP (ignore_limit ? -1 : limit, -1, G_MAXINT);
}


static PangoFontDescription *
resolve_font (GObject              *object,
              gboolean              use_system,
              PangoFontDescription *system,
              PangoFontDescription *custom)
{
  if (!use_system && custom) {
    return pango_font_description_copy (custom);
  }

  return pango_font_description_copy (system);
}


static void
kgx_settings_class_init (KgxSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = kgx_settings_dispose;
  object_class->set_property = kgx_settings_set_property;
  object_class->get_property = kgx_settings_get_property;

  /**
   * KgxSettings:theme:
   *
   * The palette to use, one of the values of #KgxTheme
   *
   * Bound to ‘theme’ GSetting so changes persist
   */
  pspecs[PROP_FONT] =
    g_param_spec_boxed ("font", NULL, NULL,
                        PANGO_TYPE_FONT_DESCRIPTION,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_FONT_SCALE] =
    g_param_spec_double ("font-scale", NULL, NULL,
                         KGX_FONT_SCALE_MIN,
                         KGX_FONT_SCALE_MAX,
                         KGX_FONT_SCALE_DEFAULT,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_SCALE_CAN_INCREASE] =
    g_param_spec_boolean ("scale-can-increase", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_SCALE_CAN_DECREASE] =
    g_param_spec_boolean ("scale-can-decrease", NULL, NULL,
                          TRUE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  /**
   * KgxSettings:scrollback-lines:
   *
   * How many lines of scrollback #KgxTerminal should keep
   *
   * Bound to ‘scrollback-lines’ GSetting so changes persist
   */
  pspecs[PROP_SCROLLBACK_LINES] =
    g_param_spec_int ("scrollback-lines", NULL, NULL,
                      -1, G_MAXINT, 10000,
                      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_AUDIBLE_BELL] =
    g_param_spec_boolean ("audible-bell", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_VISUAL_BELL] =
    g_param_spec_boolean ("visual-bell", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_USE_SYSTEM_FONT] =
    g_param_spec_boolean ("use-system-font", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_CUSTOM_FONT] =
    g_param_spec_boxed ("custom-font", NULL, NULL,
                        PANGO_TYPE_FONT_DESCRIPTION,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_SCROLLBACK_LIMIT] =
    g_param_spec_int64 ("scrollback-limit", NULL, NULL,
                        G_MININT64, G_MAXINT64, 10000,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_IGNORE_SCROLLBACK_LIMIT] =
    g_param_spec_boolean ("ignore-scrollback-limit", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_SOFTWARE_FLOW_CONTROL] =
    g_param_spec_boolean ("software-flow-control", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_LIVERY] =
    g_param_spec_boxed ("livery", NULL, NULL,
                        KGX_TYPE_LIVERY,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_TRANSPARENCY] =
    g_param_spec_boolean ("transparency", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_TRANSPARENCY_LEVEL] =
    g_param_spec_double ("transparency-level", NULL, NULL,
                         0.0, 1.0, 0.2,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_GLASS_OPACITY] =
    g_param_spec_double ("glass-opacity", NULL, NULL,
                         0.0, 1.0, 0.8,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_USE_GLASS_BG] =
    g_param_spec_boolean ("use-glass-bg", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_GLASS_COLOR] =
    g_param_spec_string ("glass-color", NULL, NULL,
                         "#000000",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_ACCENT_COLOR] =
    g_param_spec_string ("accent-color", NULL, NULL,
                         "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_PROCESS_GLASS_COLORS] =
    g_param_spec_boxed ("process-glass-colors", NULL, NULL,
                        G_TYPE_HASH_TABLE,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_OVERSCROLL] =
    g_param_spec_boolean ("edge-overscroll", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_EDGE_OVERSCROLL_COLOR] =
    g_param_spec_string ("edge-overscroll-color", NULL, NULL,
                         "",
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_OVERSCROLL_STYLE] =
    g_param_spec_int ("edge-overscroll-style", NULL, NULL,
                      0, 1, 0,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_OVERSCROLL_REVERSE] =
    g_param_spec_int ("edge-overscroll-reverse", NULL, NULL,
                      0, 2, 0,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_SETTINGS_ANIMATION] =
    g_param_spec_boolean ("edge-settings-animation", NULL, NULL,
                          TRUE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);





  pspecs[PROP_EDGE_BURST_COUNT_AMBIENT] =
    g_param_spec_int ("edge-burst-count-ambient", NULL, NULL,
                      1, 8, 8,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_BURST_SPREAD_AMBIENT] =
    g_param_spec_double ("edge-burst-spread-ambient", NULL, NULL,
                         0.5, 5.0, 3.1,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /* ── Global tunables (table-driven) ──────────────────────── */
  for (int f = 0; f < N_TUNE_FIELDS; f++) {
    int id = PROP_EDGE_GLOBAL_BASE + f;
    char *name = g_strdup_printf ("edge-%s", tune_names[f]);
    if (tune_meta[f].is_int)
      pspecs[id] = g_param_spec_int (name, NULL, NULL,
                                      tune_meta[f].imin, tune_meta[f].imax,
                                      tune_meta[f].iglobal_default,
                                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
    else
      pspecs[id] = g_param_spec_double (name, NULL, NULL,
                                         tune_meta[f].dmin, tune_meta[f].dmax,
                                         tune_meta[f].global_default,
                                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
    g_free (name);
  }

  /* ── Per-preset tunables (table-driven) ────────────────── */
  for (int p = 0; p < N_PRESETS; p++) {
    for (int f = 0; f < N_TUNE_FIELDS; f++) {
      int id = PROP_EDGE_PRESET_BASE + p * N_TUNE_FIELDS + f;
      char *name = g_strdup_printf ("edge-%s-%s", tune_names[f], preset_suffixes[p]);
      if (tune_meta[f].is_int)
        pspecs[id] = g_param_spec_int (name, NULL, NULL,
                                        tune_meta[f].imin, tune_meta[f].imax,
                                        tune_meta[f].ipreset_default,
                                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
      else
        pspecs[id] = g_param_spec_double (name, NULL, NULL,
                                           tune_meta[f].dmin, tune_meta[f].dmax,
                                           tune_meta[f].preset_default,
                                           G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_NICK | G_PARAM_STATIC_BLURB);
      g_free (name);
    }
  }

  pspecs[PROP_EDGE_BURST_COUNT] =
    g_param_spec_int ("edge-burst-count", NULL, NULL,
                      1, 8, 4,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_EDGE_BURST_SPREAD] =
    g_param_spec_double ("edge-burst-spread", NULL, NULL,
                         0.5, 5.0, 1.0,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  g_type_ensure (KGX_TYPE_SYSTEM_INFO);
  g_type_ensure (KGX_TYPE_LIVERY_MANAGER);

  kgx_templated_class_set_template_from_resource (object_class,
                                                  KGX_APPLICATION_PATH "kgx-settings.ui");

  kgx_templated_class_bind_template_child (object_class, KgxSettings, livery_manager);
  kgx_templated_class_bind_template_child (object_class, KgxSettings, settings);

  /* We don't interact with this from C but GtkExpression doesn't keep it
   * alive on it's own */
  kgx_templated_class_bind_template_child (object_class, KgxSettings, system_info);

  kgx_templated_class_bind_template_callback (object_class, resolve_lines);
  kgx_templated_class_bind_template_callback (object_class, resolve_font);
}


static gboolean
variant_to_value (GValue *value, GVariant *variant, gpointer data)
{
  g_value_set_variant (value, variant);

  return TRUE;
}


static GVariant *
value_to_variant (const GValue       *value,
                  const GVariantType *variant_ty,
                  gpointer            data)
{
  return g_value_dup_variant (value);
}


static gboolean
process_colors_from_variant (GValue *value, GVariant *variant, gpointer data)
{
  GHashTable *ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  GVariantIter iter;
  const char *process, *color;

  g_variant_iter_init (&iter, variant);
  while (g_variant_iter_next (&iter, "{&s&s}", &process, &color))
    g_hash_table_insert (ht, g_strdup (process), g_strdup (color));

  g_value_take_boxed (value, ht);
  return TRUE;
}


static GVariant *
process_colors_to_variant (const GValue       *value,
                           const GVariantType *variant_ty,
                           gpointer            data)
{
  GHashTable *ht = g_value_get_boxed (value);
  GVariantBuilder builder;
  GHashTableIter iter;
  gpointer key, val;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{ss}"));

  if (ht) {
    g_hash_table_iter_init (&iter, ht);
    while (g_hash_table_iter_next (&iter, &key, &val))
      g_variant_builder_add (&builder, "{ss}", (const char *) key, (const char *) val);
  }

  return g_variant_builder_end (&builder);
}


static gboolean
resolve_livery (GValue   *value,
                GVariant *variant,
                gpointer  data)
{
  KgxSettings *self = data;
  const char *uuid = g_variant_get_string (variant, NULL);
  KgxLivery *livery = kgx_livery_manager_resolve (self->livery_manager, uuid);

  if (!livery) {
    g_log_structured (G_LOG_DOMAIN,
                      G_LOG_LEVEL_WARNING,
                      "CODE_FILE", __FILE__,
                      "CODE_LINE", G_STRINGIFY (__LINE__),
                      "CODE_FUNC", G_STRFUNC,
                      "KGX_LIVERY_UUID", uuid,
                      "MESSAGE", "settings: ‘%s’ is unknown, using default livery",
                      uuid);

    livery = kgx_livery_manager_dup_fallback (self->livery_manager);
  }

  g_value_take_boxed (value, livery);

  return TRUE;
}


static GVariant *
select_livery (const GValue       *value,
               const GVariantType *variant_ty,
               gpointer            data)
{
  KgxLivery *livery = g_value_get_boxed (value);

  if (!livery) {
    return NULL;
  }

  return g_variant_new_string (kgx_livery_get_uuid (livery));
}


static void
restore_window_size_changed (GSettings   *settings,
                             const char  *key,
                             KgxSettings *self)
{
  if (!g_settings_get_boolean (self->settings, RESTORE_SIZE_KEY)) {
    g_settings_set (self->settings, LAST_SIZE_KEY, "(ii)", -1, -1);
  }
}


static gboolean
decode_font (GValue   *value,
             GVariant *variant,
             gpointer  data)
{
  const char *font_desc = g_variant_get_string (variant, NULL);
  g_autoptr (PangoFontDescription) font = NULL;

  if (!kgx_str_non_empty (font_desc)) {
    g_value_set_boxed (value, NULL);

    return TRUE;
  }

  font = pango_font_description_from_string (font_desc);

  if ((pango_font_description_get_set_fields (font) & (PANGO_FONT_MASK_FAMILY | PANGO_FONT_MASK_SIZE)) == 0) {
    g_value_set_boxed (value, NULL);

    g_warning ("settings: ignoring ‘%s’ as lacking family and/or size", font_desc);

    return TRUE;
  }

  g_value_set_boxed (value, font);

  return TRUE;
}


static GVariant *
encode_font (const GValue       *value,
             const GVariantType *variant_ty,
             gpointer            data)
{
  PangoFontDescription *font = g_value_get_boxed (value);

  if (!font) {
    return g_variant_new_string ("");
  }

  return g_variant_new_take_string (pango_font_description_to_string (font));
}


static void
kgx_settings_init (KgxSettings *self)
{
  kgx_templated_init_template (KGX_TEMPLATED (self));

  g_settings_bind (self->settings, "font-scale",
                   self, "font-scale",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "audible-bell",
                   self, "audible-bell",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "visual-bell",
                   self, "visual-bell",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "use-system-font",
                   self, "use-system-font",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind_with_mapping (self->settings, "custom-font",
                                self, "custom-font",
                                G_SETTINGS_BIND_DEFAULT,
                                decode_font,
                                encode_font,
                                NULL, NULL);
  g_settings_bind (self->settings, "scrollback-lines",
                   /* Yes, this is intentional */
                   self, "scrollback-limit",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "ignore-scrollback-limit",
                   self, "ignore-scrollback-limit",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "software-flow-control",
                   self, "software-flow-control",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "transparency",
                   self, "transparency",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "transparency-level",
                   self, "transparency-level",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "glass-opacity",
                   self, "glass-opacity",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "use-glass-bg",
                   self, "use-glass-bg",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "glass-color",
                   self, "glass-color",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "accent-color",
                   self, "accent-color",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind_with_mapping (self->settings, "process-glass-colors",
                                self, "process-glass-colors",
                                G_SETTINGS_BIND_DEFAULT,
                                process_colors_from_variant,
                                process_colors_to_variant,
                                NULL, NULL);
  g_settings_bind (self->settings, "edge-overscroll",
                   self, "edge-overscroll",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-overscroll-color",
                   self, "edge-overscroll-color",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-overscroll-style",
                   self, "edge-overscroll-style",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-overscroll-reverse",
                   self, "edge-overscroll-reverse",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-settings-animation",
                   self, "edge-settings-animation",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-burst-count-ambient",
                   self, "edge-burst-count-ambient",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-burst-spread-ambient",
                   self, "edge-burst-spread-ambient",
                   G_SETTINGS_BIND_DEFAULT);
  /* Global tunables */
  for (int f = 0; f < N_TUNE_FIELDS; f++) {
    char *name = g_strdup_printf ("edge-%s", tune_names[f]);
    g_settings_bind (self->settings, name, self, name, G_SETTINGS_BIND_DEFAULT);
    g_free (name);
  }
  /* Per-preset tunables */
  for (int p = 0; p < N_PRESETS; p++) {
    for (int f = 0; f < N_TUNE_FIELDS; f++) {
      char *name = g_strdup_printf ("edge-%s-%s", tune_names[f], preset_suffixes[p]);
      g_settings_bind (self->settings, name, self, name, G_SETTINGS_BIND_DEFAULT);
      g_free (name);
    }
  }
  g_settings_bind (self->settings, "edge-burst-count",
                   self, "edge-burst-count",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind (self->settings, "edge-burst-spread",
                   self, "edge-burst-spread",
                   G_SETTINGS_BIND_DEFAULT);
  g_settings_bind_with_mapping (self->settings, "custom-liveries",
                                self->livery_manager, "custom-liveries",
                                G_SETTINGS_BIND_DEFAULT, variant_to_value,
                                value_to_variant,
                                NULL, NULL);
  g_settings_bind_with_mapping (self->settings, "livery",
                                self, "livery",
                                G_SETTINGS_BIND_DEFAULT,
                                resolve_livery,
                                select_livery,
                                self, NULL);

  g_signal_connect (self->settings,
                    "changed::" RESTORE_SIZE_KEY,
                    G_CALLBACK (restore_window_size_changed),
                    self);
}


void
kgx_settings_increase_scale (KgxSettings *self)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  update_scale (self, self->scale + 0.1);
}


void
kgx_settings_decrease_scale (KgxSettings *self)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  update_scale (self, self->scale - 0.1);
}


void
kgx_settings_reset_scale (KgxSettings *self)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  update_scale (self, KGX_FONT_SCALE_DEFAULT);
}


/**
 * kgx_settings_get_shell:
 * Return: (transfer full):
 */
GStrv
kgx_settings_get_shell (KgxSettings *self)
{
  g_autofree char *user_shell = NULL;
  g_auto (GStrv) shell = NULL;
  g_auto (GStrv) custom_shell = NULL;

  g_return_val_if_fail (KGX_IS_SETTINGS (self), NULL);

  custom_shell = g_settings_get_strv (self->settings, "shell");

  if (g_strv_length (custom_shell) > 0) {
    return g_steal_pointer (&custom_shell);
  }

  user_shell = vte_get_user_shell ();

  if (G_LIKELY (user_shell)) {
    shell = g_new0 (char *, 2);
    shell[0] = g_steal_pointer (&user_shell);
    shell[1] = NULL;

    return g_steal_pointer (&shell);
  }

  /* We could probably do something other than /bin/sh */
  shell = g_new0 (char *, 2);
  shell[0] = g_strdup ("/bin/sh");
  shell[1] = NULL;
  g_warning ("No Shell! Defaulting to “%s”", shell[0]);

  return g_steal_pointer (&shell);
}


void
kgx_settings_set_custom_shell (KgxSettings *self, const char *const *shell)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  g_settings_set_strv (self->settings, "shell", shell);
}


void
kgx_settings_set_scrollback_limit (KgxSettings *self,
                                   int64_t      value)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  if (self->scrollback_limit == value) {
    return;
  }

  self->scrollback_limit = value;

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_SCROLLBACK_LIMIT]);
}


gboolean
kgx_settings_get_restore_size (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), FALSE);

  return g_settings_get_boolean (self->settings, RESTORE_SIZE_KEY);
}


void
kgx_settings_get_size (KgxSettings *self,
                       int         *width,
                       int         *height,
                       gboolean    *maximised)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));
  g_return_if_fail (width != NULL && height != NULL && maximised != NULL);

  if (!g_settings_get_boolean (self->settings, RESTORE_SIZE_KEY)) {
    *width = -1;
    *height = -1;
    *maximised = FALSE;
    return;
  }

  g_settings_get (self->settings, LAST_SIZE_KEY, "(ii)", width, height);
  g_settings_get (self->settings, LAST_MAXIMISED_KEY, "b", maximised);
}


void
kgx_settings_set_custom_size (KgxSettings *self,
                              int          width,
                              int          height,
                              gboolean     maximised)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  if (!g_settings_get_boolean (self->settings, RESTORE_SIZE_KEY)) {
    return;
  }

  g_debug ("settings: store size (%i×%i)", width, height);

  g_settings_set (self->settings, LAST_SIZE_KEY, "(ii)", width, height);
  g_settings_set (self->settings, LAST_MAXIMISED_KEY, "b", maximised);
}


gboolean
kgx_settings_get_audible_bell (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), FALSE);

  return self->audible_bell;
}


gboolean
kgx_settings_get_visual_bell (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), FALSE);

  return self->visual_bell;
}


/**
 * kgx_settings_dup_custom_font:
 *
 * Return: (transfer full):
 */
PangoFontDescription *
kgx_settings_dup_custom_font (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), NULL);

  return pango_font_description_copy (self->custom_font);
}


void
kgx_settings_set_custom_font (KgxSettings          *self,
                              PangoFontDescription *custom_font)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  if (self->custom_font == custom_font ||
      (self->custom_font && custom_font &&
       pango_font_description_equal (self->custom_font, custom_font))) {
    return;
  }

  g_clear_pointer (&self->custom_font, pango_font_description_free);
  self->custom_font = pango_font_description_copy (custom_font);

  g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_CUSTOM_FONT]);
}


gboolean
kgx_settings_get_software_flow_control (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), FALSE);

  return self->software_flow_control;
}


KgxLivery *
kgx_settings_get_livery (KgxSettings *self)
{
  g_return_val_if_fail (KGX_IS_SETTINGS (self), NULL);

  return self->livery;
}


void
kgx_settings_set_livery (KgxSettings *restrict self,
                         KgxLivery   *restrict livery)
{
  g_return_if_fail (KGX_IS_SETTINGS (self));

  if (kgx_set_livery (&self->livery, livery)) {
    g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_LIVERY]);
  }
}


const char *
kgx_settings_lookup_process_color (KgxSettings *self,
                                   const char  *process_name)
{
  GHashTableIter iter;
  gpointer key, val;

  g_return_val_if_fail (KGX_IS_SETTINGS (self), NULL);

  if (!self->process_glass_colors || !process_name)
    return NULL;

  /* Try exact match first (fast path). */
  val = g_hash_table_lookup (self->process_glass_colors, process_name);
  if (val)
    return val;

  /* Check comma-separated keys (e.g. "su, sudo" matches "su" or "sudo"). */
  g_hash_table_iter_init (&iter, self->process_glass_colors);
  while (g_hash_table_iter_next (&iter, &key, &val)) {
    if (strchr ((const char *) key, ',')) {
      g_auto (GStrv) parts = g_strsplit ((const char *) key, ",", -1);
      for (int i = 0; parts[i]; i++) {
        g_strstrip (parts[i]);
        if (g_str_equal (parts[i], process_name))
          return val;
      }
    }
  }

  return NULL;
}


