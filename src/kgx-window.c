/* kgx-window.c
 *
 * Copyright 2019-2023 Zander Brown
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

#define PCRE2_CODE_UNIT_WIDTH 0
#include <pcre2.h>

#include <adwaita.h>

#include "kgx-application.h"
#include "kgx-close-dialog.h"
#include "kgx-empty.h"
#include "kgx-file-closures.h"
#include "kgx-fullscreen-box.h"
#include "kgx-pages.h"
#include "kgx-settings.h"
#include "kgx-settings-page.h"
#include "kgx-shared-closures.h"
#include "kgx-terminal.h"
#include "kgx-utils.h"

#include "kgx-window.h"


typedef struct _KgxWindowPrivate KgxWindowPrivate;
struct _KgxWindowPrivate {
  KgxSettings          *settings;
  GBindingGroup        *settings_binds;

  gboolean              search_enabled;
  gboolean              floating;
  gboolean              translucent;

  gboolean              close_anyway;

  gboolean              settings_visible;

  /* Template widgets */
  GtkWidget            *header_bar;
  GtkWidget            *tab_bar;
  GtkWidget            *pages;
  GtkWidget            *search_bar;
  GtkWidget            *search_entry;
  GtkWidget            *content_stack;
  GtkWidget            *settings_page;

  GBindingGroup        *surface_binds;
};


G_DEFINE_TYPE_WITH_PRIVATE (KgxWindow, kgx_window, ADW_TYPE_APPLICATION_WINDOW)

/* Shared CSS provider for chrome styling — lives for the process lifetime */
static GtkCssProvider *chrome_css = NULL;


enum {
  PROP_0,
  PROP_SETTINGS,
  PROP_SEARCH_MODE_ENABLED,
  PROP_FLOATING,
  PROP_TRANSLUCENT,
  PROP_SETTINGS_VISIBLE,
  LAST_PROP
};
static GParamSpec *pspecs[LAST_PROP] = { NULL, };


static void kgx_window_update_opaque_region (KgxWindow *self);
static void kgx_window_update_chrome_opacity (KgxWindow *self);
static void chrome_opacity_changed (GObject *object, GParamSpec *pspec, KgxWindow *self);


static void
kgx_window_dispose (GObject *object)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  if (priv->settings_binds) {
    g_binding_group_set_source (priv->settings_binds, NULL);
  }
  if (priv->surface_binds) {
    g_binding_group_set_source (priv->surface_binds, NULL);
  }

  /* chrome_css is a process-lifetime singleton — don't clear it */

  g_clear_object (&priv->settings);

  G_OBJECT_CLASS (kgx_window_parent_class)->dispose (object);
}


