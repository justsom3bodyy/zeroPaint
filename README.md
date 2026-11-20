# 0Paint (zeroPaint)

A tiny, fast raster image editor for Linux written in plain C99 and SDL2.

Built to feel immediately familiar if you just want a simple classic paint app for quick screenshots, pixel edits, cropping, and rough diagrams without waiting five seconds for a heavy suite to load. No Electron, no WebView, no GPU requirement, no GTK/Qt version headaches.

## Why 0Paint

- **0% idle CPU**: runs on a blocking `SDL_WaitEvent()` loop so it actually sleeps until you move the mouse or hit a key.
- **Runs on almost anything**: compiles with standard `gcc` or `clang` across x86, x86_64, ARMv6/v7, ARM64, RISC-V, and PowerPC. Automatically falls back to `SDL_RENDERER_SOFTWARE` when there is no hardware acceleration.
- **Small memory footprint**: stays under ~10 MB idle, and caps the undo stack by RAM budget (`32 MB` by default, override with `--max-undo-mb=16`) so older low-RAM machines dont start swapping.
- **Layout-independent hotkeys**: shortcuts read physical scancodes instead of translated characters, so `Ctrl+Z`, `Ctrl+C`, `Ctrl+V`, etc. keep working on Russian and any other non-Latin keyboard layout without switching back to English.
- **Zero image/font compile dependencies**: includes small built-in C99 encoders/decoders for BMP, PNG, PPM, and TGA plus baked anti-aliased Liberation Sans UI fonts (Latin + Cyrillic) and 4x4 supersampled monochrome toolbar icons. At runtime, the Text tool dynamically loads `libfreetype.so.6` via `dlopen` if present to render any installed system `.ttf`/`.otf` font crisply at arbitrary sizes (falling back to the built-in UTF-8 font automatically). The only compile-time deps are `SDL2`, `libc`, `libm`, and `libdl`.

## Features

- Classic light UI with top menu bar, compact single-row ribbon, contextual Text bar (system font picker, pixel size, bold), 8 canvas resize handles, and bottom status bar
- Left-click draws with Color 1, right-click draws with Color 2
- Tools: Pencil, Fill bucket (non-recursive scanline flood fill), Text (full UTF-8 including Cyrillic, multi-line, selectable system fonts), Eraser (with live square footprint preview), Eyedropper, Magnifier, Brushes (pixel, round, calligraphy, marker), Airbrush
- Shapes: Line, 2-bend Bezier curve, Rectangle, Rounded rectangle, Ellipse, Polygon, Triangle, Diamond, Right arrow, Star (outline, fill, or both)
- Rectangular and free-form Lasso selection with live contour preview, 8 interactive transform/resize handles, Color-2 transparent selection toggle, crop to selection, drag move, and clipboard support
- Smooth viewport navigation: Middle-click drag or `Space + Left-click` drag to pan, `Shift + Wheel` horizontal scroll, and `Ctrl + Wheel` cursor-anchored nearest-neighbor zoom from 25% to 1600% with optional pixel grid and rulers
- Drag-and-drop file loading and CLI file argument (`./0paint image.bmp`)

## Building from source

Install a C compiler and SDL2 headers for your distro:

- Debian / Ubuntu / Mint: `sudo apt install build-essential libsdl2-dev pkg-config`
- Fedora / RHEL: `sudo dnf install gcc make SDL2-devel pkgconf-pkg-config`
- Arch / Manjaro: `sudo pacman -S base-devel sdl2 pkgconf`
- Alpine: `sudo apk add build-base sdl2-dev pkgconf`
- Void: `sudo xbps-install -S base-devel SDL2-devel pkg-config`
- openSUSE: `sudo zypper install gcc make libSDL2-devel pkg-config`

Build and run:

```sh
sh install.sh
./0paint
```

Or install system-wide (binary + `.desktop` entry):

```sh
sudo make install
```

### Optional runtime utilities

0Paint detects these automatically if installed, and falls back gracefully if they are missing:

- Wayland clipboard: `wl-clipboard` (`wl-copy` / `wl-paste`)
- X11 clipboard: `xclip`
- Native file picker: `zenity` or `kdialog` (otherwise uses a lightweight built-in path box)

## Shortcuts

All shortcuts work regardless of active keyboard layout:

- `Ctrl+N` New canvas
- `Ctrl+O` Open image
- `Ctrl+S` Save (`Ctrl+Shift+S` Save As)
- `Ctrl+Z` Undo / `Ctrl+Y` or `Ctrl+Shift+Z` Redo
- `Ctrl+X` Cut / `Ctrl+C` Copy / `Ctrl+V` Paste / `Ctrl+A` Select All / `Del` Delete selection
- `Ctrl+W` Resize canvas / `Ctrl+E` Canvas properties
- `Ctrl+Shift+X` Crop to selection / `Ctrl+Shift+I` Invert colors
- `Ctrl++` / `Ctrl+-` / `Ctrl+0` Zoom in / out / 100%
- `Ctrl+G` Toggle pixel grid / `Ctrl+R` Toggle rulers
- `P` Pencil, `B` Brush, `E` Eraser, `F` Fill, `T` Text, `I` Eyedropper, `Z` Magnifier, `S` Rect select, `L` Lasso, `X` Swap colors
- `Esc` Commit or cancel current selection/tool

## License

MIT License. See `LICENSE` for details.
