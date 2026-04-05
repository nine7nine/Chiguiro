/* kgx-settings-page.c
 *
 * Copyright 2024 Zander Brown
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

#include <glib/gi18n.h>

#include "kgx-about.h"
#include "kgx-application.h"
#include "kgx-edge.h"
#include "kgx-settings.h"
#include "kgx-sprite.h"
#include "kgx-utils.h"
#include "preferences/kgx-font-picker.h"

#include "kgx-settings-page.h"


/* --- Tunables grid descriptors --- */

enum {
  COL_SPEED, COL_THICKNESS, COL_TAIL, COL_PULSE_DEPTH, COL_PULSE_SPEED,
  COL_ENV_ATTACK, COL_ENV_RELEASE, COL_RELEASE_MODE, COL_SHAPE,
  COL_BURST_COUNT, COL_BURST_SPREAD,
  COL_ENV_CURVE, COL_THK_ATTACK, COL_THK_RELEASE, COL_THK_CURVE,
  N_TUNE_COLS
};

typedef enum { BIND_DIRECT, BIND_PERCENT, BIND_INT_BOOL, BIND_SHAPE } BindKind;
typedef enum { WID_SPIN, WID_TOGGLE, WID_BUTTON } WidgetKind;

typedef struct {
  int         grid_col;
  WidgetKind  wkind;
  BindKind    bkind;
  const char *settings_field;
  int         digits;
  double      lower, upper, step;
  const char *tooltip;
  gboolean    burst_only;
} TuneColDesc;

static const TuneColDesc tune_cols[N_TUNE_COLS] = {
  [COL_SPEED]        = {  1, WID_SPIN,   BIND_PERCENT,  "speed",        1, 10, 300, 10, NULL,               FALSE },
  [COL_THICKNESS]    = {  2, WID_SPIN,   BIND_DIRECT,   "thickness",    0,  2,  40,  1, NULL,               FALSE },
  [COL_TAIL]         = {  3, WID_SPIN,   BIND_PERCENT,  "tail-length",  0, 10, 300, 10, NULL,               FALSE },
  [COL_PULSE_DEPTH]  = {  4, WID_SPIN,   BIND_PERCENT,  "pulse-depth",  0,  0, 100,  5, NULL,               FALSE },
  [COL_PULSE_SPEED]  = {  5, WID_SPIN,   BIND_PERCENT,  "pulse-speed",  1, 10, 500, 10, NULL,               FALSE },
  [COL_ENV_ATTACK]   = {  6, WID_SPIN,   BIND_PERCENT,  "env-attack",   0,  0,  50,  5, "Attack",           FALSE },
  [COL_ENV_RELEASE]  = {  7, WID_SPIN,   BIND_PERCENT,  "env-release",  0,  0,  50,  5, "Release",          FALSE },
  [COL_RELEASE_MODE] = {  8, WID_TOGGLE, BIND_INT_BOOL, "release-mode", 0,  0,   0,  0, "Tail retraction on release", FALSE },
  [COL_SHAPE]        = {  9, WID_BUTTON, BIND_SHAPE,    "shape",        0,  0,   0,  0, "Particle shape",   FALSE },
  [COL_BURST_COUNT]  = { 10, WID_SPIN,   BIND_DIRECT,   "burst-count",  0,  1,   8,  1, "Burst Count",      TRUE },
  [COL_BURST_SPREAD] = { 11, WID_SPIN,   BIND_PERCENT,  "burst-spread", 0, 50, 500, 10, "Burst Spread",     TRUE },
  [COL_ENV_CURVE]    = { 12, WID_SPIN,   BIND_DIRECT,   "env-curve",    0,  1,   3,  1, "Envelope Curve",   FALSE },
  [COL_THK_ATTACK]   = { 13, WID_SPIN,   BIND_PERCENT,  "thk-attack",   0,  0,  50,  5, "Thickness Attack", FALSE },
  [COL_THK_RELEASE]  = { 14, WID_SPIN,   BIND_PERCENT,  "thk-release",  0,  0,  50,  5, "Thickness Release",FALSE },
  [COL_THK_CURVE]    = { 15, WID_SPIN,   BIND_DIRECT,   "thk-curve",    0,  1,   3,  1, "Thickness Curve",  FALSE },
};

#define N_PRESET_ROWS 7

typedef struct {
  const char *display_name;
  const char *css_class;
  const char *settings_suffix;  /* NULL for overscroll (global) */
  gboolean    has_burst;
} PresetRowDesc;

static const PresetRowDesc preset_rows[N_PRESET_ROWS] = {
  { "Ambient",    "preset-ambient",    "ambient",   TRUE  },
  { "Corners",    "preset-corners",    "corners",   FALSE },
  { "Fireworks",  "preset-fireworks",  "fireworks", TRUE  },
  { "Overscroll", "preset-overscroll",  NULL,        FALSE },
  { "Ping-Pong",  "preset-ping-pong",  "ping-pong", FALSE },
  { "Pulse Out",  "preset-pulse-out",  "pulse-out", FALSE },
  { "Rotate",     "preset-rotate",     "rotate",    FALSE },
};

