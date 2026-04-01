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
  GtkWidget            *chrome_color;
  GtkWidget            *chrome_opacity;
  GtkWidget            *accent_color;
  GtkWidget            *unlimited_scrollback;
  GtkWidget            *scrollback;
  GtkWidget            *logo_picture;
  KgxSprite            *sprite;

  GtkExpressionWatch   *font_watch;
};


G_DEFINE_TYPE (KgxSettingsPage, kgx_settings_page, ADW_TYPE_BIN)


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
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, chrome_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, chrome_opacity);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, accent_color);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, unlimited_scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, scrollback);
  gtk_widget_class_bind_template_child (widget_class, KgxSettingsPage, logo_picture);

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


static void
kgx_settings_page_init (KgxSettingsPage *self)
{
  g_autoptr (GtkExpression) expression = NULL;
  g_autoptr (WatchData) data = watch_data_alloc ();

  gtk_widget_init_template (GTK_WIDGET (self));

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
  g_binding_group_bind_full (self->settings_binds, "chrome-color",
                             self->chrome_color, "rgba",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             string_to_rgba,
                             rgba_to_string,
                             NULL, NULL);
  g_binding_group_bind_full (self->settings_binds, "chrome-opacity",
                             self->chrome_opacity, "value",
                             G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                             value_to_percent,
                             percent_to_value,
                             NULL, NULL);

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
}
