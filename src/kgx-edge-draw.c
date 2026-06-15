/* kgx-edge-draw.c
 *
 * Copyright 2026 jordan Johnston
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

#include <math.h>
#include <cairo.h>

#include "kgx-edge-draw.h"

/* Mask resolution at scale 1. Generous enough that the common (small) block
 * sizes stay crisp; larger blocks soften slightly under linear upscaling,
 * which is fine for a glow. */
#define KGX_PARTICLE_MASK_BASE 64

static GdkTexture *
bake_mask (KgxParticleShape shape, int size)
{
  cairo_surface_t *surf;
  cairo_t         *cr;
  GdkTexture      *tex;
  GBytes          *bytes;
  double           s = size;

  surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
  cr = cairo_create (surf);
  cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 1.0);   /* white alpha mask */

  switch (shape) {
  case KGX_PARTICLE_SHAPE_CIRCLE:
    cairo_arc (cr, s / 2.0, s / 2.0, s / 2.0, 0.0, 2.0 * G_PI);
    break;
  case KGX_PARTICLE_SHAPE_DIAMOND:
    cairo_move_to (cr, s / 2.0, 0.0);
    cairo_line_to (cr, s,       s / 2.0);
    cairo_line_to (cr, s / 2.0, s);
    cairo_line_to (cr, 0.0,     s / 2.0);
    cairo_close_path (cr);
    break;
  case KGX_PARTICLE_SHAPE_TRIANGLE:
    /* Right-pointing apex; rotated per-instance at stamp time (matches the
     * former unit-triangle geometry). */
    cairo_move_to (cr, 0.0, 0.0);
    cairo_line_to (cr, s,   s / 2.0);
    cairo_line_to (cr, 0.0, s);
    cairo_close_path (cr);
    break;
  case KGX_PARTICLE_SHAPE_SQUARE:
  default:
    cairo_rectangle (cr, 0.0, 0.0, s, s);
    break;
  }
  cairo_fill (cr);
  cairo_destroy (cr);
  cairo_surface_flush (surf);

  /* CAIRO_FORMAT_ARGB32 is premultiplied native-endian == B8G8R8A8 on LE. */
  bytes = g_bytes_new (cairo_image_surface_get_data (surf),
                       (gsize) cairo_image_surface_get_stride (surf) * size);
  tex = gdk_memory_texture_new (size, size,
                                GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                bytes,
                                cairo_image_surface_get_stride (surf));
  g_bytes_unref (bytes);
  cairo_surface_destroy (surf);

  return tex;
}

void
kgx_particle_masks_clear (KgxParticleMasks *masks)
{
  g_clear_object (&masks->circle);
  g_clear_object (&masks->diamond);
  g_clear_object (&masks->triangle);
  masks->size = 0;
}

void
kgx_particle_masks_ensure (KgxParticleMasks *masks, int scale)
{
  int size;

  if (scale < 1)
    scale = 1;
  size = KGX_PARTICLE_MASK_BASE * scale;

  if (masks->size == size && masks->circle)
    return;

  kgx_particle_masks_clear (masks);
  masks->circle   = bake_mask (KGX_PARTICLE_SHAPE_CIRCLE, size);
  masks->diamond  = bake_mask (KGX_PARTICLE_SHAPE_DIAMOND, size);
  masks->triangle = bake_mask (KGX_PARTICLE_SHAPE_TRIANGLE, size);
  masks->size     = size;
}