#define APP_GLASS_SLOTS 12

struct _KgxSettingsPage {
  AdwBin                parent_instance;

  KgxSettings          *settings;
  GBindingGroup        *settings_binds;


  GtkWidget            *audible_bell;
  GtkWidget            *visual_bell;
  GtkWidget            *use_system_font;
  GtkWidget            *custom_font;
  GtkWidget            *text_scale;
  GtkWidget            *transparency_level;
  GtkWidget            *glass_color;
  GtkWidget            *glass_opacity;
  GtkWidget            *use_glass_bg;
  GtkWidget            *accent_color;
  GtkWidget            *unlimited_scrollback;
  GtkWidget            *scrollback;
  GtkWidget            *overscroll_switch;
  GtkWidget            *overscroll_color_btn;
  GtkWidget            *privilege_switch;
  GtkWidget            *privilege_color_btn;
  GtkWidget            *privilege_preset_btn;
  GtkWidget            *ambient_switch;
  GtkWidget            *tunables_grid;
  GtkWidget            *tune_widgets[N_PRESET_ROWS][N_TUNE_COLS];
  GtkWidget            *app_glass_grid;
  gboolean              app_glass_inhibit_save;
  guint                 app_glass_save_timeout;
  GtkWidget            *ag_entries[APP_GLASS_SLOTS];
  GtkWidget            *ag_colors[APP_GLASS_SLOTS];
  GtkWidget            *ag_presets[APP_GLASS_SLOTS];
  GtkWidget            *ag_reverses[APP_GLASS_SLOTS];
  GtkWidget            *ag_pcolors[APP_GLASS_SLOTS];
  GtkWidget            *logo_picture;
  GtkWidget            *page_title;
  AdwCarousel          *carousel;
  KgxSprite            *sprite;

  GtkExpressionWatch   *font_watch;
};


G_DEFINE_TYPE (KgxSettingsPage, kgx_settings_page, ADW_TYPE_BIN)

static void app_glass_load (KgxSettingsPage *self);
static void sync_all_shapes (KgxSettingsPage *self);


enum {
  PROP_0,
  PROP_SETTINGS,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void
kgx_settings_page_dispose (GObject *object)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (object);

  if (self->sprite) {
    kgx_sprite_stop (self->sprite);
    g_clear_object (&self->sprite);
  }

  if (self->font_watch) {
    gtk_expression_watch_unwatch (self->font_watch);
    self->font_watch = NULL;
  }

  g_clear_handle_id (&self->app_glass_save_timeout, g_source_remove);

  /* Disconnect all signal handlers from App Glass widgets before
   * disposal — signals can fire during teardown and access freed state. */
  for (int i = 0; i < APP_GLASS_SLOTS; i++) {
    if (self->ag_entries[i])  g_signal_handlers_disconnect_by_data (self->ag_entries[i], self);
    if (self->ag_colors[i])   g_signal_handlers_disconnect_by_data (self->ag_colors[i], self);
    if (self->ag_presets[i])  g_signal_handlers_disconnect_by_data (self->ag_presets[i], self);
    if (self->ag_reverses[i]) g_signal_handlers_disconnect_by_data (self->ag_reverses[i], self);
    if (self->ag_pcolors[i])  g_signal_handlers_disconnect_by_data (self->ag_pcolors[i], self);
  }

  /* Disconnect shape button click handlers */
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    if (self->tune_widgets[r][COL_SHAPE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_SHAPE], self);
  }

  /* settings_binds is a template child — disconnect but do NOT free */
  if (self->settings_binds) {
    g_binding_group_set_source (self->settings_binds, NULL);
  }

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (kgx_settings_page_parent_class)->dispose (object);
}


