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

#include "kgx-edge.h"
#include "kgx-process.h"
#include "kgx-terminal.h"
#include "kgx-train.h"
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
  GtkWidget            *search_revealer;
  GtkWidget            *search_entry;
  GtkWidget            *content_stack;
  GtkWidget            *settings_page;
  KgxEdge              *edge;
  char                 *process_glass_override;
  gboolean              process_glass_force_reconcile;
  guint                 process_glass_idle;
  gboolean              process_glass_idle_deferred;
  GtkCssProvider       *override_css;
  char                 *window_css_class;
  char                 *override_css_selectors; /* pre-built CSS selector list */
  int                   override_last_r;       /* last applied RGB to skip no-ops */
  int                   override_last_g;
  int                   override_last_b;

  /* Unified glass transition — drives both VTE bg and CSS glass together. */
  AdwAnimation         *glass_transition;
  GdkRGBA               glass_from;
  GdkRGBA               glass_to;
  GdkRGBA               glass_current;
  gboolean              glass_has_current;

  /* Cached terminal for glass transition — avoids g_object_get per frame */
  KgxTerminal          *glass_cached_terminal;  /* weak pointer */
  double                glass_opacity_cached;   /* from settings at transition start */

  /* Deferred particle — fires after glass transition completes */
  gboolean              deferred_particle;
  KgxParticlePreset     deferred_preset;
  GdkRGBA               deferred_color;
  int                   deferred_reverse;
  int                   deferred_shape;
  int                   deferred_gap;
  int                   deferred_speed;
  int                   deferred_thk;

  GSignalGroup         *tab_signals;
  GSignalGroup         *train_signals;

  GBindingGroup        *surface_binds;
};


G_DEFINE_TYPE_WITH_PRIVATE (KgxWindow, kgx_window, ADW_TYPE_APPLICATION_WINDOW)

/* Shared CSS provider for glass styling — lives for the process lifetime */
static GtkCssProvider *glass_css = NULL;


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
static void kgx_window_update_glass_opacity (KgxWindow *self);
static void glass_opacity_changed (GObject *object, GParamSpec *pspec, KgxWindow *self);
static void process_glass_settings_changed (GObject *object, GParamSpec *pspec, KgxWindow *self);
static gboolean update_process_glass (KgxWindow *self);
static gboolean update_process_glass_idle (gpointer data);
static void schedule_process_glass_idle_full (KgxWindow *self,
                                              gboolean   deferred);
static void schedule_process_glass_idle (KgxWindow *self);
static void schedule_process_glass_idle_deferred (KgxWindow *self);
static gboolean sync_active_train_signals (KgxWindow *self,
                                           KgxTab    *active);
static void sync_active_page_state (KgxWindow *self);

#define PROCESS_GLASS_TAB_SWITCH_DELAY_MS 16