void
kgx_particle_emit (GtkSnapshot               *snapshot,
                   const KgxParticleInstance *inst,
                   const KgxParticleMasks    *masks)
{
  GdkTexture       *tex = NULL;
  graphene_matrix_t tint;
  graphene_vec4_t   offset;
  float             half;
  float             m[16] = { 0.0f };

  if (inst->shape != KGX_PARTICLE_SHAPE_SQUARE && masks) {
    switch (inst->shape) {
    case KGX_PARTICLE_SHAPE_CIRCLE:   tex = masks->circle;   break;
    case KGX_PARTICLE_SHAPE_DIAMOND:  tex = masks->diamond;  break;
    case KGX_PARTICLE_SHAPE_TRIANGLE: tex = masks->triangle; break;
    case KGX_PARTICLE_SHAPE_SQUARE:
    default:                          break;
    }
  }

  if (!tex) {
    /* Square (or masks not yet baked): a plain colour rect is the cheapest,
     * best-batching primitive and needs no tint. */
    gtk_snapshot_append_color (snapshot, &inst->color,
                               &GRAPHENE_RECT_INIT (inst->px, inst->py,
                                                    inst->size, inst->size));
    return;
  }

  /* Tint the white alpha-mask to the block colour and fold in the per-block
   * alpha via a diagonal colour matrix — the same technique GTK uses to
   * recolour symbolic icons. Every shape stays a batchable textured quad
   * instead of a per-block path tessellation or clip. */
  m[0] = inst->color.red;
  m[5] = inst->color.green;
  m[10] = inst->color.blue;
  m[15] = inst->color.alpha;
  graphene_matrix_init_from_float (&tint, m);
  graphene_vec4_init (&offset, 0.0f, 0.0f, 0.0f, 0.0f);

  gtk_snapshot_push_color_matrix (snapshot, &tint, &offset);

  if (inst->shape == KGX_PARTICLE_SHAPE_TRIANGLE && inst->angle != 0.0f) {
    half = inst->size / 2.0f;
    gtk_snapshot_save (snapshot);
    gtk_snapshot_translate (snapshot,
                            &GRAPHENE_POINT_INIT (inst->px + half, inst->py + half));
    gtk_snapshot_rotate (snapshot, inst->angle);
    gtk_snapshot_append_texture (snapshot, tex,
                                 &GRAPHENE_RECT_INIT (-half, -half,
                                                      inst->size, inst->size));
    gtk_snapshot_restore (snapshot);
  } else {
    gtk_snapshot_append_texture (snapshot, tex,
                                 &GRAPHENE_RECT_INIT (inst->px, inst->py,
                                                      inst->size, inst->size));
  }

  gtk_snapshot_pop (snapshot);
}