static void
kgx_settings_page_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (object);

  switch (property_id) {
    case PROP_SETTINGS:
      if (g_set_object (&self->settings, g_value_get_object (value))) {
        g_object_notify_by_pspec (G_OBJECT (self), pspecs[PROP_SETTINGS]);
        app_glass_load (self);
        sync_all_shapes (self);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_settings_page_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (object);

  switch (property_id) {
    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static PangoAttrList *
font_as_attributes (GObject *object, PangoFontDescription *font)
{
  g_autoptr (PangoAttrList) list = pango_attr_list_new ();

  if (font) {
    pango_attr_list_insert (list, pango_attr_font_desc_new (font));
  }

  return g_steal_pointer (&list);
}


static char *
font_as_label (GObject              *object,
               PangoFontDescription *font)
{
  if (!font) {
    return g_strdup (_("No Font Set"));
  }

  return pango_font_description_to_string (font);
}


static void
font_selected (KgxFontPicker   *picker,
               PangoFontDescription *font,
               KgxSettingsPage *self)
{
  kgx_settings_set_custom_font (self->settings, font);

  /* Font picker is currently one-shot — future: use AdwDialog */
}


static void
select_font_activated (GtkWidget  *widget,
                       const char *action_name,
                       GVariant   *parameter)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (widget);
  g_autoptr (PangoFontDescription) initial_value = NULL;
  AdwNavigationPage *picker;

  initial_value = kgx_settings_dup_custom_font (self->settings);

  picker = g_object_connect (g_object_new (KGX_TYPE_FONT_PICKER,
                                           "initial-font", initial_value,
                                           NULL),
                             "object-signal::selected", G_CALLBACK (font_selected), self,
                             NULL);

  adw_dialog_present (ADW_DIALOG (g_object_new (ADW_TYPE_DIALOG,
                                                 "child", picker,
                                                 "title", "Select Font",
                                                 NULL)),
                      widget);
}


static gboolean
value_to_percent (GBinding     *binding,
                  const GValue *from_value,
                  GValue       *to_value,
                  gpointer      user_data)
{
  g_value_set_double (to_value, g_value_get_double (from_value) * 100.0);

  return TRUE;
}


static gboolean
percent_to_value (GBinding     *binding,
                  const GValue *from_value,
                  GValue       *to_value,
                  gpointer      user_data)
{
  g_value_set_double (to_value, g_value_get_double (from_value) / 100.0);

  return TRUE;
}


static gboolean
string_to_rgba (GBinding     *binding,
                const GValue *from_value,
                GValue       *to_value,
                gpointer      user_data)
{
  const char *str = g_value_get_string (from_value);
  GdkRGBA rgba = { 0, 0, 0, 1 };

  if (str) {
    gdk_rgba_parse (&rgba, str);
  }
  rgba.alpha = 1.0;

  g_value_set_boxed (to_value, &rgba);
  return TRUE;
}


static gboolean
rgba_to_string (GBinding     *binding,
                const GValue *from_value,
                GValue       *to_value,
                gpointer      user_data)
{
  const GdkRGBA *rgba = g_value_get_boxed (from_value);

  if (rgba) {
    g_autofree char *str = g_strdup_printf ("#%02x%02x%02x",
      (int)(rgba->red * 255), (int)(rgba->green * 255), (int)(rgba->blue * 255));
    g_value_set_string (to_value, str);
  } else {
    g_value_set_string (to_value, "#000000");
  }
  return TRUE;
}


static gboolean
value_to_opacity_percent (GBinding     *binding,
                          const GValue *from_value,
                          GValue       *to_value,
                          gpointer      user_data)
{
  /* settings 0.0 (opaque) -> slider 100, settings 1.0 (transparent) -> slider 0 */
  g_value_set_double (to_value, (1.0 - g_value_get_double (from_value)) * 100.0);
  return TRUE;
}


static gboolean
opacity_percent_to_value (GBinding     *binding,
                          const GValue *from_value,
                          GValue       *to_value,
                          gpointer      user_data)
{
  /* slider 100 -> settings 0.0, slider 0 -> settings 1.0 */
  g_value_set_double (to_value, 1.0 - (g_value_get_double (from_value) / 100.0));
  return TRUE;
}


static gboolean
int_to_bool (GBinding     *binding,
             const GValue *from,
             GValue       *to,
             gpointer      user_data)
{
  g_value_set_boolean (to, g_value_get_int (from) != 0);
  return TRUE;
}


static gboolean
bool_to_int (GBinding     *binding,
             const GValue *from,
             GValue       *to,
             gpointer      user_data)
{
  g_value_set_int (to, g_value_get_boolean (from) ? 1 : 0);
  return TRUE;
}


/* Map KgxParticlePreset enum (1-6) <-> alphabetical indicator dropdown index (0-5).
 * Enum: 1=FIREWORKS 2=CORNERS 3=PULSE_OUT 4=ROTATE 5=PING_PONG 6=AMBIENT
 * Alpha: 0=ambient 1=corners 2=fireworks 3=ping-pong 4=pulse-out 5=rotate */
static const guint preset_enum_to_indicator[] = {
  [0] = 0,
  [KGX_PARTICLE_FIREWORKS] = 2,
  [KGX_PARTICLE_CORNERS]   = 1,
  [KGX_PARTICLE_PULSE_OUT] = 4,
  [KGX_PARTICLE_ROTATE]    = 5,
  [KGX_PARTICLE_PING_PONG] = 3,
  [KGX_PARTICLE_AMBIENT]   = 0,
};

static const int indicator_to_preset_enum[] = {
  KGX_PARTICLE_AMBIENT,   /* 0: ambient */
  KGX_PARTICLE_CORNERS,   /* 1: corners */
  KGX_PARTICLE_FIREWORKS, /* 2: fireworks */
  KGX_PARTICLE_PING_PONG, /* 3: ping-pong */
  KGX_PARTICLE_PULSE_OUT, /* 4: pulse-out */
  KGX_PARTICLE_ROTATE,    /* 5: rotate */
};

static gboolean
preset_int_to_selected (GBinding     *binding,
                        const GValue *from,
                        GValue       *to,
                        gpointer      user_data)
{
  int v = g_value_get_int (from);
  if (v < 1 || v > (int) G_N_ELEMENTS (indicator_to_preset_enum))
    v = KGX_PARTICLE_FIREWORKS;
  g_value_set_uint (to, preset_enum_to_indicator[v]);
  return TRUE;
}


static gboolean
selected_to_preset_int (GBinding     *binding,
                        const GValue *from,
                        GValue       *to,
                        gpointer      user_data)
{
  guint sel = g_value_get_uint (from);
  if (sel >= G_N_ELEMENTS (indicator_to_preset_enum))
    sel = 0;
  g_value_set_int (to, indicator_to_preset_enum[sel]);
  return TRUE;
}


static const char *shape_labels[] = { "\u25A0", "\u25CF", "\u25C6", "\u25B6" };


static void
shape_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *prop_name = g_object_get_data (G_OBJECT (button), "settings-prop");
  int current = 0, next;
  if (!prop_name || !self->settings)
    return;

  g_object_get (self->settings, prop_name, &current, NULL);
  next = (current + 1) % 4;
  g_object_set (self->settings, prop_name, next, NULL);
  gtk_button_set_label (button, shape_labels[next]);
}


static void
setup_shape_button (KgxSettingsPage *self,
                    GtkWidget       *button,
                    const char      *settings_prop)
{
  g_object_set_data_full (G_OBJECT (button), "settings-prop",
                          g_strdup (settings_prop), g_free);
  g_signal_connect (button, "clicked",
                    G_CALLBACK (shape_clicked_cb), self);
}


static void
sync_shape_label (KgxSettingsPage *self,
                  GtkWidget       *button,
                  const char      *settings_prop)
{
  int current = 0;
  if (!self->settings)
    return;
  g_object_get (self->settings, settings_prop, &current, NULL);
  current = CLAMP (current, 0, 3);
  gtk_button_set_label (GTK_BUTTON (button), shape_labels[current]);
}


static void
sync_all_shapes (KgxSettingsPage *self)
{
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    const PresetRowDesc *pr = &preset_rows[r];
    char prop[64];
    if (pr->settings_suffix)
      g_snprintf (prop, sizeof prop, "edge-shape-%s", pr->settings_suffix);
    else
      g_snprintf (prop, sizeof prop, "edge-shape");
    sync_shape_label (self, self->tune_widgets[r][COL_SHAPE], prop);
  }
}


