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
  GtkWidget            *edge_overscroll;
  GtkWidget            *edge_overscroll_color;
  GtkWidget            *edge_privilege;
  GtkWidget            *edge_privilege_color;
  GtkWidget            *edge_thickness;
  GtkWidget            *edge_speed;
  GtkWidget            *edge_pulse_depth;
  GtkWidget            *edge_tail_length;
  GtkWidget            *edge_pulse_speed;
  GtkWidget            *edge_burst_count;
  GtkWidget            *edge_burst_spread;
  gboolean              app_glass_inhibit_save;
  guint                 app_glass_save_timeout;
  GtkWidget            *app_glass_0, *app_glass_1, *app_glass_2, *app_glass_3;
  GtkWidget            *app_glass_4, *app_glass_5, *app_glass_6, *app_glass_7;
  GtkWidget            *app_glass_8, *app_glass_9, *app_glass_10, *app_glass_11;
  GtkWidget            *app_glass_12, *app_glass_13, *app_glass_14;
  GtkWidget            *app_glass_color_0, *app_glass_color_1, *app_glass_color_2, *app_glass_color_3;
  GtkWidget            *app_glass_color_4, *app_glass_color_5, *app_glass_color_6, *app_glass_color_7;
  GtkWidget            *app_glass_color_8, *app_glass_color_9, *app_glass_color_10, *app_glass_color_11;
  GtkWidget            *app_glass_color_12, *app_glass_color_13, *app_glass_color_14;
  GtkWidget            *app_glass_preset_0, *app_glass_preset_1, *app_glass_preset_2, *app_glass_preset_3;
  GtkWidget            *app_glass_preset_4, *app_glass_preset_5, *app_glass_preset_6, *app_glass_preset_7;
  GtkWidget            *app_glass_preset_8, *app_glass_preset_9, *app_glass_preset_10, *app_glass_preset_11;
  GtkWidget            *app_glass_preset_12, *app_glass_preset_13, *app_glass_preset_14;
  GtkWidget            *app_glass_reverse_0, *app_glass_reverse_1, *app_glass_reverse_2, *app_glass_reverse_3;
  GtkWidget            *app_glass_reverse_4, *app_glass_reverse_5, *app_glass_reverse_6, *app_glass_reverse_7;
  GtkWidget            *app_glass_reverse_8, *app_glass_reverse_9, *app_glass_reverse_10, *app_glass_reverse_11;
  GtkWidget            *app_glass_reverse_12, *app_glass_reverse_13, *app_glass_reverse_14;
  GtkWidget            *app_glass_pcolor_0, *app_glass_pcolor_1, *app_glass_pcolor_2, *app_glass_pcolor_3;
  GtkWidget            *app_glass_pcolor_4, *app_glass_pcolor_5, *app_glass_pcolor_6, *app_glass_pcolor_7;
  GtkWidget            *app_glass_pcolor_8, *app_glass_pcolor_9, *app_glass_pcolor_10, *app_glass_pcolor_11;
  GtkWidget            *app_glass_pcolor_12, *app_glass_pcolor_13, *app_glass_pcolor_14;
  GtkWidget            *logo_picture;
  GtkWidget            *page_title;
  AdwCarousel          *carousel;
  KgxSprite            *sprite;

  GtkExpressionWatch   *font_watch;
};


G_DEFINE_TYPE (KgxSettingsPage, kgx_settings_page, ADW_TYPE_BIN)

