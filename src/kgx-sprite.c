/* kgx-sprite.c
 *
 * Copyright 2024-2026 jordan Johnston
 * Capybara sprite credit: https://rainloaf.itch.io/capybara-sprite-sheet
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "kgx-sprite.h"

/* Exact frame positions extracted from the spritesheet (only real capybara
 * frames, filtered out tiny decoration dots) */
static const struct { int x, y, w, h; } frame_rects[] = {
  { 4, 9, 24, 20 },
  { 88, 9, 24, 20 },
  { 123, 9, 24, 20 },
  { 157, 9, 23, 20 },
  { 6, 42, 24, 18 },
  { 37, 42, 23, 18 },
  { 64, 42, 22, 18 },
  { 43, 90, 24, 20 },
  { 74, 90, 22, 20 },
  { 102, 90, 20, 20 },
  { 129, 90, 22, 20 },
  { 156, 90, 22, 20 },
  { 46, 194, 24, 18 },
  { 79, 194, 24, 18 },
  { 112, 194, 24, 18 },
  { 144, 194, 24, 18 },
  { 175, 194, 24, 18 },
  { 46, 226, 24, 21 },
  { 80, 226, 22, 21 },
  { 111, 226, 21, 21 },
  { 140, 226, 21, 21 },
  { 169, 226, 21, 21 },
  { 46, 258, 23, 20 },
  { 76, 258, 24, 20 },
  { 107, 258, 26, 20 },
  { 140, 258, 26, 20 },
  { 174, 258, 26, 20 },
  { 48, 291, 24, 16 },
  { 81, 291, 24, 16 },
  { 116, 291, 24, 16 },
  { 149, 291, 24, 16 },
  { 184, 291, 26, 16 },
  { 50, 323, 23, 25 },
  { 85, 323, 21, 25 },
  { 119, 323, 19, 25 },
  { 150, 323, 22, 25 },
  { 184, 323, 25, 25 },
  { 51, 364, 22, 16 },
  { 85, 364, 25, 16 },
  { 119, 364, 32, 16 },
  { 160, 364, 34, 16 },
  { 202, 364, 25, 16 },
};
#define N_FRAMES G_N_ELEMENTS (frame_rects)
#define CANVAS_W 34  /* max frame width */
#define CANVAS_H 25  /* max frame height */
#define SCALE    2


struct _KgxSprite {
  GObject        parent_instance;

  GdkTexture   **frames;
  int            total_frames;
  int            current_frame;
  double         fps;

  guint          tick_callback_id;
  GtkWidget     *widget;
  gint64         last_frame_time;
};

static void kgx_sprite_paintable_init (GdkPaintableInterface *iface);

G_DEFINE_TYPE_WITH_CODE (KgxSprite, kgx_sprite, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (GDK_TYPE_PAINTABLE,
                                               kgx_sprite_paintable_init))


static void
kgx_sprite_snapshot_paintable (GdkPaintable *paintable,
                               GdkSnapshot  *snapshot,
                               double        width,
                               double        height)
{
  KgxSprite *self = KGX_SPRITE (paintable);
  GdkTexture *frame;

  if (!self->frames || self->current_frame >= self->total_frames) {
    return;
  }

  frame = self->frames[self->current_frame];
  if (!frame) {
    return;
  }

  gtk_snapshot_append_scaled_texture (GTK_SNAPSHOT (snapshot),
                                      frame,
                                      GSK_SCALING_FILTER_NEAREST,
                                      &GRAPHENE_RECT_INIT (0, 0, width, height));
}


static int
kgx_sprite_get_intrinsic_width (GdkPaintable *paintable)
{
  return CANVAS_W * SCALE;
}


static int
kgx_sprite_get_intrinsic_height (GdkPaintable *paintable)
{
  return CANVAS_H * SCALE;
}


static void
kgx_sprite_paintable_init (GdkPaintableInterface *iface)
{
  iface->snapshot = kgx_sprite_snapshot_paintable;
  iface->get_intrinsic_width = kgx_sprite_get_intrinsic_width;
  iface->get_intrinsic_height = kgx_sprite_get_intrinsic_height;
}


