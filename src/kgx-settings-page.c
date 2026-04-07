/* kgx-settings-page.c
 *
 * Copyright 2024-2026 jordan Johnston
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
#include "kgx-particle.h"
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
  COL_ENV_CURVE, COL_GAP, COL_THK_ATTACK, COL_THK_RELEASE, COL_THK_RELEASE_MODE, COL_THK_CURVE,
  N_TUNE_COLS
};

typedef enum { BIND_DIRECT, BIND_PERCENT, BIND_INT_BOOL, BIND_SHAPE, BIND_RELEASE_MODE, BIND_THK_RELEASE_MODE, BIND_GAP, BIND_CURVE } BindKind;
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
  [COL_GAP]          = {  3, WID_BUTTON, BIND_GAP,      "gap",          0,  0,   0,  0, "Block gap",        FALSE },
  [COL_TAIL]         = {  4, WID_SPIN,   BIND_PERCENT,  "tail-length",  0, 10, 300, 10, NULL,               FALSE },
  [COL_PULSE_DEPTH]  = {  5, WID_SPIN,   BIND_PERCENT,  "pulse-depth",  0,  0, 100,  5, NULL,               FALSE },
  [COL_PULSE_SPEED]  = {  6, WID_SPIN,   BIND_PERCENT,  "pulse-speed",  1, 10, 500, 10, NULL,               FALSE },
  [COL_ENV_ATTACK]   = {  7, WID_SPIN,   BIND_PERCENT,  "env-attack",   0,  0,  50,  5, "Attack",           FALSE },
  [COL_ENV_RELEASE]  = {  8, WID_SPIN,   BIND_PERCENT,  "env-release",  0,  0,  50,  5, "Release",          FALSE },
  [COL_RELEASE_MODE] = {  9, WID_BUTTON, BIND_RELEASE_MODE, "release-mode", 0,  0,   0,  0, "Release mode: U/R/S", FALSE },
  [COL_SHAPE]        = { 10, WID_BUTTON, BIND_SHAPE,    "shape",        0,  0,   0,  0, "Particle shape",   FALSE },
  [COL_BURST_COUNT]  = { 11, WID_SPIN,   BIND_DIRECT,   "burst-count",  0,  1,   8,  1, "Burst Count",      TRUE },
  [COL_BURST_SPREAD] = { 12, WID_SPIN,   BIND_PERCENT,  "burst-spread", 0, 50, 500, 10, "Burst Spread",     TRUE },
  [COL_ENV_CURVE]    = { 13, WID_BUTTON, BIND_CURVE,    "env-curve",    0,  0,   0,  0, "Envelope Curve",   FALSE },
  [COL_THK_ATTACK]   = { 14, WID_SPIN,   BIND_PERCENT,  "thk-attack",   0,  0,  50,  5, "Thickness Attack", FALSE },
  [COL_THK_RELEASE]  = { 15, WID_SPIN,   BIND_PERCENT,  "thk-release",  0,  0,  50,  5, "Thickness Release",FALSE },
  [COL_THK_RELEASE_MODE] = { 16, WID_BUTTON, BIND_THK_RELEASE_MODE, "thk-release-mode", 0, 0, 0, 0, "Thickness release mode", FALSE },
  [COL_THK_CURVE]    = { 17, WID_BUTTON, BIND_CURVE,    "thk-curve",    0,  0,   0,  0, "Thickness Curve",  FALSE },
};

#define N_PRESET_ROWS 8

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
  { "Ping-Pong",  "preset-ping-pong",  "ping-pong", FALSE },
  { "Pulse Out",  "preset-pulse-out",  "pulse-out", FALSE },
  { "Rotate",     "preset-rotate",     "rotate",    FALSE },
  { "Scroll 1",   "preset-overscroll",  NULL,        FALSE },
  { "Scroll 2",   "preset-scroll2",    "scroll2",   FALSE },
};

#define APP_GLASS_SLOTS 12

struct _KgxSettingsPage {
  AdwBin                parent_instance;

  KgxSettings          *settings;
  GBindingGroup        *settings_binds;


  GtkWidget            *audible_bell;
  GtkWidget            *visual_bell;
  GtkWidget            *command_complete_notifications;
  GtkWidget            *settings_root;
  GtkWidget            *about_content;
  GtkWidget            *use_system_font;
  GtkWidget            *custom_font;
  GtkWidget            *text_scale;
  GtkWidget            *app_use_system_font;
  GtkWidget            *app_font;
  GtkWidget            *app_text_scale;
  GtkWidget            *transparency_level;
  GtkWidget            *glass_color;
  GtkWidget            *glass_opacity;
  GtkWidget            *use_glass_bg;
  GtkWidget            *accent_color;
  GtkWidget            *unlimited_scrollback;
  GtkWidget            *scrollback;
  GtkWidget            *page_header;
  GtkWidget            *overscroll_switch;
  GtkWidget            *overscroll_color_btn;
  GtkWidget            *overscroll_preset_btn;
  GtkWidget            *overscroll_reverse_btn;
  GtkWidget            *ambient_switch;
  GtkWidget            *particle_throttle_switch;
  GtkWidget            *particle_hz_row;
  GtkWidget            *particle_hz;
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
  GtkWidget            *ag_shapes[APP_GLASS_SLOTS];
  GtkWidget            *ag_gaps[APP_GLASS_SLOTS];
  GtkWidget            *ag_speeds[APP_GLASS_SLOTS];
  GtkWidget            *ag_thks[APP_GLASS_SLOTS];
  GtkWidget            *ag_tails[APP_GLASS_SLOTS];
  GtkWidget            *ag_attacks[APP_GLASS_SLOTS];
  GtkWidget            *ag_releases[APP_GLASS_SLOTS];
  GtkWidget            *ag_release_modes[APP_GLASS_SLOTS];
  GtkWidget            *ag_thk_attacks[APP_GLASS_SLOTS];
  GtkWidget            *ag_thk_releases[APP_GLASS_SLOTS];
  GtkWidget            *ag_thk_release_modes[APP_GLASS_SLOTS];
  GtkWidget            *general_page_clamp;
  GtkWidget            *app_glass_page_clamp;
  GtkWidget            *particles_page_clamp;
  GtkWidget            *shortcuts_page_clamp;
  GtkWidget            *logo_picture;
  GtkWidget            *version_label;
  GtkWidget            *page_title;
  GtkWidget            *page_dots;
  GtkWidget            *dot_buttons[4];
  AdwCarousel          *carousel;
  KgxSprite            *sprite;

  GtkExpressionWatch   *font_watch;
  GtkExpressionWatch   *app_font_watch;
};


G_DEFINE_TYPE (KgxSettingsPage, kgx_settings_page, ADW_TYPE_BIN)

static void app_glass_load (KgxSettingsPage *self);
static void app_glass_save (KgxSettingsPage *self);
static void sync_all_shapes (KgxSettingsPage *self);
static void sync_all_release_modes (KgxSettingsPage *self);
static void sync_all_gaps (KgxSettingsPage *self);
static void sync_all_curves (KgxSettingsPage *self);


enum {
  PROP_0,
  PROP_SETTINGS,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void
set_page_clamp_margins (GtkWidget *widget,
                        int        top,
                        int        bottom,
                        int        start,
                        int        end)
{
  gtk_widget_set_margin_top (widget, top);
  gtk_widget_set_margin_bottom (widget, bottom);
  gtk_widget_set_margin_start (widget, start);
  gtk_widget_set_margin_end (widget, end);
}


static void
update_layout_mode (KgxSettingsPage *self)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  gboolean compact;

  if (width <= 0 || height <= 0) {
    return;
  }

  compact = width < 680 || height < 720;

  if (compact) {
    gtk_widget_add_css_class (GTK_WIDGET (self), "compact");
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), "compact");
  }

  gtk_box_set_spacing (GTK_BOX (self->settings_root), compact ? 4 : 8);
  gtk_box_set_spacing (GTK_BOX (self->about_content), compact ? 2 : 4);
  gtk_box_set_spacing (GTK_BOX (self->page_header), compact ? 2 : 4);

  gtk_widget_set_margin_top (self->about_content, compact ? 8 : 16);
  gtk_widget_set_margin_top (self->page_header, compact ? 0 : 4);
  gtk_widget_set_margin_bottom (self->page_header, compact ? 2 : 4);

  gtk_picture_set_can_shrink (GTK_PICTURE (self->logo_picture), compact);
  gtk_widget_set_size_request (self->logo_picture,
                               compact ? 88 : -1,
                               compact ? 104 : -1);

  set_page_clamp_margins (self->general_page_clamp,
                          compact ? 4 : 8,
                          compact ? 8 : 12,
                          compact ? 16 : 24,
                          compact ? 16 : 24);
  set_page_clamp_margins (self->app_glass_page_clamp,
                          compact ? 4 : 8,
                          compact ? 8 : 12,
                          compact ? 16 : 24,
                          compact ? 16 : 24);
  set_page_clamp_margins (self->particles_page_clamp,
                          compact ? 12 : 24,
                          compact ? 8 : 12,
                          compact ? 16 : 24,
                          compact ? 16 : 24);
  set_page_clamp_margins (self->shortcuts_page_clamp,
                          compact ? 4 : 8,
                          compact ? 8 : 12,
                          compact ? 16 : 24,
                          compact ? 16 : 24);
}


static void
settings_page_size_changed (GObject    *object,
                            GParamSpec *pspec,
                            gpointer    user_data)
{
  update_layout_mode (KGX_SETTINGS_PAGE (user_data));
}


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
  if (self->app_font_watch) {
    gtk_expression_watch_unwatch (self->app_font_watch);
    self->app_font_watch = NULL;
  }

  g_clear_handle_id (&self->app_glass_save_timeout, g_source_remove);

  /* Disconnect all signal handlers from App Glass widgets before
   * disposal — signals can fire during teardown and access freed state. */
  for (int i = 0; i < APP_GLASS_SLOTS; i++) {
    if (self->ag_entries[i])  g_signal_handlers_disconnect_by_data (self->ag_entries[i], self);
    if (self->ag_colors[i])   g_signal_handlers_disconnect_by_data (self->ag_colors[i], self);
    if (self->ag_presets[i])  g_signal_handlers_disconnect_by_data (self->ag_presets[i], self);
    if (self->ag_reverses[i]) g_signal_handlers_disconnect_by_data (self->ag_reverses[i], self);
    if (self->ag_shapes[i])   g_signal_handlers_disconnect_by_data (self->ag_shapes[i], self);
    if (self->ag_gaps[i])     g_signal_handlers_disconnect_by_data (self->ag_gaps[i], self);
    if (self->ag_speeds[i])   g_signal_handlers_disconnect_by_data (self->ag_speeds[i], self);
    if (self->ag_thks[i])     g_signal_handlers_disconnect_by_data (self->ag_thks[i], self);
    if (self->ag_tails[i])    g_signal_handlers_disconnect_by_data (self->ag_tails[i], self);
    if (self->ag_attacks[i])  g_signal_handlers_disconnect_by_data (self->ag_attacks[i], self);
    if (self->ag_releases[i]) g_signal_handlers_disconnect_by_data (self->ag_releases[i], self);
    if (self->ag_release_modes[i]) g_signal_handlers_disconnect_by_data (self->ag_release_modes[i], self);
    if (self->ag_thk_attacks[i]) g_signal_handlers_disconnect_by_data (self->ag_thk_attacks[i], self);
    if (self->ag_thk_releases[i]) g_signal_handlers_disconnect_by_data (self->ag_thk_releases[i], self);
    if (self->ag_thk_release_modes[i]) g_signal_handlers_disconnect_by_data (self->ag_thk_release_modes[i], self);
    if (self->ag_pcolors[i])  g_signal_handlers_disconnect_by_data (self->ag_pcolors[i], self);
  }

  if (self->overscroll_reverse_btn)
    g_signal_handlers_disconnect_by_data (self->overscroll_reverse_btn, self);

  /* Disconnect shape and release mode button click handlers */
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    if (self->tune_widgets[r][COL_SHAPE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_SHAPE], self);
    if (self->tune_widgets[r][COL_RELEASE_MODE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_RELEASE_MODE], self);
    if (self->tune_widgets[r][COL_THK_RELEASE_MODE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_THK_RELEASE_MODE], self);
    if (self->tune_widgets[r][COL_GAP])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_GAP], self);
    if (self->tune_widgets[r][COL_ENV_CURVE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_ENV_CURVE], self);
    if (self->tune_widgets[r][COL_THK_CURVE])
      g_signal_handlers_disconnect_by_data (self->tune_widgets[r][COL_THK_CURVE], self);
  }

  /* settings_binds is a template child — disconnect but do NOT free */
  if (self->settings_binds) {
    g_binding_group_set_source (self->settings_binds, NULL);
  }

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (kgx_settings_page_parent_class)->dispose (object);
}


