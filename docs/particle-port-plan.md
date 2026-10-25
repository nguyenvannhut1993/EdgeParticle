# Port the particle layer from `OldSource/`

**Status: done, uncommitted (2026-10-16).** This is the record, file by file,
of the code that differs from the older tree in `OldSource/`: what was copied
verbatim, and every hunk that was changed or added, with the reason. Applying
steps 1 to 10 in order to a tree without particles reproduces the port.

| file | against `OldSource/` | why |
| ---- | -------------------- | --- |
| `lib/shaders/particle.vert`, `particle.frag`, `particle-latch.vert` | identical | - |
| `lib/capi/el-particle.h`, `el-particle.cpp`, `csharp/ElParticle.cs` | identical | - |
| `lib/include/core/config.h` (particle structs) | identical code, 2 doc comments | the neon's buffer now exists at every scale |
| `lib/include/renderer/particle-renderer.h` | 40 diff lines | layer identity, region map, docs |
| `lib/src/renderer/particle-renderer.cpp` | 31 diff lines | colour source at every scale, region map |
| `lib/shaders/particle-latch.frag` | 22 diff lines | region map |
| `lib/include/renderer/neon-renderer.h`, `neon-renderer.cpp` | 3 read accessors, 33 lines added, 0 changed | the particles read `mScaledBuffer` |
| `base-renderer.h`, `edge-lighting.{h,cpp}` | added | the tree's layer system, which `OldSource/` did not have |
| C ABI headers, `lib/CMakeLists.txt`, `shaders.h.in` | added, as in `OldSource/` | registration and build |
| `demo/`, `demo-capi/` | copied, 4 text edits, 2 layer names | - |

## Context

Three things in the current tree did not exist in `OldSource/`, and they are
the only reasons the particle code differs from it:

1. **The layer system.** `BaseRenderer::GetLayer` is pure virtual, and the core
   builds the stack by `RendererLayer` (`AddRenderer(RendererLayer)`,
   `CreateRenderer`, `GetDefaultLayerOrder`). The C ABI registers through it.
2. **The neon has one path.** `mScaledBuffer` is filled at every
   `resolutionScale`, 1.0 included (the old tree had no buffer at 1.0), and it
   covers a REGION of the frame around the rect, not the viewport
   (`GetBufferRegion` in `neon-renderer.cpp`).
3. **The neon belongs to someone else.** Nothing in how it renders changes; the
   particles get three const read accessors and nothing more.

The rule the colour latch keeps: a newborn copies `mScaledBuffer` at its spawn
point AS STORED, colour and alpha - a faint glow makes a faint particle, a
strong one a strong particle.

## Step 1 - Copy the files that do not change

```sh
cp OldSource/lib/shaders/particle.vert          lib/shaders/
cp OldSource/lib/shaders/particle.frag          lib/shaders/
cp OldSource/lib/shaders/particle-latch.vert    lib/shaders/
cp OldSource/lib/capi/el-particle.h             lib/capi/
cp OldSource/lib/capi/el-particle.cpp           lib/capi/
mkdir -p lib/capi/csharp
cp OldSource/lib/capi/csharp/ElParticle.cs      lib/capi/csharp/
```

Copy these three too, then apply steps 4 to 6 to them:

```sh
cp OldSource/lib/include/renderer/particle-renderer.h   lib/include/renderer/
cp OldSource/lib/src/renderer/particle-renderer.cpp     lib/src/renderer/
cp OldSource/lib/shaders/particle-latch.frag            lib/shaders/
```

## Step 2 - Build

Shaders are embedded at configure time, so each new shader goes in three places
(see `CLAUDE.md`). Same lines as `OldSource/`.

`lib/CMakeLists.txt`:

```diff
     "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-dither.glsl"
+    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.vert"
+    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.frag"
+    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle-latch.vert"
+    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle-latch.frag"
     "${CMAKE_CURRENT_SOURCE_DIR}/include/renderer/neon-tuning.h"
```

```diff
 file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spotlight-blit.frag" SPOTLIGHT_BLIT_FRAG_CONTENT)
+file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.vert" PARTICLE_VERT_CONTENT)
+file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle.frag" PARTICLE_FRAG_CONTENT)
+file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle-latch.vert" PARTICLE_LATCH_VERT_CONTENT)
+file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/particle-latch.frag" PARTICLE_LATCH_FRAG_CONTENT)
```