static void
build_tunables_grid (KgxSettingsPage *self)
{
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    const PresetRowDesc *pr = &preset_rows[r];
    int grid_row = r + 1;

    /* Column 0: preset label */
    GtkWidget *label = gtk_label_new (pr->display_name);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_add_css_class (label, pr->css_class);
    gtk_grid_attach (GTK_GRID (self->tunables_grid), label, 0, grid_row, 1, 1);

    for (int c = 0; c < N_TUNE_COLS; c++) {
      const TuneColDesc *td = &tune_cols[c];

      GtkWidget *w;

      if (td->burst_only && !pr->has_burst) {
        self->tune_widgets[r][c] = NULL;
        continue;
      }

      w = NULL;

      switch (td->wkind) {
      case WID_SPIN: {
        GtkAdjustment *adj = gtk_adjustment_new (0, td->lower, td->upper,
                                                  td->step, 0, 0);
        w = gtk_spin_button_new (adj, 1.0, td->digits);
        gtk_editable_set_alignment (GTK_EDITABLE (w), 1.0);
        if (td->tooltip)
          gtk_widget_set_tooltip_text (w, td->tooltip);
        break;
      }
      case WID_TOGGLE:
        w = gtk_toggle_button_new ();
        gtk_button_set_label (GTK_BUTTON (w), "R");
        gtk_widget_add_css_class (w, "flat");
        gtk_widget_add_css_class (w, "release-toggle");
        if (td->tooltip)
          gtk_widget_set_tooltip_text (w, td->tooltip);
        break;
      case WID_BUTTON:
        w = gtk_button_new_with_label ("\u25A0");
        gtk_widget_add_css_class (w, "flat");
        if (td->tooltip)
          gtk_widget_set_tooltip_text (w, td->tooltip);
        break;
      default:
        break;
      }

      self->tune_widgets[r][c] = w;
      gtk_grid_attach (GTK_GRID (self->tunables_grid), w,
                       td->grid_col, grid_row, 1, 1);
    }
  }
}