static void
kgx_settings_page_map (GtkWidget *widget)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (widget);

  GTK_WIDGET_CLASS (kgx_settings_page_parent_class)->map (widget);

  if (self->sprite)
    kgx_sprite_start (self->sprite, widget);
}


static void
kgx_settings_page_unmap (GtkWidget *widget)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (widget);

  if (self->sprite)
    kgx_sprite_stop (self->sprite);

  GTK_WIDGET_CLASS (kgx_settings_page_parent_class)->unmap (widget);
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
        sync_all_release_modes (self);
        sync_all_gaps (self);
        sync_all_curves (self);
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
}


static void
app_font_selected (KgxFontPicker        *picker,
                   PangoFontDescription *font,
                   KgxSettingsPage      *self)
{
  kgx_settings_set_app_custom_font (self->settings, font);
}


static void
present_font_picker (GtkWidget             *widget,
                     PangoFontDescription  *initial_value,
                     GCallback              selected_callback,
                     gpointer               user_data,
                     const char            *title)
{
  g_autoptr (AdwDialog) dialog = NULL;
  AdwNavigationPage *picker;

  picker = g_object_connect (g_object_new (KGX_TYPE_FONT_PICKER,
                                           "initial-font", initial_value,
                                           NULL),
                             "object-signal::selected", selected_callback, user_data,
                             NULL);

  dialog = g_object_new (ADW_TYPE_DIALOG,
                         "child", picker,
                         "title", title,
                         "follows-content-size", FALSE,
                         "content-width", 520,
                         "content-height", 640,
                         NULL);

  adw_dialog_present (g_steal_pointer (&dialog), widget);
}


