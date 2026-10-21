# Spotlight Shadow Caster - Parameter Reference

Per-parameter behaviour of `SpotlightConfig::shadowCaster` and
`SpotLight::castsShadow`: the picture standing off the wall that throws a
shadow of the lamps opted into it. The rest of the spotlight layer - placing
and aiming a lamp, the beam, the clip area, the resolution scale - is
[`spotlight-renderer.md`](spotlight-renderer.md); this is the shadow's own
reference, kept apart because its code is (section 6).

For why it is built the way it is, and every measurement behind it, see
[`spotlight-shadow-plan.md`](../design/spotlight-shadow-plan.md) (the caster
and the fill) and
[`spotlight-solid-frame-plan.md`](../design/spotlight-solid-frame-plan.md)
(the solid block and the frame). When either disagrees with
[`renderer/spotlight-shadow.h`](../../lib/include/renderer/spotlight-shadow.h)
or [`core/shadow-caster.h`](../../lib/include/core/shadow-caster.h), the
headers win.

---

## 1. The caster

A picture hung slightly off the wall, lit from above by a lamp close to that
wall, throws a shadow: a band under the picture, thinner bands down both sides,
nothing over the top. That is what the caster adds. **`ShadowCaster`** is one
rounded rectangle in app coordinates, standing `wallDistance` px in front of the
wall the cones are drawn on; every lamp that opts in with **`castsShadow`**
stands `SPOT_LAMP_WALL_DISTANCE` (150) px off the same wall - one distance for
the whole rig, so the caster's is the only one to set.

**The model is one line test per fragment.** Wall at z = 0, the caster's face at
z = d, the lamp at z = h over its app position L. The line from a wall fragment
p to the lamp crosses the caster's plane at `q = mix(p, L, t)` with `t = d / h`,
and p is in shadow when q is inside the caster. What falls out is the caster
**scaled by `1 / (1 - t)` about the lamp**, pushed away from it. Measured on the
reference photograph, the band under the frame and the bands beside it both
give a scale of about 1.21 (t about 0.18), which is how the demo preset's
distances were chosen.