static void
bind_tunables (KgxSettingsPage *self)
{
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    const PresetRowDesc *pr = &preset_rows[r];

    for (int c = 0; c < N_TUNE_COLS; c++) {
      const TuneColDesc *td = &tune_cols[c];
      GtkWidget *w = self->tune_widgets[r][c];
      char key[64];

      if (!w)
        continue;

      /* Fireworks burst anomaly: burst keys are unsuffixed despite other
       * Fireworks fields using "-fireworks". */
      if (pr->settings_suffix &&
          !(td->burst_only && g_str_equal (pr->settings_suffix, "fireworks")))
        g_snprintf (key, sizeof key, "edge-%s-%s",
                    td->settings_field, pr->settings_suffix);
      else
        g_snprintf (key, sizeof key, "edge-%s", td->settings_field);

      switch (td->bkind) {
      case BIND_DIRECT:
        g_binding_group_bind (self->settings_binds, key,
                              w, "value",
                              G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
        break;
      case BIND_PERCENT:
        g_binding_group_bind_full (self->settings_binds, key,
                                   w, "value",
                                   G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                                   value_to_percent, percent_to_value,
                                   NULL, NULL);
        break;
      case BIND_INT_BOOL:
        g_binding_group_bind_full (self->settings_binds, key,
                                   w, "active",
                                   G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                                   int_to_bool, bool_to_int,
                                   NULL, NULL);
        break;
      case BIND_SHAPE:
        setup_shape_button (self, w, key);
        break;
      default:
        break;
      }
    }
  }
}


/* App Glass grid columns: Name(0) Glass(1) Preset(2) Rev(3) Particle(4) Switch(5) */

static GtkWidget *
make_color_btn (void)
{
  GtkColorDialog *dlg;
  GtkWidget *btn;
  dlg = gtk_color_dialog_new ();
  gtk_color_dialog_set_with_alpha (dlg, FALSE);
  btn = gtk_color_dialog_button_new (dlg);
  gtk_widget_set_valign (btn, GTK_ALIGN_CENTER);
  return btn;
}

static void
build_app_glass_grid (KgxSettingsPage *self)
{
  GtkGrid *grid = GTK_GRID (self->app_glass_grid);
  int row = 0;

  /* --- Row 0: Column headers --- */
  {
    static const struct { int col; const char *text; } hdrs[] = {
      { 1, "Glass" }, { 2, "Preset" }, { 3, "Rev" }, { 4, "P.Clr" },
    };
    GtkWidget *title = gtk_label_new ("App Glass");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
    gtk_widget_set_hexpand (title, TRUE);
    gtk_widget_add_css_class (title, "title-4");
    gtk_grid_attach (grid, title, 0, row, 1, 1);

    for (int h = 0; h < (int) G_N_ELEMENTS (hdrs); h++) {
      GtkWidget *lbl = gtk_label_new (hdrs[h].text);
      gtk_label_set_xalign (GTK_LABEL (lbl), 1.0f);
      gtk_widget_add_css_class (lbl, "tune-header");
      gtk_grid_attach (grid, lbl, hdrs[h].col, row, 1, 1);
    }
    row++;
  }

  /* --- Row 1: Overscroll indicator --- */
  {
    GtkWidget *label = gtk_label_new ("Overscroll");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_add_css_class (label, "glass-entry");
    gtk_grid_attach (grid, label, 0, row, 1, 1);

    self->overscroll_color_btn = make_color_btn ();
    gtk_grid_attach (grid, self->overscroll_color_btn, 1, row, 1, 1);

    self->overscroll_switch = gtk_switch_new ();
    gtk_widget_set_valign (self->overscroll_switch, GTK_ALIGN_CENTER);
    gtk_grid_attach (grid, self->overscroll_switch, 5, row, 1, 1);
    row++;
  }

  /* --- Row 1: Privilege indicator --- */
  {
    GtkWidget *label = gtk_label_new ("Privilege");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_add_css_class (label, "glass-entry");
    gtk_grid_attach (grid, label, 0, row, 1, 1);

    self->privilege_color_btn = make_color_btn ();
    gtk_grid_attach (grid, self->privilege_color_btn, 1, row, 1, 1);

    self->privilege_preset_btn = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_valign (self->privilege_preset_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (self->privilege_preset_btn, "glass-preset");
    gtk_grid_attach (grid, self->privilege_preset_btn, 2, row, 1, 1);

    self->privilege_switch = gtk_switch_new ();
    gtk_widget_set_valign (self->privilege_switch, GTK_ALIGN_CENTER);
    gtk_grid_attach (grid, self->privilege_switch, 5, row, 1, 1);
    row++;
  }

  /* --- Row 2: separator spanning all columns --- */
  {
    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top (sep, 4);
    gtk_widget_set_margin_bottom (sep, 4);
    gtk_grid_attach (grid, sep, 0, row, 6, 1);
    row++;
  }

  /* --- Rows 3..14: process slots 0..11 --- */
  for (int i = 0; i < APP_GLASS_SLOTS; i++, row++) {
    GtkWidget *entry, *rev, *img;

    entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "process name");
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_widget_add_css_class (entry, "glass-entry");
    self->ag_entries[i] = entry;
    gtk_grid_attach (grid, entry, 0, row, 1, 1);

    self->ag_colors[i] = make_color_btn ();
    gtk_grid_attach (grid, self->ag_colors[i], 1, row, 1, 1);

    self->ag_presets[i] = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_valign (self->ag_presets[i], GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (self->ag_presets[i], "glass-preset");
    gtk_grid_attach (grid, self->ag_presets[i], 2, row, 1, 1);

    rev = gtk_toggle_button_new ();
    gtk_widget_set_valign (rev, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (rev, "Reverse");
    img = gtk_image_new_from_icon_name ("object-flip-horizontal-symbolic");
    gtk_image_set_pixel_size (GTK_IMAGE (img), 16);
    gtk_button_set_child (GTK_BUTTON (rev), img);
    gtk_widget_add_css_class (rev, "glass-reverse");
    gtk_widget_add_css_class (rev, "flat");
    self->ag_reverses[i] = rev;
    gtk_grid_attach (grid, rev, 3, row, 1, 1);

    self->ag_pcolors[i] = make_color_btn ();
    gtk_grid_attach (grid, self->ag_pcolors[i], 4, row, 1, 1);
  }
}


static void
kgx_settings_page_class_init (KgxSettingsPageClass *klass)
{
  GObjectClass   *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = kgx_settings_page_dispose;
  object_class->set_property = kgx_settings_page_set_property;
  object_class->get_property = kgx_settings_page_get_property;


  pspecs[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         KGX_TYPE_SETTINGS,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               KGX_APPLICATION_PATH "kgx-settings-page.ui");

  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, settings_binds);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, audible_bell);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, visual_bell);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, use_system_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, custom_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, text_scale);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, transparency_level);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, glass_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, glass_opacity);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, use_glass_bg);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, accent_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, unlimited_scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, ambient_switch);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, tunables_grid);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_grid);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, logo_picture);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, page_title);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, carousel);

  gtk_widget_class_bind_template_callback (widget_class, font_as_attributes);
  gtk_widget_class_bind_template_callback (widget_class, font_as_label);

  gtk_widget_class_install_action (widget_class,
                                   "settings.select-font",
                                   NULL,
                                   select_font_activated);

  gtk_widget_class_set_css_name (widget_class, "settings-page");
}