static void
kgx_window_dispose (GObject *object)
{
  KgxWindow *self = KGX_WINDOW (object);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  /* Disconnect binding/signal groups — prevents callbacks during teardown.
   * settings_binds and surface_binds are template children (owned by the
   * template system) — disconnect but do NOT g_clear_object them. */
  if (priv->settings_binds) {
    g_binding_group_set_source (priv->settings_binds, NULL);
  }
  if (priv->surface_binds) {
    g_binding_group_set_source (priv->surface_binds, NULL);
  }
  if (priv->tab_signals) {
    g_signal_group_set_target (priv->tab_signals, NULL);
  }
  if (priv->train_signals) {
    g_signal_group_set_target (priv->train_signals, NULL);
  }

  g_clear_handle_id (&priv->process_glass_idle, g_source_remove);
  /* glass_css is a process-lifetime singleton — don't clear it */
  if (priv->override_css) {
    gtk_style_context_remove_provider_for_display (
      gdk_display_get_default (),
      GTK_STYLE_PROVIDER (priv->override_css));
    g_clear_object (&priv->override_css);
  }
  g_clear_pointer (&priv->window_css_class, g_free);
  g_clear_pointer (&priv->override_css_selectors, g_free);
  g_clear_pointer (&priv->process_glass_override, g_free);

  g_clear_weak_pointer (&priv->glass_cached_terminal);

  if (priv->glass_transition) {
    adw_animation_reset (priv->glass_transition);
    g_clear_object (&priv->glass_transition);
  }

  /* tab_signals is created in init (not a template child) — safe to clear */
  g_clear_object (&priv->tab_signals);
  g_clear_object (&priv->train_signals);
  g_clear_object (&priv->settings);

  /* Stop all edge animations before parent disposes the widget tree. */
  if (priv->edge) {
    kgx_edge_stop_process_particle_immediate (priv->edge);
    kgx_edge_set_ambient (priv->edge, FALSE);
  }

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
                                 "notify::glass-opacity",
                                 G_CALLBACK (glass_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        g_signal_connect_object (priv->settings,
                                 "notify::glass-color",
                                 G_CALLBACK (glass_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        g_signal_connect_object (priv->settings,
                                 "notify::accent-color",
                                 G_CALLBACK (glass_opacity_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        g_signal_connect_object (priv->settings,
                                 "notify::process-glass-colors",
                                 G_CALLBACK (process_glass_settings_changed),
                                 self,
                                 G_CONNECT_DEFAULT);
        kgx_window_update_glass_opacity (self);
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
        if (priv->settings_visible) {
          g_clear_handle_id (&priv->process_glass_idle, g_source_remove);
          priv->process_glass_idle_deferred = FALSE;
        }
        if (priv->content_stack) {
          /* Lazily create the settings page on first use — avoids
           * icon lookup warnings during initial window show. */
          if (priv->settings_visible && !priv->settings_page) {
            priv->settings_page = g_object_new (KGX_TYPE_SETTINGS_PAGE,
                                                "settings", priv->settings,
                                                NULL);
            gtk_stack_add_named (GTK_STACK (priv->content_stack),
                                priv->settings_page, "settings");
          }
          gtk_stack_set_visible_child_name (
            GTK_STACK (priv->content_stack),
            priv->settings_visible ? "settings" : "terminal");
        }
        if (!priv->settings_visible && priv->pages) {
          gtk_widget_grab_focus (priv->pages);
        }
        /* Toggle ambient bursts with settings page visibility. */
        kgx_edge_set_ambient (priv->edge, priv->settings_visible);

        /* Leaving settings should not inherit governor slowdown from the
         * ambient/settings run, and the re-check can wait until the stack
         * transition back to terminal has started to settle. */
        if (!priv->settings_visible) {
          kgx_edge_stop_ambient_immediate (priv->edge);
          kgx_edge_reset_redraw_governor (priv->edge);
          schedule_process_glass_idle_deferred (KGX_WINDOW (object));
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
kgx_window_update_glass_opacity (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  double glass_opacity = 1.0;
  g_autofree char *glass_color = NULL;
  g_autofree char *accent_color_raw = NULL;
  g_autofree char *css = NULL;
  GdkRGBA rgba;
  GdkRGBA accent_rgba = { 53 / 255.0, 132 / 255.0, 228 / 255.0, 1.0 }; /* #3584e4 */
  char accent_safe[64];

  if (!priv->settings || !glass_css) {
    return;
  }

  g_object_get (priv->settings,
                "glass-opacity", &glass_opacity,
                "glass-color", &glass_color,
                "accent-color", &accent_color_raw,
                NULL);

  /* Parse the hex color, default to black if invalid */
  if (!glass_color || !gdk_rgba_parse (&rgba, glass_color)) {
    rgba = (GdkRGBA) { 0.0, 0.0, 0.0, 1.0 };
  }

  /* Parse accent color into safe rgb() string to prevent CSS injection */
  if (accent_color_raw && accent_color_raw[0] &&
      gdk_rgba_parse (&accent_rgba, accent_color_raw)) {
    /* parsed successfully */
  }
  snprintf (accent_safe, sizeof (accent_safe), "rgb(%d,%d,%d)",
            (int)(accent_rgba.red * 255),
            (int)(accent_rgba.green * 255),
            (int)(accent_rgba.blue * 255));

  /* Use CSS background-color with alpha instead of widget opacity —
   * this keeps labels, buttons, and icons at full opacity. */
  {
    int r = (int)(rgba.red * 255);
    int g = (int)(rgba.green * 255);
    int b = (int)(rgba.blue * 255);
    double a = glass_opacity;

    css = g_strdup_printf (
      /* Only the headerbar, settings-page, and scrollbar get the glass
       * color — everything nested inside must be transparent so alpha
       * doesn't compound across layers when glass_opacity < 1.0. */
      ".terminal-window headerbar,"
      ".terminal-window tabbar,"
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
      /* Search entry in headerbar — transparent so alpha doesn't compound */
      ".terminal-window headerbar entry,"
      ".terminal-window headerbar entry > text,"
      ".terminal-window headerbar entry:focus,"
      ".terminal-window headerbar entry:focus > text,"
      ".terminal-window headerbar revealer button,"
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
      /* Prevent negative min-width from cleared borders/padding */
      ".terminal-window tabbar tabboxchild,"
      ".terminal-window settings-page scale marks {"
      "  min-width: 0;"
      "  min-height: 0;"
      "}"
      /* Settings page section titles — accent color */
      ".terminal-window settings-page .title-1,"
      ".terminal-window settings-page preferencesgroup > box > label,"
      ".terminal-window settings-page .title-4,"
      ".terminal-window settings-page .heading,"
      ".terminal-window settings-page button.link > label,"
      ".terminal-window settings-page .about-accent {"
      "  color: %s;"
      "}"
      /* Search entry — accent color at 50%% opacity, toned-down focus */
      ".terminal-window headerbar entry {"
      "  border-color: color-mix(in srgb, %s 50%%, transparent);"
      "}"
      ".terminal-window headerbar entry:focus-within {"
      "  outline-color: color-mix(in srgb, %s 30%%, transparent);"
      "  border-color: color-mix(in srgb, %s 60%%, transparent);"
      "}"
      /* Switch active state — accent color */
      ".terminal-window settings-page switch:checked {"
      "  background-color: %s;"
      "  background-image: none;"
      "}"
      /* Tab close button hover — accent color at 85%% opacity */
      ".terminal-window tabbar button.image-button:hover {"
      "  background-color: color-mix(in srgb, %s 85%%, transparent);"
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
      accent_safe, accent_safe, accent_safe, accent_safe,
      accent_safe, accent_safe, accent_safe);
  }

  /* Apply accent color as CSS custom properties */
  {
    g_autofree char *accent_css = g_strdup_printf (
      "@define-color accent_bg_color %s;"
      "@define-color accent_color %s;"
      "@define-color accent_fg_color white;"
      ".terminal-window, .terminal-window * {"
      "  --accent-bg-color: %s;"
      "  --accent-color: %s;"
      "  --accent-fg-color: white;"
      "}", accent_safe, accent_safe, accent_safe, accent_safe);
    g_autofree char *combined = g_strconcat (css, accent_css, NULL);
    gtk_css_provider_load_from_string (glass_css, combined);
  }
}


static void
glass_opacity_changed (G_GNUC_UNUSED GObject    *object,
                        G_GNUC_UNUSED GParamSpec *pspec,
                        KgxWindow                *self)
{
  kgx_window_update_glass_opacity (self);
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


static gboolean update_process_glass (KgxWindow *self);


static void
copy_opaque_rgba (GdkRGBA       *dest,
                  const GdkRGBA *src)
{
  *dest = *src;
  dest->alpha = 1.0f;
}


static gboolean
same_opaque_rgb (const GdkRGBA *a,
                 const GdkRGBA *b)
{
  return a->red == b->red &&
         a->green == b->green &&
         a->blue == b->blue;
}


static void
apply_glass_color (KgxWindow     *self,
                    const GdkRGBA *color)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  int r = (int)(color->red * 255.0 + 0.5);
  int g = (int)(color->green * 255.0 + 0.5);
  int b = (int)(color->blue * 255.0 + 0.5);

  /* Update CSS glass every frame — skip only if RGB unchanged. */
  if (priv->override_css && priv->override_css_selectors &&
      (r != priv->override_last_r ||
       g != priv->override_last_g ||
       b != priv->override_last_b)) {
    double opacity = priv->glass_opacity_cached;
    char buf[512];
    snprintf (buf, sizeof (buf),
              "%s { background-color: rgba(%d, %d, %d, %f); }",
              priv->override_css_selectors, r, g, b, opacity);
    gtk_css_provider_load_from_string (priv->override_css, buf);
    priv->override_last_r = r;
    priv->override_last_g = g;
    priv->override_last_b = b;
  }

  /* Update VTE terminal background using the cached pointer from
   * start_glass_transition — avoids two g_object_get per frame. */
  if (priv->glass_cached_terminal)
    kgx_terminal_apply_bg_immediate (priv->glass_cached_terminal, color);
}


static void
glass_lerp_cb (double    value,
                KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  GdkRGBA c;

  /* Guard against callbacks during dispose. */
  if (!priv->pages)
    return;

  c.red   = priv->glass_from.red   + (priv->glass_to.red   - priv->glass_from.red)   * value;
  c.green = priv->glass_from.green + (priv->glass_to.green - priv->glass_from.green) * value;
  c.blue  = priv->glass_from.blue  + (priv->glass_to.blue  - priv->glass_from.blue)  * value;
  c.alpha = 1.0f;

  priv->glass_current = c;

  apply_glass_color (self, &c);
}


static void
glass_transition_done_cb (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  g_clear_weak_pointer (&priv->glass_cached_terminal);

  /* Reset dedup so the next transition's first frame always updates CSS. */
  priv->override_last_r = -1;
  priv->override_last_g = -1;
  priv->override_last_b = -1;

  if (priv->deferred_particle) {
    priv->deferred_particle = FALSE;
    priv->deferred_color.alpha = 1.0f;
    kgx_edge_set_process_particle (priv->edge,
                                   priv->deferred_preset,
                                   &priv->deferred_color,
                                   priv->deferred_reverse,
                                   priv->deferred_shape,
                                   priv->deferred_gap,
                                   priv->deferred_speed,
                                   priv->deferred_thk);
  }
}

static void
start_glass_transition (KgxWindow     *self,
                         const GdkRGBA *target)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  priv->glass_from = priv->glass_current;
  priv->glass_to = *target;

  /* Cache glass opacity from settings at transition start. */
  priv->glass_opacity_cached = 0.94;
  if (priv->settings)
    g_object_get (priv->settings, "glass-opacity", &priv->glass_opacity_cached, NULL);

  /* Cache the active terminal for the duration of the transition
   * so apply_glass_color doesn't need g_object_get per frame. */
  g_clear_weak_pointer (&priv->glass_cached_terminal);
  {
    g_autoptr (KgxTab) active = NULL;
    g_object_get (priv->pages, "active-page", &active, NULL);
    if (active) {
      g_autoptr (KgxTerminal) terminal = NULL;
      g_object_get (active, "terminal", &terminal, NULL);
      if (terminal)
        g_set_weak_pointer (&priv->glass_cached_terminal, terminal);
    }
  }

  if (!priv->glass_transition) {
    AdwAnimationTarget *t = adw_callback_animation_target_new (
        (AdwAnimationTargetFunc) glass_lerp_cb, self, NULL);
    priv->glass_transition = adw_timed_animation_new (GTK_WIDGET (self),
                                                        0.0, 1.0, 200, t);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (priv->glass_transition),
                                    ADW_EASE_IN_OUT_CUBIC);
    g_signal_connect_swapped (priv->glass_transition, "done",
                              G_CALLBACK (glass_transition_done_cb), self);
  }

  /* Force CSS update on the first frame of every transition. */
  priv->override_last_r = -1;
  priv->override_last_g = -1;
  priv->override_last_b = -1;

  adw_animation_reset (priv->glass_transition);
  adw_animation_play (priv->glass_transition);
}


static GdkRGBA
get_default_glass_color (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  GdkRGBA def = { 0.106f, 0.106f, 0.122f, 1.0f }; /* fallback #1b1b1f */
  g_autofree char *hex = NULL;

  if (priv->settings) {
    g_object_get (priv->settings, "glass-color", &hex, NULL);
    if (hex)
      gdk_rgba_parse (&def, hex);
  }

  def.alpha = 1.0f;
  return def;
}


static void
process_glass_settings_changed (GObject    *object,
                                GParamSpec *pspec,
                                KgxWindow  *self)
{
  schedule_process_glass_idle (self);
}


static gboolean
update_process_glass (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  g_autoptr (KgxTab) active = NULL;
  g_autoptr (KgxSettings) settings = NULL;
  const char *match = NULL;
  GdkRGBA target;

  g_object_get (priv->pages, "active-page", &active, NULL);
  g_object_get (self, "settings", &settings, NULL);

  if (active && settings && kgx_settings_has_process_colors (settings)) {
    g_autoptr (KgxTrain) train = NULL;
    g_object_get (active, "train", &train, NULL);

    if (train) {
      if (kgx_train_get_child_count (train) > 0) {
        g_autoptr (GPtrArray) children = kgx_train_get_children (train);

        for (int i = children->len - 1; i >= 0 && !match; i--) {
          KgxProcess *proc = g_ptr_array_index (children, i);
          const char *name = kgx_process_get_name (proc);

          if (name && name[0] != '\0')
            match = kgx_settings_lookup_process_color (settings, name);
        }
      }
    }
  }

  /* Parse extended config: "glasscolor;preset;reverse;particlecolor" */
  {
    g_autofree char *glass_hex = NULL;
    KgxParticlePreset preset = KGX_PARTICLE_NONE;
    int reverse = 0;
    GdkRGBA particle_color = { 0.5f, 0.5f, 0.5f, 1.0f };
    int shape_override = -1, gap_override = -1, speed_override = 0, thk_override = 0;
    if (match) {
      kgx_parse_process_config (match, &glass_hex, &preset, &reverse,
                                &particle_color, &shape_override, &gap_override,
                                &speed_override, &thk_override);
    }

    /* Determine target glass color. */
    if (glass_hex && gdk_rgba_parse (&target, glass_hex)) {
      target.alpha = 1.0f;
    } else {
      target = get_default_glass_color (self);
      if (preset == KGX_PARTICLE_NONE)
        match = NULL;
    }

    /* Nothing changed — don't interrupt running particle animations.
     * The timer and idle callbacks re-enter here frequently; without
     * this guard each re-entry kills and restarts the particle, which
     * flips the alternating-mode toggle mid-animation. */
    if (g_strcmp0 (match, priv->process_glass_override) == 0 &&
        !priv->process_glass_force_reconcile)
      return G_SOURCE_REMOVE;

    /* Defer particle until glass transition completes. The previous
     * active tab's process preset is stopped during the active-page
     * handoff, so this path only decides what to start next. */
    if (preset != KGX_PARTICLE_NONE) {
      priv->deferred_particle = TRUE;
      priv->deferred_preset = preset;
      priv->deferred_color = particle_color;
      priv->deferred_color.alpha = 1.0f;
      priv->deferred_reverse = reverse;
      priv->deferred_shape = shape_override;
      priv->deferred_gap = gap_override;
      priv->deferred_speed = speed_override;
      priv->deferred_thk = thk_override;
    } else {
      priv->deferred_particle = FALSE;
    }
  }

  g_free (priv->process_glass_override);
  priv->process_glass_override = match ? g_strdup (match) : NULL;
  priv->process_glass_force_reconcile = FALSE;

  /* Seed glass_current on first call so transitions have a start point. */
  if (!priv->glass_has_current) {
    priv->glass_current = get_default_glass_color (self);
    priv->glass_has_current = TRUE;
  }

  /* Update the terminal's override string so apply_palette knows. */
  {
    g_autoptr (KgxTerminal) terminal = NULL;
    if (active)
      g_object_get (active, "terminal", &terminal, NULL);
    if (terminal)
      kgx_terminal_set_process_override (terminal, match);
  }

  /* Skip if already at target — no glass transition needed, so fire
   * the deferred particle immediately (it would never get fired by
   * glass_transition_done_cb since no transition starts). */
  if (priv->glass_current.red   == target.red   &&
      priv->glass_current.green == target.green &&
      priv->glass_current.blue  == target.blue) {
    if (priv->deferred_particle) {
      priv->deferred_particle = FALSE;
      kgx_edge_set_process_particle (priv->edge,
                                     priv->deferred_preset,
                                     &priv->deferred_color,
                                     priv->deferred_reverse,
                                     priv->deferred_shape,
                                     priv->deferred_gap,
                                     priv->deferred_speed,
                                     priv->deferred_thk);
    }
    return G_SOURCE_REMOVE;
  }

  /* Animate both VTE and CSS glass together.
   * glass_transition_done_cb fires the deferred particle on completion. */
  start_glass_transition (self, &target);

  return G_SOURCE_REMOVE;
}


static void
tab_train_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  KgxWindow *self = KGX_WINDOW (data);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  g_autoptr (KgxTab) active = NULL;

  g_object_get (priv->pages, "active-page", &active, NULL);

  /* Only react here when the active tab's train object actually changed,
   * then retarget the direct train signal group. Child churn is handled
   * by direct KgxTrain signals. */
  if (active == KGX_TAB (object) &&
      sync_active_train_signals (self, active))
    schedule_process_glass_idle (self);
}


static void
active_train_children_changed (KgxTrain   *train,
                               KgxProcess *child,
                               gpointer    data)
{
  KgxWindow *self = KGX_WINDOW (data);

  /* Children changed on the active train — re-check process glass.
   * This catches name-based glass matches (e.g. pwsh, htop) without
   * routing the window through synthetic notify::train churn. */
  schedule_process_glass_idle (self);
}


static gboolean
update_process_glass_idle (gpointer data)
{
  KgxWindow *self = KGX_WINDOW (data);
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  priv->process_glass_idle = 0;
  priv->process_glass_idle_deferred = FALSE;
  update_process_glass (self);

  return G_SOURCE_REMOVE;
}


static void
schedule_process_glass_idle_full (KgxWindow *self,
                                  gboolean   deferred)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  if (priv->process_glass_idle != 0) {
    if (deferred || !priv->process_glass_idle_deferred)
      return;

    g_clear_handle_id (&priv->process_glass_idle, g_source_remove);
  }

  priv->process_glass_idle_deferred = deferred;

  if (deferred) {
    priv->process_glass_idle = g_timeout_add_full (G_PRIORITY_DEFAULT_IDLE,
                                                   PROCESS_GLASS_TAB_SWITCH_DELAY_MS,
                                                   update_process_glass_idle,
                                                   g_object_ref (self),
                                                   g_object_unref);
    g_source_set_name_by_id (priv->process_glass_idle,
                             "[kgx] process glass deferred");
  } else {
    priv->process_glass_idle = g_idle_add_full (G_PRIORITY_HIGH_IDLE,
                                                update_process_glass_idle,
                                                g_object_ref (self),
                                                g_object_unref);
    g_source_set_name_by_id (priv->process_glass_idle,
                             "[kgx] process glass");
  }
}


static void
schedule_process_glass_idle (KgxWindow *self)
{
  schedule_process_glass_idle_full (self, FALSE);
}


static void
schedule_process_glass_idle_deferred (KgxWindow *self)
{
  schedule_process_glass_idle_full (self, TRUE);
}


static gboolean
sync_active_train_signals (KgxWindow *self,
                           KgxTab    *active)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  g_autoptr (KgxTrain) train = NULL;
  g_autoptr (KgxTrain) current = NULL;

  if (active)
    g_object_get (active, "train", &train, NULL);

  current = g_signal_group_dup_target (priv->train_signals);

  if (current == train)
    return FALSE;

  g_signal_group_set_target (priv->train_signals, train);

  return TRUE;
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
sync_active_page_state (KgxWindow *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);
  g_autoptr (KgxTab) active = NULL;

  g_object_get (priv->pages, "active-page", &active, NULL);
  g_signal_group_set_target (priv->tab_signals, active);
  sync_active_train_signals (self, active);

  /* The process preset belongs to the previously active tab. Stop it as
   * soon as ownership changes; process-glass reconciliation will decide
   * what, if anything, to start for the newly active tab. */
  kgx_edge_stop_process_particle_immediate (priv->edge);
  priv->process_glass_force_reconcile = TRUE;

  /* Tab handoff churn should not leave the edge governor in a degraded mode
   * after the close/open animation is over. */
  kgx_edge_reset_redraw_governor (priv->edge);

  /* Immediately snap the new terminal's background to the current
   * glass color so there's no flash between the tab switch (which
   * makes the new terminal visible) and the glass transition start
   * (which runs from an idle). Without this, the new terminal shows
   * its own stale bg_current for 1+ frames while the CSS chrome still
   * shows the old glass color — a visible two-color flicker. */
  if (priv->glass_has_current && active) {
    g_autoptr (KgxTerminal) terminal = NULL;
    g_object_get (active, "terminal", &terminal, NULL);
    if (terminal) {
      GdkRGBA current_bg;
      GdkRGBA target_bg;

      kgx_terminal_get_current_bg (terminal, &current_bg);
      copy_opaque_rgba (&target_bg, &priv->glass_current);

      if (!same_opaque_rgb (&current_bg, &target_bg))
        kgx_terminal_apply_bg_immediate (terminal, &target_bg);
    }
  }

  /* Ambient is allowed to finish gracefully when leaving settings, but once
   * the user starts switching/opening/closing tabs again, don't keep those
   * lingering bursts competing with the tab handoff frame. */
  if (!priv->settings_visible)
    kgx_edge_stop_ambient_immediate (priv->edge);
 
  /* Re-evaluate process glass for the newly active tab after the handoff
   * frame so AdwTabView's own relayout/transition gets first claim on it. */
  schedule_process_glass_idle_deferred (self);
}

static void
active_page_changed (GObject *object, GParamSpec *pspec, gpointer data)
{
  KgxWindow *self = KGX_WINDOW (object);

  sync_active_page_state (self);
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



/* Returns: (transfer full): caller must unref with g_object_unref */
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

  return terminal ? VTE_TERMINAL (terminal) : NULL;
}


static void
search_enabled (GObject    *object,
                GParamSpec *pspec,
                KgxWindow  *self)
{
  KgxWindowPrivate *priv = kgx_window_get_instance_private (self);

  if (gtk_revealer_get_reveal_child (GTK_REVEALER (priv->search_revealer))) {
    gtk_widget_grab_focus (priv->search_entry);
  } else {
    gtk_widget_grab_focus (GTK_WIDGET (priv->pages));
  }
}


static void
search_changed (GtkSearchEntry *entry,
                KgxWindow      *self)
{
  g_autoptr (GObject) terminal_ref = NULL;
  VteTerminal *terminal;
  const char *search = NULL;
  VteRegex *regex;
  g_autoptr (GError) error = NULL;
  guint32 flags = PCRE2_MULTILINE;

  terminal_ref = (GObject *) get_active_terminal (self);
  terminal = (VteTerminal *) terminal_ref;
  if (!terminal) {
    return;
  }

  search = gtk_editable_get_text (GTK_EDITABLE (entry));

  if (search) {
    g_autofree char *lowercase = g_utf8_strdown (search, -1);

    if (!g_strcmp0 (lowercase, search))
      flags |= PCRE2_CASELESS;
  }

  {
    g_autofree char *escaped = g_regex_escape_string (search, -1);
    regex = vte_regex_new_for_search (escaped, -1, flags, &error);
  }

  if (error) {
    g_warning ("Search error: %s", error->message);
    return;
  }

  vte_terminal_search_find_previous (terminal);

  vte_terminal_search_set_regex (terminal, regex, 0);
  vte_regex_unref (regex);

  vte_terminal_search_find_next (terminal);
}


static void
search_next (GtkWidget *widget,
             KgxWindow *self)
{
  g_autoptr (GObject) terminal_ref = (GObject *) get_active_terminal (self);
  VteTerminal *terminal = (VteTerminal *) terminal_ref;

  if (terminal) {
    vte_terminal_search_find_next (terminal);
  }
}


static void
search_prev (GtkWidget *widget,
             KgxWindow *self)
{
  g_autoptr (GObject) terminal_ref = (GObject *) get_active_terminal (self);
  VteTerminal *terminal = (VteTerminal *) terminal_ref;

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
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, search_revealer);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, content_stack);
  /* settings_page created lazily on first use — not in the template */

  g_type_ensure (KGX_TYPE_EDGE);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, edge);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, settings_binds);
  gtk_widget_class_bind_template_child_private (widget_class, KgxWindow, surface_binds);

  gtk_widget_class_bind_template_callback (widget_class, fullscreened_changed);
  gtk_widget_class_bind_template_callback (widget_class, zoom);
  gtk_widget_class_bind_template_callback (widget_class, create_tearoff_host);
  gtk_widget_class_bind_template_callback (widget_class, maybe_close_window);
  gtk_widget_class_bind_template_callback (widget_class, status_changed);
  gtk_widget_class_bind_template_callback (widget_class, active_page_changed);
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

  /* Shared CSS provider for glass styling — create once */
  if (!glass_css) {
    glass_css = gtk_css_provider_new ();
    gtk_style_context_add_provider_for_display (
      gdk_display_get_default (),
      GTK_STYLE_PROVIDER (glass_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }

  /* Per-window CSS provider for process-specific glass override. */
  {
    static int win_id = 0;
    priv->window_css_class = g_strdup_printf ("kgx-win-%d", win_id++);
    gtk_widget_add_css_class (GTK_WIDGET (self), priv->window_css_class);
    priv->override_css_selectors = g_strdup_printf (
      ".%s headerbar,"
      ".%s tabbar,"
      ".%s settings-page,"
      ".%s scrollbar,"
      ".%s scrollbar trough",
      priv->window_css_class, priv->window_css_class,
      priv->window_css_class, priv->window_css_class,
      priv->window_css_class);
    priv->override_last_r = -1;
    priv->override_last_g = -1;
    priv->override_last_b = -1;
    priv->override_css = gtk_css_provider_new ();
    gtk_style_context_add_provider_for_display (
      gdk_display_get_default (),
      GTK_STYLE_PROVIDER (priv->override_css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
  }

  /* Track the active tab and its current train for process-glass updates. */
  priv->tab_signals = g_signal_group_new (KGX_TYPE_TAB);
  g_signal_group_connect (priv->tab_signals,
                          "notify::train",
                          G_CALLBACK (tab_train_changed),
                          self);
  priv->train_signals = g_signal_group_new (KGX_TYPE_TRAIN);
  g_signal_group_connect (priv->train_signals,
                          "child-added",
                          G_CALLBACK (active_train_children_changed),
                          self);
  g_signal_group_connect (priv->train_signals,
                          "child-removed",
                          G_CALLBACK (active_train_children_changed),
                          self);

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
kgx_window_fire_overscroll (KgxWindow       *self,
                           GtkPositionType  edge)
{
  KgxWindowPrivate *priv;

  g_return_if_fail (KGX_IS_WINDOW (self));

  priv = kgx_window_get_instance_private (self);

  kgx_edge_fire_overscroll (priv->edge, edge);
}


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