```diff
     src/renderer/spotlight-renderer.cpp
+    src/renderer/particle-renderer.cpp
     src/util/capture-util.cpp
```

```diff
     capi/el-modulator.cpp
+    capi/el-particle.cpp
     capi/el-deprecated.cpp
```

`lib/shaders/shaders.h.in`, after `SPOTLIGHT_BLIT_FRAG_SRC`:

```cpp
                static const char *const PARTICLE_VERT_SRC = R"shadersrc(@GLSL_VERSION@
@PARTICLE_VERT_CONTENT@
)shadersrc";

                static const char *const PARTICLE_FRAG_SRC = R"shadersrc(@GLSL_VERSION@
@PARTICLE_FRAG_CONTENT@
)shadersrc";

                static const char *const PARTICLE_LATCH_VERT_SRC = R"shadersrc(@GLSL_VERSION@
@PARTICLE_LATCH_VERT_CONTENT@
)shadersrc";

                static const char *const PARTICLE_LATCH_FRAG_SRC = R"shadersrc(@GLSL_VERSION@
@PARTICLE_LATCH_FRAG_CONTENT@
)shadersrc";
```

## Step 3 - Config: `lib/include/core/config.h`

Copy `OldSource/lib/include/core/config.h` lines 1130-1333 (`ParticleMode`,
`ParticleArcConfig`, `ParticleConfig`) in after `SpotlightConfig`, and add the
member and its comparison to `Config`:

```diff
         SpotlightConfig spotlight; ///< Freely placed and aimed cones of light
+        ParticleConfig particle;   ///< Point-sprite sparks / motes, from the neon's arcs or the layer's own
```

```diff
                    lensFlare == o.lensFlare &&
-                   spotlight == o.spotlight;
+                   spotlight == o.spotlight &&
+                   particle == o.particle;
```

The code of the three structs is unchanged. Two doc comments described the old
neon, which kept no buffer at scale 1.0:

```diff
         /// @ref ParticleConfig - for an own arc too, so without this an own
-        /// arc's motes vanish where the neon draws nothing (while
-        /// @c NeonConfig::resolutionScale is below 1).
+        /// arc's motes vanish where the neon draws nothing.
```

```diff
     ///   - the glow of the @ref NeonRenderer attached with
-    ///     @c ParticleManager::SetNeonSource, as DRAWN at the spawn point -
-    ///     colour and coverage, so a particle copies the neon pixel it was
-    ///     born on and vanishes where the neon drew nothing. Only while
-    ///     @c NeonConfig::resolutionScale is below 1: at 1.0 the neon draws
-    ///     straight to the target and keeps no buffer to read.
-    ///   - otherwise (scale 1.0, neon off, @c DebugConfig::opaqueOnly, no
-    ///     source attached), the hue-rotated @c NeonConfig::colorStops ring.
+    ///     @c ParticleManager::SetNeonSource, as DRAWN into its scaled buffer
+    ///     at the spawn point - colour and coverage as stored, so a particle
+    ///     copies the glow it was born on, faint where it is faint and strong
+    ///     where it is strong, and vanishes where the neon drew nothing. That
+    ///     buffer holds the glow around the line, not the line (the neon's
+    ///     edge ring draws it), so a particle is dimmer than the line on
+    ///     screen, and a line with no glow round it
+    ///     (@c NeonConfig::glowRadius near 0) leaves it nothing to copy. At
+    ///     every @c NeonConfig::resolutionScale.
+    ///   - otherwise (neon off, @c DebugConfig::opaqueOnly, no source
+    ///     attached), the hue-rotated @c NeonConfig::colorStops ring.
```

## Step 4 - `lib/include/renderer/particle-renderer.h`

The layer identity, which `BaseRenderer` now requires:

```diff
         virtual void OnConfigChanged(const Config &config) override;
 
+        /// @return @ref RendererLayer::PARTICLES.
+        virtual RendererLayer GetLayer() const override { return RendererLayer::PARTICLES; }
+
```

The latch pass takes the neon's region map instead of a rect centre and a
viewport (why: step 6), so the header needs the map's name:

```diff
     class NeonRenderer;
+    namespace NeonDetail
+    {
+        struct UVMap;
+    }
```