struct _WatchData {
  KgxSettingsPage      *page;
  GtkExpressionWatch   *watch;
};


KGX_DEFINE_DATA (WatchData, watch_data)


static inline void
watch_data_cleanup (WatchData *self)
{
  g_clear_weak_pointer (&self->page);
}


static void
notify_use_system (gpointer user_data)
{
  WatchData *data = user_data;
  g_auto (GValue) value = G_VALUE_INIT;

  if (G_UNLIKELY (!data->page)) {
    return;
  }

  if (G_LIKELY (gtk_expression_watch_evaluate (data->watch, &value))) {
    gtk_widget_action_set_enabled (GTK_WIDGET (data->page),
                                   "settings.select-font",
                                   !g_value_get_boolean (&value));
  } else {
    gtk_widget_action_set_enabled (GTK_WIDGET (data->page),
                                   "settings.select-font",
                                   FALSE);
  }
}




/* Muted dark defaults for empty slots — slight color tints on near-black */
static const GdkRGBA glass_default_colors[APP_GLASS_SLOTS] = {
  { 0.11f, 0.11f, 0.14f, 1.0f },  /* blue-grey */
  { 0.13f, 0.11f, 0.15f, 1.0f },  /* violet */
  { 0.11f, 0.13f, 0.12f, 1.0f },  /* green */
  { 0.14f, 0.12f, 0.11f, 1.0f },  /* warm */
  { 0.11f, 0.12f, 0.14f, 1.0f },  /* steel */
  { 0.13f, 0.11f, 0.12f, 1.0f },  /* mauve */
  { 0.11f, 0.14f, 0.13f, 1.0f },  /* teal */
  { 0.14f, 0.11f, 0.12f, 1.0f },  /* rose */
  { 0.12f, 0.12f, 0.11f, 1.0f },  /* olive */
  { 0.11f, 0.11f, 0.13f, 1.0f },  /* slate */
  { 0.12f, 0.13f, 0.11f, 1.0f },  /* moss */
  { 0.13f, 0.12f, 0.14f, 1.0f },  /* plum */
};


static void
app_glass_save (KgxSettingsPage *self)
{
  if (self->app_glass_inhibit_save)
    return;

  g_autoptr (GHashTable) ht = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_free);

  for (int i = 0; i < APP_GLASS_SLOTS; i++) {
    const char *name = gtk_editable_get_text (GTK_EDITABLE (self->ag_entries[i]));
    if (name && name[0] != '\0') {
      const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (
                               GTK_COLOR_DIALOG_BUTTON (self->ag_colors[i]));
      guint preset_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->ag_presets[i]));
      gboolean rev = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (self->ag_reverses[i]));
      const GdkRGBA *pc = gtk_color_dialog_button_get_rgba (
                             GTK_COLOR_DIALOG_BUTTON (self->ag_pcolors[i]));

      const char *preset_names[] = { "none", "ambient", "corners", "fireworks", "ping-pong", "pulse-out", "rotate" };
      const char *preset_str = (preset_idx < G_N_ELEMENTS (preset_names))
                                 ? preset_names[preset_idx] : "none";

      char *value = g_strdup_printf ("#%02x%02x%02x;%s;%d;#%02x%02x%02x",
        (int)(rgba->red * 255), (int)(rgba->green * 255), (int)(rgba->blue * 255),
        preset_str, rev ? 1 : 0,
        (int)(pc->red * 255), (int)(pc->green * 255), (int)(pc->blue * 255));
      g_hash_table_insert (ht, g_strdup (name), value);
    }
  }

  if (self->settings)
    g_object_set (self->settings, "process-glass-colors", ht, NULL);
}


