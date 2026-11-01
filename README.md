# POMPBR

A plugin for Call of Duty: Black Ops II on the REDACTED client (`t6mpv43.exe`) that does two things:

- **POM:** parallax occlusion mapping on the textures you choose, picked in game. Press a key, click a surface, press Convert to
  POM, and it has relief right away. Everything you didn't choose stays exactly as it was.
- **PBR:** GGX highlights, Smith shadowing, anti-sparkle, highlights on omni lights and screen-space reflections on everything lit.
  It's on as soon as the plugin loads, and works from the game's own gloss and spec maps, so it needs no new textures.

The plugin file is `POM.Red32n`, its settings are in `POM.ini`, and it goes in REDACTED's `Plugins` folder. PBR hasn't been tested
in the game yet: everything below marked as tested was tested through the game's own shaders, outside the game.

## Using it

`F7` freezes the camera and opens the tool (`ToolKey` in `POM.ini`). It needs a map running, in a match or in theater.

1. **Click anything in the world.** Map geometry, models and players all work. What you clicked lights up in amber, along with
   everything else on the same normal map, since those all change together.
2. **Pick a surface type** if `Auto` doesn't suit it. `Auto` is the metal and detail recipe, which suits the most surfaces; there's
   also stone and brick, boards and panels, and bark.
3. **Convert to POM.** It takes under a second, the game keeps running, and it's saved: the texture is on POM from now on, on any
   map that uses it.
4. Set **Depth** with the slider. **Invert** makes carved parts stand out instead, with flat parts staying where they are. **Bake
   again** or changing the surface type redoes it, **Remove POM** puts it back.

**Quality** (Low to Ultra, then Extreme and Insane) sets how many height samples a pixel takes, for the relief and for its shadows. **Fade out
distance** sets where, in world units, POM starts to flatten and where it's gone. **Crevices** sets how much gaps darken in any light,
and **Relief** how much the relief shadows itself. All four apply to every texture, and take a moment to apply when you let go of
the slider: each texture's shaders are remade.

**PBR** has its own section lower in the window, and these show as you drag, on everything lit:

| Slider | What it does |
|---|---|
| Highlights | How bright lights show on surfaces |
| Width | How wide highlights spread: 1 is what the game's textures say |
| Reflections | How strong reflections are |
| On screen | How much what's on screen replaces the game's blurry reflection maps. Past 1 it gets stronger |
| Steps | Samples per reflection at most. More finds thin things and costs GPU time on glossy surfaces |
| Reach | How far away reflected things can be, in world units |

CTRL+click any slider except Quality to type any number, past its maximum too.

`F7` or `Esc` leaves. Converted textures on the current map are listed in the window. `CTRL+F8` toggles POM and `CTRL+F7` toggles
PBR, for comparing.
`POM.ini` (next to the plugin, applied on save) holds all of this: `Enabled`, `ToolKey`, `Quality`, `Steepest`, `FadeStart`,
`FadeEnd`, `Crevices`, `ReliefShadows`, the `[Depth]` multiplier per texture, where below 0 is inverted, and a `[PBR]` section with
`Enabled`, `Highlights`, `Roughness`, `Reflections`, `ScreenReflections`, `ScreenSteps` and `ScreenReach`. `POM.log` lists every
material that got POM, and how PBR went: look for `PBR on:` and `screen reflections: running`.

MP v43 only (`t6mpv43.exe`): addresses are checked by byte signature at start, and on any other exe nothing is installed.

## How it works

**Picking** (`src/Picker.cpp`). Every draw of one frame runs a second time, clipped to the pixel under the cursor, writing no
color, inside an occlusion query. The game's own shaders and depth test decide, so it needs no idea where anything is in the
world: the last draw to reach that pixel is what you see, a hole in an alpha-tested surface lets the click through, and blended
draws are skipped, so glass and decals don't get in the way. So is anything not tested against the scene's depth, like the
fullscreen passes that end every frame. The views bound at `t0` and `t1` then say which material it was. The frame on screen
doesn't change. Each click writes a line to `POM.log` saying how many draws it saw and what reached the pixel.

