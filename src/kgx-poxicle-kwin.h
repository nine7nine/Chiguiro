/* kgx-poxicle-kwin.h
 *
 * Compositor-render backend: instead of drawing the edge particles in-process
 * (GSK or the poxicle-wl subsurface), Chiguiro streams its already-simulated
 * instance frames to the poxicle-kwin compositor effect, which draws them on
 * Chiguiro's window. Chiguiro stays the sim/trigger producer (all presets,
 * overscroll, terminal bell / process spawn-exit); KWin does the drawing.
 *
 * Selected by the "compositor" poxicle-renderer backend (see kgx_poxicle_backend
 * in kgx-poxicle.h); a no-op for any other backend, and when HAVE_POXICLE is
 * undefined. Mutually exclusive with the in-app subsurface (both consume the same
 * global edge-draw sink).
 *
 * Copyright 2026 jordan Johnston
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Attach the compositor stream to the window. Returns TRUE if it is now streaming
 * to the poxicle-kwin effect (or already was); FALSE if the effect is unreachable
 * (not installed/loaded) or setup failed — the caller should then fall back to the
 * in-app subsurface so particles still render. A no-op returning FALSE when
 * HAVE_POXICLE is undefined. */
gboolean kgx_poxicle_kwin_attach (GtkWindow *window);
void     kgx_poxicle_kwin_detach (GtkWindow *window);

G_END_DECLS