static void
select_font_activated (GtkWidget  *widget,
                       const char *action_name,
                       GVariant   *parameter)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (widget);
  g_autoptr (PangoFontDescription) initial_value = NULL;

  initial_value = kgx_settings_dup_custom_font (self->settings);
  present_font_picker (widget,
                       initial_value,
                       G_CALLBACK (font_selected),
                       self,
                       _("Select Terminal Font"));
}


static void
select_app_font_activated (GtkWidget  *widget,
                           const char *action_name,
                           GVariant   *parameter)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (widget);
  g_autoptr (PangoFontDescription) initial_value = NULL;

  initial_value = kgx_settings_dup_app_custom_font (self->settings);
  present_font_picker (widget,
                       initial_value,
                       G_CALLBACK (app_font_selected),
                       self,
                       _("Select Chiguiro Font"));
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


static gboolean
int_to_double (GBinding     *binding,
               const GValue *from_value,
               GValue       *to_value,
               gpointer      user_data)
{
  g_value_set_double (to_value, g_value_get_int (from_value));
  return TRUE;
}


static gboolean
double_to_int (GBinding     *binding,
               const GValue *from_value,
               GValue       *to_value,
               gpointer      user_data)
{
  g_value_set_int (to_value, (int) g_value_get_double (from_value));
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


/* --- Release mode cycling buttons --- */
/* Rls cycles U/R/S (3), T.Rls cycles U/R/S/G/A (5). */

static const char *release_mode_labels[] = { "U", "R", "S", "G", "A" };


static void
release_mode_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *prop_name = g_object_get_data (G_OBJECT (button), "settings-prop");
  int n_modes = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "n-modes"));
  int current = 0, next;
  if (!prop_name || !self->settings || n_modes < 2)
    return;

  g_object_get (self->settings, prop_name, &current, NULL);
  next = (current + 1) % n_modes;
  g_object_set (self->settings, prop_name, next, NULL);
  gtk_button_set_label (button, release_mode_labels[next]);
}


static void
setup_release_mode_button (KgxSettingsPage *self,
                           GtkWidget       *button,
                           const char      *settings_prop,
                           int              n_modes)
{
  g_object_set_data_full (G_OBJECT (button), "settings-prop",
                          g_strdup (settings_prop), g_free);
  g_object_set_data (G_OBJECT (button), "n-modes", GINT_TO_POINTER (n_modes));
  g_signal_connect (button, "clicked",
                    G_CALLBACK (release_mode_clicked_cb), self);
}


static void
sync_release_mode_label (KgxSettingsPage *self,
                         GtkWidget       *button,
                         const char      *settings_prop)
{
  int current = 0;
  if (!self->settings)
    return;
  g_object_get (self->settings, settings_prop, &current, NULL);
  current = CLAMP (current, 0, 3);
  gtk_button_set_label (GTK_BUTTON (button), release_mode_labels[current]);
}


static void
sync_all_release_modes (KgxSettingsPage *self)
{
  static const int release_cols[] = { COL_RELEASE_MODE, COL_THK_RELEASE_MODE };
  static const char *release_fields[] = { "release-mode", "thk-release-mode" };

  for (int ci = 0; ci < (int) G_N_ELEMENTS (release_cols); ci++) {
    int col = release_cols[ci];
    for (int r = 0; r < N_PRESET_ROWS; r++) {
      const PresetRowDesc *pr;
      char prop[64];
      GtkWidget *w = self->tune_widgets[r][col];
      if (!w)
        continue;
      pr = &preset_rows[r];
      if (pr->settings_suffix)
        g_snprintf (prop, sizeof prop, "edge-%s-%s", release_fields[ci], pr->settings_suffix);
      else
        g_snprintf (prop, sizeof prop, "edge-%s", release_fields[ci]);
      sync_release_mode_label (self, w, prop);
    }
  }
}


/* --- Gap cycling button: ▦ (gapped) / ▬ (solid) --- */

static const char *gap_labels[] = { "\u25A1", "\u25A0" };  /* 0=□ gapped, 1=■ solid */

static void
gap_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *prop_name = g_object_get_data (G_OBJECT (button), "settings-prop");
  gint current = 0, next;
  if (!prop_name || !self->settings)
    return;

  g_object_get (self->settings, prop_name, &current, NULL);
  next = current ? 0 : 1;
  g_object_set (self->settings, prop_name, next, NULL);
  gtk_button_set_label (button, gap_labels[next]);
}

static void
setup_gap_button (KgxSettingsPage *self,
                  GtkWidget       *button,
                  const char      *settings_prop)
{
  g_object_set_data_full (G_OBJECT (button), "settings-prop",
                          g_strdup (settings_prop), g_free);
  g_signal_connect (button, "clicked",
                    G_CALLBACK (gap_clicked_cb), self);
}

static void
sync_gap_label (KgxSettingsPage *self,
                GtkWidget       *button,
                const char      *settings_prop)
{
  int current = 0;
  if (!self->settings)
    return;
  g_object_get (self->settings, settings_prop, &current, NULL);
  current = CLAMP (current, 0, 1);
  gtk_button_set_label (GTK_BUTTON (button), gap_labels[current]);
}

static void
sync_all_gaps (KgxSettingsPage *self)
{
  for (int r = 0; r < N_PRESET_ROWS; r++) {
    const PresetRowDesc *pr;
    char prop[64];
    GtkWidget *w = self->tune_widgets[r][COL_GAP];
    if (!w)
      continue;
    pr = &preset_rows[r];
    if (pr->settings_suffix)
      g_snprintf (prop, sizeof prop, "edge-gap-%s", pr->settings_suffix);
    else
      g_snprintf (prop, sizeof prop, "edge-gap");
    sync_gap_label (self, w, prop);
  }
}


