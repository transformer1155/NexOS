# MiniOS Win11 Desktop Simulator

A Windows 11 style **desktop environment simulator** written in **C++17 + SDL2**.
It is a shell/desktop simulation (not a real OS kernel) - focusing on visual
fidelity and interaction, with an abstracted desktop environment.

> Build: `cmake -B build && cmake --build build -j`  (or `make` with the included Makefile)

---

## Feature Matrix

### Desktop Shell
| Feature | Status |
|---------|--------|
| Bloom gradient wallpaper + glow | ✅ |
| Desktop icons (drag / select / double-click launch) | ✅ |
| Right-click context menu (desktop / icon / taskbar) | ✅ |
| Centered taskbar (Start / Search / Task view / Widgets) | ✅ |
| System tray (clock / date / wifi / volume / battery / chevron) | ✅ |
| Full start menu (search filter / pinned grid / all apps / power / user) | ✅ |
| Notification center (right flyout) | ✅ |
| Widgets panel (weather / calendar / news) | ✅ |

### Window System
| Feature | Status |
|---------|--------|
| Rounded corners (12px, disabled when maximized) | ✅ |
| Centered title bar | ✅ |
| Hamburger menu button | ✅ |
| Min / Max / Close buttons (hover states) | ✅ |
| Drag window + snap on edge release | ✅ |
| 8-way edge/corner resize | ✅ |
| Snap Layouts: Win+Left/Right/Up, corner snap | ✅ |
| Maximize / minimize / restore | ✅ |
| Alt+Tab switcher (with thumbnail cards) | ✅ |
| Open / close / snap animations (200ms / 150ms) | ✅ |

### Themes
| Feature | Status |
|---------|--------|
| Light theme (accent `#0067C0`) | ✅ |
| Dark theme (accent `#4CC2FF`) | ✅ |
| Theme switch (right-click desktop -> Dark mode, or Settings app) | ✅ |
| Mica / Acrylic tint approximation | ✅ |
| Segoe UI typography scale (bitmap fallback) | ✅ |

### Built-in Apps
| App | Status |
|-----|--------|
| Settings (System rows + Appearance theme card) | ✅ |
| File Explorer (nav pane / address bar / command bar / file grid / status bar) | ✅ |
| Calculator (working arithmetic) | ✅ |
| Notepad (line numbers + text) | ✅ |
| Terminal (fake PowerShell) | ✅ |
| Task Manager (tabs + process list) | ✅ |
| About MiniOS | ✅ |

### Keyboard Shortcuts
| Shortcut | Action |
|----------|--------|
| `Win` (or click Start) | Start menu |
| `Win+E` | File Explorer |
| `Win+I` | Settings |
| `Win+W` | Widgets |
| `Win+S` | Search (opens Start) |
| `Win+Up` | Maximize |
| `Win+Left` / `Win+Right` | Snap left / right |
| `Win+Down` | Minimize |
| `Alt+Tab` | App switcher |
| `Alt+F4` | Close window |
| `Esc` | Close menu / start / flyouts |

---

## Build

### Linux / WSL
```bash
sudo apt install libsdl2-dev cmake g++
cmake -B build && cmake --build build -j
./build/win11desktop
```

### Windows (MSYS2/MinGW)
```bash
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc
cmake -B build -G "MinGW Makefiles"
cmake --build build -j
build/win11desktop.exe
```

### Headless self-test (screenshots)
```bash
SDL_VIDEODRIVER=dummy ./build/win11desktop --selftest
# writes shot_01_desktop.bmp ... shot_06_widgets.bmp
```

---

## Project Layout
```
src/
  main.cpp      entry, main loop, shortcuts, selftest
  theme.h       design tokens (colors / fonts / animations)
  font8x8.h     embedded bitmap font
  gfx.h/.cpp    SDL2 software renderer (rounded rect, shadow, text, blur)
  window.h/.cpp window manager (snap, alt-tab, resize, animations)
  shell.h/.cpp  desktop shell (taskbar, start menu, tray, flyouts, menus)
  apps.cpp      built-in app content painters
CMakeLists.txt cross-platform build
Makefile       simple fallback build
```

## Scope Notes
- This is a **simulation/shell**, not a real kernel: file system, processes and
  memory are abstracted (e.g. File Explorer shows a fake virtual file tree).
- The bitmap font approximates Segoe UI; no TTF dependency is required.
- Mica/Acrylic are approximated with tinted translucent surfaces + blur region
  (blur available via `Gfx::blurRegion`).
