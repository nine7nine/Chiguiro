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

void kgx_poxicle_attach (GtkWindow *window);
void kgx_poxicle_detach (GtkWindow *window);

G_END_DECLS