/* --- Curve cycling button: ∪ (concave/1) / ╱ (linear/2) / ∩ (convex/3) --- */

static const char *curve_labels[] = { "(", "(", "/", ")" };
/* Index 0 unused (values are 1-based): 1=( concave, 2=/ linear, 3=) convex */

static void
curve_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *prop_name = g_object_get_data (G_OBJECT (button), "settings-prop");
  gint current = 1, next;
  if (!prop_name || !self->settings)
    return;

  g_object_get (self->settings, prop_name, &current, NULL);
  next = (current % 3) + 1;  /* cycle 1→2→3→1 */
  g_object_set (self->settings, prop_name, next, NULL);
  gtk_button_set_label (button, curve_labels[next]);
}

static void
setup_curve_button (KgxSettingsPage *self,
                    GtkWidget       *button,
                    const char      *settings_prop)
{
  g_object_set_data_full (G_OBJECT (button), "settings-prop",
                          g_strdup (settings_prop), g_free);
  g_signal_connect (button, "clicked",
                    G_CALLBACK (curve_clicked_cb), self);
}

static void
sync_curve_label (KgxSettingsPage *self,
                  GtkWidget       *button,
                  const char      *settings_prop)
{
  gint current = 2;
  if (!self->settings)
    return;
  g_object_get (self->settings, settings_prop, &current, NULL);
  current = CLAMP (current, 1, 3);
  gtk_button_set_label (GTK_BUTTON (button), curve_labels[current]);
}

static void
sync_all_curves (KgxSettingsPage *self)
{
  static const int curve_cols[] = { COL_ENV_CURVE, COL_THK_CURVE };
  static const char *curve_fields[] = { "env-curve", "thk-curve" };

  for (int ci = 0; ci < (int) G_N_ELEMENTS (curve_cols); ci++) {
    int col = curve_cols[ci];
    for (int r = 0; r < N_PRESET_ROWS; r++) {
      const PresetRowDesc *pr;
      char prop[64];
      GtkWidget *w = self->tune_widgets[r][col];
      if (!w)
        continue;
      pr = &preset_rows[r];
      if (pr->settings_suffix)
        g_snprintf (prop, sizeof prop, "edge-%s-%s", curve_fields[ci], pr->settings_suffix);
      else
        g_snprintf (prop, sizeof prop, "edge-%s", curve_fields[ci]);
      sync_curve_label (self, w, prop);
    }
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
        gtk_widget_add_css_class (w, "tune-shape");
        break;
      case BIND_RELEASE_MODE:
        setup_release_mode_button (self, w, key, 3);  /* U/R/S */
        gtk_widget_add_css_class (w, "tune-rls");
        break;
      case BIND_THK_RELEASE_MODE:
        setup_release_mode_button (self, w, key, 5);  /* U/R/S/G/A */
        gtk_widget_add_css_class (w, "tune-thk-rls");
        break;
      case BIND_GAP:
        setup_gap_button (self, w, key);
        gtk_widget_add_css_class (w, "tune-gap");
        break;
      case BIND_CURVE:
        setup_curve_button (self, w, key);
        gtk_widget_add_css_class (w, "tune-curve");
        break;
      default:
        break;
      }
    }
  }
}


/* --- Reverse direction cycling button (▶/◀) --- */

static const char *reverse_labels[] = { "\u25B6", "\u25C0", "\u25C6" };  /* ▶ ◀ ◆ */

static void
reverse_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *current = gtk_button_get_label (button);
  int cur = 0;
  int next;
  if (current && g_str_equal (current, reverse_labels[1])) cur = 1;
  else if (current && g_str_equal (current, reverse_labels[2])) cur = 2;
  next = (cur + 1) % 3;
  gtk_button_set_label (button, reverse_labels[next]);
  app_glass_save (self);
}


/* --- Per-app shape override cycling button --- */
static const char *app_shape_labels[] = { "\u2014", "\u25A0", "\u25CF", "\u25C6", "\u25B6" };
/* —(default/-1), ■(0), ●(1), ◆(2), ▶(3) */

static void
app_shape_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *current = gtk_button_get_label (button);
  int cur = 0;
  int next;
  for (int k = 1; k < (int) G_N_ELEMENTS (app_shape_labels); k++) {
    if (current && g_str_equal (current, app_shape_labels[k])) { cur = k; break; }
  }
  next = (cur + 1) % (int) G_N_ELEMENTS (app_shape_labels);
  gtk_button_set_label (button, app_shape_labels[next]);
  app_glass_save (self);
}


/* --- Per-app gap override cycling button --- */
static const char *app_gap_labels[] = { "\u2014", "\u25A1", "\u25A0" };
/* —(default/-1), □(0=gapped), ■(1=solid) */

static const char *app_release_labels[] = { "\u2014", "U", "R", "S" };
static const char *app_thk_release_labels[] = { "\u2014", "U", "R", "S", "G", "A" };

static void
app_gap_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *current = gtk_button_get_label (button);
  int cur = 0;
  int next;
  for (int k = 1; k < (int) G_N_ELEMENTS (app_gap_labels); k++) {
    if (current && g_str_equal (current, app_gap_labels[k])) { cur = k; break; }
  }
  next = (cur + 1) % (int) G_N_ELEMENTS (app_gap_labels);
  gtk_button_set_label (button, app_gap_labels[next]);
  app_glass_save (self);
}

static void
app_release_mode_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *current = gtk_button_get_label (button);
  int cur = 0;
  int next;

  for (int k = 1; k < (int) G_N_ELEMENTS (app_release_labels); k++) {
    if (current && g_str_equal (current, app_release_labels[k])) {
      cur = k;
      break;
    }
  }

  next = (cur + 1) % (int) G_N_ELEMENTS (app_release_labels);
  gtk_button_set_label (button, app_release_labels[next]);
  app_glass_save (self);
}

static void
app_thk_release_mode_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  const char *current = gtk_button_get_label (button);
  int cur = 0;
  int next;

  for (int k = 1; k < (int) G_N_ELEMENTS (app_thk_release_labels); k++) {
    if (current && g_str_equal (current, app_thk_release_labels[k])) {
      cur = k;
      break;
    }
  }

  next = (cur + 1) % (int) G_N_ELEMENTS (app_thk_release_labels);
  gtk_button_set_label (button, app_thk_release_labels[next]);
  app_glass_save (self);
}