```diff
         /// Write this frame's newborn colours into their @c mColorLatch slots:
         /// @p neon's output buffer for a newborn queued to copy it, its queued
-        /// CPU colour otherwise. @p rectCenter (window px, y up) and
-        /// @p viewport place the spawn points on the output buffer. Retargets
-        /// the framebuffer and viewport, so it restores both before returning.
+        /// CPU colour otherwise. @p neonUV places the spawn points (rect-local
+        /// full-res px) on the output buffer - the neon's own map
+        /// (@ref NeonRenderer::GetOutputUVMap), since the buffer covers a
+        /// region of the frame rather than the viewport. Retargets the
+        /// framebuffer and viewport, so it restores both before returning.
         /// @pre Blending disabled - a latch write replaces the texel.
         /// @pre @p neon is non-null when @c mColorSource is NEON_OUTPUT.
-        void renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
-                             const glm::vec2 &viewport);
+        void renderLatchPass(const NeonRenderer *neon, const NeonDetail::UVMap &neonUV);
```

Doc comments only:

```diff
-        NEON_OUTPUT, ///< The neon's drawn glow (scaled path only): colour AND coverage as drawn.
+        NEON_OUTPUT, ///< The neon's drawn glow, from its scaled buffer under the spawn point.
```

```diff
-    ///   - the DRAWN glow of the neon attached with @ref SetNeonSource, the
-    ///     pixel under the spawn point - colour and coverage exactly as
-    ///     composited, so a mote is a copy of the neon pixel it was born on
-    ///     and is invisible where the neon drew nothing. Scaled path only
-    ///     (@c NeonConfig::resolutionScale < 1): at 1.0 the neon draws
-    ///     straight to the target and keeps no buffer.
+    ///   - the DRAWN glow of the neon attached with @ref SetNeonSource: its
+    ///     scaled buffer (@ref NeonRenderer::HasOutputBuffer), at every
+    ///     resolution scale, read under the spawn point through the neon's own
+    ///     map (@ref NeonRenderer::GetOutputUVMap - the buffer covers a region
+    ///     of the frame, not the viewport) and copied AS STORED, colour and
+    ///     coverage: a faint glow makes a faint mote, a strong one a strong
+    ///     mote, and a mote is invisible where the neon drew nothing. A
+    ///     newborn's latch texel is that buffer value to 8-bit rounding;
+    ///     particle.frag then scales colour and coverage alike by the mote's
+    ///     own sprite and age. That buffer holds the glow around the line
+    ///     rather than the line, which the neon's edge ring draws at full
+    ///     resolution, so a mote is dimmer than the line on screen (about two
+    ///     thirds at the defaults), and a line with no glow round it
+    ///     (glowRadius near 0) leaves it nothing to copy.
```

## Step 5 - `lib/src/renderer/particle-renderer.cpp`

The colour source no longer needs a scale below 1.0: the neon fills its buffer
at every scale now.

```diff
         // Where this frame's newborns take their colour from. The drawn glow
-        // lives in the neon's scaled buffer, which only exists on its scaled
-        // path - neon on, Res Scale below 1. The neon is registered before
-        // this layer, so its Render has already filled the buffer this frame;
-        // the buffer check itself guards the frames it could not (an
+        // lives in the neon's scaled buffer, which the neon now fills at every
+        // Res Scale, 1.0 included (it has one path). The neon is registered
+        // before this layer, so its Render has already filled the buffer this
+        // frame; the buffer check itself guards the frames it could not (an
         // allocation failure, or a config change the neon has not drawn yet).
         // opaqueOnly keeps the buffer but skips the gather that fills it, so
         // it would hand back a stale glow. Anything else: the CPU ring.
         const std::shared_ptr<const NeonRenderer> neon = mNeonSource.lock();
-        const bool neonDrawsToBuffer = config.neon.enable && !config.debug.opaqueOnly &&
-                                       config.neon.resolutionScale < 1.0f;
+        const bool neonDrawsToBuffer = config.neon.enable && !config.debug.opaqueOnly;
         if (neon && neonDrawsToBuffer && neon->HasOutputBuffer())
```

`Render` asks the neon where its buffer sits, and hands that to the latch pass:

