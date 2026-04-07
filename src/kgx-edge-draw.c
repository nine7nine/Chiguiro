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

#include "kgx-edge-draw.h"

static void
append_circle (GtkSnapshot   *snapshot,
               float          px,
               float          py,
               float          size,
               const GdkRGBA *color)
{
  GskRoundedRect rrect;
  gsk_rounded_rect_init_from_rect (&rrect,
    &GRAPHENE_RECT_INIT (px, py, size, size), size / 2.0f);
  gtk_snapshot_push_rounded_clip (snapshot, &rrect);
  gtk_snapshot_append_color (snapshot, color,
                             &GRAPHENE_RECT_INIT (px, py, size, size));
  gtk_snapshot_pop (snapshot);
}

static void
append_diamond (GtkSnapshot   *snapshot,
                GskPath       *unit_path,
                float          px,
                float          py,
                float          size,
                const GdkRGBA *color)
{
  float half = size / 2.0f;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_scale (snapshot, size, size);
  gtk_snapshot_append_fill (snapshot, unit_path, GSK_FILL_RULE_WINDING, color);
  gtk_snapshot_restore (snapshot);
}

static void
append_triangle (GtkSnapshot   *snapshot,
                 GskPath       *unit_path,
                 float          px,
                 float          py,
                 float          size,
                 float          angle,
                 const GdkRGBA *color)
{
  float half = size / 2.0f;
  gtk_snapshot_save (snapshot);
  gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (px + half, py + half));
  gtk_snapshot_rotate (snapshot, angle);
  gtk_snapshot_scale (snapshot, size, size);
  gtk_snapshot_append_fill (snapshot, unit_path, GSK_FILL_RULE_WINDING, color);
  gtk_snapshot_restore (snapshot);
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
                       GskPath                   *unit_triangle,
                       GskPath                   *unit_diamond)
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

  if (budget_remaining) {
    if (*budget_remaining <= 0)
      return;

    blocks = MIN (blocks, *budget_remaining);
    *budget_remaining -= blocks;
  }

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
    float bw;
    float bh;
    float bb;
    float tri_angle = 0.0f;

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
      bw = bb;
      bh = bb;
      tri_angle = (trail_dir == -1) ? 0.0f : 180.0f;
    } else if (d < width + height) {
      block_side = GTK_POS_RIGHT;
      px = width - bb;
      py = (float) (d - width);
      bw = bb;
      bh = bb;
      tri_angle = (trail_dir == -1) ? 90.0f : 270.0f;
    } else if (d < 2 * width + height) {
      block_side = GTK_POS_BOTTOM;
      px = width - (float) (d - width - height) - bb;
      py = height - bb;
      bw = bb;
      bh = bb;
      tri_angle = (trail_dir == -1) ? 180.0f : 0.0f;
    } else {
      block_side = GTK_POS_LEFT;
      px = 0.0f;
      py = height - (float) (d - 2 * width - height) - bb;
      bw = bb;
      bh = bb;
      tri_angle = (trail_dir == -1) ? 270.0f : 90.0f;
    }

    if (block_side != side)
      continue;

    if (side == GTK_POS_RIGHT)
      px -= width - strip_extent;
    else if (side == GTK_POS_BOTTOM)
      py -= height - strip_extent;

    switch (tune->shape) {
    case KGX_PARTICLE_SHAPE_CIRCLE:
      append_circle (snapshot, px, py, bb, &c);
      break;
    case KGX_PARTICLE_SHAPE_DIAMOND:
      append_diamond (snapshot, unit_diamond, px, py, bb, &c);
      break;
    case KGX_PARTICLE_SHAPE_TRIANGLE:
      append_triangle (snapshot, unit_triangle, px, py, bb, tri_angle, &c);
      break;
    default:
      gtk_snapshot_append_color (snapshot, &c,
                                 &GRAPHENE_RECT_INIT (px, py, bw, bh));
      break;
    }
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
                          GskPath                   *unit_triangle,
                          GskPath                   *unit_diamond,
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
                           unit_triangle, unit_diamond);
    kgx_edge_draw_segment (snapshot, v_head, segment_len, a,
                           width, height, perim, color, v_trail, tune, progress, side, strip_extent,
                           budget_remaining,
                           unit_triangle, unit_diamond);
  }
}