/* --- Spin button value-changed handler for app glass overrides --- */
static void
app_glass_spin_changed (GtkSpinButton   *spin,
                        KgxSettingsPage *self)
{
  app_glass_save (self);
}

static gboolean
app_override_spin_output_cb (GtkSpinButton *spin,
                             gpointer       user_data)
{
  if ((int) gtk_spin_button_get_value (spin) == 0) {
    gtk_editable_set_text (GTK_EDITABLE (spin), "--");
    return TRUE;
  }

  return FALSE;
}

static gint
app_override_spin_input_cb (GtkSpinButton *spin,
                            double        *new_value,
                            gpointer       user_data)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (spin));

  if (!text || text[0] == '\0' || g_str_equal (text, "--")) {
    *new_value = 0.0;
    return TRUE;
  }

  return GTK_INPUT_ERROR;
}

static void
setup_app_override_spin (GtkWidget *spin,
                         int        width,
                         const char *tooltip)
{
  gtk_editable_set_alignment (GTK_EDITABLE (spin), 0.5);
  gtk_widget_set_size_request (spin, width, -1);
  gtk_widget_set_tooltip_text (spin, tooltip);
  g_signal_connect (spin, "output",
                    G_CALLBACK (app_override_spin_output_cb), NULL);
  g_signal_connect (spin, "input",
                    G_CALLBACK (app_override_spin_input_cb), NULL);
}


/* App Glass grid columns:
 * Name(0) Glass(1) Preset(2) Rev(3) Shape(4) Gap(5) Spd(6) Thk(7)
 * Tail(8) Atk(9) Rel(10) Rls(11) TAtk(12) TRel(13) TRls(14) P.Clr(15) Switch(16) */

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
overscroll_reverse_clicked_cb (GtkButton *button, gpointer user_data)
{
  KgxSettingsPage *self = KGX_SETTINGS_PAGE (user_data);
  static const char *labels[] = { "\u25B6", "\u25C0", "\u21C6" };  /* ▶ ◀ ⇆ */
  int cur = 0;
  int next;
  if (self->settings)
    g_object_get (self->settings, "edge-overscroll-reverse", &cur, NULL);
  next = (cur + 1) % 3;
  gtk_button_set_label (button, labels[next]);
  if (self->settings)
    g_object_set (self->settings, "edge-overscroll-reverse", next, NULL);
}