static void
kgx_sprite_dispose (GObject *object)
{
  KgxSprite *self = KGX_SPRITE (object);

  kgx_sprite_stop (self);

  if (self->frames) {
    for (int i = 0; i < self->total_frames; i++) {
      g_clear_object (&self->frames[i]);
    }
    g_free (self->frames);
    self->frames = NULL;
  }

  G_OBJECT_CLASS (kgx_sprite_parent_class)->dispose (object);
}


static void
kgx_sprite_class_init (KgxSpriteClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  object_class->dispose = kgx_sprite_dispose;
}


static void
kgx_sprite_init (KgxSprite *self)
{
}


static gboolean
tick_callback (GtkWidget     *widget,
               GdkFrameClock *frame_clock,
               gpointer       user_data)
{
  KgxSprite *self = KGX_SPRITE (user_data);
  gint64 now = gdk_frame_clock_get_frame_time (frame_clock);
  gint64 frame_duration = (gint64)(1000000.0 / self->fps);

  if (now - self->last_frame_time >= frame_duration) {
    self->current_frame = (self->current_frame + 1) % self->total_frames;
    self->last_frame_time = now;
    gdk_paintable_invalidate_contents (GDK_PAINTABLE (self));
  }

  return G_SOURCE_CONTINUE;
}


KgxSprite *
kgx_sprite_new (const char *resource_path,
                int         frame_width,
                int         frame_height,
                int         columns,
                int         total_frames,
                double      fps)
{
  KgxSprite *self = g_object_new (KGX_TYPE_SPRITE, NULL);
  GdkTexture *sheet;
  int sheet_w, sheet_h, stride;
  g_autofree guchar *pixels = NULL;

  sheet = gdk_texture_new_from_resource (resource_path);
  sheet_w = gdk_texture_get_width (sheet);
  sheet_h = gdk_texture_get_height (sheet);
  stride = sheet_w * 4;
  pixels = g_malloc (stride * sheet_h);
  gdk_texture_download (sheet, pixels, stride);

  self->fps = fps;
  self->current_frame = 0;
  self->total_frames = N_FRAMES;

  /* Extract each frame into a uniform CANVAS_W x CANVAS_H texture,
   * centered within the canvas */
  self->frames = g_new0 (GdkTexture *, N_FRAMES);
  for (int i = 0; i < (int) N_FRAMES; i++) {
    int fx = frame_rects[i].x;
    int fy = frame_rects[i].y;
    int fw = frame_rects[i].w;
    int fh = frame_rects[i].h;
    int ox = (CANVAS_W - fw) / 2;  /* center horizontally */
    int oy = (CANVAS_H - fh);      /* align to bottom */
    guchar *canvas = g_malloc0 (CANVAS_W * CANVAS_H * 4);
    GBytes *bytes;

    for (int row = 0; row < fh && (fy + row) < sheet_h; row++) {
      for (int col = 0; col < fw && (fx + col) < sheet_w; col++) {
        int src = ((fy + row) * sheet_w + (fx + col)) * 4;
        int dst = ((oy + row) * CANVAS_W + (ox + col)) * 4;
        canvas[dst + 0] = pixels[src + 0];
        canvas[dst + 1] = pixels[src + 1];
        canvas[dst + 2] = pixels[src + 2];
        canvas[dst + 3] = pixels[src + 3];
      }
    }

    bytes = g_bytes_new_take (canvas, CANVAS_W * CANVAS_H * 4);
    self->frames[i] = gdk_memory_texture_new (CANVAS_W, CANVAS_H,
                                               GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                               bytes, CANVAS_W * 4);
  }

  g_object_unref (sheet);

  return self;
}


void
kgx_sprite_start (KgxSprite *self, GtkWidget *widget)
{
  g_return_if_fail (KGX_IS_SPRITE (self));

  if (self->tick_callback_id) {
    return;
  }

  self->widget = widget;
  self->last_frame_time = 0;
  self->tick_callback_id = gtk_widget_add_tick_callback (widget,
                                                          tick_callback,
                                                          g_object_ref (self),
                                                          g_object_unref);
}


void
kgx_sprite_stop (KgxSprite *self)
{
  g_return_if_fail (KGX_IS_SPRITE (self));

  if (self->tick_callback_id && self->widget) {
    gtk_widget_remove_tick_callback (self->widget, self->tick_callback_id);
    self->tick_callback_id = 0;
  }
}