```diff
             glDisable(GL_BLEND);
-            renderLatchPass(neon.get(), center,
-                            glm::vec2(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight)));
+            // Where the spawn points land in the neon's buffer: its own map,
+            // for this frame's config and viewport. The buffer covers a region
+            // of the frame, not the viewport, so (center + p) / viewport would
+            // read the wrong texel - by an amount that moves with the rect's
+            // position and size.
+            NeonDetail::UVMap neonUV;
+            if (mColorSource == ParticleColorSource::NEON_OUTPUT)
+            {
+                neonUV = neon->GetOutputUVMap(viewportWidth, viewportHeight, config);
+            }
+            renderLatchPass(neon.get(), neonUV);
         }
```

```diff
-    void ParticleManager::renderLatchPass(const NeonRenderer *neon, const glm::vec2 &rectCenter,
-                                          const glm::vec2 &viewport)
+    void ParticleManager::renderLatchPass(const NeonRenderer *neon, const NeonDetail::UVMap &neonUV)
     {
```

```diff
         if (mColorSource == ParticleColorSource::NEON_OUTPUT)
         {
             neon->BindOutputBuffer(0);
-            mLatchShader.SetUniform("uRectCenter", rectCenter);
-            mLatchShader.SetUniform("uViewport", viewport);
+            mLatchShader.SetUniform("uUVScale", neonUV.scale);
+            mLatchShader.SetUniform("uUVOffset", neonUV.offset);
         }
```

`center` is still computed in `Render`: the particle's own MVP uses it.

## Step 6 - `lib/shaders/particle-latch.frag`

Only WHERE the buffer is read changes. What is written is still the buffer as
stored - `textureLod`, the same bilinear read the neon's blit makes.

```diff
-//   vFromNeon 1 - the neon's DRAWN glow (its scaled output buffer): the pixel
-//                 under the spawn point, exactly as the blit composites it.
-//                 Where the neon drew nothing, coverage is 0 and so is the
-//                 particle. Only ever set while that buffer is bound.
+//   vFromNeon 1 - the neon's DRAWN glow (its scaled output buffer): the pixel
+//                 under the spawn point, as stored - colour AND coverage, so
+//                 a faint glow makes a faint particle and a strong one a
+//                 strong particle. Where the neon drew nothing, coverage is 0
+//                 and so is the particle. Only ever set while that buffer is
+//                 bound.
 //   vFromNeon 0 - vColor, from the CPU: the ring colour at alpha 1, or the
 //                 arc's custom colour, whose alpha is its coverage.
 uniform sampler2D uNeonOutput;
-uniform vec2 uRectCenter; // rect centre in window px (y up)
-uniform vec2 uViewport;   // viewport size in px - the buffer spans it
+// The buffer covers a REGION of the frame, not the viewport: rect-local
+// full-res px -> its uv, the map the neon's blit reads it through
+// (NeonRenderer::GetOutputUVMap).
+uniform vec2 uUVScale;
+uniform vec2 uUVOffset;
```

```diff
     if (vFromNeon > 0.5) {
-        // The same uv the blit samples at this window position. textureLod,
-        // not texture: a 1 px point has no meaningful derivatives.
-        vec2 uv   = (uRectCenter + vSpawnPos) / uViewport;
+        // The same uv, and the same bilinear read, the blit samples at this
+        // point. textureLod, not texture: a 1 px point has no meaningful
+        // derivatives.
+        vec2 uv   = vSpawnPos * uUVScale + uUVOffset;
         fragColor = textureLod(uNeonOutput, uv, 0.0);
```

Why the old formula cannot stay. `GetBufferRegion` places the buffer over a
box around the rect, snapped to the texel grid and rounded up to 16 texels, and
falls back to the whole viewport only on an axis the box would fill anyway.
Measured at 1920 x 1080 with the demo's start-up rect (960 x 540 at 480, 270):
the buffer is 816 x 540 texels over x 158..1790, and `(center + p) / viewport`
reads the rect's top-left corner 86 px off; moved by (200, 100), 234 px off; at
`glowRadius` 30, where the box covers the whole viewport, 0 px. So the old read
was right only sometimes, and wrong by an amount that moved with x, y, width
and height. The texture's size alone cannot place the region: its origin
depends on the neon's private `mScaledOuter` and on that rounding.

## Step 7 - The neon's read accessors

The only change to the neon: three const methods, none of which changes how it
renders. The first two are `OldSource/`'s, verbatim; the third is new.

`lib/include/renderer/neon-renderer.h`, after `GetLayer()`:

```cpp
        // --- Output, for other layers (read only) ---------------------------
        //
        // The reduced buffer, @c mScaledBuffer, as pass 1b (or pass 1c) left
        // it: premultiplied colour + coverage alpha, after the tone map and
        // before the one-sided cut and the cutoffs, which the blit applies.
        // @ref ParticleManager copies it at each spawn point. It holds the
        // glow, not the line: the edge ring draws the filament at full
        // resolution, so pass 1b leaves it out (filamentLit in neon.frag).

        /// Whether the reduced buffer exists - allocated by the first
        /// @ref Render with the layer on, released when it is switched off.
        bool HasOutputBuffer() const { return mScaledBuffer.IsValid(); }

        /// Bind the reduced buffer's texture to unit @p unit (bilinear, as the
        /// blit samples it). Only meaningful while @ref HasOutputBuffer.
        void BindOutputBuffer(GLuint unit) const { mScaledBuffer.BindTexture(unit); }

        /// Where a point lands in the reduced buffer: <tt>uv = px * scale +
        /// offset</tt>, @c px in rect-local FULL-RES px (from the rect's
        /// centre, y up) - the map the blit reads it through. The buffer covers
        /// a region of the frame, not the viewport (GetBufferRegion), so
        /// <tt>windowPos / viewport</tt> reads the wrong texel. For the
        /// @p config and viewport the last @ref Render drew.
        NeonDetail::UVMap GetOutputUVMap(int viewportWidth, int viewportHeight, const Config &config) const;
```

`lib/src/renderer/neon-renderer.cpp`, after `OnConfigChanged`. It maps the
region through the neon's own `GetFieldRegion`, the same region `Render` draws
`mScaledBuffer` over:

```cpp
    NeonDetail::UVMap NeonRenderer::GetOutputUVMap(int viewportWidth, int viewportHeight, const Config &config) const
    {
        // The region Render draws mScaledBuffer over - the field's, which is
        // that buffer's by construction (GetFieldRegion) - mapped from
        // full-res px, as Render maps it for the blit.
        return GetRegionUVMap(GetFieldRegion(config, mScaledOuter, viewportWidth, viewportHeight), 1.0f);
    }
```

## Step 8 - The layer: `base-renderer.h`, `edge-lighting.{h,cpp}`

`OldSource/` had no `RendererLayer`. The particles take bit 4, the same bit
`EL_RENDERER_PARTICLES` held there.

`lib/include/renderer/base-renderer.h`:

```diff
         SPOTLIGHT = 1 << 3,
+        PARTICLES = 1 << 4,
         DEBUG = 1 << 30
```

`lib/src/core/edge-lighting.cpp` - create it, put it in the default order after
every light layer (newborns copy the glow the neon drew THIS frame), and wire
the neon as its colour source whenever both are added by layer:

```diff
 #include "renderer/spotlight-renderer.h"
+#include "renderer/particle-renderer.h"
 #include "renderer/debug-renderer.h"
```

```diff
             return std::make_shared<SpotlightRenderer>();
         }
+        case RendererLayer::PARTICLES:
+        {
+            return std::make_shared<ParticleManager>();
+        }
         case RendererLayer::DEBUG:
```

```diff
             RendererLayer::SPOTLIGHT,
+            // After every light layer: newborn particles copy the glow the
+            // neon drew THIS frame, so they have to follow it.
+            RendererLayer::PARTICLES,
             RendererLayer::DEBUG,
```

```diff
         AddRenderer(renderer);
         // AddRenderer(ptr) drops a late registration that fails to initialise.
-        return mRenderers.size() > before;
+        if (mRenderers.size() == before)
+        {
+            return false;
+        }
+        if (layer == RendererLayer::NEON || layer == RendererLayer::PARTICLES)
+        {
+            connectParticleSource();
+        }
+        return true;
     }
```

```cpp
    void EdgeLightingEffect::connectParticleSource()
    {
        // By layer, then by type: a host's own renderer could claim either
        // layer without being the class this wires.
        std::shared_ptr<NeonRenderer> neon;
        std::shared_ptr<ParticleManager> particles;
        for (const auto &renderer : mRenderers)
        {
            if (renderer->GetLayer() == RendererLayer::NEON)
            {
                neon = std::dynamic_pointer_cast<NeonRenderer>(renderer);
            }
            else if (renderer->GetLayer() == RendererLayer::PARTICLES)
            {
                particles = std::dynamic_pointer_cast<ParticleManager>(renderer);
            }
        }
        if (neon && particles)
        {
            particles->SetNeonSource(neon);
        }
    }
```

