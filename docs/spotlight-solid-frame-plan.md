# Spotlight solid frame - plan

> **Ported into this tree on 2026-10-16** from `Shadow_Source/`, the older copy
> of the repository this was built in. The measurements in sections 8 and 9 were
> taken on that tree, and the port renders byte-identically to it (16 shadow
> scenes; see `progress-log.md`, 2026-10-16). The code was regrouped on the
> way in, not changed: the shader's shadow is one THE SHADOW section of
> `spotlight.frag`, entered from `main()` through one call, `spotShadowCover`
> (the `SPOT_FILL` lift included), and the CPU side is one THE SHADOW CASTER
> block plus one method group of `SpotlightRenderer`. Where this document
> names a line that has since moved, section 4.7 of
> [`spotlight-renderer.md`](spotlight-renderer.md) - "Where the code lives" -
> is current.

> **Changed since, the same day:** lamps no longer carry a `wallDistance` of
> their own - every lamp stands `SPOT_LAMP_WALL_DISTANCE` (150) px off the wall,
> since only the distances' ratios reach the picture - and
> `ShadowCaster::darkness` / `color` darken the backdrop under a shadow, the
> answer to the light-only limitation recorded here. Mentions below of
> `SpotLight::wallDistance` or of lowering a lamp record the tree this was
> written on; section 4.7 of `spotlight-renderer.md` is current.

A spotlight shining down onto a picture in a wooden frame. The frame is a solid
block with real area and depth: light cannot pass through it, it lights only
the parts of the frame the beam reaches, and what lies behind the frame (seen
from the lamp) stays dark. This plan adds that as an option of the existing
shadow caster. It builds on [`spotlight-shadow-plan.md`](spotlight-shadow-plan.md),
which describes the caster as it stood before.

Status: built. Section 8 records what changed while building it, section 9
what was measured.

---

## 1. Is this the clip area? No.

| | Clip area (`SpotlightConfig::clipArea`) | Solid frame (this plan) |
| - | - | - |
| What decides the dark region | A fixed rectangle in app coordinates | The frame AND the lamp: the region behind the frame as seen from the lamp |
| Moves when the lamp moves | No | Yes - the shadow swings with the lamp |
| Depends on how far the lamp is from the wall | No | Yes - a lamp close to the wall throws a long shadow; one at or below the frame's depth throws one that never ends |
| Light between the lamp and the frame | `KEEP_INSIDE` on the frame cuts it off too | Kept - only what is behind the frame is dark |
| The frame itself | `KEEP_OUTSIDE` on the frame darkens it | Lit wherever the beam reaches it |
| The picture inside the frame | Same rule as the rest of the area | Partly shaded by the frame's own rails |

No clip setting reproduces it: the clip has no notion of a lamp position, a
depth, or "behind".

## 2. Is this the current shadow caster? Not quite.

The caster today is a **panel**: a flat rectangle floating `wallDistance` px in
front of the wall, with nothing between it and the wall. Three things a solid
wooden frame does that a floating panel cannot:

1. **The shadow is attached to the frame.** A panel's shadow is its outline
   projected onto the wall - a copy pushed away from the lamp, which starts some
   distance down the frame's sides. A block's shadow is everything its whole
   depth projects: the panel's copy AND the frame's own footprint, joined. Under
   a lamp above the frame, the side shadows start at the frame's top corners and
   widen downward (the reference photograph shows exactly this taper).
2. **A lamp close to the wall throws a shadow that never ends.** A lamp no
   further from the wall than the frame is deep sends its light along the wall
   into the frame's side; nothing behind the frame is reached. The panel model
   drops such a lamp's shadow entirely (a lamp between a floating panel and the
   wall really is unshadowed).
3. **The rails shade the picture.** A frame's rails stand proud of the picture
   inside them. Lit from above, the top rail casts a band of shadow across the
   top of the picture.

## 3. Model

Wall at z = 0. The frame's front face at z = d (`ShadowCaster::wallDistance`,
as now). A lamp at z = h (`SpotLight::wallDistance`) over its app position L.

