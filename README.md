# Chigüiro

A GTK4 terminal emulator with event-driven glass backgrounds and a single-dimension particle system. Per-app process detection drives smooth glass color transitions and configurable edge particle animations. Forked from [GNOME Console](https://gitlab.gnome.org/GNOME/console).

## Features

- **App Glass** — per-process translucent backgrounds with smooth animated color transitions triggered by process detection
- **Edge Particles** — single-dimension particle system rendering along window edges with 7 animation presets, per-block envelope shaping, and per-app overrides
- **Overscroll Presets** — Scroll 1 (corner burst snakes) and Scroll 2 (full-bar edge fill) with independent tunables
- **Per-App Configuration** — comma-separated process names with overrides for preset, direction, shape, gap, speed, thickness, glass color, and particle color
- **Dynamic Tab Titles** — running process name with braille activity indicator (`bash: htop ⠲`)
- **Paginated Settings** — carousel-based overlay with clickable page dots (General, App Glass, Particles, Shortcuts)
- **Headless Server Mode** — systemd user service for instant window startup via D-Bus
- **GTK4 + libadwaita** — always-dark theme, VTE terminal, configurable scrollback, bell, and fonts

## Particle System

Seven animation presets, each with independently tunable parameters:

**Presets:** Ambient, Corners, Fireworks, Ping-Pong, Pulse Out, Rotate, Scroll 2

| Column | Range | Description |
|--------|-------|-------------|
| Spd | 10 – 300 | Animation speed (percent) |
| Thk | 2 – 40 | Block size in pixels |
| Gap | Gapped / Solid | Space between blocks or continuous bar |
| Tail | 10 – 300 | Trail length (percent) |
| P.Dep | 0 – 100 | Shimmer intensity (percent) |
| P.Spd | 10 – 500 | Shimmer wave speed (percent) |
| Atk | 0 – 50 | Envelope attack (grow-in fraction) |
| Rel | 0 – 50 | Envelope release (fade-out fraction) |
| Rls | U / R / S | Release mode: Uniform, Retract, Spread |
| Shape | Square / Circle / Diamond / Triangle | Block shape |
| Crv | ( / / / ) | Envelope curve: concave, linear, convex |
| T.Atk | 0 – 50 | Thickness envelope attack |
| T.Rel | 0 – 50 | Thickness envelope release |
| T.Rls | U / R / S / G | Thickness release: Uniform, Retract (all blocks shrink), Shrink (per-block), Grow (per-block) |
| T.Crv | ( / / / ) | Thickness envelope curve |

### App Glass Per-Process Overrides

Each process entry supports: glass color, preset, reverse direction, shape, gap, speed, thickness, and particle color. Reverse has three modes: forward, reverse, and alternating (diamond mode).

## Building

```bash
# Development build
meson setup builddir -Ddevel=true
meson compile -C builddir
GTK_DEBUG=interactive GSETTINGS_SCHEMA_DIR=builddir/data builddir/src/chiguiro-devel

# Release build
meson setup builddir-release -Ddevel=false --prefix=/usr
meson compile -C builddir-release
sudo meson install -C builddir-release
```

## Server Mode

Chigüiro can run as a background service for instant window startup:

```bash
systemctl --user enable --now org.ninez.Chiguiro.service
```

Once running, opening new windows or tabs is near-instant since GTK and VTE are already initialized.

## Dependencies

GLib >= 2.80, GTK >= 4.19, libadwaita >= 1.8, VTE >= 0.77.0 (vte-2.91-gtk4), Pango >= 1.51.2, libgtop, PCRE2 >= 10.32

## Acknowledgments

Chigüiro is forked from [GNOME Console (KGX)](https://gitlab.gnome.org/GNOME/console), originally developed by Zander Brown and the GNOME Project. The capybara sprite is from [rainloaf on itch.io](https://rainloaf.itch.io/capybara-sprite-sheet).

## License

GPL-3.0-or-later