**Highlighting.** Each draw whose normal map is the selected one runs again with a flat additive shader. The draw hooks are only
installed the first time the tool opens, and do nothing while it's closed.

**Baking** (`src/Bake.cpp`). The tool reads the normal and color maps back from the GPU (`src/Decode.cpp`), so it works from what's
really on screen, a retexture included, and skips mips the streamer hasn't filled. Then, on a thread of its own:
- The world shaders do `N = normalize(n + x * tangent + y * binormal)`, so the normal map holds slopes. Integrating those
  (Frankot-Chellappa, periodic, so what tiles stays tiling) gives the height it was made from.
- Which way the channels run is found from the map itself: only the right way round is the gradient of anything. Custom textures
  can have green flipped or red and green swapped. The shading stays as it was either way.
- Normal maps understate how deep a gap is, so the dark seams of the color map are dropped further. Boards take their height
  from the seams alone, since their normal maps are too flat to integrate: all that's left is compression noise.
- The `.pom` is the normal map's own red and green with the height in blue, which BC5 doesn't have.

**Applying** (`src/Engine.cpp`). Each material whose `normalMap` has a `.pom` gets a copy of its technique set, with patched pixel shaders in all 22 lit techniques, and the material's pointer moves to the copy. The image
behind the normal map gets the `.pom` as its texture. So nothing runs per draw and nothing extra is bound, and the blended
variants of a surface come along by themselves: the game generates those as separate materials on the same normal map.
Depth prepass and shadow maps keep the native techniques.

**The patch** (`src/Dxbc.cpp`) puts `src/Prologue.hlsl` in front of the game's shader and turns its samples of layer 0 into
`sample_d` at the UV the prologue found. Other reads of that UV, like the camo's, use the new one too. Shaders that don't read
the view position (no specular) still receive it, so the patch declares it for them. Fog, lightmaps and other layers
stay the game's own. The prologue takes the direction the texture runs from screen derivatives, not from vertex tangents, so
mirrored, rotated and stretched UVs just work.

**Lighting.** Reading the normal map at the shifted UV already makes the shading follow the relief. On top of that:

- **Crevices.** `.pom` alpha holds how far each point sits below what's around it (`Bake::AddShading`), and the color sample is
  multiplied by it. It costs one sample and works in any light, baked ones included. Files from before this get it the first time
  they load.
- **Relief shadows.** From the point the ray hit, the prologue walks up through the height towards the light, and anything rising
  past the walk is in its way. The sun's color (`cb0[19]`) or a primary light's (`cb0[78]`) is read once per shader, so the
  shader reads it from a temp with that taken off. Placed spots and omnis are multiplied by theirs right after their blocks; four
  omnis share one trace, from where most of their light comes from.
- **Shadows from other things** follow the relief too: shadow lookups use the point the ray hit, not the flat surface. `TEXCOORD4`
  comes from `cb0[48..51]`, which pixel shaders can read as well, so the move can be worked out. The placed spot's lookup is
  worked out in the shader from the position, which gets the same move.

`src/Prologue.hlsl` compiles to one variant per kind of shader (`build-shaders.bat`): which light, whether it has the placed-light
blocks, whether it reads a shadow lookup. The prologue's working registers are the game's own, since it runs before any of the
game's code.

### PBR

BO2's lighting is further along than it looks. Its specular is already normalized, with Schlick's Fresnel on a real F0, gloss in the
alpha of the color, normal or spec map, and F0 from the spec map (0.04 without one). So those maps are the roughness and metal
inputs, and no extra maps are loaded: slots `t4` and `t5` are taken by the layer shaders anyway, and BO2 has no separate roughness or
metallic maps. What's replaced is what made highlights look like hard dots (`src/Pbr.hlsl` has the formulas):