static void
kgx_window_set_property (GObject      *object,
                         guint         property_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  switch (property_id) {
    case PROP_SETTINGS:
      if (g_set_object (&priv->settings, g_value_get_object (value)) &&
          priv->settings) {
        g_signal_connect_object (priv->settings,
                                 "notify::chrome-opacity",
                                 G_CALLBACK (chrome_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        g_signal_connect_object (priv->settings,
                                 "notify::chrome-color",
                                 G_CALLBACK (chrome_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        g_signal_connect_object (priv->settings,
                                 "notify::accent-color",
                                 G_CALLBACK (chrome_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        kgx_window_update_chrome_opacity (self);
      }
      break;
    case PROP_SEARCH_MODE_ENABLED:
      kgx_set_boolean_prop (object, pspec, &priv->search_enabled, value);
      break;
    case PROP_FLOATING:
      kgx_set_boolean_prop (object, pspec, &priv->floating, value);
      break;
    case PROP_TRANSLUCENT:
      /* Always translucent */
      priv->translucent = TRUE;
      gtk_widget_add_css_class (GTK_WIDGET (self), "translucent");
      kgx_window_update_opaque_region (self);
      gtk_widget_queue_allocate (GTK_WIDGET (self));
      gtk_widget_queue_draw (GTK_WIDGET (self));
      g_object_notify_by_pspec (object, pspec);
      break;
    case PROP_SETTINGS_VISIBLE:
      if (kgx_set_boolean_prop (object, pspec, &priv->settings_visible, value)) {
        if (priv->content_stack) {
          gtk_stack_set_visible_child_name (
            GTK_STACK (priv->content_stack),
            priv->settings_visible ? "settings" : "terminal");
        }
        if (!priv->settings_visible && priv->pages) {
          gtk_widget_grab_focus (priv->pages);
        }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_window_get_property (GObject    *object,
                         guint       property_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  switch (property_id) {
    case PROP_SETTINGS:
      g_value_set_object (value, priv->settings);
      break;
    case PROP_SEARCH_MODE_ENABLED:
      g_value_set_boolean (value, priv->search_enabled);
      break;
    case PROP_FLOATING:
      g_value_set_boolean (value, priv->floating);
      break;
    case PROP_TRANSLUCENT:
      g_value_set_boolean (value, priv->translucent);
      break;
    case PROP_SETTINGS_VISIBLE:
      g_value_set_boolean (value, priv->settings_visible);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


static void
kgx_window_update_opaque_region (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  GdkSurface *surface;

  if (!gtk_widget_get_realized (GTK_WIDGET (self))) {
    return;
  }

  surface = gtk_native_get_surface (GTK_NATIVE (self));
  if (!surface) {
    return;
  }

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  {
    /* Always set empty opaque region — entire window is transparent */
    cairo_region_t *region = cairo_region_create ();
    gdk_surface_set_opaque_region (surface, region);
    cairo_region_destroy (region);
  }
  G_GNUC_END_IGNORE_DEPRECATIONS
}


static void
kgx_window_realize (GtkWidget *widget)
{
  KgxWindow *self = KGX_WINDOW (widget);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  GTK_WIDGET_CLASS (kgx_window_parent_class)->realize (widget);

  g_binding_group_set_source (priv->surface_binds,
                              gtk_native_get_surface (GTK_NATIVE (widget)));
  kgx_window_update_opaque_region (self);
}


static void
kgx_window_unrealize (GtkWidget *widget)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (KGX_WINDOW (widget));

  g_binding_group_set_source (priv->surface_binds, NULL);

  GTK_WIDGET_CLASS (kgx_window_parent_class)->unrealize (widget);
}


static void
kgx_window_css_changed (GtkWidget         *widget,
                        GtkCssStyleChange *change)
{
  GTK_WIDGET_CLASS (kgx_window_parent_class)->css_changed (widget, change);

  kgx_window_update_opaque_region (KGX_WINDOW (widget));
}


static void
got_close (GObject      *source,
           GAsyncResult *res,
           gpointer      user_data)
{
  g_autoptr (KgxCloseDialog) dialogue = KGX_CLOSE_DIALOG (source);
  g_autoptr (KgxWindow) self = user_data;
  g_autoptr (GError) error = NULL;
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  KgxCloseDialogResult result;

  result = kgx_close_dialog_run_finish (dialogue, res, &error);

  if (G_UNLIKELY (error)) {
    g_critical ("Unexpected: %s", error->message);
    return;
  }

  if (result == KGX_CLOSE_ANYWAY) {
    priv->close_anyway = TRUE;
    gtk_window_close (GTK_WINDOW (self));
  }
}


static gboolean
kgx_window_close_request (GtkWindow *window)
{
  KgxWindow *self = KGX_WINDOW (window);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  g_autoptr (GPtrArray) children = NULL;

  children = kgx_pages_get_children (KGX_PAGES (priv->pages));

  if (children->len < 1 || priv->close_anyway) {
    if (gtk_window_is_active (GTK_WINDOW (self)) &&
        !adw_application_window_get_adaptive_preview (ADW_APPLICATION_WINDOW (self))) {
      gboolean maximised = gtk_window_is_maximized (GTK_WINDOW (self));
      int width, height;

      gtk_window_get_default_size (GTK_WINDOW (self), &width, &height);
      kgx_settings_set_custom_size (priv->settings, width, height, maximised);
    }

    return FALSE; /* Aka no, I don’t want to block closing */
  }

  kgx_close_dialog_run (g_object_new (KGX_TYPE_CLOSE_DIALOG,
                                      "context", KGX_CONTEXT_WINDOW,
                                      "commands", children,
                                      NULL),
                        GTK_WIDGET (self),
                        NULL,
                        got_close,
                        g_object_ref (self));

  return TRUE; /* Block the close */
}


static void
fullscreened_changed (KgxWindow *self)
{
  gboolean fullscreen = gtk_window_is_fullscreen (GTK_WINDOW (self));
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.fullscreen", !fullscreen);
  gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.unfullscreen", fullscreen);
}



static void
kgx_window_update_chrome_opacity (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  double chrome_opacity = 1.0;
  g_autofree char *chrome_color = NULL;
  g_autofree char *accent_color = NULL;
  g_autofree char *css = NULL;
  GdkRGBA rgba;

  if (!priv->settings || !chrome_css) {
    return;
  }

  g_object_get (priv->settings,
                "chrome-opacity", &chrome_opacity,
                "chrome-color", &chrome_color,
                "accent-color", &accent_color,
                NULL);

  /* Parse the hex color, default to black if invalid */
  if (!chrome_color || !gdk_rgba_parse (&rgba, chrome_color)) {
    rgba = (GdkRGBA) { 0.0, 0.0, 0.0, 1.0 };
  }

  /* Use CSS background-color with alpha instead of widget opacity —
   * this keeps labels, buttons, and icons at full opacity. */
  {
    int r = (int)(rgba.red * 255);
    int g = (int)(rgba.green * 255);
    int b = (int)(rgba.blue * 255);
    double a = chrome_opacity;

    css = g_strdup_printf (
      /* Only the headerbar, settings-page, and scrollbar get the chrome
       * color — everything nested inside must be transparent so alpha
       * doesn't compound across layers when chrome_opacity < 1.0. */
      ".terminal-window headerbar,"
      ".terminal-window tabbar,"
      ".terminal-window searchbar,"
      ".terminal-window settings-page,"
      ".terminal-window scrollbar,"
      ".terminal-window scrollbar trough {"
      "  background-color: rgba(%d, %d, %d, %f);"
      "  background-image: none;"
      "  border-color: transparent;"
      "  box-shadow: none;"
      "}"
      /* Scrollbar slider — lighter for contrast */
      ".terminal-window scrollbar slider {"
      "  background-color: rgba(%d, %d, %d, %f);"
      "  border: none;"
      "  min-width: 6px;"
      "}"
      /* Internal children of tabbar/searchbar — transparent so they
       * don't compound alpha with their parent's background. */
      ".terminal-window tabbar tab,"
      ".terminal-window tabbar tab:checked,"
      ".terminal-window tabbar tab:hover,"
      ".terminal-window tabbar tab label,"
      ".terminal-window tabbar tab indicator,"
      ".terminal-window tabbar tabbox,"
      ".terminal-window tabbar > revealer,"
      ".terminal-window tabbar > revealer > widget,"
      ".terminal-window tabbar > revealer > widget > box,"
      ".terminal-window tabbar > revealer > widget > box > scrolledwindow,"
      ".terminal-window tabbar > revealer > widget > box > scrolledwindow > tabbox,"
      ".terminal-window searchbar > revealer,"
      ".terminal-window searchbar > revealer > box,"
      /* Search bar internals — entry, text area, and nav buttons */
      ".terminal-window searchbar entry,"
      ".terminal-window searchbar entry > text,"
      ".terminal-window searchbar entry:focus,"
      ".terminal-window searchbar entry:focus > text,"
      ".terminal-window searchbar button,"
      /* AdwToolbarView internal revealers wrapping the header area */
      ".terminal-window toolbarview > stack > revealer,"
      ".terminal-window toolbarview > stack > revealer > windowhandle,"
      ".terminal-window toolbarview > stack > revealer > windowhandle > box,"
      /* Settings page internals */
      ".terminal-window settings-page list,"
      ".terminal-window settings-page row,"
      ".terminal-window settings-page row:hover,"
      ".terminal-window settings-page row:active,"
      ".terminal-window settings-page scale trough,"
      ".terminal-window settings-page spinbutton,"
      ".terminal-window settings-page spinbutton button,"
      ".terminal-window settings-page preferencesgroup,"
      ".terminal-window settings-page preferencesgroup > box,"
      ".terminal-window settings-page preferencesgroup > box > box,"
      ".terminal-window settings-page preferencesgroup > box > list {"
      "  background-color: transparent;"
      "  background-image: none;"
      "  border-color: transparent;"
      "  box-shadow: none;"
      "}"
      /* Row separators — Adwaita uses border-bottom on rows */
      ".terminal-window settings-page list row,"
      ".terminal-window settings-page list row:not(:last-child) {"
      "  border-bottom-color: transparent;"
      "  border-color: transparent;"
      "}"
      /* Switch widget rows — also clear outline/focus ring artifacts */
      ".terminal-window settings-page switch,"
      ".terminal-window settings-page switch slider {"
      "  box-shadow: none;"
      "}"
      /* Settings page section titles — accent color */
      ".terminal-window settings-page .title-1,"
      ".terminal-window settings-page preferencesgroup > box > label,"
      ".terminal-window settings-page .title-4,"
      ".terminal-window settings-page .heading,"
      ".terminal-window settings-page button.link > label {"
      "  color: %s;"
      "}"
      /* Search entry border — accent color */
      ".terminal-window searchbar entry {"
      "  border-color: %s;"
      "}"
      /* Switch active state — accent color */
      ".terminal-window settings-page switch:checked {"
      "  background-color: %s;"
      "  background-image: none;"
      "}"
      /* Shortcut key badges — accent color border */
      ".terminal-window settings-page .keycap {"
      "  background-color: transparent;"
      "  background-image: none;"
      "  border-color: %s;"
      "  box-shadow: none;"
      "}",
      r, g, b, a,
      CLAMP (r + 100, 0, 255), CLAMP (g + 100, 0, 255), CLAMP (b + 100, 0, 255), 0.5,
      accent_color && accent_color[0] ? accent_color : "#3584e4",
      accent_color && accent_color[0] ? accent_color : "#3584e4",
      accent_color && accent_color[0] ? accent_color : "#3584e4",
      accent_color && accent_color[0] ? accent_color : "#3584e4");
  }

  /* Apply accent color override */
  if (accent_color && accent_color[0] != '\0') {
    GdkRGBA accent_rgba;
    if (gdk_rgba_parse (&accent_rgba, accent_color)) {
      g_autofree char *accent_css = g_strdup_printf (
        "@define-color accent_bg_color %s;"
        "@define-color accent_color %s;"
        "@define-color accent_fg_color white;"
        ".terminal-window, .terminal-window * {"
        "  --accent-bg-color: %s;"
        "  --accent-color: %s;"
        "  --accent-fg-color: white;"
        "}", accent_color, accent_color, accent_color, accent_color);
      g_autofree char *combined = g_strconcat (css, accent_css, NULL);
      gtk_css_provider_load_from_string (chrome_css, combined);
    } else {
      gtk_css_provider_load_from_string (chrome_css, css);
    }
  } else {
    gtk_css_provider_load_from_string (chrome_css, css);
  }
}


static void
chrome_opacity_changed (G_GNUC_UNUSED GObject    *object,
                        G_GNUC_UNUSED GParamSpec *pspec,
                        KgxWindow                *self)
{
  kgx_window_update_chrome_opacity (self);
}


static void
zoom (KgxPages  *pages,
      KgxZoom    dir,
      KgxWindow *self)
{
  GAction *action = NULL;
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));

  switch (dir) {
    case KGX_ZOOM_IN:
      action = g_action_map_lookup_action (G_ACTION_MAP (app), "zoom-in");
      break;
    case KGX_ZOOM_OUT:
      action = g_action_map_lookup_action (G_ACTION_MAP (app), "zoom-out");
      break;
    default:
      g_return_if_reached ();
  }

  g_action_activate (action, NULL);
}


static KgxPages *
create_tearoff_host (KgxPages *pages, KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  GtkApplication *application = gtk_window_get_application (GTK_WINDOW (self));
  KgxWindow *new_window;
  KgxWindowPrivate *new_priv;
  int width, height;

  gtk_window_get_default_size (GTK_WINDOW (self), &width, &height);

  new_window = g_object_new (KGX_TYPE_WINDOW,
                             "application", application,
                             "settings", priv->settings,
                             "default-width", width,
                             "default-height", height,
                             NULL);
  gtk_window_present (GTK_WINDOW (new_window));

  new_priv = kgx_window_get_instance_private (new_window);

  return KGX_PAGES (new_priv->pages);
}


static void
maybe_close_window (KgxWindow *self)
{
  gtk_window_close (GTK_WINDOW (self));
}


static void
status_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  KgxStatus status;

  status = kgx_pages_current_status (KGX_PAGES (priv->pages));

  if (status & KGX_PLAYBOX) {
    gtk_widget_add_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_PLAYBOX);
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_PLAYBOX);
  }

  if (status & KGX_REMOTE) {
    gtk_widget_add_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_REMOTE);
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_REMOTE);
  }

  if (status & KGX_PRIVILEGED) {
    gtk_widget_add_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_ROOT);
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_ROOT);
  }
}


static void
ringing_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  if (kgx_pages_is_ringing (KGX_PAGES (priv->pages))) {
    gtk_widget_add_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_RINGING);
  } else {
    gtk_widget_remove_css_class (GTK_WIDGET (self), KGX_WINDOW_STYLE_RINGING);
  }
}



