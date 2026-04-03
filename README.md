# Chiguiro

A fast, transparent, and customizable terminal emulator for Linux, forked from [GNOME Console](https://gitlab.gnome.org/GNOME/console).

## Features

- **App Glass** — translucent window with configurable glass opacity, color, and accent. Process-specific glass colors with smooth animated transitions
- **Edge particles** — overscroll bursts and ambient privilege/settings animations along window edges, fully configurable
- **Dynamic tab titles** — running process name with braille activity indicator (`bash: htop ⠲`)
- **No menus** — paginated settings overlay with carousel, keyboard shortcuts side-by-side
- **Headless server mode** — systemd user service for instant window startup via D-Bus
- **Smooth animations** — swing transitions, translucent hover states, animated sprite branding
- **GTK4 + libadwaita** — always-dark, VTE terminal, configurable scrollback, bell, and fonts

## Building

```bash
# Development build
meson setup builddir -Ddevel=true
meson compile -C builddir
GSETTINGS_SCHEMA_DIR=builddir/data builddir/src/chiguiro-devel

# Release build
meson setup builddir-release -Ddevel=false --prefix=/usr
meson compile -C builddir-release
sudo meson install -C builddir-release
```

## Server Mode

Chiguiro can run as a background service for instant window startup:

```bash
systemctl --user enable --now org.ninez.Chiguiro.service
```

Once running, opening new windows or tabs is near-instant since GTK and VTE are already initialized.

## Dependencies

GLib >= 2.80, GTK >= 4.19, libadwaita >= 1.8, VTE >= 0.77.0 (vte-2.91-gtk4), Pango >= 1.51.2, libgtop, PCRE2 >= 10.32

## Acknowledgments

Chiguiro is forked from [GNOME Console (KGX)](https://gitlab.gnome.org/GNOME/console), originally developed by Zander Brown and the GNOME Project. The capybara sprite is from [rainloaf on itch.io](https://rainloaf.itch.io/capybara-sprite-sheet).

## License

GPL-3.0-or-later
