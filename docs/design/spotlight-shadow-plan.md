# Spotlight shadow caster and shadow fill - plan and record

> **Merged into this tree on 2026-10-22**, from `source_shadow/` - the tree the
> two notes below describe - and this time kept OUT of the light's files,
> because `SpotlightRenderer` and its shaders are edited by several people. The
> shadow is now its own module: `SpotlightShadow` (`spotlight-shadow.{h,cpp}`),
> `spotlight-shadow.glsl` and `spotlight-shadow-vert.glsl` (injected ahead of
> `spotlight.frag` / `spotlight.vert`, each reached through one call),
> `spotlight-shadow-tuning.h` and `core/shadow-caster.h`. Its per-lamp
> attributes moved out of `StripVertex` into a buffer of their own at
> locations 13-15 (they were 7-9), its three programs are built on first use,
> and the fill's first pass runs in `RenderOffscreen`. Nothing it draws
> changed: 31 scenes (24 with a shadow) byte-identical to `source_shadow`'s
> build, and the 7 without one byte-identical to the tree before the merge
> (`progress-log.md`, 2026-10-22). Where this document names a section,
> function or attribute of the light's files, section 6 of
> [`spotlight-shadow.md`](../guides/spotlight-shadow.md) - "Where the code
> lives" - is current.