- **GGX.** Blinn-Phong has a bright core and nothing around it. With the usual match, `alpha^2 = 2 / (power + 2)`, all of GGX comes
  down to replacing `pow(NoH, power)` with `1 / (1 + power * (1 - NoH^2) / 2)^2`. The game's `(power + 2) / 8` stays as the
  normalization, so the peak and the energy stay the same and the tail appears.
- **Smith with Schlick-GGX** replaces the game's visibility term, which only knew the light angle and stayed dim at grazing views.
- **Anti-sparkle.** Where the normal changes faster than a pixel can show, the lobe widens for that pixel (Kaplanyan et al.).
- **Omni lights** get a highlight, which the game never gave them.
- **Reflections.** Probes are 64x64 a face, so a smooth surface could never look sharp. Rays march last frame's depth and read last
  frame's color, and the probe fills in what the screen doesn't have. Probe reflections also fade where a bent normal would send
  them under the surface.

**The patch** (`src/Brdf.cpp`) splices those pieces into each lit shader's bytecode. It finds its way by following the game's
registers from constants only that code has: the 13 of `2 ^ (13 * gloss)`, the -9.28 of the environment term, the 1.01 of visibility.
Shader families disagree on which `TEXCOORD` holds what, so the position and normal are traced back to the interpolator they came
from. **Applying** (`src/Pbr.cpp`) swaps the object, code and size in the game's own shader records, so it's global, costs nothing per
draw, and POM's copies are made on top of it. Patched shaders are shared by content, and shaders in zones that stay loaded are kept
between maps.

**Reflections at runtime** (`src/Reflect.cpp`). Constants at `b12`, color at `t16`, depth at `t17`: the game never touches those, so
one bind per scene is enough, and a slot left empty reads as nothing to reflect. The scene opens at the first patched shader and
closes when the render targets change, which is when color and depth are copied, before post effects and the HUD. Passes under half the
screen are extra cameras. Multisampled scenes fall back to probes. Surfaces closer than 32 units skip screen reflections, since a first
person gun has its own field of view. The constants are offsets from 1, so an unbound buffer means the game's own values.

## Limits

- A surface is chosen by its normal map, so POM is per texture, not per object, and the highlight shows what will change.
- A surface needs a normal map of its own. One with none, or on a small generic one shared with unrelated materials
  (`$identitynormalmap`, `global_normal_flat_16x16`), can be selected but not converted, and the window says so.
- Polished surfaces with nothing but grain (`t6_test_wood_varnish_red01_nml`) convert fine and show almost nothing: there's no
  structure to raise.
- If the big mips of a texture aren't streamed in yet, it bakes from what's there. The window shows the size; get closer and use
  Bake again.
- It moves what a pixel shows, not the pixel: silhouettes, depth and collision stay flat, and things standing on the surface
  meet it at the geometry.
- The relief can't cast shadows onto other things: shadow maps come from the real, flat geometry.
- Surfaces lit only by baked light get crevices but no relief shadows: those shaders have no light to trace towards.
- Screen reflections only know what's on screen: what's behind the camera or off the edge comes from the probe. They're also one
  frame old.
- There's no per-surface roughness or metal yet, and no loading of custom maps: PBR uses what the game's textures say, and the
  sliders apply to everything.
- PBR omni highlights don't get POM's relief shadows.
- The first map load takes a moment longer while shaders are made, and `POM.log` says how long.

## Files