#define APP_GLASS_SLOTS 15
static void app_glass_load (KgxSettingsPage *self);
static inline GtkWidget **app_glass_entries (KgxSettingsPage *self);
static inline GtkWidget **app_glass_colors (KgxSettingsPage *self);
static inline GtkWidget **app_glass_presets (KgxSettingsPage *self);
static inline GtkWidget **app_glass_reverses (KgxSettingsPage *self);
static inline GtkWidget **app_glass_pcolors (KgxSettingsPage *self);


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

  /* Disconnect all signal handlers from App Glass template children before
   * disposal — signals can fire during teardown and access freed state. */
  {
    GtkWidget **entries  = app_glass_entries (self);
    GtkWidget **colors   = app_glass_colors (self);
    GtkWidget **presets  = app_glass_presets (self);
    GtkWidget **reverses = app_glass_reverses (self);
    GtkWidget **pcolors  = app_glass_pcolors (self);
    for (int i = 0; i < APP_GLASS_SLOTS; i++) {
      if (entries[i])  g_signal_handlers_disconnect_by_data (entries[i], self);
      if (colors[i])   g_signal_handlers_disconnect_by_data (colors[i], self);
      if (presets[i])  g_signal_handlers_disconnect_by_data (presets[i], self);
      if (reverses[i]) g_signal_handlers_disconnect_by_data (reverses[i], self);
      if (pcolors[i])  g_signal_handlers_disconnect_by_data (pcolors[i], self);
    }
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
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_overscroll);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_overscroll_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_privilege);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_privilege_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_thickness);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_speed);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_pulse_depth);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_tail_length);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_pulse_speed);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_burst_count);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, edge_burst_spread);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_0);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_1);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_2);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_3);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_4);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_5);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_6);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_7);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_0);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_1);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_2);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_3);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_4);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_5);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_6);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_7);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_8);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_9);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_10);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_11);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_12);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_13);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_14);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_8);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_9);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_10);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_11);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_12);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_13);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_color_14);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_0);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_1);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_2);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_3);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_4);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_5);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_6);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_7);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_8);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_9);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_10);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_11);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_12);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_13);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_preset_14);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_0);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_1);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_2);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_3);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_4);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_5);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_6);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_7);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_8);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_9);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_10);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_11);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_12);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_13);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_reverse_14);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_0);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_1);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_2);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_3);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_4);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_5);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_6);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_7);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_8);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_9);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_10);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_11);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_12);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_13);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, app_glass_pcolor_14);
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


static inline GtkWidget **
app_glass_entries (KgxSettingsPage *self)
{
  static GtkWidget *arr[APP_GLASS_SLOTS];
  arr[0] = self->app_glass_0;   arr[1] = self->app_glass_1;
  arr[2] = self->app_glass_2;   arr[3] = self->app_glass_3;
  arr[4] = self->app_glass_4;   arr[5] = self->app_glass_5;
  arr[6] = self->app_glass_6;   arr[7] = self->app_glass_7;
  arr[8] = self->app_glass_8;   arr[9] = self->app_glass_9;
  arr[10] = self->app_glass_10; arr[11] = self->app_glass_11;
  arr[12] = self->app_glass_12; arr[13] = self->app_glass_13;
  arr[14] = self->app_glass_14;
  return arr;
}


static inline GtkWidget **
app_glass_colors (KgxSettingsPage *self)
{
  static GtkWidget *arr[APP_GLASS_SLOTS];
  arr[0] = self->app_glass_color_0;   arr[1] = self->app_glass_color_1;
  arr[2] = self->app_glass_color_2;   arr[3] = self->app_glass_color_3;
  arr[4] = self->app_glass_color_4;   arr[5] = self->app_glass_color_5;
  arr[6] = self->app_glass_color_6;   arr[7] = self->app_glass_color_7;
  arr[8] = self->app_glass_color_8;   arr[9] = self->app_glass_color_9;
  arr[10] = self->app_glass_color_10; arr[11] = self->app_glass_color_11;
  arr[12] = self->app_glass_color_12; arr[13] = self->app_glass_color_13;
  arr[14] = self->app_glass_color_14;
  return arr;
}


static inline GtkWidget **
app_glass_presets (KgxSettingsPage *self)
{
  static GtkWidget *arr[APP_GLASS_SLOTS];
  arr[0] = self->app_glass_preset_0;   arr[1] = self->app_glass_preset_1;
  arr[2] = self->app_glass_preset_2;   arr[3] = self->app_glass_preset_3;
  arr[4] = self->app_glass_preset_4;   arr[5] = self->app_glass_preset_5;
  arr[6] = self->app_glass_preset_6;   arr[7] = self->app_glass_preset_7;
  arr[8] = self->app_glass_preset_8;   arr[9] = self->app_glass_preset_9;
  arr[10] = self->app_glass_preset_10; arr[11] = self->app_glass_preset_11;
  arr[12] = self->app_glass_preset_12; arr[13] = self->app_glass_preset_13;
  arr[14] = self->app_glass_preset_14;
  return arr;
}


