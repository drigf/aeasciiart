# ASCII Depth – After Effects effect (macOS)

Turns the layer into ASCII text. Character choice follows the picture's brightness; the
**glyph grey level follows depth**: near = white, far = dark grey. Output is always greyscale.

Find it under **Effect ▸ Stylize ▸ ASCII Depth**.

## Workflow: edit in VS Code, build in Xcode

```
Source/
  AsciiDepth.h / .cpp      After Effects glue: params, render hand-off
  AsciiDepthPiPL.r         PiPL resource (name, match name, flags) – keep in sync with GlobalSetup()
  Info.plist
  Core/
    AsciiCore.h / .cpp     cell analysis, depth estimate, glyph choice, rasteriser (no AE dependency)
    GlyphAtlas.mm          CoreText glyph bitmaps, sorted by ink density, cached
    SubjectMask.mm         Apple Vision foreground mask (macOS 14+)
Config/AsciiDepth.xcconfig AE_SDK_ROOT, install-on-build settings
scripts/gen_xcodeproj.py   regenerates AsciiDepth.xcodeproj from the files on disk
Tools/Harness/             command-line renderer for testing without After Effects
```

1. `code .` in this folder (VS Code will suggest the C/C++ extension).
2. Edit under `Source/`. **⇧⌘B** runs the default task (`make`): regenerate the Xcode project,
   build a universal (arm64 + x86_64) Release, and copy it into
   `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore`.
3. Or open `AsciiDepth.xcodeproj` in Xcode and press **⌘B**. Same build phases, same result.
4. **Restart After Effects** after each build (it only loads plug-ins at launch).
5. Added or removed a source file? Run `make xcodeproj` (task: *Regenerate Xcode project*).
   Files are picked up automatically; there's nothing to drag into Xcode.

The Adobe SDK is expected at `~/Developer/AfterEffectsSDK/Examples`. If yours is elsewhere, create
`Config/Local.xcconfig` (git-ignored) containing `AE_SDK_ROOT = /path/to/AfterEffectsSDK/Examples`,
and update the include paths in `.vscode/c_cpp_properties.json` for IntelliSense.
To stop auto-installing, add `AE_INSTALL_ON_BUILD = NO` to `Local.xcconfig`.

Debugging in Xcode: the scheme launches After Effects; set breakpoints in `Source/`, ⌘R.
(Use the Debug configuration. It's optimised at `-O1`, so rendering is still usable.)

### Testing without After Effects

```
make samples        # builds Tools/Harness and renders synthetic images into Tools/Harness/out/
build/asciidepth-harness in.png out.png cell=14 charset=1 depth=2 far=0.2
```
Keys are listed at the top of `Tools/Harness/main.mm`. The harness drives exactly the same core as the plug-in.

## Parameters

| Group | Parameter | Notes |
|---|---|---|
| Characters | Cell Size / Cell Aspect | Cell size is in full-res px; scales automatically at half/quarter preview |
| | Character Set | Classic, Simple, Binary, Blocks. Glyphs are auto-sorted by ink density |
| | Invert Characters | Dense glyphs on dark areas instead of light |
| Tone | Brightness / Contrast / Gamma | Affect *which character* is picked |
| Depth | Depth Source | **Auto Estimate**, **Depth Layer**, **Subject (Apple Vision)** |
| | Depth Layer | Pick a layer; white = near. Used when source is *Depth Layer* |
| | Invert Depth, Normalize Depth Layer, Depth Contrast, Depth Smoothing | Shape the depth map |
| | Near Level / Far Level | Glyph grey for nearest / furthest (default 100 % → 25 %) |
| Auto Depth Cues | Focus / Height / Brightness | Weights for the automatic estimate |
| | Background | Black, Transparent, Dimmed Source |

## About "depth"

A flat video frame doesn't contain depth, so there are three honest options:

- **Depth Layer** – accurate. Feed a real depth map (a Z-depth pass from 3D, a depth-estimation
  plug-in/AI depth-map render, a precomp'd matte, etc.). White = near.
- **Subject (Apple Vision)** – uses macOS's foreground-instance segmentation (people/animals/objects) to
  split subject from background, then adds the auto cues inside each. Roughly 150–800 ms per frame
  at 640 px analysis size, so expect slow renders. It falls back to Auto if Vision finds nothing.
- **Auto Estimate** – instant heuristic: in-focus detail, lower-in-frame and brighter all read as nearer.
  Works best on shallow depth-of-field footage. It is per-frame, so it can shimmer on busy footage;
  raise *Depth Smoothing*.

## Known limits / ideas

- 8 and 16 bpc only (32 bpc projects are converted by After Effects). Float support needs SmartFX.
- Source and depth layers are sampled by normalised coordinates; layers with different sizes still line up.
- Character sets are fixed presets. Custom text would need an arbitrary-data or text param.
- The plug-in is ad-hoc signed. That's fine for local use. For distribution, sign with a Developer ID and notarise.
