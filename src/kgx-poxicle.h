/* kgx-poxicle.h
 *
 * Experimental bridge to the standalone poxicle particle library: attaches a
 * poxicle overlay (its own subsurface + GLES3 renderer) to a window, running
 * alongside the built-in KgxEdge engine for A/B comparison. Gated at build time
 * by HAVE_POXICLE and at runtime by the KGX_POXICLE environment variable, so it
 * is inert unless explicitly enabled. When HAVE_POXICLE is undefined these are
 * no-ops.
 *
 * Copyright 2026 jordan Johnston
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Which renderer draws the edge particles. The three are mutually exclusive (the
 * two poxicle backends both consume the global edge-draw sink). */
typedef enum {
  KGX_POXICLE_BACKEND_GSK = 0,    /* in-process GSK (no poxicle)            */
  KGX_POXICLE_BACKEND_SUBSURFACE, /* in-app poxicle-wl subsurface overlay   */
  KGX_POXICLE_BACKEND_COMPOSITOR, /* stream to the poxicle-kwin compositor  */
} KgxPoxicleBackend;

/* Resolve the active backend from the KGX_POXICLE env override and the
 * poxicle-overlay / poxicle-renderer GSettings. Always returns GSK when poxicle
 * is disabled (setting off, KGX_POXICLE=0) or the build lacks HAVE_POXICLE. */
KgxPoxicleBackend kgx_poxicle_backend (void);

void kgx_poxicle_attach (GtkWindow *window);
void kgx_poxicle_detach (GtkWindow *window);

G_END_DECLS