void
kgx_edge_draw_segment (GtkSnapshot               *snapshot,
                       double                     head_d,
                       double                     seg_len,
                       float                      alpha,
                       float                      width,
                       float                      height,
                       double                     perim,
                       const GdkRGBA             *color,
                       int                        trail_dir,
                       const KgxParticleTunables *tune,
                       double                     phase,
                       GtkPositionType            side,
                       float                      strip_extent,
                       int                       *budget_remaining,
                       const KgxParticleMasks    *masks)
{
  double base_blk = (double) tune->thickness;
  double blk      = base_blk;
  float thk_attack_env = 1.0f;
  float thk_release_factor = 0.0f;
  double gap;
  double step;
  int blocks;
  double d;
  double delta;
  float inv_blocks;
  float phase_offset;
  int s;

  /* Thickness attack (all blocks grow from 0). */
  if (tune->thk_attack > 0.0)
    thk_attack_env = kgx_particle_thickness_envelope (phase, tune->thk_attack, 0.0, tune->thk_curve);

  /* R = uniform shrink (all blocks together, original behavior).
   * S/G = per-block (head stays, tail changes) — computed in the loop.
   * A = all blocks shrink (head included) — uniform release factor. */
  if (tune->thk_release_mode == KGX_RELEASE_RETRACT) {
    blk *= kgx_particle_thickness_envelope (phase, tune->thk_attack, tune->thk_release, tune->thk_curve);
  } else {
    blk *= thk_attack_env;
    if ((tune->thk_release_mode == KGX_RELEASE_SPREAD ||
         tune->thk_release_mode == KGX_RELEASE_GROW ||
         tune->thk_release_mode == KGX_RELEASE_ALL) &&
        tune->thk_release > 0.0 && phase > 1.0 - tune->thk_release) {
      thk_release_factor = (float) ((phase - (1.0 - tune->thk_release)) / tune->thk_release);
    }
  }
  if (blk < 1.0)
    blk = 1.0;

  /* Gap between blocks: 0 = gapped (default), 1 = solid (no gap). */
  gap = tune->gap ? 0.0 : base_blk;

  if (tune->release_mode == KGX_RELEASE_SPREAD &&
      tune->env_release > 0.0 && phase > 1.0 - tune->env_release) {
    double rt = (phase - (1.0 - tune->env_release)) / tune->env_release;
    gap *= (1.0 + rt * 3.0);
  }

  step = blk + gap;
  blocks = (int) (seg_len / step);

  if (blocks < 1)
    blocks = 1;

  /* The budget is shared across all four edge widgets and is decremented per
   * block actually appended (below), so it caps real GPU draw calls instead of
   * 4x-counting whole segments — most of whose blocks belong to other sides. */
  if (budget_remaining && *budget_remaining <= 0)
    return;

  d = fmod (head_d + perim, perim);
  delta = trail_dir * step;
  inv_blocks = 1.0f / (float) (blocks > 1 ? blocks - 1 : 1);
  phase_offset = (float) (phase * 40.0 * tune->pulse_speed);

  for (s = 0; s < blocks; s++) {
    float t;
    double block_blk;
    float a;
    GdkRGBA c;
    GtkPositionType block_side;
    float px;
    float py;
    float bb;
    float tri_angle = 0.0f;
    KgxParticleInstance inst;

    if (s > 0) {
      d += delta;
      if (d >= perim)
        d -= perim;
      if (d < 0.0)
        d += perim;
    }

    t = (float) s * inv_blocks;
    block_blk = blk;
    if (thk_release_factor > 0.0f) {
      if (tune->thk_release_mode == KGX_RELEASE_ALL)
        block_blk = blk * (1.0 - (double) (thk_release_factor));
      else if (tune->thk_release_mode == KGX_RELEASE_GROW)
        block_blk = blk * (1.0 + (double) (t * thk_release_factor));
      else
        block_blk = blk * (1.0 - (double) (t * thk_release_factor));
      if (block_blk < 1.0)
        block_blk = 1.0;
    }

    a = alpha * (1.0f - 0.7f * t);
    if (s > 0 && tune->pulse_depth > 0.0) {
      float intensity = t * (float) tune->pulse_depth;
      float pulse = 1.0f - intensity
                    + intensity * sinf ((float) s * 1.2f - phase_offset);
      a *= pulse;
    }

    c = *color;
    c.alpha = a;

    bb = (float) block_blk;
    if (d < width) {
      block_side = GTK_POS_TOP;
      px = (float) d;
      py = 0.0f;
      tri_angle = (trail_dir == -1) ? 0.0f : 180.0f;
    } else if (d < width + height) {
      block_side = GTK_POS_RIGHT;
      px = width - bb;
      py = (float) (d - width);
      tri_angle = (trail_dir == -1) ? 90.0f : 270.0f;
    } else if (d < 2 * width + height) {
      block_side = GTK_POS_BOTTOM;
      px = width - (float) (d - width - height) - bb;
      py = height - bb;
      tri_angle = (trail_dir == -1) ? 180.0f : 0.0f;
    } else {
      block_side = GTK_POS_LEFT;
      px = 0.0f;
      py = height - (float) (d - 2 * width - height) - bb;
      tri_angle = (trail_dir == -1) ? 270.0f : 90.0f;
    }

    if (block_side != side)
      continue;

    if (side == GTK_POS_RIGHT)
      px -= width - strip_extent;
    else if (side == GTK_POS_BOTTOM)
      py -= height - strip_extent;

    if (budget_remaining) {
      if (*budget_remaining <= 0)
        break;
      (*budget_remaining)--;
    }

    /* Sim → render seam: this loop is pure layout/colour; emission is the
     * backend's job. inst is renderer-agnostic data. */
    inst.px = px;
    inst.py = py;
    inst.size = bb;
    inst.angle = (tune->shape == KGX_PARTICLE_SHAPE_TRIANGLE) ? tri_angle : 0.0f;
    inst.shape = tune->shape;
    inst.color = c;
    kgx_particle_emit (snapshot, &inst, masks);
  }
}

