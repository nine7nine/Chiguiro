/* kgx-poxicle-kwin.c
 *
 * Copyright 2026 jordan Johnston
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE   /* memfd_create / MFD_CLOEXEC */

#include "kgx-config.h"

#include "kgx-poxicle-kwin.h"

#ifdef HAVE_POXICLE

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <gio/gunixfdlist.h>

#include <poxicle.h>   /* PoxInstance layout (header only — no GL/WL link) */

#include "kgx-edge-draw.h"   /* the global comparison sink */
#include "kgx-particle.h"    /* KgxParticleInstance */
#include "poxbridge.h"       /* vendored cross-process protocol */

/* The producer fills the shared body straight from the sink as KgxParticleInstance,
 * relying on it being byte-identical to poxicle's PoxInstance — same contract the
 * in-app consumer (kgx-poxicle.c) pins. Re-assert it here so a compositor-only
 * build still fails loudly if either record drifts. */
G_STATIC_ASSERT (sizeof (KgxParticleInstance) == sizeof (PoxInstance));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, px)    == offsetof (PoxInstance, x));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, py)    == offsetof (PoxInstance, y));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, size)  == offsetof (PoxInstance, size));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, angle) == offsetof (PoxInstance, angle));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, shape) == offsetof (PoxInstance, shape));
G_STATIC_ASSERT (offsetof (KgxParticleInstance, color) == offsetof (PoxInstance, color));

/* Body capacity: match the sink's own buffer so a busy frame is never truncated
 * (KGX_POX_SINK_MAX in kgx-edge-draw.c). ~36 B/instance => ~288 KB region. */
#define KGX_POXICLE_KWIN_CAPACITY 8192

/* Keep publishing empty frames this many ticks after the sim goes quiet so the
 * effect composites the last particles away, then park the publish loop so the
 * frame clock (and KWin) can sleep. Mirrors poxicle-wl's idle grace; the effect
 * has its own grace on top. A wake (new sim frame) re-arms us. */
#define KGX_POXICLE_KWIN_IDLE_GRACE 4

/* Per-window producer state, stashed on the GtkWindow via g_object_set_data_full. */
typedef struct {
  GtkWindow       *window;
  GDBusConnection *bus;        /* session bus (owned ref) */
  int              pid;        /* our pid; the effect binds the stream by it */
  int              fd;         /* memfd backing the shared region (owned) */
  PoxBridgeHeader *hdr;        /* RW mmap base (owned) */
  gsize            map_size;
  guint32          capacity;
  guint            tick_id;    /* frame-clock tick callback; 0 = parked */
  int              idle;       /* consecutive empty publishes */
  gboolean         registered; /* the effect accepted our Register */
} KgxPoxicleKwin;

/* Drain the sink's latest frame into the shared body and publish it through the
 * seqlock (writer side of poxbridge.h: odd = writing, even = complete). Returns
 * TRUE if the frame carried any particles. */
static gboolean
publish_frame (KgxPoxicleKwin *self)
{
  PoxBridgeHeader     *h = self->hdr;
  KgxParticleInstance *body = (KgxParticleInstance *) ((char *) h + sizeof *h);
  int w = gtk_widget_get_width (GTK_WIDGET (self->window));
  int ht = gtk_widget_get_height (GTK_WIDGET (self->window));
  guint32 s = h->seq;   /* even (we are the only writer) */
  int n;

  __atomic_store_n (&h->seq, s + 1, __ATOMIC_RELAXED);   /* odd: writing */
  __atomic_thread_fence (__ATOMIC_RELEASE);

  /* The sink writes window-local (frame-local) coords; the effect offsets them by
   * the window's frameGeometry, so no conversion is needed here. */
  n = kgx_particle_sink_take (body, (int) self->capacity);
  h->count  = (guint32) n;
  h->width  = w  > 0 ? (guint32) w  : 0;   /* identity hint for the effect (Phase 3) */
  h->height = ht > 0 ? (guint32) ht : 0;

  __atomic_thread_fence (__ATOMIC_RELEASE);
  __atomic_store_n (&h->seq, s + 2, __ATOMIC_RELEASE);   /* even: published */

  return n > 0;
}

static gboolean
tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  KgxPoxicleKwin *self = data;

  (void) widget; (void) clock;

  if (publish_frame (self)) {
    self->idle = 0;
    return G_SOURCE_CONTINUE;
  }
  /* No particles this frame: emit a few trailing empties so the effect clears,
   * then park. The sink's wake hook re-arms us when the sim restarts. */
  if (++self->idle >= KGX_POXICLE_KWIN_IDLE_GRACE) {
    self->tick_id = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void
arm_tick (KgxPoxicleKwin *self)
{
  if (self->tick_id || !self->window)
    return;
  self->idle = 0;
  self->tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self->window),
                                                tick_cb, self, NULL);
}

/* Sink wake: a non-empty frame was promoted after we parked. Restart our publish
 * loop and nudge the effect, which parks its own repaint loop when we go quiet. */