**Why the lamps carry no distance.** Everything the shadow draws is a function
of the distances divided by one another - `t = d / h` for the scale, the
penumbra and the solid caster's grazing case, `frameDepth / (h - d +
frameDepth)` for the rails - so scaling every distance by one factor changes no
pixel, and holding `h` at 150 loses nothing a single lamp could do. Each lamp
had its own `wallDistance` until 2026-10-16; with it fixed at its old default,
a rig that never set it renders byte-identically, and so do the demo presets
rescaled to the same ratios (caster 29 of 160 -> 27.1875 of 150). What it gives
up is lamps at different distances in one rig: each still throws its shadow
away from its own position, at the same scale.

**The penumbra is the lamp's own size.** The emitter is taken as a disc of
radius `apertureWidth`; seen from p it covers a disc of radius
`apertureWidth * t` on the caster's plane, and the fraction of that disc the
caster covers is the light p loses (a `smoothstep` across the caster's SDF
stands in for it). On the wall that makes the shadow's edge
`2 * apertureWidth * t / (1 - t) + softness` px wide - the further the picture
stands off the wall relative to the lamp, the longer and softer the shadow.

**The caster's face stays lit.** It is in front of the plane it blocks, so only
the wall around it loses light. A host draws its picture into that rectangle,
under the effect, and gets both the lit picture and its shadow from this layer.

**Light only by default.** A shadow is this layer's light taken away - a
coverage multiply after the highlight shoulder, exactly where the clip's is -
not a darkening of what the host drew. Over a backdrop darker than the lamp it
reads as a shadow (the umbra shows the bare backdrop, alpha 0 over a
transparent clear); over a backdrop already brighter than the lamp there is
little light to remove. `darkness` (below) darkens the backdrop too.

## 2. Fields

| Field | Default | Effect |
| ----- | ------- | ------ |
| `shadowCaster.position`, `.width`, `.height`, `.cornerRadius` | 0, 0 x 0, 0 | The picture's rectangle. 0 x 0 casts nothing. |
| `shadowCaster.wallDistance` | 0 | How far the picture stands off the wall, against lamps 150 px out. 0 is flush: the shadow lies behind it. Larger throws it further and softer; near 150 a panel's shadow leaves the view (at or past it, none), a solid caster's never ends. |
| `shadowCaster.softness` | 1.0 | Extra feather on the wall, px, on top of the lamp's penumbra. |
| `shadowCaster.opacity` | 1.0 | Fraction of the light the picture stops. Below 1 lets some through. |
| `shadowCaster.fill` | 0 | How far the other lamps' light washes a lamp's shadow out. 0 is physical. See "Two lamps" below. |
| `shadowCaster.darkness` | 0 | How far the shadow darkens what is behind the layer. 0 is light only. See "Over a bright backdrop" below. |
| `shadowCaster.color` | black | The colour `darkness` paints the shadow with, linear RGB. |
| `shadowCaster.solid`, `.frameWidth`, `.frameDepth` | `false`, 0, 0 | A solid block, and a frame around a recessed picture - section 4. |
| `SpotLight::castsShadow` | `false` | Per-lamp opt-in. The only gate. |

**Inert by default, and not a trap.** Unlike a zero-size `KEEP_INSIDE` clip,
which blanks an opted-in lamp, a 0 x 0 or flush caster blocks nothing, so
opting a lamp in early changes nothing.

**It changes nothing until a lamp opts in.** Verified offscreen on seven scenes
(one lamp, a five-lamp fan, both clip modes, a boosted tint, an eight-lamp rig
at scale 0.5): byte-identical to the renderer before the caster existed, colour
and alpha. A lamp with `castsShadow` set and no caster, and a caster with no
lamp opted in, are both byte-identical to the plain lamp.

**It matches the projection.** On a 200 x 260 caster under a lamp 100 px above
it (t = 28 / 150), the shadow's half-light edges landed within 0.05 px of the
predicted `1 / (1 - t)` scaling on the bottom and both sides (562.63 against
562.62, 517.00 against 517.05, 763.00 against 762.95, measured at pixel
centres); the picture's face
and everything past the shadow are byte-identical to the unshadowed render;
`opacity` 0.5 lets half the light through; and a lamp behind the caster's face
renders as if it cast nothing.

**It costs nothing in geometry.** A shadow only removes light, so the strips
solved for the lamp still bound it, and none of the cost numbers in section 6
of [`spotlight-renderer.md`](spotlight-renderer.md) move. Per
fragment of a casting lamp a panel adds two rounded-box SDFs and two
`smoothstep`s. A lamp that casts nothing skips the mask altogether, behind a
branch on its flat shadow weight, and renders byte-identically to the plain
lamp.

**It pins `resolutionScale` to 1.0** while a drawn lamp casts, like the clip and
by the same predicate (`HasMaskedLamp`): where the lit face meets its own shadow
the mask steps across one pixel, which a reduced buffer would smear exactly as
it smears the clip's edge.

**Where the code lives** is section 6.

**The demo preset.** "Picture Light", under Spotlights > Shadow Caster in both
demos, matches the caster to the rect and replaces the lamp list with one warm
downlight a rect-height above it (beam 64 deg, throw 1400, spread falloff 0.6,
aperture 22, intensity 4, 2900 K, caster 27.1875 px off the wall - the 29 of a
160 px lamp it was tuned at - softness 6, `darkness` 0.6). To light a real
picture, load one in the Border Color Picker and tick "Show as backdrop".

## 3. Two lamps, and `fill`

Each lamp casts its own shadow, away from itself, and **a shadow only removes
that lamp's light**. So a point in lamp A's shadow still receives every other
lamp's light, exactly - measured on a two-lamp rig, the band under the picture
was byte-for-byte the second lamp alone. That is physical, and it has a
consequence people read as a bug: where lamp B is strong the shadow vanishes,
but where B is weaker than A it only gets lighter, because the wall around it
has A and B while the shadow has only B. (The screen blend already makes that
contrast smaller than an additive sum of the two lamps would.)

**`ShadowCaster::fill`** (default 0) trades that for a look: A's shadow is
washed out by however much light the OTHER lamps put there. For each lamp the
shadow's coverage `v` becomes `v + (1 - v) * fill * smoothstep(0,
SPOT_FILL_FULL_RATIO, others / own)`, where `own` is what that lamp would have
put there unshadowed and `others` is the light reaching the point from every
other lamp - so at `fill` 1 a shadow is gone wherever the others give half the
shadowed lamp's light (`SPOT_FILL_FULL_RATIO`, 0.5 in
[`spotlight-shadow-tuning.h`](../../lib/include/renderer/spotlight-shadow-tuning.h), which
explains why half and not all).

How: a frame with `fill` above 0 and a lamp casting draws the strips **twice**.
The plain program draws every lamp, shadows applied, into a viewport-sized RGBA8
buffer; then a `SPOT_FILL` variant of the same `spotlight.frag` draws them onto
the target, reads that buffer at its own pixel and takes its own share back out
of the screen - `others = 1 - (1 - R) / (1 - own)` - so a lamp can never wash
out its own penumbra. That is why it cannot be a post-process on the composite:
from the total alone, a lamp's own half-lit penumbra looks like light from
somewhere else. A dead zone of two 8-bit steps (scaled by `1 / (1 - own)`, the
division's gain) absorbs the buffer's own rounding and dither; without it a lone
lamp nudged its own penumbra by one level in ~500 channels.

Verified offscreen:

- **`fill` 0 changes nothing**: no second pass, no buffer, and the seven
  regression scenes are byte-identical to the renderer before the caster.
- **A lone lamp cannot fill itself**: `fill` 1 is byte-identical to `fill` 0.
- **It only touches shadows**: on the two-lamp rig, `fill` 1 changed 124,364
  pixels, none of them outside a shadow, and no channel ended up more than one
  level above the same frame with no caster at all - it restores light, it
  never adds any.

What it does to that rig, along the band under the picture (red channel, clear
13), with the two alternatives that need no new code - a stronger second lamp
(option 1: beam 40 deg, softness 0.9, intensity 1.8) and a lighter shadow
everywhere (option 2: `opacity` 0.7):

| x | no caster | physical | fill 0.5 | fill 1 | option 1 | option 2 |
|---|---|---|---|---|---|---|
| 1100 | 112 | 111 | 111 | 112 | 126 | 111 |
| 1300 | 87 | 65 | 76 | 87 | 99 | 72 |
| 1500 | 77 | 34 | 53 | 72 | 70 | 47 |
| 1700 | 27 | 21 | 24 | 27 | 51 | 23 |
| 1900 | 16 | 16 | 16 | 16 | 39 | 16 |

`fill` 1 closes the band wherever the second lamp reaches and leaves it where
it does not (x 1900). Option 1 gets close at x 1500 physically, but brightens
the whole wall and makes the second lamp's OWN shadow (above and right of the
picture) stronger. Option 2 lightens every shadow, including ones no other lamp
is near.

## 4. Solid frame: `solid`, `frameWidth`, `frameDepth`

A wooden frame is not a panel floating in front of the wall: it is a block
from the wall out, and light cannot pass through it. `ShadowCaster::solid`
makes the caster that block. Its shadow is everything its whole depth projects,
so it hangs from the frame (under a lamp above it, the side shadows start at the
top corners and taper, as in the reference photo); and a lamp no further from
the wall than the frame is deep grazes its side and throws a shadow that never
ends - only the frame is lit where the beam reaches it, and nothing behind it.
`frameWidth` and `frameDepth` give the caster rails around a recessed picture,
and the top rail shades the top of the picture when lit from above. All three
default to the panel above. Measured: the solid search costs about 0.6 ms a
frame at 1080p on an Intel Iris Plus 655 for one lamp over a caster filling its
beam; the panel costs about what no shadow does.

| Field | Default | Effect |
| ----- | ------- | ------ |
| `shadowCaster.solid` | `false` | A block from the wall to `wallDistance`, not a floating panel |
| `shadowCaster.frameWidth` | 0 | Rail width, px. The picture is the rect inset by it |
| `shadowCaster.frameDepth` | 0 | How far the picture sits behind the rails' front, px |

The model, the search behind the soft edge, its exact early-out and every
measurement are in [`spotlight-solid-frame-plan.md`](../design/spotlight-solid-frame-plan.md).
Both demos have a "Solid Frame" preset beside "Picture Light".

What it does not do: one caster, axis aligned; no bevels, no frame sides lit
from the front, the rails do not shadow each other, no shadows of other layers,
and no animatable `wallDistance` (`SpotlightField` has no entry for it yet -
moving the lamp already moves the shadow).

## 5. Over a bright backdrop: `darkness`, `color`

Light only, the contrast between the lit wall and the umbra is
`L * (1 - backdrop)`: it vanishes as the backdrop brightens, and over a
transparent surface the umbra writes alpha 0, so a compositor shows what is
behind it untouched. Measured with one lamp over a caster, red channel, the
lit wall under the shadow against the umbra:

| backdrop | lit wall | umbra, `darkness` 0 | 0.6 | 1.0 |
| -------- | -------- | ------------------- | --- | --- |
| 0.10 | 53 | 25 | 15 | 9 |
| 0.50 | 143 | 128 | 78 | 44 |
| 0.85 | 222 | 217 (5 levels of contrast) | 132 | 75 |
| transparent | 31, alpha 31 | 0, alpha 0 | 0, alpha 100 | 0, alpha 166 |

(On the brighter "Picture Light" preset the umbra over 0.85 reads 217, 87 and 0:
its beam is past the full reach there.)

**How.** With `darkness` above 0, a pass BEFORE the light draws the casting
lamps' strips through a `SPOT_SHADOW_DARKEN` variant of `spotlight.frag`,
which writes `(color * D, D)` under premultiplied OVER, `D = darkness * (1 -
visibility) * smoothstep(0, SPOT_SHADOW_FULL_REACH, lamp light)`: the backdrop
under the shadow becomes `backdrop * (1 - D) + color * D`, and its alpha gains
`D`, so a video plane behind the surface darkens as well. The lamp's own light
weights it - no light, no shadow, and the shadow fades out with the beam
(`SPOT_SHADOW_FULL_REACH` 0.25, which also keeps it inside the strip). The
light pass then screens every lamp on top as before, so a second lamp still
lightens the first one's shadow, and `fill` lifts the darkening with the
shadow. It darkens whatever the framebuffer already holds there.

Verified offscreen: `darkness` 0 is byte-identical to before on every scene
(no pass runs); at 0.6, no pixel outside a shadow changes on any backdrop; a
lone lamp at `fill` 1 equals `fill` 0; scale 0.5 is pinned to 1.0; a lamp with
`castsShadow` off equals no caster; swapping two lamps moves at most 2 levels
(1 at `darkness` 0 - the 8-bit rounding between draws). Costs one more raster
of the casting lamps' strips; its two programs are compiled the first frame it
is used.

---

## 6. Where the code lives

The spotlight renderer and its shaders are edited by several people, so the
shadow is a module of its own and enters the light's files only at single,
marked hooks. A change to the shadow touches only the files in the first table;
a change to the light needs no edit to the shadow unless it moves one of the
hooks in the second.

| File | What it holds |
| ---- | ------------- |
| [`renderer/spotlight-shadow.h`](../../lib/include/renderer/spotlight-shadow.h), [`spotlight-shadow.cpp`](../../lib/src/renderer/spotlight-shadow.cpp) | `SpotlightShadow`: the CPU derivation (`ShadowSolve`, `DeriveShadow`, `LampCastsShadow`, `DeriveLampShadow`, `AnyDrawnLampCasts`, `UsesFillPass`, `UsesDarkenPass`) and every GL object the shadow owns - its attribute buffer, the fill buffer, the three program variants (built on the first frame that needs each), the casting lamps' vertex runs. Also `SpotShadowHost`, the contract below. |
| [`spotlight-shadow.glsl`](../../lib/shaders/spotlight-shadow.glsl) | The fragment half: the model, the per-lamp varyings, the caster's uniforms, the panel, solid and recess tests, the `SPOT_FILL` lift and the `SPOT_SHADOW_DARKEN` output. Injected ahead of `spotlight.frag` (`@SPOTLIGHT_SHADOW@`); reaches nothing of it but `sdRoundBox`, through a prototype. |
| [`spotlight-shadow-vert.glsl`](../../lib/shaders/spotlight-shadow-vert.glsl) | The vertex half: the three per-lamp attributes and `spotShadowForward`. Injected ahead of `spotlight.vert` (`@SPOTLIGHT_SHADOW_VERT@`). |
| [`spotlight-shadow-tuning.h`](../../lib/include/renderer/spotlight-shadow-tuning.h) | `SPOT_LAMP_WALL_DISTANCE`, `SPOT_SHADOW_FULL_REACH`, `SPOT_FILL_FULL_RATIO`, `SPOT_SOLID_SEARCH_STEPS`, and the attribute locations `SPOT_SHADOW_ATTRIB_*` (13-15). Injected into both spotlight shaders (`@SPOTLIGHT_SHADOW_TUNING@`). |
| [`core/shadow-caster.h`](../../lib/include/core/shadow-caster.h) | `ShadowCaster`, included by `config.h`. |
| [`gl/vertex-buffer.h`](../../lib/include/gl/vertex-buffer.h) | `VertexBuffer`, a VBO feeding attributes of a `VertexArray` it does not own - how the shadow's attributes reach the light's strip VAO. |

| Hook | Where | What it calls |
| ---- | ----- | ------------- |
| Fragment | `spotlight.frag` `main()`, right after the clip | `spotShadowLight` (or `spotShadowDarken` under `SPOT_SHADOW_DARKEN`) |
| Vertex | `spotlight.vert` `main()` | `spotShadowForward()` |
| Config | `config.h` | `SpotLight::castsShadow`, `SpotlightConfig::shadowCaster`, their `operator==` lines, the `#include` |
| Scale pin | `GetClampedSpotScale` / `LogOnClipPinTransition`, via `HasMaskedLamp` | `SpotlightShadow::PinsResolutionScale` |
| Strip buffer | `ensureBuffer` | `AttachAttributes` |
| Strip rebuild | `buildStrips` (records each lamp's run in `mLampStrips`) | `OnStripsBuilt` |
| Config change | `OnConfigChanged` | `OnConfigChanged` (the fill buffer's lifetime) |
| Strip uniforms | `uploadStripUniforms` | `UploadUniforms` |
| Phase 1 | `RenderOffscreen` | `RenderOffscreen` (the fill's first pass) |
| Phase 2 | `Render`, the direct path | `RenderBeforeLight` (the darkening), then `RenderLight` (the fill, in place of `drawLamps`) |

**The contract, `SpotShadowHost`.** What the shadow needs of the light,
implemented privately by `SpotlightRenderer`: `UploadStripUniforms` (the
light's own projection, dither, clip and shadow uniforms, onto any of the
shadow's programs), `UseLightBlend` (the layer's screen), `DrawLamps` (the plain
light pass, for the fill buffer) and `DrawStrips` (a range of the strip VAO).
So the shadow's variants always draw the strips under exactly the light's
values: change how the light uploads a uniform or blends, and every shadow pass
follows with nothing in the shadow to edit.

**The per-lamp attributes ride in a buffer of their own** (7 floats a vertex),
bound into the light's strip VAO at locations 13-15 - the top of the 16 every
target guarantees - so `StripVertex` carries none of them and the light's
attributes can grow from 7 upwards. `buildStrips` hands the shadow each drawn
lamp's run of vertices (`SpotLampStrip`, with the aperture the light's solve
floored) and the shadow writes its attributes over those runs.

**The variants are built from the light's own source** at run time
(`#define SPOT_FILL` / `SPOT_SHADOW_DARKEN` inserted after the `#version`
line of `SPOTLIGHT_FRAG_SRC`), not as sources of their own in `shaders.h.in`,
so a chunk added to the light's program can never be missing from them.

---

## 7. C ABI

Same conventions as the rest of the spotlight's ABI (see
[`spotlight-renderer.md`](spotlight-renderer.md) section 7): every setter has a
getter, every float is checked finite. The naming follows the clip's split:
`..._shadow_<noun>` is the caster (no index), `..._casts_shadow` is a lamp's
(indexed). `el_effect_set_spotlight_shadow_rect` takes the same five
parameters in the same order as `el_effect_set_geometry`, so a host can forward
its rect straight in.

| Function | Field |
| -------- | ----- |
| `el_effect_set_spotlight_casts_shadow` | per-lamp `castsShadow` |
| `el_effect_set_spotlight_shadow_rect` | `shadowCaster.width`, `.height`, `.position.x`, `.position.y`, `.cornerRadius` |
| `el_effect_set_spotlight_shadow_distance` | `shadowCaster.wallDistance` |
| `el_effect_set_spotlight_shadow_softness` | `shadowCaster.softness` |
| `el_effect_set_spotlight_shadow_opacity` | `shadowCaster.opacity` |
| `el_effect_set_spotlight_shadow_fill` | `shadowCaster.fill` |
| `el_effect_set_spotlight_shadow_darkness` | `shadowCaster.darkness` |
| `el_effect_set_spotlight_shadow_color` | `shadowCaster.color` |
| `el_effect_set_spotlight_shadow_solid` | `shadowCaster.solid` |
| `el_effect_set_spotlight_shadow_frame` | `shadowCaster.frameWidth`, `.frameDepth` |

---

## 8. Interaction cheatsheet

| Want | Turn |
| ---- | ---- |
| A picture that throws a shadow | `SpotLight::castsShadow` + `spotlight.shadowCaster.*` (set `wallDistance` > 0) |
| A longer, softer shadow | raise `shadowCaster.wallDistance` |
| A softer shadow edge only | `apertureWidth` (the emitter's size), or `shadowCaster.softness` |
| A shadow that is not fully dark | `shadowCaster.opacity` below 1 |
| No shadow where a second lamp shines | `shadowCaster.fill` (1 = gone where the others give half the light; two passes) |
| A shadow that shows over a bright backdrop or a video | `shadowCaster.darkness` (0.6 is a natural start), `shadowCaster.color` |
| A solid frame light cannot pass through | `shadowCaster.solid`; for "nothing behind it is lit", `shadowCaster.wallDistance` 150 or more |
| Rails that shade the picture inside | `shadowCaster.frameWidth` + `shadowCaster.frameDepth` |
