/* kgx-poxicle.c
 *
 * Copyright 2026 jordan Johnston
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kgx-config.h"

#include "kgx-poxicle.h"

#ifdef HAVE_POXICLE

#include <gdk/wayland/gdkwayland.h>
#include <wayland-client.h>

#include <poxicle.h>
#include <poxicle-wl.h>

#include "kgx-edge-draw.h"   /* KgxParticleInstance + the comparison sink */

/* Per-window overlay state, stashed on the GtkWindow via g_object_set_data_full. */
typedef struct {
  PoxWl                   *overlay;
  struct wl_compositor    *compositor;
  struct wl_subcompositor *subcompositor;
  GtkWindow               *window;
  GdkSurface              *surface;
  gulong                   width_id;
  gulong                   height_id;
  guint                    resync_tick;     /* settles geometry over first frames */
  int                      resync_frames;
  int                      last_x, last_y, last_w, last_h, last_scale;
} KgxPoxicle;

/* The GdkSurface includes the CSD shadow margins, so cover only the window's
 * actual geometry: the surface transform is the top-left shadow offset, and the
 * window widget's allocation is the visible size. */
static void
overlay_geometry (KgxPoxicle *self, int *x, int *y, int *w, int *h)
{
  double tx = 0.0, ty = 0.0;

  gtk_native_get_surface_transform (GTK_NATIVE (self->window), &tx, &ty);
  *x = (int) tx;
  *y = (int) ty;
  *w = gtk_widget_get_width (GTK_WIDGET (self->window));
  *h = gtk_widget_get_height (GTK_WIDGET (self->window));
  if (*w <= 0) *w = gdk_surface_get_width (self->surface);
  if (*h <= 0) *h = gdk_surface_get_height (self->surface);
}

/* Render source: hand poxicle the exact instances kgx-edge captured this frame
 * (its real presets / overscroll / ambient / process), converted to PoxInstance.
 * This is the comparison — same simulation, drawn by poxicle-gl instead of GSK. */
static size_t
poxicle_source (PoxInstance *out, size_t cap, void *user)
{
  static KgxParticleInstance scratch[8192];
  int max = (int) MIN (cap, (size_t) G_N_ELEMENTS (scratch));
  int n = kgx_particle_sink_take (scratch, max);

  (void) user;
  for (int i = 0; i < n; i++) {
    out[i].x       = scratch[i].px;
    out[i].y       = scratch[i].py;
    out[i].size    = scratch[i].size;
    out[i].angle   = scratch[i].angle;
    out[i].shape   = (PoxShape) scratch[i].shape;
    out[i].color.r = scratch[i].color.red;
    out[i].color.g = scratch[i].color.green;
    out[i].color.b = scratch[i].color.blue;
    out[i].color.a = scratch[i].color.alpha;
  }
  return (size_t) n;
}

/* Restart the overlay's render loop when kgx-edge produces a new frame after the
 * overlay parked itself for being idle (see pox_wl_wake / the sink). */
static void
poxicle_wake (gpointer user)
{
  KgxPoxicle *self = user;

  if (self->overlay)
    pox_wl_wake (self->overlay);
}

static void
reg_global (void *data, struct wl_registry *reg, uint32_t name,
            const char *iface, uint32_t version)
{
  KgxPoxicle *self = data;

  if (g_strcmp0 (iface, "wl_compositor") == 0)
    self->compositor = wl_registry_bind (reg, name, &wl_compositor_interface,
                                         MIN (version, 4u));
  else if (g_strcmp0 (iface, "wl_subcompositor") == 0)
    self->subcompositor = wl_registry_bind (reg, name, &wl_subcompositor_interface, 1);
}

static void
reg_remove (void *data, struct wl_registry *reg, uint32_t name)
{
  (void) data; (void) reg; (void) name;
}

static const struct wl_registry_listener reg_listener = {
  .global = reg_global, .global_remove = reg_remove,
};

/* Push the overlay's current geometry to poxicle, skipping the round-trip when
 * nothing changed. */
static void
kgx_poxicle_sync_geometry (KgxPoxicle *self)
{
  int x, y, w, h;
  int scale;

  if (!self->overlay)
    return;

  scale = gdk_surface_get_scale_factor (self->surface);
  overlay_geometry (self, &x, &y, &w, &h);

  if (w == self->last_w && h == self->last_h && x == self->last_x &&
      y == self->last_y && scale == self->last_scale)
    return;

  self->last_x = x; self->last_y = y; self->last_w = w; self->last_h = h;
  self->last_scale = scale;
  pox_wl_resize (self->overlay, w, h, scale);
  pox_wl_set_position (self->overlay, x, y);
}

static void
on_size_changed (GdkSurface *surface, GParamSpec *pspec, gpointer data)
{
  (void) surface; (void) pspec;
  kgx_poxicle_sync_geometry (data);
}

/* The window's final allocation and CSD shadow transform are not settled the
 * instant it maps, so the geometry captured in kgx_poxicle_attach can be a hair
 * off (the overlay overdraws the right/bottom edge). The surface size often does
 * not change afterward, so notify::width/height never corrects it — only a
 * manual resize did. Re-sync for the first frames after map until it settles. */