static void
build_app_glass_grid (KgxSettingsPage *self)
{
  GtkGrid *grid = GTK_GRID (self->app_glass_grid);
  int row = 0;

  /* --- Row 0: Column headers --- */
  {
    static const struct { int col; const char *text; } hdrs[] = {
      { 1, "Glass" }, { 2, "Preset" }, { 3, "Rev" },
      { 4, "Shape" }, { 5, "Gap" }, { 6, "Spd" }, { 7, "Thk" },
      { 8, "Tail" }, { 9, "Atk" }, { 10, "Rel" }, { 11, "Rls" },
      { 12, "TAtk" }, { 13, "TRel" }, { 14, "TRls" }, { 15, "P.Clr" },
    };
    GtkWidget *title = gtk_label_new ("App Glass");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0f);
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

    self->overscroll_preset_btn = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_valign (self->overscroll_preset_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->overscroll_preset_btn, 86, -1);
    gtk_widget_add_css_class (self->overscroll_preset_btn, "glass-preset");
    gtk_grid_attach (grid, self->overscroll_preset_btn, 2, row, 1, 1);

    self->overscroll_reverse_btn = gtk_button_new_with_label ("\u25B6");
    gtk_widget_set_valign (self->overscroll_reverse_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (self->overscroll_reverse_btn, "Reverse direction");
    gtk_widget_add_css_class (self->overscroll_reverse_btn, "glass-reverse");
    gtk_widget_add_css_class (self->overscroll_reverse_btn, "flat");
    gtk_grid_attach (grid, self->overscroll_reverse_btn, 3, row, 1, 1);

    self->overscroll_switch = gtk_switch_new ();
    gtk_widget_set_valign (self->overscroll_switch, GTK_ALIGN_CENTER);
    gtk_grid_attach (grid, self->overscroll_switch, 16, row, 1, 1);
    row++;
  }

  /* --- Row 1: separator spanning all columns --- */
  {
    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top (sep, 4);
    gtk_widget_set_margin_bottom (sep, 4);
    gtk_grid_attach (grid, sep, 0, row, 17, 1);
    row++;
  }

  /* --- Rows 3..14: process slots 0..11 --- */
  for (int i = 0; i < APP_GLASS_SLOTS; i++, row++) {
    GtkWidget *entry, *rev;

    entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), "process name");
    g_object_set (entry,
                  "width-chars", 9,
                  "max-width-chars", 12,
                  NULL);
    gtk_widget_add_css_class (entry, "glass-entry");
    self->ag_entries[i] = entry;
    gtk_grid_attach (grid, entry, 0, row, 1, 1);

    self->ag_colors[i] = make_color_btn ();
    gtk_grid_attach (grid, self->ag_colors[i], 1, row, 1, 1);

    self->ag_presets[i] = gtk_drop_down_new (NULL, NULL);
    gtk_widget_set_valign (self->ag_presets[i], GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->ag_presets[i], 92, -1);
    gtk_widget_add_css_class (self->ag_presets[i], "glass-preset");
    gtk_grid_attach (grid, self->ag_presets[i], 2, row, 1, 1);

    rev = gtk_button_new_with_label ("\u25B6");
    gtk_widget_set_valign (rev, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (rev, "Reverse direction");
    gtk_widget_add_css_class (rev, "glass-reverse");
    gtk_widget_add_css_class (rev, "flat");
    g_object_set_data (G_OBJECT (rev), "slot-index", GINT_TO_POINTER (i));
    self->ag_reverses[i] = rev;
    gtk_grid_attach (grid, rev, 3, row, 1, 1);

    /* Shape override */
    {
      GtkWidget *shp = gtk_button_new_with_label ("\u2014");
      gtk_widget_add_css_class (shp, "flat");
      gtk_widget_add_css_class (shp, "tune-shape");
      gtk_widget_set_tooltip_text (shp, "Shape override");
      self->ag_shapes[i] = shp;
      gtk_grid_attach (grid, shp, 4, row, 1, 1);
    }

    /* Gap override */
    {
      GtkWidget *gp = gtk_button_new_with_label ("\u2014");
      gtk_widget_add_css_class (gp, "flat");
      gtk_widget_add_css_class (gp, "tune-gap");
      gtk_widget_set_tooltip_text (gp, "Gap override");
      self->ag_gaps[i] = gp;
      gtk_grid_attach (grid, gp, 5, row, 1, 1);
    }

    /* Speed override */
    {
      GtkAdjustment *spd_adj = gtk_adjustment_new (0, 0, 300, 10, 0, 0);
      GtkWidget *spd = gtk_spin_button_new (spd_adj, 1.0, 0);
      setup_app_override_spin (spd, 46, "Speed override (--=default)");
      self->ag_speeds[i] = spd;
      gtk_grid_attach (grid, spd, 6, row, 1, 1);
    }

    /* Thickness override */
    {
      GtkAdjustment *thk_adj = gtk_adjustment_new (0, 0, 40, 1, 0, 0);
      GtkWidget *thk = gtk_spin_button_new (thk_adj, 1.0, 0);
      setup_app_override_spin (thk, 38, "Thickness override (--=default)");
      self->ag_thks[i] = thk;
      gtk_grid_attach (grid, thk, 7, row, 1, 1);
    }

    {
      GtkAdjustment *tail_adj = gtk_adjustment_new (0, 0, 300, 10, 0, 0);
      GtkWidget *tail = gtk_spin_button_new (tail_adj, 1.0, 0);
      setup_app_override_spin (tail, 42, "Tail length override (--=default)");
      self->ag_tails[i] = tail;
      gtk_grid_attach (grid, tail, 8, row, 1, 1);
    }

    {
      GtkAdjustment *atk_adj = gtk_adjustment_new (0, 0, 50, 5, 0, 0);
      GtkWidget *atk = gtk_spin_button_new (atk_adj, 1.0, 0);
      setup_app_override_spin (atk, 38, "Envelope attack override (--=default)");
      self->ag_attacks[i] = atk;
      gtk_grid_attach (grid, atk, 9, row, 1, 1);
    }

    {
      GtkAdjustment *rel_adj = gtk_adjustment_new (0, 0, 50, 5, 0, 0);
      GtkWidget *rel = gtk_spin_button_new (rel_adj, 1.0, 0);
      setup_app_override_spin (rel, 38, "Envelope release override (--=default)");
      self->ag_releases[i] = rel;
      gtk_grid_attach (grid, rel, 10, row, 1, 1);
    }

    {
      GtkWidget *rls = gtk_button_new_with_label ("\u2014");
      gtk_widget_add_css_class (rls, "flat");
      gtk_widget_add_css_class (rls, "tune-rls");
      gtk_widget_set_tooltip_text (rls, "Release mode override");
      self->ag_release_modes[i] = rls;
      gtk_grid_attach (grid, rls, 11, row, 1, 1);
    }

    {
      GtkAdjustment *tatk_adj = gtk_adjustment_new (0, 0, 50, 5, 0, 0);
      GtkWidget *tatk = gtk_spin_button_new (tatk_adj, 1.0, 0);
      setup_app_override_spin (tatk, 38, "Thickness attack override (--=default)");
      self->ag_thk_attacks[i] = tatk;
      gtk_grid_attach (grid, tatk, 12, row, 1, 1);
    }

    {
      GtkAdjustment *trel_adj = gtk_adjustment_new (0, 0, 50, 5, 0, 0);
      GtkWidget *trel = gtk_spin_button_new (trel_adj, 1.0, 0);
      setup_app_override_spin (trel, 38, "Thickness release override (--=default)");
      self->ag_thk_releases[i] = trel;
      gtk_grid_attach (grid, trel, 13, row, 1, 1);
    }

    {
      GtkWidget *trls = gtk_button_new_with_label ("\u2014");
      gtk_widget_add_css_class (trls, "flat");
      gtk_widget_add_css_class (trls, "tune-thk-rls");
      gtk_widget_set_tooltip_text (trls, "Thickness release mode override");
      self->ag_thk_release_modes[i] = trls;
      gtk_grid_attach (grid, trls, 14, row, 1, 1);
    }

    self->ag_pcolors[i] = make_color_btn ();
    gtk_grid_attach (grid, self->ag_pcolors[i], 15, row, 1, 1);
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
  widget_class->map = kgx_settings_page_map;
  widget_class->unmap = kgx_settings_page_unmap;


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
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, command_complete_notifications);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, settings_root);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, about_content);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, use_system_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, custom_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, text_scale);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_use_system_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_font);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_text_scale);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, transparency_level);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, glass_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, glass_opacity);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, use_glass_bg);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, accent_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, unlimited_scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, page_header);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, ambient_switch);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, particle_throttle_switch);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, particle_hz_row);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, particle_hz);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, tunables_grid);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_grid);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, general_page_clamp);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_page_clamp);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, particles_page_clamp);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, shortcuts_page_clamp);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, logo_picture);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, version_label);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, page_title);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, page_dots);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, carousel);

  gtk_widget_class_bind_template_callback (widget_class, font_as_attributes);
  gtk_widget_class_bind_template_callback (widget_class, font_as_label);

  gtk_widget_class_install_action (widget_class,
                                   "settings.select-font",
                                   NULL,
                                   select_font_activated);
  gtk_widget_class_install_action (widget_class,
                                   "settings.select-app-font",
                                   NULL,
                                   select_app_font_activated);

  gtk_widget_class_set_css_name (widget_class, "settings-page");
}


struct _WatchData {
  KgxSettingsPage      *page;
  GtkExpressionWatch   *watch;
  const char           *action_name;
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
                                   data->action_name,
                                   !g_value_get_boolean (&value));
  } else {
    gtk_widget_action_set_enabled (GTK_WIDGET (data->page),
                                   data->action_name,
                                   FALSE);
  }
}