static void
extra_drag_drop (AdwTabBar        *bar,
                 AdwTabPage       *page,
                 GValue           *value,
                 KgxWindow        *self)
{
  KgxTab *tab = KGX_TAB (adw_tab_page_get_child (page));

  kgx_tab_extra_drop (tab, value);
}



static VteTerminal *
get_active_terminal (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  AdwTabPage *page;
  KgxTab *tab;
  KgxTerminal *terminal = NULL;

  page = kgx_pages_get_selected_page (KGX_PAGES (priv->pages));
  if (!page) {
    return NULL;
  }

  tab = KGX_TAB (adw_tab_page_get_child (page));
  g_object_get (tab, "terminal", &terminal, NULL);

  if (!terminal) {
    return NULL;
  }

  /* g_object_get returns a ref, drop it — terminal is owned by the tab */
  g_object_unref (terminal);

  return VTE_TERMINAL (terminal);
}


static void
search_enabled (GObject    *object,
                GParamSpec *pspec,
                KgxWindow  *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  if (!gtk_search_bar_get_search_mode (GTK_SEARCH_BAR (priv->search_bar))) {
    gtk_widget_grab_focus (GTK_WIDGET (priv->pages));
  }
}


static void
search_changed (GtkSearchEntry *entry,
                KgxWindow      *self)
{
  VteTerminal *terminal;
  const char *search = NULL;
  VteRegex *regex;
  g_autoptr (GError) error = NULL;
  guint32 flags = PCRE2_MULTILINE;

  terminal = get_active_terminal (self);
  if (!terminal) {
    return;
  }

  search = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (search) {
    g_autofree char *lowercase = g_utf8_strdown (search, -1);

    if (!g_strcmp0 (lowercase, search))
      flags |= PCRE2_CASELESS;
  }

  regex = vte_regex_new_for_search (g_regex_escape_string (search, -1),
                                    -1, flags, &error);

  if (error) {
    g_warning ("Search error: %s", error->message);
    return;
  }

  vte_terminal_search_find_previous (terminal);

  vte_terminal_search_set_regex (terminal, regex, 0);

  vte_terminal_search_find_next (terminal);
}