static gboolean
resync_tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  KgxPoxicle *self = data;

  (void) widget; (void) clock;
  kgx_poxicle_sync_geometry (self);

  if (--self->resync_frames <= 0) {
    self->resync_tick = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
kgx_poxicle_free (gpointer data)
{
  KgxPoxicle *self = data;

  /* Stop diverting kgx-edge's instances back to GSK, and drop the wake hook so a
   * promoted frame can't call into a half-freed overlay. */
  kgx_particle_sink_set_wake (NULL, NULL);
  kgx_particle_sink_set_active (FALSE);

  if (self->resync_tick && self->window)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self->window), self->resync_tick);

  if (self->surface) {
    if (self->width_id)
      g_signal_handler_disconnect (self->surface, self->width_id);
    if (self->height_id)
      g_signal_handler_disconnect (self->surface, self->height_id);
  }
  if (self->overlay)
    pox_wl_free (self->overlay);
  if (self->subcompositor)
    wl_subcompositor_destroy (self->subcompositor);
  if (self->compositor)
    wl_compositor_destroy (self->compositor);
  g_free (self);
}

void
kgx_poxicle_attach (GtkWindow *window)
{
  GdkSurface *surface;
  GdkDisplay *gdpy;
  struct wl_display *display;
  struct wl_surface *parent;
  struct wl_event_queue *queue;
  struct wl_registry *reg;
  KgxPoxicle *self;
  int x, y, w, h, scale;

  /* On by default via the poxicle-overlay GSetting; KGX_POXICLE overrides it
   * (KGX_POXICLE=0 forces off) for quick A/B without changing the setting. */
  {
    const char *env = g_getenv ("KGX_POXICLE");
    gboolean enabled;

    if (env) {
      enabled = !g_str_equal (env, "0");
    } else {
      g_autoptr (GSettings) settings = g_settings_new (KGX_APPLICATION_ID);
      enabled = g_settings_get_boolean (settings, "poxicle-overlay");
    }
    if (!enabled)
      return;
  }

  if (g_object_get_data (G_OBJECT (window), "kgx-poxicle"))
    return;

  surface = gtk_native_get_surface (GTK_NATIVE (window));
  if (!surface || !GDK_IS_WAYLAND_SURFACE (surface))
    return;
  gdpy = gdk_surface_get_display (surface);
  if (!GDK_IS_WAYLAND_DISPLAY (gdpy))
    return;

  display = gdk_wayland_display_get_wl_display (GDK_WAYLAND_DISPLAY (gdpy));
  parent  = gdk_wayland_surface_get_wl_surface (GDK_WAYLAND_SURFACE (surface));
  if (!display || !parent)
    return;

  self = g_new0 (KgxPoxicle, 1);

  /* Bind the globals we need on a private queue so the roundtrip doesn't pump
   * GTK's own event queue mid-realize. */
  queue = wl_display_create_queue (display);
  reg = wl_display_get_registry (display);
  wl_proxy_set_queue ((struct wl_proxy *) reg, queue);
  wl_registry_add_listener (reg, &reg_listener, self);
  wl_display_roundtrip_queue (display, queue);
  wl_registry_destroy (reg);

  if (!self->compositor || !self->subcompositor) {
    wl_event_queue_destroy (queue);
    kgx_poxicle_free (self);
    return;
  }

  /* Move the bound globals (and thus everything created from them — the
   * subsurface and its frame callbacks) back to the default queue, which GTK
   * dispatches, so the overlay animates. */
  wl_proxy_set_queue ((struct wl_proxy *) self->compositor, NULL);
  wl_proxy_set_queue ((struct wl_proxy *) self->subcompositor, NULL);
  wl_event_queue_destroy (queue);

  self->window = window;
  self->surface = surface;

  scale = gdk_surface_get_scale_factor (surface);
  overlay_geometry (self, &x, &y, &w, &h);

  self->overlay = pox_wl_new (display, self->compositor, self->subcompositor,
                              parent, w, h, scale);
  if (!self->overlay) {
    kgx_poxicle_free (self);
    return;
  }
  pox_wl_set_position (self->overlay, x, y);
  self->last_x = x; self->last_y = y; self->last_w = w; self->last_h = h;
  self->last_scale = scale;

  /* Drive poxicle from kgx-edge's real instance stream (and divert it from GSK). */
  kgx_particle_sink_set_active (TRUE);
  kgx_particle_sink_set_wake (poxicle_wake, self);
  pox_wl_set_source (self->overlay, poxicle_source, NULL);

  self->width_id  = g_signal_connect (surface, "notify::width",
                                      G_CALLBACK (on_size_changed), self);
  self->height_id = g_signal_connect (surface, "notify::height",
                                      G_CALLBACK (on_size_changed), self);

  /* Correct the just-captured geometry once the window settles (see above). */
  self->resync_frames = 8;
  self->resync_tick = gtk_widget_add_tick_callback (GTK_WIDGET (window),
                                                    resync_tick_cb, self, NULL);

  g_object_set_data_full (G_OBJECT (window), "kgx-poxicle", self, kgx_poxicle_free);
}

void
kgx_poxicle_detach (GtkWindow *window)
{
  /* Clearing the data key runs kgx_poxicle_free via the GDestroyNotify. */
  g_object_set_data (G_OBJECT (window), "kgx-poxicle", NULL);
}

#else /* !HAVE_POXICLE */

void kgx_poxicle_attach (GtkWindow *window) { (void) window; }
void kgx_poxicle_detach (GtkWindow *window) { (void) window; }

#endif /* HAVE_POXICLE */