`lib/include/core/edge-lighting.h` declares it (private, after
`refreshActiveConfig`) and documents the order and the wiring:

```diff
         void refreshActiveConfig();
+        /// Hand the registered particle layer the registered neon as its
+        /// colour source, when both are there. Leaves the source alone with
+        /// no neon registered, so a source a host chose is not cleared.
+        void connectParticleSource();
```

```diff
-        /// flare, spotlight, debug. The debug overlays go last because they
-        /// annotate the layers under them.
+        /// flare, spotlight, particles, debug. The particles follow the neon
+        /// because newborns copy the glow it drew THIS frame; the debug
+        /// overlays go last because they annotate the layers under them.
```

```diff
         /// Creates the renderer for @p layer and registers it through
         /// @ref AddRenderer, with the same initialise-if-late contract.
+        ///
+        /// Also wires the two layers that talk to each other: once both
+        /// @ref RendererLayer::NEON and @ref RendererLayer::PARTICLES are
+        /// registered, in either order, the particles take the neon as their
+        /// colour source (@ref ParticleManager::SetNeonSource). A host that
+        /// registers renderers by pointer wires them itself.
```

```diff
-        /// the framebuffer, which is exactly what the order decides.
+        /// the framebuffer, which is exactly what the order decides - with one
+        /// exception, the particles, whose newborns copy the neon's glow from
+        /// its own buffer: moved under the neon, they read the previous
+        /// frame's.
```

## Step 9 - C ABI

`lib/capi/el-effect.cpp` needs NOTHING. In `OldSource/`,
`el_effect_init_with_renderers` built the layer and wired it itself:

```cpp
        if (rendererMask & EL_RENDERER_PARTICLES)
        {
            LOG_I("registering ParticleManager");
            auto particles = std::make_shared<EdgeLighting::ParticleManager>();
            particles->SetNeonSource(neon);
            effect->impl->AddRenderer(particles);
        }
```

Now every layer goes through `AddRenderer(RendererLayer)`, which creates it
(step 8) and wires the source (`connectParticleSource`).

`lib/capi/el-types.h` - the flag, as in `OldSource/`:

```diff
         EL_RENDERER_SPOTLIGHT = 1 << 3,  /**< Freely placed and aimed cones of light.
                                           *   Light only - no backdrop, no fixtures. */
+        EL_RENDERER_PARTICLES = 1 << 4,  /**< Point-sprite motes from the neon's arcs or
+                                          *   the layer's own - see el-particle.h. With
+                                          *   EL_RENDERER_NEON registered too they can copy
+                                          *   the neon's drawn glow; without it they take
+                                          *   its colour ring. Starts switched off. */
```

```diff
-     *           A content layer added later takes the next free bit above
-     *           @ref EL_RENDERER_LENS_FLARE and joins @ref EL_RENDERER_ALL
-     *           without a further ABI change, leaving the debug bit alone. */
+     *           A content layer added later takes the next free bit above the
+     *           last content layer (@ref EL_RENDERER_PARTICLES today) and joins
+     *           @ref EL_RENDERER_ALL without a further ABI change, leaving the
+     *           debug bit alone. */
```

`lib/capi/capi-internal.h` - the include and the mode asserts as in
`OldSource/`, plus the layer's own assert next to the other layers':

```diff
 #include "renderer/spotlight-renderer.h"
+#include "renderer/particle-renderer.h"
 #include "animation/neon-animations.h"
```

```diff
+static_assert(static_cast<int>(EdgeLighting::ParticleMode::MOVEMENT) == EL_PARTICLE_MODE_MOVEMENT);
+static_assert(static_cast<int>(EdgeLighting::ParticleMode::HEAD_SPARKS) == EL_PARTICLE_MODE_HEAD_SPARKS);
```

```diff
 static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::SPOTLIGHT) == EL_RENDERER_SPOTLIGHT);
+static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::PARTICLES) == EL_RENDERER_PARTICLES);
 static_assert(static_cast<uint32_t>(EdgeLighting::RendererLayer::DEBUG) == EL_RENDERER_DEBUG);
```

`lib/capi/edge-lighting-capi.h` - as in `OldSource/`:

```diff
- * uses only fixed-width scalar types + opaque pointers, and never leaks a
+ * uses only fixed-width scalar types + opaque pointers - plus one plain struct
+ * of them, @ref el_particle_config_t - and never leaks a
```

```diff
 #include "el-modulator.h"
+#include "el-particle.h"
```

`lib/capi/el-effect.h` - the default order, in two doc comments:

```diff
      *           order, bottom first: neon, droplets, lens flare, spotlight,
-     *           debug.
+     *           particles, debug.
```

```diff
      *           they go in the default order (neon, droplets, lens flare,
-     *           spotlight, debug - bottom first) whatever bits are combined.
+     *           spotlight, particles, debug - bottom first) whatever bits are
+     *           combined.
```

## Step 10 - Demos

`demo/src/main.cpp` - the include, the `OldSource/` block (lines 112-117) with
its comment updated for the scale, the registration, the readout and the `K`
key:

```diff
 #include "renderer/spotlight-renderer.h"
+#include "renderer/particle-renderer.h"
 #include "animation/neon-animations.h"
```

```diff
     auto dropletsRenderer = std::make_shared<EdgeLighting::DropletsRenderer>();
+    // One manager for every arc's motes - an emitter per arc, one draw.
+    auto particleManager = std::make_shared<EdgeLighting::ParticleManager>();
+    // Newborn particles copy the neon's drawn glow at their spawn point (the
+    // CPU colour ring when there is none) - which is why the particles are
+    // registered after the neon below.
+    particleManager->SetNeonSource(neonRenderer);
```

(`OldSource/` read: "...at their spawn point when Neon > Res Scale is below 1
(the CPU colour ring otherwise) - which is why...".)

```diff
     gEffect->AddRenderer(spotlightRenderer);
+    // Sparks sit on top of every light layer, still under the debug overlays.
+    gEffect->AddRenderer(particleManager);
     gEffect->AddRenderer(debugRenderer);
 
+    // The Particles section reads the live count back for its readout.
+    debugUI.SetParticleManager(particleManager);
+
```

```diff
+    case GLFW_KEY_K:
+    {
+        config.particle.enable = !config.particle.enable;
+        break;
+    }
     case GLFW_KEY_O:
```

`demo/src/debug-ui.h` - the same four additions as `OldSource/`:

```diff
     class AnimationManager;
+    class ParticleManager;
 }
```

```diff
     void SetLastRenderTimeMs(float ms) { mLastRenderTimeMs = ms; }
 
+    /// The registered particle layer, read back for the live-count readout.
+    /// Optional: without it the Particles section just omits the count.
+    void SetParticleManager(std::shared_ptr<const EdgeLighting::ParticleManager> manager)
+    {
+        mParticleManager = std::move(manager);
+    }
+
```

```diff
     void buildSpotlightSection(EdgeLighting::Config &cfg);
+    void buildParticleSection(EdgeLighting::Config &cfg);
```

```diff
     int mSpotlightSelected = 0;
 
+    // --- Particles ---
+    std::shared_ptr<const EdgeLighting::ParticleManager> mParticleManager;
+    /// Which arc the Particles section is editing. Clamped to the listed arcs
+    /// every frame, so removing one cannot leave it dangling.
+    int mParticleSelected = 0;
+
```

`demo/src/debug-ui.cpp` - copy `DebugUI::buildParticleSection` from
`OldSource/demo/src/debug-ui.cpp` lines 1105-1359 in after
`buildSpotlightSection`, then two edits for the scale:

```diff
-        // needs Neon > Res Scale below 1 and the neon drawing (not off, not
-        // Debug > Opaque only) - so it is worth seeing here.
+        // needs the neon drawing (not off, not Debug > Opaque only) - so it is
+        // worth seeing here.
```

```diff
-                                "(needs Neon on and Neon > Res Scale below 1)");
+                                "(needs Neon on and not Debug > Opaque only)");
```

plus the include, the call, and the layer's name for the Layer order section
(which `OldSource/` did not have):

```diff
 #include "renderer/neon-tuning.h"
+#include "renderer/particle-renderer.h"
 #include "ui-controls.h"
```

```diff
     buildSpotlightSection(cfg);
+    buildParticleSection(cfg);
     buildColorPickerSection(cfg);
```

```diff
+        case EdgeLighting::RendererLayer::PARTICLES:
+        {
+            return "Particles";
+        }
         case EdgeLighting::RendererLayer::DEBUG:
```

`demo/src/ui-controls.h` - the help line and the dump's doc comment, and the
`OldSource/` dump block (lines 453-541, from `// Particles. One emitter per
arc` up to `const auto &d = config.debug;`) before the debug section, with one
string updated:

```diff
         std::cout << "  [D]            - Toggle Water Droplets Pane\n";
+        std::cout << "  [K]            - Toggle Particles\n";
         std::cout << "  [W]            - Toggle Winding (CW / CCW)\n";
```

```diff
-    /// spotlight, debug - in registration order, enabled or not. A disabled
+    /// spotlight, particles, debug - in registration order, enabled or not. A disabled
```

```diff
-                std::cout << "the neon's (drawn glow below Res Scale 1, else the colour ring)\n";
+                std::cout << "the neon's (its drawn glow, else the colour ring)\n";
```

`demo-capi/src/debug-ui.cpp` - only the layer's name; as in `OldSource/`,
the C ABI demo has no Particles section:

```diff
+        case EL_RENDERER_PARTICLES:
+        {
+            return "Particles";
+        }
         case EL_RENDERER_DEBUG:
```

## Verification

- **Build**: the static library, the C dylib and both demos, with no new
  warning.
- **The latch against the buffer, texel for texel.** A scratch harness, after
  every frame: read back the latch pass's VBO (still bound to
  `GL_ARRAY_BUFFER` after the particles render: slot, spawn x / y and the
  copy-the-neon flag of each newborn), the colour latch (texture unit 1) and
  `mScaledBuffer` (`BindOutputBuffer` + `GetOutputUVMap`). For every newborn
  that copies the neon, its latch texel against the buffer's bilinear sample
  at its spawn point: 15,795 newborns over nine scenes (the defaults; the rect
  moving and resizing every frame; intensity 0.3; a half arc; bloom 1.5 at
  intensity 2; glowRadius 1; an OUTSIDE glow; scales 0.25 and 1.0) - all
  within 1 level on R, G, B and alpha, 8-bit rounding. Faint stays faint
  (intensity 0.3: alpha 66-107 of 255), strong stays strong (the bloom scene:
  209-230).