static void
bind_font_action_watch (KgxSettingsPage    *self,
                        const char         *property_name,
                        const char         *action_name,
                        GtkExpressionWatch **watch_slot)
{
  g_autoptr (GtkExpression) expression = NULL;
  g_autoptr (WatchData) data = watch_data_alloc ();

  g_set_weak_pointer (&data->page, self);
  data->action_name = action_name;

  expression =
    gtk_property_expression_new (KGX_TYPE_SETTINGS,
                                 gtk_property_expression_new (G_TYPE_BINDING_GROUP,
                                                              gtk_object_expression_new (G_OBJECT (self->settings_binds)),
                                                              "source"),
                                 property_name);

  *watch_slot = gtk_expression_watch (expression, self, notify_use_system, data, watch_data_free);
  data->watch = *watch_slot;
  g_steal_pointer (&data);
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
  g_autoptr (GHashTable) ht = NULL;

  if (self->app_glass_inhibit_save)
    return;

  ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (int i = 0; i < APP_GLASS_SLOTS; i++) {
    const char *name = gtk_editable_get_text (GTK_EDITABLE (self->ag_entries[i]));
    if (name && name[0] != '\0') {
      const GdkRGBA *rgba;
      guint preset_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->ag_presets[i]));
      const char *rev_label;
      int rev = 0;
      const GdkRGBA *pc;
      const char *shp_label;
      int shape = -1;
      const char *gap_label;
      int gap = -1;
      int tail;
      int attack;
      int release;
      const char *rls_label;
      int release_mode = -1;
      int thk_attack;
      int thk_release;
      const char *trls_label;
      int thk_release_mode = -1;
      int speed;
      int thk   = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_thks[i]));
      const char *preset_names[] = { "none", "ambient", "corners", "fireworks", "ping-pong", "pulse-out", "rotate" };
      const char *preset_str;
      char *value;

      rgba = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_colors[i]));

      /* Reverse: ▶=0, ◀=1, ◆=2 */
      rev_label = gtk_button_get_label (GTK_BUTTON (self->ag_reverses[i]));
      if (rev_label && g_str_equal (rev_label, reverse_labels[1])) rev = 1;
      else if (rev_label && g_str_equal (rev_label, reverse_labels[2])) rev = 2;

      pc = gtk_color_dialog_button_get_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_pcolors[i]));

      /* Shape: —=-1, ■=0, ●=1, ◆=2, ▶=3 */
      shp_label = gtk_button_get_label (GTK_BUTTON (self->ag_shapes[i]));
      for (int k = 1; k < (int) G_N_ELEMENTS (app_shape_labels); k++) {
        if (shp_label && g_str_equal (shp_label, app_shape_labels[k])) { shape = k - 1; break; }
      }

      /* Gap: —=-1, □=0, ■=1 */
      gap_label = gtk_button_get_label (GTK_BUTTON (self->ag_gaps[i]));
      for (int k = 1; k < (int) G_N_ELEMENTS (app_gap_labels); k++) {
        if (gap_label && g_str_equal (gap_label, app_gap_labels[k])) { gap = k - 1; break; }
      }

      speed = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_speeds[i]));
      tail = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_tails[i]));
      attack = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_attacks[i]));
      release = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_releases[i]));
      thk_attack = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_thk_attacks[i]));
      thk_release = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (self->ag_thk_releases[i]));
      rls_label = gtk_button_get_label (GTK_BUTTON (self->ag_release_modes[i]));
      trls_label = gtk_button_get_label (GTK_BUTTON (self->ag_thk_release_modes[i]));
      for (int k = 1; k < (int) G_N_ELEMENTS (app_release_labels); k++) {
        if (rls_label && g_str_equal (rls_label, app_release_labels[k])) {
          release_mode = k - 1;
          break;
        }
      }
      for (int k = 1; k < (int) G_N_ELEMENTS (app_thk_release_labels); k++) {
        if (trls_label && g_str_equal (trls_label, app_thk_release_labels[k])) {
          thk_release_mode = k - 1;
          break;
        }
      }
      preset_str = (preset_idx < G_N_ELEMENTS (preset_names))
                     ? preset_names[preset_idx] : "none";

      value = g_strdup_printf ("#%02x%02x%02x;%s;%d;#%02x%02x%02x;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d;%d",
        (int)(rgba->red * 255), (int)(rgba->green * 255), (int)(rgba->blue * 255),
        preset_str, rev,
        (int)(pc->red * 255), (int)(pc->green * 255), (int)(pc->blue * 255),
        shape, gap, speed, thk, tail, attack, release, release_mode,
        thk_attack, thk_release, thk_release_mode);
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
    gtk_button_set_label (GTK_BUTTON (self->ag_reverses[j]), reverse_labels[0]);
    gtk_button_set_label (GTK_BUTTON (self->ag_shapes[j]), "\u2014");
    gtk_button_set_label (GTK_BUTTON (self->ag_gaps[j]), "\u2014");
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_speeds[j]), 0);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thks[j]), 0);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_tails[j]), 0);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_attacks[j]), 0);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_releases[j]), 0);
    gtk_button_set_label (GTK_BUTTON (self->ag_release_modes[j]), "\u2014");
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thk_attacks[j]), 0);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thk_releases[j]), 0);
    gtk_button_set_label (GTK_BUTTON (self->ag_thk_release_modes[j]), "\u2014");
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_pcolors[j]),
                                      &glass_default_colors[j]);
  }

  if (ht) {
    g_hash_table_iter_init (&iter, ht);
    while (g_hash_table_iter_next (&iter, &key, &val) && i < APP_GLASS_SLOTS) {
      g_autofree char *glass_hex = NULL;
      KgxParticlePreset preset = KGX_PARTICLE_NONE;
      int reverse = 0;
      GdkRGBA particle_color = { 0.5f, 0.5f, 0.5f, 1.0f };
      KgxProcessParticleOverrides overrides = {
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
      gtk_editable_set_text (GTK_EDITABLE (self->ag_entries[i]), (const char *) key);
      kgx_parse_process_config ((const char *) val,
                                 &glass_hex, &preset, &reverse, &particle_color,
                                 &overrides);

      if (glass_hex) {
        GdkRGBA rgba = { 0, 0, 0, 1 };
        gdk_rgba_parse (&rgba, glass_hex);
        rgba.alpha = 1.0f;
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (self->ag_colors[i]), &rgba);
      }

      gtk_drop_down_set_selected (GTK_DROP_DOWN (self->ag_presets[i]),
                                   preset_string_to_index (kgx_particle_preset_to_string (preset)));

      /* Reverse: 0→▶, 1→◀, 2→◆ */
      if (reverse >= 0 && reverse < (int) G_N_ELEMENTS (reverse_labels))
        gtk_button_set_label (GTK_BUTTON (self->ag_reverses[i]), reverse_labels[reverse]);
      else
        gtk_button_set_label (GTK_BUTTON (self->ag_reverses[i]), reverse_labels[0]);

      /* Shape: -1→—, 0→■, 1→●, 2→◆, 3→▶ */
      if (overrides.shape >= 0 && overrides.shape + 1 < (int) G_N_ELEMENTS (app_shape_labels))
        gtk_button_set_label (GTK_BUTTON (self->ag_shapes[i]), app_shape_labels[overrides.shape + 1]);
      else
        gtk_button_set_label (GTK_BUTTON (self->ag_shapes[i]), app_shape_labels[0]);

      /* Gap: -1→—, 0→□, 1→■ */
      if (overrides.gap >= 0 && overrides.gap + 1 < (int) G_N_ELEMENTS (app_gap_labels))
        gtk_button_set_label (GTK_BUTTON (self->ag_gaps[i]), app_gap_labels[overrides.gap + 1]);
      else
        gtk_button_set_label (GTK_BUTTON (self->ag_gaps[i]), app_gap_labels[0]);

      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_speeds[i]), overrides.speed);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thks[i]), overrides.thickness);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_tails[i]), overrides.tail_length);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_attacks[i]), overrides.env_attack);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_releases[i]), overrides.env_release);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thk_attacks[i]), overrides.thk_attack);
      gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->ag_thk_releases[i]), overrides.thk_release);

      if (overrides.release_mode >= 0 &&
          overrides.release_mode + 1 < (int) G_N_ELEMENTS (app_release_labels))
        gtk_button_set_label (GTK_BUTTON (self->ag_release_modes[i]),
                              app_release_labels[overrides.release_mode + 1]);
      else
        gtk_button_set_label (GTK_BUTTON (self->ag_release_modes[i]), app_release_labels[0]);

      if (overrides.thk_release_mode >= 0 &&
          overrides.thk_release_mode + 1 < (int) G_N_ELEMENTS (app_thk_release_labels))
        gtk_button_set_label (GTK_BUTTON (self->ag_thk_release_modes[i]),
                              app_thk_release_labels[overrides.thk_release_mode + 1]);
      else
        gtk_button_set_label (GTK_BUTTON (self->ag_thk_release_modes[i]), app_thk_release_labels[0]);

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
  "· General ·", "· App Glass ·", "· Particles ·", "· Shortcuts ·"
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
dot_clicked_cb (GtkButton *button, KgxSettingsPage *self)
{
  int page = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "page-index"));
  GtkWidget *target = adw_carousel_get_nth_page (self->carousel, page);
  adw_carousel_scroll_to (self->carousel, target, TRUE);
}