### 3.1 Solid block: shadow on the wall

The frame is the outer rectangle C extruded from z = 0 to z = d. A wall point p
is in shadow when the straight line from p to the lamp passes through the block.
Along that line the app position is `mix(p, L, s)` at height `s * h`, and it is
inside the block's depth while `s <= t`, with

    t = min(d / h, 1)

So p is in shadow when the 2D segment from p to `mix(p, L, t)` meets C - not
only its far end, which is the panel test. For `h <= d`, `t = 1` and the
segment runs all the way to the lamp: the classic 2D shadow, unbounded.

Geometrically the shadow is the convex hull of C and its panel projection
`K * C` about L, `K = 1 / (1 - t)` - the union of every depth's projection.

**Soft edge.** Along the segment, the emitter (a disc of radius
`apertureWidth`, as now) is seen through a window that grows linearly with s.
The visibility is

    v = smoothstep(-1, 1, min over s in [0, t] of u(s)),
    u(s) = sd(mix(p, L, s)) / (apertureWidth * s + a * (1 - s)),
    a = softness / 2

`sd` is the caster's rounded-box SDF. At `s = t` this is exactly the panel's
edge, so far edges match the panel model; near the frame the window shrinks to
`a`, so the shadow is sharp where it touches the frame and softens away from it
(contact hardening).

**Finding the minimum exactly.** `sd` is the signed distance to a convex shape,
so it is convex along any line; `u` is that over a positive linear function, so
it is quasiconvex in s - one minimum, no false ones. A fixed-count
golden-section search finds it (`SPOT_SOLID_SEARCH_STEPS` iterations, one SDF
each, no data-dependent loop). One special case: when the minimum sits at
`s = 0` the line leaves the block immediately - p is on the lit side, next to
the frame - and v is 1. Without that rule the contact feather would draw a thin
dark outline round the lit side of the frame.

### 3.2 Recessed picture: the rails' shadow

`ShadowCaster::frameWidth` is the rail width; the picture is the outer
rectangle inset by it (corner radius reduced by the same amount).
`ShadowCaster::frameDepth` is how far the picture sits behind the rails' front,
so its surface is at `dp = d - frameDepth`.

A point p on the picture is lit when the line from it to the lamp clears the
rails, i.e. when it is still inside the picture opening at the rails' front
height:

    t_in = frameDepth / (h - dp),   q_in = mix(p, L, t_in),   lit iff q_in in the opening

The opening is convex and contains p, so testing the end point is exact. The
penumbra is the emitter projected the same way, `apertureWidth * t_in`, plus the
softness feather. A lamp no further from the wall than the rails' front cannot
see the picture over them at all: `t_in >= 1` puts `q_in` past the lamp and out
of the opening, so the picture is dark - no special case needed.

This applies to panels and blocks alike.

### 3.3 The frame's face

The rails' front is lit wherever the cone reaches it, as the panel's face is
now. A lamp at or below the frame's depth would, strictly, graze the front face
and not light it; this model keeps the face lit there, as the limit of a lamp
just in front of it, because that is what a viewer of a 2D picture expects
("only the frame is lit where the beam hits it").

### 3.4 What does not change

- Only light is removed: the strip bound, the dither, the highlight shoulder
  and the screen blend are untouched.
- `castsShadow` is still the per-lamp gate, `opacity` still scales it, and the
  fill (`ShadowCaster::fill`) works on top of either model unchanged.
- A shadow-casting lamp still pins `resolutionScale` to 1.0.
- `solid = false` and no frame (`frameWidth = 0` or `frameDepth = 0`) is
  exactly today's panel.

## 4. Config

`ShadowCaster` gains three fields, all defaulting to today's behaviour:

| Field | Default | Meaning |
| ----- | ------- | ------- |
| `solid` | `false` | The caster is a block from the wall to `wallDistance`, not a floating panel |
| `frameWidth` | 0 | Rail width in px; the picture is the rectangle inset by it. 0 = no frame |
| `frameDepth` | 0 | How far the picture sits behind the rails' front, px (clamped to `wallDistance`). 0 = flush |