static void
poxicle_kwin_wake (gpointer user)
{
  KgxPoxicleKwin *self = user;

  if (!self->window)
    return;
  arm_tick (self);
  if (self->bus && self->registered)
    g_dbus_connection_call (self->bus, POX_BRIDGE_SERVICE, POX_BRIDGE_PATH,
                            POX_BRIDGE_IFACE, "Wake",
                            g_variant_new ("(i)", self->pid),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

/* Hand the memfd to the effect over D-Bus. Synchronous: we want the effect to
 * have mapped (or rejected) the region before we start streaming. */
static gboolean
poxicle_kwin_register (KgxPoxicleKwin *self)
{
  g_autoptr (GUnixFDList) fdlist = g_unix_fd_list_new ();
  g_autoptr (GError) error = NULL;
  g_autoptr (GVariant) ret = NULL;
  int idx;
  gboolean ok = FALSE;

  idx = g_unix_fd_list_append (fdlist, self->fd, &error);
  if (idx < 0) {
    g_warning ("poxicle-kwin: fd list append: %s", error->message);
    return FALSE;
  }
  ret = g_dbus_connection_call_with_unix_fd_list_sync (
      self->bus, POX_BRIDGE_SERVICE, POX_BRIDGE_PATH, POX_BRIDGE_IFACE,
      "Register", g_variant_new ("(ih)", self->pid, idx),
      G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000,
      fdlist, NULL, NULL, &error);
  if (!ret) {
    g_warning ("poxicle-kwin: Register failed (is the poxicle-kwin effect loaded?): %s",
               error->message);
    return FALSE;
  }
  g_variant_get (ret, "(b)", &ok);
  return ok;
}

static void
kgx_poxicle_kwin_free (gpointer data)
{
  KgxPoxicleKwin *self = data;

  /* Stop feeding the sink first so a promoted frame can't wake into a half-freed
   * producer (the in-app path's UAF-on-close lesson). */
  kgx_particle_sink_set_wake (NULL, NULL);
  kgx_particle_sink_set_active (FALSE);

  if (self->tick_id && self->window)
    gtk_widget_remove_tick_callback (GTK_WIDGET (self->window), self->tick_id);

  /* Tell the effect to drop + unmap its view before we release the region.
   * Synchronous so the unmap is ordered ahead of our munmap/close. */
  if (self->bus && self->registered) {
    g_dbus_connection_call_sync (self->bus, POX_BRIDGE_SERVICE, POX_BRIDGE_PATH,
                                 POX_BRIDGE_IFACE, "Unregister",
                                 g_variant_new ("(i)", self->pid),
                                 NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
  }

  if (self->hdr)
    munmap (self->hdr, self->map_size);
  if (self->fd >= 0)
    close (self->fd);
  if (self->bus)
    g_object_unref (self->bus);
  g_free (self);
}

gboolean
kgx_poxicle_kwin_attach (GtkWindow *window)
{
  KgxPoxicleKwin  *self;
  g_autoptr (GError) error = NULL;
  GDBusConnection *bus;
  gsize            map_size;
  int              fd;
  void            *base;

  /* The window-map coordinator only calls this for the compositor backend; it
   * decides on the subsurface fallback from our return value. */
  if (g_object_get_data (G_OBJECT (window), "kgx-poxicle-kwin"))
    return TRUE;   /* already streaming */

  /* The effect owns org.ninez.PoxicleBridge on the session bus. */
  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (!bus) {
    g_warning ("poxicle-kwin: no session bus: %s", error->message);
    return FALSE;
  }

  map_size = (gsize) pox_bridge_map_size (KGX_POXICLE_KWIN_CAPACITY,
                                          sizeof (PoxInstance));
  fd = memfd_create ("kgx-poxbridge", MFD_CLOEXEC);
  if (fd < 0) {
    g_warning ("poxicle-kwin: memfd_create: %s", g_strerror (errno));
    g_object_unref (bus);
    return FALSE;
  }
  if (ftruncate (fd, (off_t) map_size) != 0) {
    g_warning ("poxicle-kwin: ftruncate: %s", g_strerror (errno));
    close (fd);
    g_object_unref (bus);
    return FALSE;
  }
  base = mmap (NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (base == MAP_FAILED) {
    g_warning ("poxicle-kwin: mmap: %s", g_strerror (errno));
    close (fd);
    g_object_unref (bus);
    return FALSE;
  }

  self = g_new0 (KgxPoxicleKwin, 1);
  self->window   = window;
  self->bus      = bus;            /* transfer the ref */
  self->pid      = (int) getpid ();
  self->fd       = fd;
  self->hdr      = base;
  self->map_size = map_size;
  self->capacity = KGX_POXICLE_KWIN_CAPACITY;

  memset (self->hdr, 0, sizeof *self->hdr);
  self->hdr->magic     = POX_BRIDGE_MAGIC;
  self->hdr->version   = POX_BRIDGE_VERSION;
  self->hdr->capacity  = self->capacity;
  self->hdr->inst_size = (guint32) sizeof (PoxInstance);
  self->hdr->seq       = 0;        /* even: no frame yet */

  /* Divert kgx-edge's instances into the sink, then publish one (likely empty)
   * valid frame so the region is consistent before the effect binds. */
  kgx_particle_sink_set_active (TRUE);
  publish_frame (self);

  if (!poxicle_kwin_register (self)) {
    /* No effect, or it refused the region: tear down and report failure so the
     * window map falls back to the in-app subsurface overlay. */
    kgx_particle_sink_set_active (FALSE);
    munmap (self->hdr, self->map_size);
    close (self->fd);
    g_object_unref (self->bus);
    g_free (self);
    return FALSE;
  }
  self->registered = TRUE;

  kgx_particle_sink_set_wake (poxicle_kwin_wake, self);
  arm_tick (self);

  g_object_set_data_full (G_OBJECT (window), "kgx-poxicle-kwin", self,
                          kgx_poxicle_kwin_free);
  return TRUE;
}

void
kgx_poxicle_kwin_detach (GtkWindow *window)
{
  /* Clearing the data key runs kgx_poxicle_kwin_free via the GDestroyNotify. */
  g_object_set_data (G_OBJECT (window), "kgx-poxicle-kwin", NULL);
}

#else /* !HAVE_POXICLE */

gboolean kgx_poxicle_kwin_attach (GtkWindow *window) { (void) window; return FALSE; }
void     kgx_poxicle_kwin_detach (GtkWindow *window) { (void) window; }

#endif /* HAVE_POXICLE */