static inline GtkWidget **
app_glass_reverses (KgxSettingsPage *self)
{
  static GtkWidget *arr[APP_GLASS_SLOTS];
  arr[0] = self->app_glass_reverse_0;   arr[1] = self->app_glass_reverse_1;
  arr[2] = self->app_glass_reverse_2;   arr[3] = self->app_glass_reverse_3;
  arr[4] = self->app_glass_reverse_4;   arr[5] = self->app_glass_reverse_5;
  arr[6] = self->app_glass_reverse_6;   arr[7] = self->app_glass_reverse_7;
  arr[8] = self->app_glass_reverse_8;   arr[9] = self->app_glass_reverse_9;
  arr[10] = self->app_glass_reverse_10; arr[11] = self->app_glass_reverse_11;
  arr[12] = self->app_glass_reverse_12; arr[13] = self->app_glass_reverse_13;
  arr[14] = self->app_glass_reverse_14;
  return arr;
}


static inline GtkWidget **
app_glass_pcolors (KgxSettingsPage *self)
{
  static GtkWidget *arr[APP_GLASS_SLOTS];
  arr[0] = self->app_glass_pcolor_0;   arr[1] = self->app_glass_pcolor_1;
  arr[2] = self->app_glass_pcolor_2;   arr[3] = self->app_glass_pcolor_3;
  arr[4] = self->app_glass_pcolor_4;   arr[5] = self->app_glass_pcolor_5;
  arr[6] = self->app_glass_pcolor_6;   arr[7] = self->app_glass_pcolor_7;
  arr[8] = self->app_glass_pcolor_8;   arr[9] = self->app_glass_pcolor_9;
  arr[10] = self->app_glass_pcolor_10; arr[11] = self->app_glass_pcolor_11;
  arr[12] = self->app_glass_pcolor_12; arr[13] = self->app_glass_pcolor_13;
  arr[14] = self->app_glass_pcolor_14;
  return arr;
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
  { 0.11f, 0.13f, 0.14f, 1.0f },  /* ocean */
  { 0.14f, 0.13f, 0.11f, 1.0f },  /* amber */
  { 0.12f, 0.11f, 0.13f, 1.0f },  /* dusk */
};


static void
app_glass_save (KgxSettingsPage *self)
{
  if (self->app_glass_inhibit_save)
    return;
  GtkWidget **entries  = app_glass_entries (self);
  GtkWidget **colors   = app_glass_colors (self);
  GtkWidget **presets  = app_glass_presets (self);
  GtkWidget **reverses = app_glass_reverses (self);
  GtkWidget **pcolors  = app_glass_pcolors (self);
  g_autoptr (GHashTable) ht = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_free);

  for (int i = 0; i < APP_GLASS_SLOTS; i++) {
    const char *name = gtk_editable_get_text (GTK_EDITABLE (entries[i]));
    if (name && name[0] != '\0') {
      const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (
                               GTK_COLOR_DIALOG_BUTTON (colors[i]));
      guint preset_idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (presets[i]));
      gboolean rev = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (reverses[i]));
      const GdkRGBA *pc = gtk_color_dialog_button_get_rgba (
                             GTK_COLOR_DIALOG_BUTTON (pcolors[i]));

      const char *preset_names[] = { "none", "fireworks", "corners", "pulse-out", "rotate" };
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
  if (g_str_equal (s, "fireworks"))  return 1;
  if (g_str_equal (s, "corners"))    return 2;
  if (g_str_equal (s, "pulse-out"))  return 3;
  if (g_str_equal (s, "rotate"))     return 4;
  return 0;
}