- **C ABI smoke test** against the dylib alone: `el_effect_init` registers
  `0x1 0x2 0x4 0x8 0x10 0x40000000`, a config set at index 2 grows the list to
  3 and reads back, a mode outside `el_particle_mode_e` is refused,
  `glGetError` is 0.
- Not run: `neon-scale-check`. The neon's rendering is not changed.

## What a mote shows on screen

These follow from copying the buffer as stored, and are not defects of the
port:

- The buffer holds the glow AROUND the line, not the line: the neon's edge ring
  draws the filament at full resolution, so pass 1b leaves it out
  (`filamentLit`). A mote born on a lit line copies ~0.62 coverage where the
  screen shows ~0.95 at the defaults - the same hue, within 1 level.
- With `glowRadius` near 0 the buffer is empty at the line, so neon-coloured
  motes do not show. A custom colour does.
- With `glowSide` INSIDE or OUTSIDE, motes show on both sides of the line: the
  colour is read on the line, before the blit's one-sided cut - as in
  `OldSource/`.
- `particle.frag` scales colour and alpha alike by the mote's sprite and its
  age, as in `OldSource/`. A newborn's centre pixel is the latched value.
- Moved under the neon with `SetLayerOrder`, the particles read the previous
  frame's buffer.

## Tried and reverted

So they are not tried again without a reason:

- **Drawing the filament into `mScaledBuffer` for the particles**
  (`NeonRenderer::SetOutputFilament`, `uBufferFilament` in `neon.frag`): the
  frame stayed byte-identical on 16 scenes and the cost was within noise, but
  it changes the neon, which belongs to someone else.
- **Dividing by the coverage in the latch** to show the line's brightness
  (hue exact, within ~8 levels of the line at the defaults): dropped - the
  particles are to copy the buffer as it is.
- **Reading at the mote's scattered birth position** instead of the spawn
  point (it kept a one-sided glow one-sided): reverted at the owner's request.
- **Dropping `GetOutputUVMap`**: there is no exact alternative. The texture's
  size does not place it - the origin depends on `mScaledOuter` and the
  16-texel rounding - so without the map the read is exact only when the buffer
  covers the whole viewport.