static void
search_next (GtkWidget *widget,
             KgxWindow *self)
{
  VteTerminal *terminal = get_active_terminal (self);

  if (terminal) {
    vte_terminal_search_find_next (terminal);
  }
}


static void
search_prev (GtkWidget *widget,
             KgxWindow *self)
{
  VteTerminal *terminal = get_active_terminal (self);

  if (terminal) {
    vte_terminal_search_find_previous (terminal);
  }
}


static void
close_tab_activated (GtkWidget  *widget,
                     const char *action_name,
                     GVariant   *parameter)
{
  KgxWindow *self = KGX_WINDOW (widget);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  kgx_pages_close_page (KGX_PAGES (priv->pages));
}


static void
detach_tab_activated (GtkWidget  *widget,
                      const char *action_name,
                      GVariant   *parameter)
{
  KgxWindow *self = KGX_WINDOW (widget);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  kgx_pages_detach_page (KGX_PAGES (priv->pages));
}


static void
new_activated (GtkWidget  *widget,
               const char *action_name,
               GVariant   *parameter)
{
  KgxWindow *self = KGX_WINDOW (widget);
  GtkApplication *application = NULL;
  g_autoptr (GFile) dir = NULL;

  application = gtk_window_get_application (GTK_WINDOW (self));
  dir = kgx_window_get_working_dir (self);

  kgx_application_add_terminal (KGX_APPLICATION (application),
                                NULL,
                                dir,
                                NULL,
                                NULL);
}


