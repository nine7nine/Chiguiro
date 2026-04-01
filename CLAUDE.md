# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Setup build directory
meson setup builddir -Ddevel=true                        # dev build (org.ninez.Chiguiro.Devel)
meson setup builddir-release -Ddevel=false --prefix=/usr # release build for install
meson setup builddir-asan -Ddevel=true -Db_sanitize=address  # AddressSanitizer build

# Compile
meson compile -C builddir

# Run (dev)
GSETTINGS_SCHEMA_DIR=builddir/data builddir/src/chiguiro-devel

# Run with ASan (for debugging memory issues)
GSETTINGS_SCHEMA_DIR=builddir-asan/data ASAN_OPTIONS=detect_leaks=0 builddir-asan/src/chiguiro-devel

# Install to system
sudo meson install -C builddir-release

# Run tests
meson test -C builddir
meson test -C builddir --suite gnome-console

# Tests requiring a display server
dbus-run-session -- mutter --headless --wayland --no-x11 \
  --virtual-monitor 1024x768 -- meson test -C builddir

# Coverage
meson setup builddir -Db_coverage=true -Dtests=true
meson compile -C builddir && meson test -C builddir
gcovr --json --output=coverage.json
```

Tests use GSettings memory backend and a dummy process (`tests/dummy-process.c`) for spawn testing. Test environment sets `G_DEBUG=gc-friendly,fatal-warnings`.

## Architecture

GNOME Console (kgx) is a GTK4/libadwaita terminal emulator written in C using GObject. The class hierarchy:

```
KgxApplication (AdwApplication)
 └─ KgxWindow (AdwApplicationWindow)
     └─ KgxPages (AdwBin, wraps AdwTabView)
         └─ KgxTab (AdwBin, abstract with virtual start/start_finish)
             └─ KgxSimpleTab (concrete implementation)
                 ├─ KgxTerminal (VteTerminal)
                 └─ KgxTrain (session/process group)
```

**Key subsystems:**
- **Process management**: `KgxDepot` spawns processes, `KgxTrain` represents a shell session, `KgxProcess` wraps individual process info, `KgxWatcher` monitors active trains
- **Color/theming**: `KgxLivery` (color scheme with UUID), `KgxPalette` (night/day palettes), `KgxLiveryManager` (available schemes). Liveries are referenced by UUID in settings
- **Settings**: `KgxSettings` wraps GSettings, owns the schema at `data/org.ninez.Chiguiro.gschema.xml.in`. Changes propagate via GObject property bindings (`GBindingGroup`, `GSignalGroup`)
- **Process info**: `pids/kgx-gtop.c` uses libgtop for process tree inspection

**Patterns used throughout:**
- GObject async (`GAsyncReadyCallback` / `GAsyncResult`) for process spawning
- GObject signals for lifecycle events (`died`, `bell`)
- UI templates (`.ui` files) bound to widget classes
- Generated code: `kgx-enums.c/h` (enum types), `kgx-marshals.c/h` (signal marshalers), `kgx-resources.c/h` (compiled resources), `xdg-fm1.c/h` (D-Bus FileManager1)

## Build Options

| Option | Default | Effect |
|--------|---------|--------|
| `devel` | false | Builds as `org.ninez.Chiguiro.Devel` with separate app ID |
| `tests` | true | Enable test suite |

## Dependencies

GLib >= 2.80, GTK >= 4.19, libadwaita >= 1.8, VTE >= 0.77.0 (vte-2.91-gtk4), Pango >= 1.51.2, libgtop, PCRE2 >= 10.32. Subproject wraps available for gtk, adwaita, vte, glib, pango.

## Project Policy

This project bans AI-generated contributions (code, docs, issues, artwork). Claude Code may be used locally for development assistance but generated code must not be submitted upstream.