static gboolean
app_glass_enable_save (gpointer data)
{
  KGX_SETTINGS_PAGE (data)->app_glass_inhibit_save = FALSE;
  return G_SOURCE_REMOVE;
}


static guint
preset_string_to_index (const char *s)
{
  if (!s) return 0;
  if (g_str_equal (s, "ambient"))    return 1;
  if (g_str_equal (s, "corners"))    return 2;
  if (g_str_equal (s, "fireworks"))  return 3;
  if (g_str_equal (s, "ping-pong"))  return 4;
  if (g_str_equal (s, "pulse-out"))  return 5;
  if (g_str_equal (s, "rotate"))     return 6;
  return 0;
}


static void
app_glass_load (KgxSettingsPage *self)
{
  GHashTable *ht = NULL;
  GHashTableIter iter;
  gpointer key, val;
  int i = 0;

  if (!self->settings)
    return;

  self->app_glass_inhibit_save = TRUE;

  g_object_get (self->settings, "process-glass-colors", &ht, NULL);

  /* Clear all slots and set muted default colors. */
  for (int j = 0; j < APP_GLASS_SLOTS; j++) {
    gtk_editable_set_text (GTK_EDITABLE (self->ag_entries[j]), "");
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_colors[j]),
                                      &glass_default_colors[j]);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (self->ag_presets[j]), 0);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->ag_reverses[j]), FALSE);
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_pcolors[j]),
                                      &glass_default_colors[j]);
  }

  if (ht) {
    g_hash_table_iter_init (&iter, ht);
    while (g_hash_table_iter_next (&iter, &key, &val) && i < APP_GLASS_SLOTS) {
      g_autofree char *glass_hex = NULL;
      KgxParticlePreset preset = KGX_PARTICLE_NONE;
      gboolean reverse = FALSE;
      GdkRGBA particle_color = { 0.5f, 0.5f, 0.5f, 1.0f };
      gtk_editable_set_text (GTK_EDITABLE (self->ag_entries[i]), (const char *) key);
      kgx_parse_process_config ((const char *) val,
                                 &glass_hex, &preset, &reverse, &particle_color);

      if (glass_hex) {
        GdkRGBA rgba = { 0, 0, 0, 1 };
        gdk_rgba_parse (&rgba, glass_hex);
        rgba.alpha = 1.0f;
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_colors[i]), &rgba);
      }

      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->ag_presets[i]),
                                   preset_string_to_index (kgx_particle_preset_to_string (preset)));
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->ag_reverses[i]), reverse);
      particle_color.alpha = 1.0f;
      gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_pcolors[i]), &particle_color);
      i++;
    }
    g_hash_table_unref (ht);
  }

  g_clear_handle_id (&self->app_glass_save_timeout, g_source_remove);
  self->app_glass_save_timeout = g_timeout_add (500, app_glass_enable_save, self);
}


static void
app_glass_changed (GtkEditable      *editable,
                   KgxSettingsPage  *self)
{
  app_glass_save (self);
}


static void
app_glass_color_changed (GObject          *button,
                         GParamSpec       *pspec,
                         KgxSettingsPage  *self)
{
  app_glass_save (self);
}


static const char *page_titles[] = {
  "· General ·", "· Particles ·", "· App Glass ·", "· Shortcuts ·"
};


static gboolean
carousel_scroll (GtkEventControllerScroll *controller,
                 double                    dx,
                 double                    dy,
                 KgxSettingsPage          *self)
{
  int n_pages = adw_carousel_get_n_pages (self->carousel);
  double pos  = adw_carousel_get_position (self->carousel);
  int target;

  if (dy > 0)
    target = (int) pos + 1;
  else if (dy < 0)
    target = (int) pos - 1;
  else
    return FALSE;

  if (target < 0 || target >= n_pages)
    return FALSE;

  adw_carousel_scroll_to (self->carousel,
                          adw_carousel_get_nth_page (self->carousel, target),
                          TRUE);
  return TRUE;
}


static void
carousel_page_changed (AdwCarousel     *carousel,
                       guint            index,
                       KgxSettingsPage *self)
{
  guint page = adw_carousel_get_position (carousel) + 0.5;

  if (page < G_N_ELEMENTS (page_titles))
    gtk_label_set_label (GTK_LABEL (self->page_title), page_titles[page]);
}