All three join `operator==`.

## 5. Renderer

- `ShadowSolve` gains `solid`, the inner opening (half extent, radius) and
  `hasRecess`.
- `DeriveLampShadow`:
  - panel: as now (a lamp at or behind the face casts nothing);
  - solid: `t = min(d / h, 1)`; a lamp at or behind the face now casts the
    unbounded shadow, except a lamp INSIDE the block (its app position inside
    the footprint, `h <= d`), which casts nothing;
  - recess: `t_in` and its penumbra half width.
- `StripVertex` grows by a `recess[2]` attribute (location 9): `t_in`, half
  width. 22 -> 24 floats.
- New uniforms: `uShadowSolid`, `uShadowSoftness` (the solid search needs the
  feather per sample), `uShadowInner` (inner half extent, radius, recess on/off).
- Shader: `spotShadowMask` becomes "wall part, face part, mixed by the face's
  coverage"; the wall part is the panel test or the solid search, chosen by a
  uniform; the face part is 1 on the rails and the recess test on the picture.
- **The mask is now skipped for a lamp that casts nothing** (a branch on the
  flat weight). With the solid search in it the mask is a dozen SDFs, and every
  lamp in the draw paid for it before. `lit *= 1.0` for those lamps keeps them
  byte-identical.

## 6. C ABI and demos

- `el_effect_set/get_spotlight_shadow_solid(bool)`
- `el_effect_set/get_spotlight_shadow_frame(frameWidth, frameDepth)` - one call
  for the pair, since they describe one frame.
- Both demos, under Shadow Caster: a **Solid** checkbox, **Frame Width** and
  **Frame Depth** sliders, and a **Solid Frame** preset beside Picture Light:
  the same rig with the caster solid, rails 4% of the rect's shorter side, the
  picture 10 px behind them. Lower the lamp's Wall Distance under the caster's
  (29) to see the light stop at the frame. The C++ demo's config dump prints
  the new fields and says when a lamp is grazing a solid caster.

## 7. Verification

Offscreen, with the same throwaway harness as before (single lamp L = (640, 120),
caster 200 x 260 at (540, 220), d = 28, h = 150 unless stated):

| Check | Expected |
| ----- | -------- |
| The seven regression scenes against `main` | byte-identical |
| `solid = false`, no frame: existing shadow probes | unchanged results |
| Solid, lamp in front: far shadow edges | where the panel's are (+/- 1 px) |
| Solid: point just outside the frame's left edge near its top, (538, 230) | dark (panel: lit) - the shadow is attached |
| Solid: lit side next to the frame | no dark outline |
| Solid, `h <= d` (grazing): far below the frame | dark (panel: lamp casts nothing) |
| Recess (`frameWidth` 20, `frameDepth` 10): picture just under the top rail | dark; band ends where `q_in` meets the opening (+/- 1 px) |
| Recess: the rails, and the picture lower down | lit, unchanged |
| A lamp that casts nothing, with solid on | byte-identical to that lamp without a caster |
| Fill invariants (lone lamp fill 1 == fill 0) with solid on | hold |
| Cost: solid against panel, timed | recorded |
| C ABI round trip; full build | pass; no new warnings |

## 8. Built: what changed from the plan

- **An exact early-out in the solid search.** The caster lies inside the
  circle through its corners, so along the segment its SDF is at least the
  segment's clearance from that circle; the window is linear in s, so it is
  widest at one end. Clearance at least that wide puts every `u(s)` at or above
  1, where the smoothstep is already exactly 1 - so the shader returns 1
  without searching. A/B on five scenes (panel, solid, solid with a wide beam,
  grazing, recess) with the early-out compiled out: **byte-identical**. It cut
  the solid frame time by 17% on the timing rig below.