`deploy.bat` builds the plugin and copies it in. It leaves `Plugins\POM\` alone: that's what's been converted.
`build-shaders.bat` recompiles `src/*.hlsl` into `src/*.h`, which are checked in.

`tools/bake_pom.py` does the same bake in numpy, offline, from OAT's dumped images. The plugin doesn't need it: it's what the
plugin's baker is tested against, and a way to bake without the game. A `.pom` from it goes in `Plugins\POM\`, and saving
`POM.ini` picks it up.

    Unlinker --image-format DDS --include-assets image -o <dump>\?zone? mp_raid.ff
    python tools\bake_pom.py <dump>\mp_raid\images baked --only <normal image> ...

## Tests

`build-test.bat` builds `build\pom_test.exe`, `build\pbr_test.exe` and `build\panel_test.exe`. `--hardware` uses the GPU instead of
WARP. Set `VSROOT` if Visual Studio isn't in Documents, and `REDACTED` for `deploy.bat` if the game is somewhere else. The `.cso` and
`.dds` files the tests take come from an OAT dump of your own game files; none are in this repo.

**`pbr_test.exe --brdf <ps> [spec mapped ps]`**, **`--primary <ps>`** and **`--omni <ps>`** measure what the game's shaders really
output against the formulas. The native shader has to match BO2's own formula first, which shows the test measures the right thing,
then the patched one has to match GGX and Smith. Results were within 0.04%, or under 1% where the test's 8-bit F0 can't be exact.

**`pbr_test.exe --mirror <ps> <prefix>`** stages a mirror floor and a wall with its height written in its color. A reflection has to
show the point geometry says it should, and nothing past the wall's top. It also reports GPU cost.

**`pbr_test.exe --capture`** runs `src/Reflect.cpp` on a staged frame, in the game's formats.

**`pbr_test.exe --all <list>`** runs every shader: it has to patch, be accepted, take POM on top, stay finite under random input, and
render exactly what the original renders with nothing specular in the scene.

**`pom_test.exe --pick`** puts the picker on a staged scene: surfaces behind each other, an alpha-tested hole, glass, a draw it
doesn't know, a material with no normal map, and fullscreen passes after the scene, as every real frame has. It has to find
what's visible at the pixel and leave the frame exactly as it was, and
the highlight has to change the selected surface's visible pixels and nothing else.

**`pom_test.exe --bake <reference .pom> <folders of .dds>`** compares the plugin's decoders and baker with `tools/bake_pom.py` on
real textures. **`pom_test.exe --read <.dds>`** reads a texture back through the GPU as the tool does.

**`panel_test.exe clicks`** checks each button of the window reports what it should, and that none do while busy or refused.
**`panel_test.exe <state> <out.ppm>`** draws the window to look at.
**`pom_test.exe --ray`** runs the prologue alone against pure geometry: flat heights on floors, walls and slanted planes,
with plain, mirrored, rotated and stretched UVs, where the hit is a line/plane intersection. Stripes check it stops at the
first surface it meets, against a much finer march on the CPU.

**`pom_test.exe --shadow`** runs the sun's relief shadow alone over a height made of walls, inverted too, against the same walk on
the CPU in far finer steps.

**`pom_test.exe --shaders <list>`** takes one `.cso` path per line (OAT dumps them with
`--include-assets material,techniqueset`). Every shader must patch and be accepted, and must render: what the original
renders at depth 0, what the original renders at height 1 everywhere, and at height 0 what the original renders with layer 0
moved by the expected shift. The same comparison with the opposite shift must differ, or the check would prove nothing. With
crevices and relief shadows on, a surface with no relief must still render the same, and a moved shadow lookup must equal the
original's moved by hand. Only the 52 sun shaders can show that one in the test's scene; the other 208 use the same rewrite, but
their random lights don't reach the surface.

**`pom_test.exe --picture <ps> <color.dds> <normal.dds> <.pom> <prefix> [floor|wall] [scale] [mode]`** renders real textures through
the game's shader without and with POM, as `.ppm` files, and times both. Mode `pbr` compares native with PBR, with the sun ahead.

Those cover the bake, the shaders, the picker and the window. The rest can only run in game: the hooks, the key and the frozen
camera (`src/Overlay.cpp`), the material and shader swaps (`src/Engine.cpp`, `src/Pbr.cpp`), and finding the real scene for screen
reflections. `POM.log` says what they did.