> **Ported into this tree on 2026-10-16** from `Shadow_Source/`, the older copy
> of the repository this was built in. The measurements in sections 7 and 8 were
> taken on that tree, and the port renders byte-identically to it (16 shadow
> scenes; see `progress-log.md`, 2026-10-16). The code was regrouped on the
> way in, not changed: the shader's shadow is one THE SHADOW section of
> `spotlight.frag`, entered from `main()` through one call, `spotShadowCover`
> (the `SPOT_FILL` lift included), and the CPU side is one THE SHADOW CASTER
> block plus one method group of `SpotlightRenderer`. Where this document
> names a line that has since moved (the `#ifndef SPOT_FILL` line of section
> 7.2; the shadow's uniforms, now in `uploadShadowUniforms`), the "Where the code
> lives" section of [`spotlight-shadow.md`](../guides/spotlight-shadow.md) is
> current.

> **Changed since, the same day:** lamps no longer carry a `wallDistance` of
> their own - every lamp stands `SPOT_LAMP_WALL_DISTANCE` (150) px off the wall,
> since only the distances' ratios reach the picture - and
> `ShadowCaster::darkness` / `color` darken the backdrop under a shadow, the
> answer to the light-only limitation recorded here (section 1). Mentions below of
> `SpotLight::wallDistance` or of lowering a lamp record the tree this was
> written on; [`spotlight-shadow.md`](../guides/spotlight-shadow.md) is current.

What was added to the spotlight layer so a lamp can light a 2D picture and the
picture throws a shadow on the wall behind it, why each piece is built the way
it is, and how it was verified. Two stages, in the order they were built:

1. **The shadow caster** - one picture, standing off the wall, whose shadow
   each opted-in lamp throws away from itself (sections 1-6).
2. **The shadow fill** - an optional, non-physical look in which the other
   lamps' light washes a lamp's shadow out (section 7), added after a two-lamp
   rig looked wrong in the demo.

The per-field reference is
[`spotlight-shadow.md`](../guides/spotlight-shadow.md). When this document and the headers disagree, the headers under
`lib/include/` win.

---

## 1. Context

The reference was a photograph of a framed picture on a dark wall, lit by a
downlight close to the wall: the picture itself lit, and on the wall a thick
soft shadow under it, thinner bands down both sides, nothing above.

Before this work `SpotlightRenderer` had nothing that stopped light except
`SpotlightConfig::clipArea`, which is a cut, not a shadow - nothing in the
scene casts it and it does not move with the lamp.

Two decisions were taken up front, with the user:

| Decision | Chosen | Rejected, and why |
| -------- | ------ | ----------------- |
| How dark a shadow is | **Light only**: a shadow is the lamp's own light removed, so the umbra shows the bare backdrop | A darkening multiply pass: breaks the layer's light-only contract and its coverage alpha on embedded surfaces |
| Shape of the caster | **Rounded rectangle** in app coordinates, like `ClipArea` | An alpha-mask texture: needs a texture API into the library and a blurred penumbra lookup |

Consequence of the first: over a backdrop brighter than the lamp there is little
light to remove and so little shadow to see. That is documented, not fixed.

---

## 2. The model

Wall at z = 0. The caster's face at z = d (`ShadowCaster::wallDistance`). A lamp
at z = h (`SpotLight::wallDistance`) over its app position L. With
`t = d / h`, the line from a wall fragment p to the lamp crosses the caster's
plane at

    q = mix(p, L, t)

and p is in shadow when q is inside the caster. Consequences that fall out of
that one line:

- **The shadow is the caster scaled by `1 / (1 - t)` about L**, pushed away
  from the lamp. Measured on the reference photo, the band under the frame and
  the bands beside it both give a scale of about 1.21 (t about 0.18) - the
  model fits the photograph, and that ratio set the demo preset.
- **Penumbra from the lamp's size.** The emitter is a disc of radius
  `apertureWidth`; from p it covers a disc of radius `apertureWidth * t` on the
  caster's plane, and `smoothstep` across the caster's SDF stands in for the
  covered fraction. Plus `ShadowCaster::softness` carried from the wall to the
  plane. On the wall the edge is `2 * apertureWidth * t / (1 - t) + softness` px
  wide.
- **The caster's face stays lit**: it is in front of the plane it blocks. The
  shader evaluates a second SDF at p itself, and
  `visibility = 1 - blocked * (1 - onFace)`.
- **A lamp no further off the wall than the caster casts nothing** - it is
  behind the face, and `t >= 1` would turn the shadow inside out.
- **Only removes light.** It is a coverage multiply, so the strip bound
  `SolveConeAcross` solves needs nothing new, and none of the layer's measured
  costs move.

---

## 3. Config surface (`lib/include/core/config.h`)

New struct **`ShadowCaster`**, declared just before `SpotLight`:

| Field | Default | Meaning |
| ----- | ------- | ------- |
| `position`, `width`, `height`, `cornerRadius` | origin, 0 x 0, 0 | The picture, top-left + size in app px |
| `wallDistance` | 0 | How far the face stands off the wall. 0 is flush: no visible shadow |
| `softness` | 1.0 | Extra feather on the wall, px, on top of the lamp's penumbra |
| `opacity` | 1.0 | Fraction of light the caster stops |
| `fill` | 0 | Stage 2 - see section 7. 0 is physical |

New `SpotLight` fields:

| Field | Default | Meaning |
| ----- | ------- | ------- |
| `wallDistance` | 150 | The lamp's distance off the wall. Read by the shadow only - the cone does not change |
| `castsShadow` | `false` | Per-lamp opt-in, the only gate (`ShadowCaster` has no enable) |

`SpotlightConfig::shadowCaster` holds the caster. Every new field is in its
struct's `operator==`.

**Inert by default, and not a trap.** A 0 x 0 caster, or one flush with the
wall, blocks nothing - unlike a zero-size `KEEP_INSIDE` clip, which blanks an
opted-in lamp. Opting a lamp in before the rectangle is set changes nothing.

---

## 4. Renderer (`spotlight-renderer.{h,cpp}`, `spotlight.{vert,frag}`)

### 4.1 Per-lamp data rides as vertex attributes

Same rule the layer already follows - no per-index uniform arrays.
`StripVertex` grew from 17 to 22 floats:

- `shadow[4]` at attribute 7: the lamp's app position, `t`, and the penumbra's
  half width on the caster's plane.
- `shadowWeight` at attribute 8: `ShadowCaster::opacity` for a lamp that casts,
  0 for one that does not.

Only the caster's rectangle is a uniform (`uShadowRect`, `uShadowCorner`).

### 4.2 CPU derivation

- **`DeriveShadow(const ShadowCaster &)`** - centred box, clamped radius,
  clamped opacity, and `casts` (false for a caster that blocks nothing whoever
  looks at it). One function for `Render` and `buildStrips`, as `DeriveClip` is.
- **`DeriveLampShadow(...)`** - the five per-lamp floats. Writes well-formed
  values even for a lamp that casts nothing, because the shader evaluates the
  mask before weighting it.
  - `MIN_SHADOW_HALF_WIDTH` (1e-4): `smoothstep` with equal edges can return
    NaN, and a zero weight does not cancel it - `mix(1, NaN, 0)` is NaN.
  - `MAX_SHADOW_RATIO` (0.99): caps `t`, so a lamp lowered to the caster's
    plane cannot grow the shadow without bound.

### 4.3 Fragment stage

`spotShadowMask(vApp, vShadow)` in `spotlight.frag`: two `sdRoundBox` calls and
two `smoothstep`s. Applied as
`lit *= mix(1.0, spotShadowMask(...), vShadowWeight)` - right beside the clip's
multiply: after the highlight shoulder, before the coverage alpha and the
dither. So the light that survives is the unshadowed light times a fraction, a
fully shadowed fragment writes no coverage, and the dither cannot round it back
up to 1. Reads `vApp`, never `gl_FragCoord`.

### 4.4 Resolution scale pinned

The edge where the lit face meets its own shadow is a one-pixel step, which a
reduced buffer would smear exactly as it smears the clip's edge. So a
shadow-casting lamp pins `resolutionScale` to 1.0, like a clipped lamp:

- `HasClippedLamp` renamed **`HasMaskedLamp`**: clipped OR castsShadow, drawn
  lamps only.
- `LogOnClipPinTransition` renamed **`LogOnScalePinTransition`**, message
  updated.
- `GetClampedSpotScale` and `UsesScaledBuffer` follow through the one
  predicate.

---

## 5. C ABI (`lib/capi/el-effect.{h,cpp}`)

Naming follows the clip's split - `_shadow_<noun>` is the caster (no index),
the others are a lamp's (indexed). Every setter has a getter.

| Function | Field |
| -------- | ----- |
| `el_effect_set_spotlight_shadow_rect(w, h, x, y, cornerRadius)` | caster rectangle, same parameter order as `el_effect_set_geometry` |
| `el_effect_set_spotlight_shadow_distance` | `shadowCaster.wallDistance` |
| `el_effect_set_spotlight_shadow_softness` | `shadowCaster.softness` |
| `el_effect_set_spotlight_shadow_opacity` | `shadowCaster.opacity` |
| `el_effect_set_spotlight_shadow_fill` | `shadowCaster.fill` |
| `el_effect_set_spotlight_casts_shadow(index, bool)` | `SpotLight::castsShadow` |
| `el_effect_set_spotlight_wall_distance(index, px)` | `SpotLight::wallDistance` |

No new enum, so no new `static_assert` in `capi-internal.h`.

---

## 6. Demos (`demo/`, `demo-capi/`)

Both UIs, kept in step:

- **Spotlights > Shadow Caster** block under Clip Area: X, Y, Width, Height,
  Corner Radius, Wall Distance, Softness, Opacity, Fill, a **Match Rect**
  button, and a **Picture Light** preset.
- Per lamp: **Cast Shadow** checkbox and a **Wall Distance** slider.
- The "Held at 1.0" note beside the resolution-scale slider now counts
  shadow-casting lamps.
- `demo/src/ui-controls.h` prints the caster, each lamp's shadow state, and
  warns when a caster casts nothing or a lamp sits behind it.

**Picture Light** matches the caster to the rect and replaces the lamp list with
one warm downlight a rect-height above it: beam 64 deg, throw 1400, spread
falloff 0.6, aperture 22, softness 0.75, intensity 4, bloom 0.6 / 22 px,
2900 K, lamp 160 px off the wall, caster 29 px off, caster softness 6, opacity
1, fill 0. Those were tuned offscreen against the reference photo.

---

## 7. Stage 2: two lamps, and the fill

### 7.1 What the demo showed, and what it was

**First report** ("with two lamps the shadow looks wrong"). Rebuilt offscreen
from the screenshot: the second lamp had `castsShadow` off - lamps added with
"+ Add" default to off - and its default 215 px throw ended before the far
side of the picture, where its shadow would fall. With the box ticked it
changed **0 pixels**; given a long throw, its shadow appeared above and right of
the picture as the model predicts. Not a renderer defect.

**Second report** ("where lamp 2 shines, lamp 1's shadow should be gone").
Measured on the screenshot and on a rebuild with lamp 2's exact settings:
inside lamp 1's shadow band the two-lamp render was **byte-for-byte lamp 2
alone** - lamp 2's light was not being blocked. Where lamp 2 was strong the band
had vanished; where it was weaker than lamp 1 the band remained, lighter. That
is physical: the shadow gets only lamp 2's light, the wall around it gets both.
The user asked for the look anyway, as an option.

### 7.2 Design

`ShadowCaster::fill` in [0, 1], default 0. For each lamp, its shadow coverage
`v` becomes

    v + (1 - v) * fill * smoothstep(0, SPOT_FILL_FULL_RATIO, others / own)

where `own` is the light this lamp would have put here unshadowed and `others`
is the light reaching this point from every OTHER lamp (shadows applied).

Why it needs per-lamp separation, and so cannot be a post-process on the
composite: from the total alone, a lamp's own half-lit penumbra looks like
light from somewhere else, and filling with it would shrink every shadow.

**Two passes**, only while `fill > 0` and some lamp actually throws a shadow
(`UsesFillPass`):

1. The plain program draws every lamp, shadows applied, into **`mFillBuffer`** -
   viewport-sized RGBA8, `GL_NEAREST`, allocated on first use, released by
   `OnConfigChanged` when the predicate goes false.
2. **`mFillShader`** - `spotlight.frag` with `#define SPOT_FILL` - draws the same
   strips onto the caller's target. Each fragment reads the buffer at its own
   pixel through `vApp` (`texelFetch`, y flipped by `uFillHeight`) and takes its
   own share back out of the screen:
   `others = 1 - (1 - R) / (1 - own)`.

Supporting changes:

- **`uploadStripUniforms`** - one function for both programs' shared uniforms,
  so the two passes cannot draw the strips under different values.
- **`GLUtils::WithDefine`** - moved from `neon-renderer.cpp` into
  `util/gl-utils.h` so the neon and the spotlight share one copy.
- **The fill variant is optional.** If it fails to compile, `Initialize` still
  succeeds, an error is logged, and a filled frame draws the physical shadow.
- Never alongside the scaled path: a casting lamp pins the scale to 1.0.
- The plain program keeps the original line under `#ifndef SPOT_FILL`, so
  `fill = 0` is byte-identical to before.

### 7.3 Two things found while tuning it

**The dead zone.** The buffer is 8-bit and dithered, so with nothing else
lighting a pixel, `R` still differs from `own` by up to a step, and the division
amplifies that by `1 / (1 - own)`. A lone lamp read its own rounding as light
from elsewhere and moved its own penumbra by one level in about 500 channels.
`othersPeak` now has `(2 / 255) / (1 - ownPeak)` subtracted before use; after
that a lone lamp at fill 1 is byte-identical to fill 0. (Measured directly:
the largest `|R - own|` on a lone lamp was exactly one step, so the read lands on
the right texel - the dead zone is the fix, not an alignment bug.)

**The knee.** `SPOT_FILL_FULL_RATIO` started at 1.0 (shadow gone only where the
others match the shadowed lamp). On the user's rig a point where lamp 2 gave
about 45% of lamp 0's light kept more than half its shadow at fill 1
(34 -> 48 of an unshadowed 77). At **0.5** that point reads 72. Lower would let a
soft beam's long gaussian tail wash out shadows far outside where the beam
reads as lit. The constant lives in `spotlight-tuning.h` (shader only).

### 7.4 The alternatives that need no code, compared

Same rig, along the band under the picture, red channel, clear 13:

| x | no caster | physical | fill 0.5 | fill 1 | option 1: lamp 2 stronger | option 2: opacity 0.7 |
|---|---|---|---|---|---|---|
| 1100 | 112 | 111 | 111 | 112 | 126 | 111 |
| 1300 | 87 | 65 | 76 | 87 | 99 | 72 |
| 1500 | 77 | 34 | 53 | 72 | 70 | 47 |
| 1700 | 27 | 21 | 24 | 27 | 51 | 23 |
| 1900 | 16 | 16 | 16 | 16 | 39 | 16 |

- **fill 1** closes the band wherever lamp 2 reaches and leaves it where it does
  not (x 1900).
- **Option 1** (beam 40 deg, softness 0.9, intensity 1.8) gets close physically,
  but brightens the whole wall and strengthens lamp 2's own shadow.
- **Option 2** lightens every shadow, including ones no other lamp is near.

---

## 8. Verification

All offscreen, with a throwaway harness linked against the Release library,
using `OffscreenCapture` the way `tools/neon-scale-check` does. The harness is
not in the repository (see section 10).

| Check | Result |
| ----- | ------ |
| 7 existing spotlight scenes (one lamp over two clears, five-lamp fan, clip inside, clip outside, boosted tint, eight lamps at scale 0.5) against `main` | byte-identical, colour and alpha |
| `castsShadow` with an unset caster; a caster with no lamp opted in | byte-identical to the plain lamp |
| Shadow edges against the `1 / (1 - t)` prediction (200 x 260 caster, t = 28 / 150) | within 0.05 px on the bottom and both sides, at pixel centres |
| Umbra | colour 0 and alpha 0 over a transparent clear |
| Picture face, beyond the shadow, above the picture | byte-identical to the unshadowed render |
| `opacity` 0.5 | half the light in the umbra |
| Lamp behind the caster's face | identical to no shadow |
| `resolutionScale` 0.5 with a casting lamp | identical to 1.0 (pinned) |
| Lone lamp, fill 1 against fill 0 | byte-identical |
| Two-lamp rig, fill 1 | 124,364 px changed, 0 outside a shadow, 0 channels more than one level above the no-caster frame |
| C ABI round trip through the dylib (defaults, set/get, bad index, null out-pointer) | 16 / 16 |
| `neon-scale-check check` and `partition` (after the `WithDefine` move) | PASS / PASS (1000 configs, 0 overlap, 0 gap) |
| Full build: library, C dylib, both demos | clean, no new warnings |

---

## 9. Files touched

| Area | Files |
| ---- | ----- |
| Config | `lib/include/core/config.h` |
| Renderer | `lib/include/renderer/spotlight-renderer.h`, `lib/src/renderer/spotlight-renderer.cpp`, `lib/include/renderer/spotlight-tuning.h` |
| Shaders | `lib/shaders/spotlight.vert`, `lib/shaders/spotlight.frag` |
| Shared util | `lib/include/util/gl-utils.h` (`WithDefine`), `lib/src/renderer/neon-renderer.cpp` (now calls it) |
| C ABI | `lib/capi/el-effect.h`, `lib/capi/el-effect.cpp` |
| Demos | `demo/src/debug-ui.cpp`, `demo/src/ui-controls.h`, `demo-capi/src/debug-ui.cpp` |
| Docs | `spotlight-renderer.md` (section 4.7, C ABI table, cheatsheet - now `docs/guides/spotlight-shadow.md`), `review-findings.md` (rename note under I21), `CLAUDE.md`, this file |

No shader file was added, so `lib/CMakeLists.txt` and `shaders.h.in` are
unchanged.

---

## 10. Open items

- **Fill cost is not measured.** Structurally it is every strip rasterised
  twice plus a viewport-sized clear; no GPU timing was taken.
- **The demos were not driven interactively** by the implementer. Shaders,
  passes and the C ABI were verified offscreen; the UI was verified by build and
  by the user's screenshots.
- **"+ Add" lamps default to `castsShadow` off**, which is what made the first
  two-lamp report look like a bug. Proposed and not done: copy `castsShadow` and
  `wallDistance` from the selected lamp when a caster is set.
- **No animatable `wallDistance`** - `SpotlightField` has no entry. Moving a
  lamp already moves its shadow.
- **One flat caster**, parallel to the wall: no frame thickness, no
  self-shadowing, no shadows of other layers, no alpha-mask silhouettes.
  (Frame thickness and the rails' shadow on the picture have since been added
  as options - see [`spotlight-solid-frame-plan.md`](spotlight-solid-frame-plan.md).)
- **The verification harness lives outside the repo.** Committing it as a
  `tools/` target (beside `neon-scale-check`) would turn the checks in section 8
  into a repeatable gate.