- **The mask is skipped for weight-0 lamps** (section 5) - a branch on a flat
  value. Those lamps are multiplied by nothing at all, so they stay
  byte-identical to the renderer before the caster.
- **One predicate, `LampCastsShadow`,** decides whether a lamp throws a shadow,
  for both the per-lamp bake and the fill's two-pass decision, now that the
  solid rule (grazing lamps cast, lamps inside the block do not) made it too
  subtle for two copies. It needs the caster's SDF on the CPU: `SdRoundBox`,
  the C++ twin of the shader's.
- **A second preset**, Solid Frame, rather than changing Picture Light, whose
  values are recorded in [`spotlight-shadow-plan.md`](spotlight-shadow-plan.md).

## 9. Verification results

Offscreen harness, single lamp L = (640, 120), caster 200 x 260 at (540, 220),
d = 28, h = 150 unless stated. Pixel positions below are pixel indices; a
pixel's centre is index + 0.5.

| Check | Result |
| ----- | ------ |
| Seven regression scenes against `main` (one lamp over two clears, five-lamp fan, clip inside, clip outside, boosted tint, eight lamps at scale 0.5) | byte-identical, colour and alpha |
| Panel, no frame: the earlier shadow probes (edges, umbra, face, opacity, scale pin, lamp behind the face) | all pass, unchanged |
| Fill checks (lone lamp fill 1 == fill 0; two-lamp rig only changes shadows, never exceeds no-caster) | all pass, unchanged |
| Solid, lamp in front: far edges | bottom 562.13 / left 516.50 / right 762.50 - identical to the panel's |
| Solid: attached at the top corners, wide beam | (537, 230) and (743, 230): no caster 24 and 25, panel 24 and 25, solid **0** |
| Solid: the attached side edge at y = 230 | 530.00 at the pixel centre; the line from the lamp through the corner predicts 530.0 |
| Solid: lit side | every pixel above the frame identical to no caster - no outline |
| Solid, grazing (h 20, d 28) | (640, 620) 11 -> **0**, (560, 520) 12 -> **0**; face and the region between lamp and frame unchanged; the same lamp off a panel still casts nothing |
| Recess (`frameWidth` 20, `frameDepth` 10): rail shadow's lower edge | 249.85 at the pixel centre; `q_in` meeting the opening predicts 249.84 |
| Recess: under the top rail / the rail / the picture lower down / the wall shadow | (640, 244) 78 -> **0**; the rest unchanged |
| Solid caster, lamp with `castsShadow` off | identical to no caster |
| Solid, lone lamp, fill 1 against fill 0 | identical |
| C ABI round trip (now 20 checks: defaults, set/get, bad index, null pointers) | 20 / 20 |
| Full build: library, C dylib, both demos | clean, no new warnings |

**Cost.** 1920 x 1080, one lamp (throw 1200) over a 600 x 400 caster that fills
much of its beam, ms per frame with `glFinish`, best of five runs of 300 frames,
Intel Iris Plus Graphics 655:

| | no shadow | panel | solid | solid + frame (40 px rails, 12 px recess) |
|---|---|---|---|---|
| early-out compiled out | 0.39 | 0.45 | 1.28 | 1.36 |
| as shipped | 0.37 | 0.43 | **1.06** | 1.14 |

So the solid search costs about 0.6 ms a frame on this rig and this GPU, and the
frame recess about 0.08 ms more. The panel costs about what no shadow does. The
rig is a heavy case on purpose - a caster filling the beam keeps most fragments
out of the early-out; a smaller frame in a larger beam pays less.

## 10. Limitations, by design

- One caster, one frame, axis aligned. No bevels, no rails of different widths,
  no frame sides lit from the front.
- The face is kept lit for grazing lamps (section 3.3), which is a choice, not
  physics.
- The rails' shadow falls on the picture only; the rails do not shadow each
  other.
- The solid search's cost (section 9) is measured on one integrated GPU only.
- The early-out uses the caster's bounding CIRCLE; a box test would skip more
  fragments of a long, thin caster. Not needed so far.