static void
carousel_page_changed (AdwCarousel     *carousel,
                       guint            index,
                       KgxSettingsPage *self)
{
  guint page = adw_carousel_get_position (carousel) + 0.5;

  if (page < G_N_ELEMENTS (page_titles))
    gtk_label_set_label (GTK_LABEL (self->page_title), page_titles[page]);

  /* Update dot highlights */
  for (int i = 0; i < (int) G_N_ELEMENTS (page_titles); i++) {
    if (self->dot_buttons[i]) {
      if ((guint) i == page)
        gtk_widget_add_css_class (self->dot_buttons[i], "page-dot-active");
      else
        gtk_widget_remove_css_class (self->dot_buttons[i], "page-dot-active");
    }
  }
}


static void
kgx_settings_page_init (KgxSettingsPage *self)
{
  self->app_glass_inhibit_save = TRUE;

  gtk_widget_init_template (GTK_WIDGET (self));

  g_signal_connect (self, "notify::width",
                    G_CALLBACK (settings_page_size_changed), self);
  g_signal_connect (self, "notify::height",
                    G_CALLBACK (settings_page_size_changed), self);

  /* Set version label from build-time PACKAGE_VERSION */
  {
    g_autofree char *ver = g_strdup_printf ("v%s", PACKAGE_VERSION);
    gtk_label_set_label (GTK_LABEL (self->version_label), ver);
  }

  build_tunables_grid (self);
  build_app_glass_grid (self);

  /* Build clickable page dots */
  for (int i = 0; i < (int) G_N_ELEMENTS (page_titles); i++) {
    GtkWidget *dot = gtk_button_new_with_label ("\u25CF");  /* ● */
    gtk_widget_add_css_class (dot, "flat");
    gtk_widget_add_css_class (dot, "page-dot");
    if (i == 0)
      gtk_widget_add_css_class (dot, "page-dot-active");
    g_object_set_data (G_OBJECT (dot), "page-index", GINT_TO_POINTER (i));
    g_signal_connect (dot, "clicked", G_CALLBACK (dot_clicked_cb), self);
    gtk_box_append (GTK_BOX (self->page_dots), dot);
    self->dot_buttons[i] = dot;
  }

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
      g_signal_connect (self->ag_reverses[i], "clicked",
                        G_CALLBACK (reverse_clicked_cb), self);
      g_signal_connect (self->ag_shapes[i], "clicked",
                        G_CALLBACK (app_shape_clicked_cb), self);
      g_signal_connect (self->ag_gaps[i], "clicked",
                        G_CALLBACK (app_gap_clicked_cb), self);
      g_signal_connect (self->ag_speeds[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_thks[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_tails[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_attacks[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_releases[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_release_modes[i], "clicked",
                        G_CALLBACK (app_release_mode_clicked_cb), self);
      g_signal_connect (self->ag_thk_attacks[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_thk_releases[i], "value-changed",
                        G_CALLBACK (app_glass_spin_changed), self);
      g_signal_connect (self->ag_thk_release_modes[i], "clicked",
                        G_CALLBACK (app_thk_release_mode_clicked_cb), self);
      g_signal_connect (self->ag_pcolors[i], "notify::rgba",
                        G_CALLBACK (app_glass_color_changed), self);
    }
  }

  /* Overscroll preset dropdown and reverse button */
  {
    const char *overscroll_preset_names[] = { "Scroll 1", "Scroll 2", NULL };
    GtkStringList *model = gtk_string_list_new (overscroll_preset_names);
    gtk_drop_down_set_model (GTK_DROP_DOWN (self->overscroll_preset_btn), G_LIST_MODEL (model));
    g_object_unref (model);

    g_signal_connect (self->overscroll_reverse_btn, "clicked",
                      G_CALLBACK (overscroll_reverse_clicked_cb), self);
  }

  /* Capybara sprite animation — credit: https://rainloaf.itch.io/capybara-sprite-sheet */
  self->sprite = kgx_sprite_new (KGX_APPLICATION_PATH "icons/capybara.png",
                                 24, 28, 10, 96, 10.0);
  gtk_picture_set_paintable (GTK_PICTURE (self->logo_picture),
                             GDK_PAINTABLE (self->sprite));
  update_layout_mode (self);

  bind_font_action_watch (self, "use-system-font",
                          "settings.select-font",
                          &self->font_watch);
  bind_font_action_watch (self, "app-use-system-font",
                          "settings.select-app-font",
                          &self->app_font_watch);

  g_binding_group_bind (self->settings_binds, "audible-bell",
                        self->audible_bell, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "visual-bell",
                        self->visual_bell, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "command-complete-notifications",
                        self->command_complete_notifications, "active",
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
  g_binding_group_bind (self->settings_binds, "app-use-system-font",
                        self->app_use_system_font, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "app-font-scale",
                             self->app_text_scale, "value",
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
  g_binding_group_bind (self->settings_binds, "edge-overscroll-style",
                        self->overscroll_preset_btn, "selected",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "edge-settings-animation",
                        self->ambient_switch, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind (self->settings_binds, "edge-particle-throttle",
                        self->particle_throttle_switch, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-particle-hz",
                             self->particle_hz, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             int_to_double, double_to_int, NULL, NULL);

  bind_tunables (self);
}