static void
kgx_settings_page_init (KgxSettingsPage *self)
{
  g_autoptr (GtkExpression) expression = NULL;
  g_autoptr (WatchData) data = watch_data_alloc ();

  self->app_glass_inhibit_save = TRUE;

  gtk_widget_init_template (GTK_WIDGET (self));
  build_tunables_grid (self);
  build_app_glass_grid (self);

  g_signal_connect (self->carousel, "notify::position",
                    G_CALLBACK (carousel_page_changed), self);

  {
    GtkEventController *scroll_ctrl;
    scroll_ctrl = gtk_event_controller_scroll_new (
                    GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect (scroll_ctrl, "scroll",
                      G_CALLBACK (carousel_scroll), self);
    gtk_widget_add_controller (GTK_WIDGET (self->carousel), scroll_ctrl);
  }

  /* App Glass — connect change signals and load initial data. */
  {
    const char *preset_names[] = { "none", "ambient", "corners", "fireworks", "ping-pong", "pulse-out", "rotate", NULL };
    for (int i = 0; i < APP_GLASS_SLOTS; i++) {
      GtkStringList *model = gtk_string_list_new (preset_names);
      gtk_drop_down_set_model (GTK_DROP_DOWN (self->ag_presets[i]), G_LIST_MODEL (model));
      g_object_unref (model);
    }

    for (int i = 0; i < APP_GLASS_SLOTS; i++) {
      g_signal_connect (self->ag_entries[i], "changed",
                        G_CALLBACK (app_glass_changed), self);
      g_signal_connect (self->ag_colors[i], "notify::rgba",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (self->ag_presets[i], "notify::selected",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (self->ag_reverses[i], "notify::active",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (self->ag_pcolors[i], "notify::rgba",
                        G_CALLBACK (app_glass_color_changed), self);
    }
  }

  /* Indicator preset dropdowns (no "none" — presets are 1-indexed). */
  {
    const char *indicator_presets[] = { "ambient", "corners", "fireworks", "ping-pong", "pulse-out", "rotate", NULL };
    GtkStringList *model = gtk_string_list_new (indicator_presets);
    gtk_drop_down_set_model (GTK_DROP_DOWN (self->privilege_preset_btn), G_LIST_MODEL (model));
    g_object_unref (model);
  }

  /* Capybara sprite animation — credit: https://rainloaf.itch.io/capybara-sprite-sheet */
  self->sprite = kgx_sprite_new (KGX_APPLICATION_PATH "icons/capybara.png",
                                 24, 28, 10, 96, 10.0);
  gtk_picture_set_paintable (GTK_PICTURE (self->logo_picture),
                             GDK_PAINTABLE (self->sprite));
  kgx_sprite_start (self->sprite, GTK_WIDGET (self));

  g_set_weak_pointer (&data->page, self);

  expression =
    gtk_property_expression_new (KGX_TYPE_SETTINGS,
                                 gtk_property_expression_new (G_TYPE_BINDING_GROUP,
                                                              gtk_object_expression_new (G_OBJECT (self->settings_binds)),
                                                              "source"),
                                 "use-system-font");

  self->font_watch =
    gtk_expression_watch (expression, self, notify_use_system, data, watch_data_free);
  data->watch = self->font_watch;
  g_steal_pointer (&data); /* this is actually stolen by the watch */

  g_binding_group_bind (self->settings_binds, "audible-bell",
                        self->audible_bell, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "visual-bell",
                        self->visual_bell, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "use-system-font",
                        self->use_system_font, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "font-scale",
                             self->text_scale, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent,
                             percent_to_value,
                             NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "transparency-level",
                             self->transparency_level, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_opacity_percent,
                             opacity_percent_to_value,
                             NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "glass-color",
                             self->glass_color, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba,
                             rgba_to_string,
                             NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "glass-opacity",
                             self->glass_opacity, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent,
                             percent_to_value,
                             NULL, NULL);

  g_binding_group_bind (self->settings_binds, "use-glass-bg",
                        self->use_glass_bg, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

  g_binding_group_bind_full (self->settings_binds, "accent-color",
                             self->accent_color, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba,
                             rgba_to_string,
                             NULL, NULL);

  g_binding_group_bind (self->settings_binds, "ignore-scrollback-limit",
                        self->unlimited_scrollback, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "scrollback-limit",
                        self->scrollback, "value",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

  /* Indicator rows (in App Glass list) */
  g_binding_group_bind (self->settings_binds, "edge-overscroll",
                        self->overscroll_switch, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-overscroll-color",
                             self->overscroll_color_btn, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba, rgba_to_string, NULL, NULL);
  g_binding_group_bind (self->settings_binds, "edge-privilege",
                        self->privilege_switch, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-privilege-color",
                             self->privilege_color_btn, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba, rgba_to_string, NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "edge-privilege-preset",
                             self->privilege_preset_btn, "selected",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             preset_int_to_selected, selected_to_preset_int,
                             NULL, NULL);
  g_binding_group_bind (self->settings_binds, "edge-settings-animation",
                        self->ambient_switch, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

  bind_tunables (self);
}