static void
app_glass_load (KgxSettingsPage *self)
{
  GtkWidget **entries  = app_glass_entries (self);
  GtkWidget **colors   = app_glass_colors (self);
  GtkWidget **presets  = app_glass_presets (self);
  GtkWidget **reverses = app_glass_reverses (self);
  GtkWidget **pcolors  = app_glass_pcolors (self);
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
    gtk_editable_set_text (GTK_EDITABLE (entries[j]), "");
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (colors[j]),
                                      &glass_default_colors[j]);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (presets[j]), 0);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (reverses[j]), FALSE);
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (pcolors[j]),
                                      &glass_default_colors[j]);
  }

  if (ht) {
    g_hash_table_iter_init (&iter, ht);
    while (g_hash_table_iter_next (&iter, &key, &val) && i < APP_GLASS_SLOTS) {
      g_autofree char *glass_hex = NULL;
      KgxParticlePreset preset = KGX_PARTICLE_NONE;
      gboolean reverse = FALSE;
      GdkRGBA particle_color = { 0.5f, 0.5f, 0.5f, 1.0f };

      gtk_editable_set_text (GTK_EDITABLE (entries[i]), (const char *) key);
      kgx_parse_process_config ((const char *) val,
                                 &glass_hex, &preset, &reverse, &particle_color);

      if (glass_hex) {
        GdkRGBA rgba = { 0, 0, 0, 1 };
        gdk_rgba_parse (&rgba, glass_hex);
        rgba.alpha = 1.0f;
        gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (colors[i]), &rgba);
      }

      gtk_drop_down_set_selected (GTK_DROP_DOWN (presets[i]),
                                   preset_string_to_index (kgx_particle_preset_to_string (preset)));
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (reverses[i]), reverse);
      particle_color.alpha = 1.0f;
      gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (pcolors[i]), &particle_color);
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
    GtkWidget **entries  = app_glass_entries (self);
    GtkWidget **colors   = app_glass_colors (self);
    GtkWidget **presets  = app_glass_presets (self);
    GtkWidget **reverses = app_glass_reverses (self);
    GtkWidget **pcolors  = app_glass_pcolors (self);

    /* Each dropdown gets its own model to avoid double-free on dispose. */
    const char *preset_names[] = { "none", "fireworks", "corners", "pulse-out", "rotate", NULL };
    for (int i = 0; i < APP_GLASS_SLOTS; i++) {
      GtkStringList *model = gtk_string_list_new (preset_names);
      gtk_drop_down_set_model (GTK_DROP_DOWN (presets[i]), G_LIST_MODEL (model));
      g_object_unref (model);
    }

    for (int i = 0; i < APP_GLASS_SLOTS; i++) {
      g_signal_connect (entries[i], "changed",
                        G_CALLBACK (app_glass_changed), self);
      g_signal_connect (colors[i], "notify::rgba",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (presets[i], "notify::selected",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (reverses[i], "toggled",
                        G_CALLBACK (app_glass_color_changed), self);
      g_signal_connect (pcolors[i], "notify::rgba",
                        G_CALLBACK (app_glass_color_changed), self);
    }
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

  /* Edge effects */
  g_binding_group_bind (self->settings_binds, "edge-overscroll",
                        self->edge_overscroll, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-overscroll-color",
                             self->edge_overscroll_color, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba, rgba_to_string, NULL, NULL);
  g_binding_group_bind (self->settings_binds, "edge-privilege",
                        self->edge_privilege, "active",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-privilege-color",
                             self->edge_privilege_color, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba, rgba_to_string, NULL, NULL);
  g_binding_group_bind (self->settings_binds, "edge-thickness",
                        self->edge_thickness, "value",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-speed",
                             self->edge_speed, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent, percent_to_value, NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "edge-pulse-depth",
                             self->edge_pulse_depth, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent, percent_to_value, NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "edge-tail-length",
                             self->edge_tail_length, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent, percent_to_value, NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "edge-pulse-speed",
                             self->edge_pulse_speed, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent, percent_to_value, NULL, NULL);
  g_binding_group_bind (self->settings_binds, "edge-burst-count",
                        self->edge_burst_count, "value",
                        G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
  g_binding_group_bind_full (self->settings_binds, "edge-burst-spread",
                             self->edge_burst_spread, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent, percent_to_value, NULL, NULL);
}
