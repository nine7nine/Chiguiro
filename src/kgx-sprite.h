/* kgx-sprite.h
 *
 * Copyright 2024 Chigüiro Contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define KGX_TYPE_SPRITE (kgx_sprite_get_type())

G_DECLARE_FINAL_TYPE (KgxSprite, kgx_sprite, KGX, SPRITE, GObject)

KgxSprite *kgx_sprite_new (const char *resource_path,
                           int         frame_width,
                           int         frame_height,
                           int         columns,
                           int         total_frames,
                           double      fps);

void kgx_sprite_start (KgxSprite *self, GtkWidget *widget);
void kgx_sprite_stop  (KgxSprite *self);

G_END_DECLS
