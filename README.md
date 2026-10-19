# Glance

A fast, fluid photo viewer for Windows in the spirit of the old Picasa Photo Viewer: it opens over a dimmed desktop, shows the photo almost immediately, and everything moves with soft, slightly springy, Apple-style animation.

It is a single ~500 KB `Glance.exe` with no installer and no runtime.

## Download

- **Latest build** (updated on every change to `main`): [Glance.exe](https://github.com/LazySeaHorse/glance/releases/download/latest/Glance.exe)
- **Numbered releases:** [Releases page](https://github.com/LazySeaHorse/glance/releases)

Windows SmartScreen will warn that the file isn't signed: *More info → Run anyway*. Each release also carries a `Glance.exe.sha256` checksum.

## Using it

- **Open a photo:** drag it onto `Glance.exe`, or run `Glance.exe "C:\path\photo.jpg"`. Run it with nothing and it shows a file picker.
- **Make it your viewer:** run `Glance.exe --register` once (no admin needed). Then right-click any photo → *Open with* → *Choose another app* → **Glance** → *Always*. `--unregister` removes it again.
- If you move `Glance.exe` later, run `--register` again from the new location.
- Opening another photo while Glance is already showing one reuses that window instantly instead of starting a second copy.

### Controls

| Action | Mouse | Keyboard |
|---|---|---|
| Next / previous | swipe (drag) left/right, two-finger swipe on a touchpad, side mouse buttons, filmstrip | → ← / Page Down Page Up / Space / Backspace |
| First / last | | Home / End |
| Zoom in / out | scroll wheel or touchpad pinch (zooms where the pointer is) | + / − |
| Fit / 100% | double-click toggles | 0 / 1 |
| Pan when zoomed | drag (flicks keep coasting), two fingers on a touchpad | ↑ ↓ |
| Rotate | ↺ ↻ buttons | R (clockwise), L or Shift+R (counter-clockwise), [ ] |
| Slideshow | ▶ button | S or F5; Esc stops |
| Photo info | | I |
| Delete / undo | | Delete / Ctrl+Z |
| Copy | | Ctrl+C |
| Show in Explorer | | Ctrl+E |
| Close | click the dark background, right-click, × button | Esc, Ctrl+W, Alt+F4 |

You can zoom out to a tenth of the fitted size. Zooming in or out through the fitted size pauses there for one wheel notch, so it's easy to land on it exactly.

The filmstrip sits small at the bottom and grows to full size as the pointer moves towards it, like the macOS Dock. It and the toolbar fade away after a couple of seconds without mouse movement.

**Photo info** (I) shows when it was taken, camera, lens, exposure, focal length, size, file size and folder. It stays open as you browse; I or Esc closes it.

**Delete** hides the photo at once and really deletes it a few seconds later, so **Ctrl+Z** can bring it back in the meantime (several deletes undo in reverse order). The file goes to the Recycle Bin; if its drive has none or it's switched off, it's deleted permanently, as Picasa did, and the message says so. Anything still pending is deleted when you close Glance.

**Copy** puts the photo on the clipboard twice over: as the file (paste into Explorer, mail or a chat app to attach the original) and as a picture turned the way you see it (paste into Word, Paint or an image editor).

**Touchpads:** on a precision touchpad, two fingers swipe between photos or pan a zoomed one, and pinching zooms. A mouse wheel still zooms.

Rotation is view-only for now; it doesn't change the file.

### Formats

Glance opens whatever Windows itself can decode (Windows Imaging Component): JPEG, PNG, GIF, BMP, TIFF, ICO, JPEG-XR, DDS out of the box, plus HEIC/HEIF, WebP, AVIF and camera RAW when the matching free extensions from the Microsoft Store are installed (*HEIF Image Extensions*, *Webp Image Extensions*, *AV1 Video Extension*, *Raw Image Extension*). Glance asks Windows which codecs exist at start-up, so new ones are picked up automatically.

Animated GIFs play, with their own frame timing (frames are composited properly, so GIFs that only redraw part of the picture each frame look right).

### Colour

Photos tagged with a wide-gamut colour profile are converted to sRGB as they're decoded. This covers Display P3 (iPhones and most recent phones) and Adobe RGB (many cameras), which otherwise look dull or washed-out. Untagged and sRGB photos skip this step, so it costs nothing for them.

## Why it's fast

1. **Decoding starts before the window exists.** The very first thing Glance does is hand your photo to a background decoder; the GPU and window are set up in parallel.
2. **Something sharp-enough appears first.** Most camera JPEGs carry a small preview inside the file; Glance shows that instantly, then crossfades to the real decode a moment later. For large JPEGs it asks the decoder to decode at 1/2, 1/4 or 1/8 size directly (much faster than decoding full size and shrinking).
3. **It decodes for your screen, not for the file.** A 24-megapixel photo is decoded at screen resolution; full resolution is only decoded when you zoom past that.
4. **Neighbours are preloaded.** The next two photos in the direction you're browsing (and the previous ones) are decoded in the background, so arrow keys feel instant. Filmstrip thumbnails load after those.
5. **Everything is drawn on the GPU** (Direct2D on Direct3D 11) through DirectComposition, so the dimmed desktop behind the photo is live and transparent rather than a screenshot (a video playing underneath keeps playing).
6. **Nothing runs when nothing moves.** The render loop sleeps between interactions (0% CPU while you look at a photo) and, while animating, wakes on your display's refresh and samples every motion at the moment the frame will be shown.
7. **A second launch costs nothing.** If Glance is already open, a new launch just passes the file to it and exits.
8. **The filmstrip borrows Explorer's thumbnails.** If Explorer has shown a folder before, Windows already has thumbnails for it on disk; Glance asks for those first and only decodes photos Windows hasn't seen.
9. **Work you've moved past is dropped.** Decodes run in short strips; when you flick past a photo, its unfinished decode stops, and background work steps aside the moment the photo on screen needs a worker.
10. **Huge photos stream in as tiles.** Zooming past screen resolution decodes the full image top to bottom into 512-pixel tiles that appear as they're ready. Only one strip is held in memory at a time, and images wider or taller than the GPU's 16,384-pixel limit show at true full resolution instead of being shrunk. Above 160 megapixels the "full" level is decoded smaller, to keep GPU memory reasonable.

## Animations

Every motion is a damped spring (`src/spring.h`) described the way Apple describes theirs: a *response* (roughly how long it takes) and a *damping fraction* (1.0 = no overshoot, lower = a little bounce). Springs keep their velocity when the target changes, so interrupting an animation (scrolling again mid-zoom, pressing → twice quickly, flicking while zoomed) stays smooth instead of restarting.

The values that shape the feel are grouped at the call sites in `src/main.cpp`:

| Motion | Where | Response / damping |
|---|---|---|
| Open (photo grows in) | `TryAppear`, `APPEAR_OPEN` | 0.50 / 0.78 |
| Next/previous slide | `TryAppear`, `APPEAR_SLIDE`; `PushGhost` | 0.42 / 0.90 |
| Zoom and pan (wheel) | `ZoomTo` | 0.30 / 0.90 |
| Filmstrip grow/shrink | `g_stripK` | 0.24 / 0.92 |
| Flick coasting | `OnMouseUp` | 0.55 / 0.92 |
| Rotate | `Rotate` | 0.48 / 0.78 |
| Close | `Close` | 0.20–0.30 / 1.0 |
| Slideshow crossfade | `APPEAR_FADE` | 0.90 / 1.0 |

## Building

On Linux (Ubuntu/Debian):

```
sudo apt install mingw-w64
./build.sh                         # -> build/Glance.exe, version from ./VERSION
GLANCE_VERSION=0.3.0 ./build.sh    # stamp a specific version
```

Builds are reproducible: the same sources and compiler give a byte-identical `Glance.exe`.

### On GitHub

`.github/workflows/build.yml` builds on every push and pull request (the `.exe` is attached to each run as an artifact). A push to `main` refreshes the rolling **latest** pre-release. To publish a numbered release, bump `VERSION`, commit, then tag and push:

```
git tag v0.3.0
git push origin v0.3.0
```

`tools/make_icon.py` regenerates the icon; `tools/make_test_images.py` makes a folder of test photos (various formats, EXIF rotations, a panorama, a broken file); `tools/wine_run.sh` drives the viewer under Wine on a virtual display and takes screenshots.

## Timing a launch

Set the environment variable `GLANCE_TRACE=1` (or pass `--trace`) and Glance appends timings to `%TEMP%\glance-trace.txt`, measured from the moment Windows created the process:

```
   12.0 ms  ---- start
   41.3 ms  GPU device ready
   55.9 ms  window ready (composition)
   58.1 ms  decoded IMG_0001.JPG level 0 (160x120) in 3.9 ms
   60.4 ms  first image on screen
  121.7 ms  screen-resolution image ready
```

(Illustrative numbers. Real ones on your PC are the useful thing to send back.)

## Code map

- `src/main.cpp` — window, GPU setup, view state, input, animation, drawing, filmstrip, file list, "Open with" registration.
- `src/loader.cpp` — WIC decoding on worker threads: Windows thumbnail cache, preview/screen/full (tiled) levels, cancellation and priorities, EXIF and photo info, GIF frames, colour management, codec discovery.
- `src/fileops.cpp` — delete (Recycle Bin or permanent), Show in Explorer, clipboard.
- `src/spring.h` — the spring.
- `src/com.h` — a tiny COM smart pointer.
- `res/` — icon, manifest (per-monitor DPI aware), version info.

## Not yet verified on real Windows

It was built and exercised under Wine on Linux, which can't show DirectComposition or vsync timing, so these need a check on your PC:

- Animation smoothness at your monitor's refresh rate, and open-to-first-pixel time.
- RAW orientation: Glance assumes the Windows decoder already rotates pixels itself. If RAW photos appear sideways, that assumption is wrong.
- Colour management: Wine doesn't implement colour profiles, so this was only checked by reading the code. A Display P3 or Adobe RGB photo should look the same as in the Windows Photos app.
- High-DPI (125–200%) layout of the controls and filmstrip.
- The Windows thumbnail cache (Wine doesn't provide one).
- Touchpad two-finger sideways swipe: see below.

## Verified on real Windows

- HEIC photos open correctly (with the HEIF Image Extensions installed).
- The live transparent backdrop works, and video playing underneath it keeps playing.
- Precision touchpad: pinch to zoom and two-finger up/down panning work.

### Known issue

On a precision touchpad, the two-finger sideways swipe between photos only starts working after another gesture (a pinch or an up/down scroll) has happened, and again after each photo change. Pinch and vertical scroll are unaffected.