static void
new_tab_activated (GtkWidget  *widget,
                   const char *action_name,
                   GVariant   *parameter)
{
  KgxWindow *self = KGX_WINDOW (widget);
  GtkApplication *application = NULL;
  g_autoptr (GFile) dir = NULL;

  application = gtk_window_get_application (GTK_WINDOW (self));
  dir = kgx_window_get_working_dir (self);

  kgx_application_add_terminal (KGX_APPLICATION (application),
                                self,
                                dir,
                                NULL,
                                NULL);
}






static void
kgx_window_class_init (KgxWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkWindowClass *window_class = GTK_WINDOW_CLASS (klass);

  object_class->dispose = kgx_window_dispose;
  object_class->set_property = kgx_window_set_property;
  object_class->get_property = kgx_window_get_property;

  widget_class->css_changed = kgx_window_css_changed;
  widget_class->realize = kgx_window_realize;
  widget_class->unrealize = kgx_window_unrealize;
  window_class->close_request = kgx_window_close_request;

  pspecs[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         KGX_TYPE_SETTINGS,
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);

  pspecs[PROP_SEARCH_MODE_ENABLED] =
    g_param_spec_boolean ("search-mode-enabled", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_FLOATING] =
    g_param_spec_boolean ("floating", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_TRANSLUCENT] =
    g_param_spec_boolean ("translucent", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  pspecs[PROP_SETTINGS_VISIBLE] =
    g_param_spec_boolean ("settings-visible", NULL, NULL,
                          FALSE,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, LAST_PROP, pspecs);

  gtk_widget_class_install_property_action (widget_class,
                                            "win.find",
                                            "search-mode-enabled");
  gtk_widget_class_install_property_action (widget_class,
                                            "win.toggle-settings",
                                            "settings-visible");

  g_type_ensure (KGX_TYPE_EMPTY);
  g_type_ensure (KGX_TYPE_FULLSCREEN_BOX);
  g_type_ensure (KGX_TYPE_PAGES);
  g_type_ensure (KGX_TYPE_SETTINGS_PAGE);
  gtk_widget_class_set_template_from_resource (widget_class,
                                               KGX_APPLICATION_PATH "kgx-window.ui");

  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, header_bar);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, tab_bar);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, pages);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, search_bar);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, content_stack);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, settings_page);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, settings_binds);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, surface_binds);

  gtk_widget_class_bind_template_callback (widget_class, fullscreened_changed);
  gtk_widget_class_bind_template_callback (widget_class, zoom);
  gtk_widget_class_bind_template_callback (widget_class, create_tearoff_host);
  gtk_widget_class_bind_template_callback (widget_class, maybe_close_window);
  gtk_widget_class_bind_template_callback (widget_class, status_changed);
  gtk_widget_class_bind_template_callback (widget_class, ringing_changed);
  gtk_widget_class_bind_template_callback (widget_class, extra_drag_drop);
  gtk_widget_class_bind_template_callback (widget_class, search_enabled);
  gtk_widget_class_bind_template_callback (widget_class, search_changed);
  gtk_widget_class_bind_template_callback (widget_class, search_next);
  gtk_widget_class_bind_template_callback (widget_class, search_prev);

  gtk_widget_class_bind_template_callback (widget_class, kgx_gtk_settings_for_display);
  gtk_widget_class_bind_template_callback (widget_class, kgx_text_or_fallback);
  gtk_widget_class_bind_template_callback (widget_class, kgx_bool_and);
  gtk_widget_class_bind_template_callback (widget_class, kgx_decoration_layout_is_inverted);
  gtk_widget_class_bind_template_callback (widget_class, kgx_file_as_subtitle);

  gtk_widget_class_install_action (widget_class, "tab.close", NULL, close_tab_activated);
  gtk_widget_class_install_action (widget_class, "tab.detach", NULL, detach_tab_activated);

  gtk_widget_class_install_action (widget_class,
                                   "win.new-window",
                                   NULL,
                                   new_activated);
  gtk_widget_class_install_action (widget_class,
                                   "win.new-tab",
                                   NULL,
                                   new_tab_activated);
  gtk_widget_class_install_action (widget_class,
                                   "win.close-tab",
                                   NULL,
                                   close_tab_activated);
  gtk_widget_class_install_action (widget_class,
                                   "win.fullscreen",
                                   NULL,
                                   (GtkWidgetActionActivateFunc) gtk_window_fullscreen);
  gtk_widget_class_install_action (widget_class,
                                   "win.unfullscreen",
                                   NULL,
                                   (GtkWidgetActionActivateFunc) gtk_window_unfullscreen);
}