void
kgx_edge_draw_overscroll (GtkSnapshot               *snapshot,
                          double                     progress,
                          GtkPositionType            edge,
                          int                        style,
                          gboolean                   reverse,
                          float                      width,
                          float                      height,
                          double                     perim,
                          const GdkRGBA             *color,
                          const KgxParticleTunables *tune,
                          GtkPositionType            side,
                          float                      strip_extent,
                          int                       *budget_remaining,
                          const KgxParticleMasks    *masks,
                          double                     segment_len)
{
  float env = kgx_particle_envelope (progress, tune->env_attack, tune->env_release, tune->env_curve);
  float a = env * 0.9f;

  if (style == 1) {
    float thk;
    float thk_env_val = 1.0f;
    float bar_len;
    GdkRGBA c;

    /* Scroll 2: solid bar along the active edge. */
    if (edge != side)
      return;

    thk = (float) tune->thickness;
    if (tune->thk_attack > 0.0)
      thk_env_val = kgx_particle_thickness_envelope (progress, tune->thk_attack, 0.0, tune->thk_curve);
    thk *= thk_env_val;
    if (thk < 1.0f)
      thk = 1.0f;

    bar_len = (float) width * kgx_particle_envelope (progress, tune->env_attack, 0.0, tune->env_curve);
    c = *color;
    c.alpha = a;

    if (edge == GTK_POS_TOP) {
      float x = reverse ? width - bar_len : 0.0f;
      gtk_snapshot_append_color (snapshot, &c,
                                 &GRAPHENE_RECT_INIT (x, 0, bar_len, thk));
    } else {
      float x = reverse ? width - bar_len : 0.0f;
      gtk_snapshot_append_color (snapshot, &c,
                                 &GRAPHENE_RECT_INIT (x, strip_extent - thk, bar_len, thk));
    }
  } else {
    double corner;
    double h_head;
    double v_head;
    int h_trail;
    int v_trail;

    /* Scroll 1: corner burst along two adjacent edges. */
    if (edge == GTK_POS_BOTTOM) {
      if (reverse) {
        corner = width + height + width;
        h_head = fmod (corner - width * progress + perim, perim);
        v_head = fmod (corner + height * progress, perim);
        h_trail = +1;
        v_trail = -1;
      } else {
        corner = width + height;
        h_head = fmod (corner + width * progress, perim);
        v_head = fmod (corner - height * progress + perim, perim);
        h_trail = -1;
        v_trail = +1;
      }
    } else {
      if (reverse) {
        corner = 0.0;
        h_head = fmod (width * progress, perim);
        v_head = fmod (perim - height * progress + perim, perim);
        h_trail = -1;
        v_trail = +1;
      } else {
        corner = width;
        h_head = fmod (corner - width * progress + perim, perim);
        v_head = fmod (corner + height * progress, perim);
        h_trail = +1;
        v_trail = -1;
      }
    }

    kgx_edge_draw_segment (snapshot, h_head, segment_len, a,
                           width, height, perim, color, h_trail, tune, progress, side, strip_extent,
                           budget_remaining,
                           masks);
    kgx_edge_draw_segment (snapshot, v_head, segment_len, a,
                           width, height, perim, color, v_trail, tune, progress, side, strip_extent,
                           budget_remaining,
                           masks);
  }
}