static gboolean
state_to_floating (GBinding     *binding,
                   const GValue *from_value,
                   GValue       *to_value,
                   gpointer      user_data)
{
  GdkToplevelState state = g_value_get_flags (from_value);

  g_value_set_boolean (to_value,
                       !((state & (GDK_TOPLEVEL_STATE_FULLSCREEN |
                                   GDK_TOPLEVEL_STATE_MAXIMIZED |
                                   GDK_TOPLEVEL_STATE_TILED |
                                   GDK_TOPLEVEL_STATE_TOP_TILED |
                                   GDK_TOPLEVEL_STATE_RIGHT_TILED |
                                   GDK_TOPLEVEL_STATE_BOTTOM_TILED |
                                   GDK_TOPLEVEL_STATE_LEFT_TILED)) > 0));

  return TRUE;
}


static void
kgx_window_init (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  GType drop_types[] = { GDK_TYPE_FILE_LIST, G_TYPE_STRING };
  g_autoptr (GtkWindowGroup) group = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Always translucent */
  priv->translucent = TRUE;
  gtk_widget_add_css_class (GTK_WIDGET (self), "translucent");

  /* Shared CSS provider for chrome styling — create once */
  if (!chrome_css) {
    chrome_css = gtk_css_provider_new ();
    gtk_style_context_add_provider_for_display (
      gdk_display_get_default (),
      GTK_STYLE_PROVIDER (chrome_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }

  g_binding_group_bind_full (priv->surface_binds, "state",
                             self, "floating",
                             G_BINDING_SYNC_CREATE,
                             state_to_floating, NULL, NULL, NULL);

  #ifdef IS_DEVEL
  gtk_widget_add_css_class (GTK_WIDGET (self), "devel");
  #endif

  /* Note this unfortunately doesn't allow us to workaround the portal
     situation, but hopefully dropping folders on tabs is relatively rare */
  adw_tab_bar_setup_extra_drop_target (ADW_TAB_BAR (priv->tab_bar),
                                       GDK_ACTION_COPY,
                                       drop_types,
                                       G_N_ELEMENTS (drop_types));

  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (priv->search_bar),
                                GTK_EDITABLE (priv->search_entry));

  fullscreened_changed (self);

  group = gtk_window_group_new ();
  gtk_window_group_add_window (group, GTK_WINDOW (self));
}


/**
 * kgx_window_get_working_dir:
 * @self: the #KgxWindow
 *
 * Get the working directory path of this window, used to open new windows
 * in the same directory
 *
 * Returns: (transfer full):
 */
GFile *
kgx_window_get_working_dir (KgxWindow *self)
{
  KgxWindowPrivate *priv;
  GFile *file = NULL;

  g_return_val_if_fail (KGX_IS_WINDOW (self), NULL);

  priv = kgx_window_get_instance_private (self);

  g_object_get (priv->pages, "path", &file, NULL);

  return file;
}


/**
 * kgx_window_add_tab:
 * @self: the #KgxWindow
 * @tab: (transfer none): a #KgxTab
 *
 * Adopt a (currently unowned) tab into @self, and present it
 */
void
kgx_window_add_tab (KgxWindow *self,
                    KgxTab    *tab)
{
  KgxWindowPrivate *priv;

  g_return_if_fail (KGX_IS_WINDOW (self));
  g_return_if_fail (KGX_IS_TAB (tab));

  priv = kgx_window_get_instance_private (self);

  kgx_pages_add_page (KGX_PAGES (priv->pages), tab);

  /* Only focus immediately if this is the first tab (nothing else to show).
   * Otherwise, defer focus until the terminal spawns to avoid a blank flash. */
  if (kgx_pages_count (KGX_PAGES (priv->pages)) <= 1) {
    kgx_pages_focus_page (KGX_PAGES (priv->pages), tab);
  }
}
