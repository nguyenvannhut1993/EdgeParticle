// ElParticle.cs - C# (P/Invoke) binding for the particle layer of the
// EdgeLighting C API, lib/capi/el-particle.h.
//
// No struct crosses the ABI: every arc's settings are written and read one
// field at a time, by arc index - SetLifetime(effect, index, v),
// GetLifetime(effect, index), ...
//
// Which indices exist (0 to GetArcCount - 1):
//   - SetUseNeonArc(effect, true) (the default): one arc per NEON arc. The
//     count is the neon's (el_effect_set_arc_count); SetArcCount is refused.
//   - SetUseNeonArc(effect, false): the layer's own arcs. The count is
//     SetArcCount, and each arc sits at its own ArcStart / ArcLength.
// Set the neon's arc count and the switch FIRST: the index is checked against
// them at the time of the call. Any other index throws.
//
// Each index is one arc - writing a field on arc i changes arc i alone. A new
// effect has one arc, on the library's defaults; ResetArc puts one back.
//
// Every setter writes the effect's STAGING config: nothing reaches the screen
// until the next el_effect_update.
//
//     ElParticle.SetRendererEnabled(effect, true);
//
//     // Neon arcs: sparks on every one of them, green.
//     for (int i = 0; i < ElParticle.GetArcCount(effect); i++)
//     {
//         ElParticle.SetMode(effect, i, ElParticleMode.HeadSparks);
//         ElParticle.SetEmissionRate(effect, i, 300f);  // sparks per second
//         ElParticle.SetCustomColor(effect, i, 0f, 1f, 0f);
//         ElParticle.SetUseCustomColor(effect, i, true);
//     }
//     ElParticle.ResetArc(effect, 1);                   // neon arc 1: default trail
//
//     // Two own arcs, one on each side:
//     ElParticle.SetUseNeonArc(effect, false);
//     ElParticle.SetArcCount(effect, 2);
//     ElParticle.SetArcStart(effect, 0, 0.03f);
//     ElParticle.SetArcLength(effect, 0, 0.12f);
//     ElParticle.SetArcStart(effect, 1, 0.53f);
//     ElParticle.SetArcLength(effect, 1, 0.12f);
//
// Not compiled by this repository's build (it has no .NET toolchain), so keep
// it in step with el-particle.h by hand.
using System;
using System.Runtime.InteropServices;

namespace EdgeLighting.Interop
{
    /// <summary>el_result_e: what every el_* call returns.</summary>
    public enum ElResult : int
    {
        Success = 0,
        InvalidHandle = -1,
        InitFailed = -2,
        OutOfMemory = -3,
        InvalidParameter = -4,
        FileNotFound = -5,
        UnsupportedFormat = -6
    }

    /// <summary>el_particle_mode_e: how one arc's emitter spawns.</summary>
    public enum ElParticleMode : int
    {
        /// <summary>Trail motes scattered along the arc, settling back onto the line.</summary>
        Movement = 0,
        /// <summary>Sparks shot from the arc's leading edge.</summary>
        HeadSparks = 1
    }

    /// <summary>
    /// The particle functions of edge-lighting-c (libedge-lighting-c.so on
    /// Tizen/Linux, .dylib on macOS, edge-lighting-c.dll on Windows). The
    /// effect handle is the IntPtr el_effect_create returned. Call from the
    /// thread that owns the GL context, like every other el_effect_* function.
    /// </summary>
    public static class ElParticle
    {
        /// <summary>The most arcs the layer drives.</summary>
        public const int MaxArcs = 8;

        private const string Lib = "edge-lighting-c";

        // ---- The C API as declared in el-particle.h -------------------------
        // el_bool_t is a 4-byte int on the C side, so booleans cross as int.

        // The layer

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_renderer_enabled(IntPtr effect, int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_renderer_enabled(IntPtr effect, out int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_use_neon_arc(IntPtr effect, int useNeonArc);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_use_neon_arc(IntPtr effect, out int useNeonArc);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_arc_count(IntPtr effect, int count);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_arc_count(IntPtr effect, out int count);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_reset_arc(IntPtr effect, int index);

        // One arc's fields

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_enabled(IntPtr effect, int index, int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_enabled(IntPtr effect, int index, out int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_arc_start(IntPtr effect, int index, float arcStart);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_arc_start(IntPtr effect, int index, out float arcStart);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_arc_length(IntPtr effect, int index, float arcLength);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_arc_length(IntPtr effect, int index, out float arcLength);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_use_custom_color(IntPtr effect, int index, int useCustomColor);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_use_custom_color(IntPtr effect, int index, out int useCustomColor);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_custom_color(IntPtr effect, int index,
                                                                   float r, float g, float b, float a);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_custom_color(IntPtr effect, int index,
                                                                   out float r, out float g, out float b, out float a);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_mode(IntPtr effect, int index, ElParticleMode mode);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_mode(IntPtr effect, int index, out ElParticleMode mode);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_emission_rate(IntPtr effect, int index, float emissionRate);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_emission_rate(IntPtr effect, int index, out float emissionRate);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_lifetime(IntPtr effect, int index, float lifetime);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_lifetime(IntPtr effect, int index, out float lifetime);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_speed(IntPtr effect, int index, float speed);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_speed(IntPtr effect, int index, out float speed);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_drag(IntPtr effect, int index, float drag);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_drag(IntPtr effect, int index, out float drag);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_size_start(IntPtr effect, int index, float sizeStart);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_size_start(IntPtr effect, int index, out float sizeStart);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_max_particles(IntPtr effect, int index, int maxParticles);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_max_particles(IntPtr effect, int index, out int maxParticles);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_head_bias(IntPtr effect, int index, float headBias);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_head_bias(IntPtr effect, int index, out float headBias);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_scatter_radius(IntPtr effect, int index, float scatterRadius);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_scatter_radius(IntPtr effect, int index, out float scatterRadius);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_tail_size_fraction(IntPtr effect, int index, float tailSizeFraction);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_tail_size_fraction(IntPtr effect, int index, out float tailSizeFraction);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_spread_deg(IntPtr effect, int index, float spreadDeg);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_spread_deg(IntPtr effect, int index, out float spreadDeg);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_set_outward_bias(IntPtr effect, int index, float outwardBias);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_get_outward_bias(IntPtr effect, int index, out float outwardBias);

        // ---- Convenience wrappers: throw on anything but Success -------------

        // The layer

        /// <summary>Switch the layer on or off. Off clears every mote and frees the layer's memory.</summary>
        public static void SetRendererEnabled(IntPtr effect, bool enabled)
        {
            Check(el_particle_set_renderer_enabled(effect, enabled ? 1 : 0), "el_particle_set_renderer_enabled");
        }

        public static bool GetRendererEnabled(IntPtr effect)
        {
            Check(el_particle_get_renderer_enabled(effect, out int enabled), "el_particle_get_renderer_enabled");
            return enabled != 0;
        }

        /// <summary>True: one arc per neon arc. False: the layer's own, SetArcCount of them.</summary>
        public static void SetUseNeonArc(IntPtr effect, bool useNeonArc)
        {
            Check(el_particle_set_use_neon_arc(effect, useNeonArc ? 1 : 0), "el_particle_set_use_neon_arc");
        }

        public static bool GetUseNeonArc(IntPtr effect)
        {
            Check(el_particle_get_use_neon_arc(effect, out int useNeonArc), "el_particle_get_use_neon_arc");
            return useNeonArc != 0;
        }

        /// <summary>
        /// How many OWN arcs, 0 to 8 - throws while UseNeonArc is on, where
        /// the count is the neon's. Growing copies the last arc.
        /// </summary>
        public static void SetArcCount(IntPtr effect, int count)
        {
            Check(el_particle_set_arc_count(effect, count), "el_particle_set_arc_count");
        }

        /// <summary>How many arcs the layer emits from - the valid indices are 0 to this minus 1.</summary>
        public static int GetArcCount(IntPtr effect)
        {
            Check(el_particle_get_arc_count(effect, out int count), "el_particle_get_arc_count");
            return count;
        }

        /// <summary>Put arc <paramref name="index"/> back on the library's defaults.</summary>
        public static void ResetArc(IntPtr effect, int index)
        {
            Check(el_particle_reset_arc(effect, index), "el_particle_reset_arc");
        }

        // One arc's fields

        /// <summary>This arc's emitter. Off clears the arc's motes at once.</summary>
        public static void SetEnabled(IntPtr effect, int index, bool enabled)
        {
            Check(el_particle_set_enabled(effect, index, enabled ? 1 : 0), "el_particle_set_enabled");
        }

        public static bool GetEnabled(IntPtr effect, int index)
        {
            Check(el_particle_get_enabled(effect, index, out int enabled), "el_particle_get_enabled");
            return enabled != 0;
        }

        /// <summary>Own arc's tail, perimeter position in [0, 1). Read only while UseNeonArc is off.</summary>
        public static void SetArcStart(IntPtr effect, int index, float arcStart)
        {
            Check(el_particle_set_arc_start(effect, index, arcStart), "el_particle_set_arc_start");
        }

        public static float GetArcStart(IntPtr effect, int index)
        {
            Check(el_particle_get_arc_start(effect, index, out float arcStart), "el_particle_get_arc_start");
            return arcStart;
        }

        /// <summary>Own arc's length, perimeter fraction. Read only while UseNeonArc is off.</summary>
        public static void SetArcLength(IntPtr effect, int index, float arcLength)
        {
            Check(el_particle_set_arc_length(effect, index, arcLength), "el_particle_set_arc_length");
        }

        public static float GetArcLength(IntPtr effect, int index)
        {
            Check(el_particle_get_arc_length(effect, index, out float arcLength), "el_particle_get_arc_length");
            return arcLength;
        }

        /// <summary>Paint every mote the custom colour instead of taking the neon's.</summary>
        public static void SetUseCustomColor(IntPtr effect, int index, bool useCustomColor)
        {
            Check(el_particle_set_use_custom_color(effect, index, useCustomColor ? 1 : 0), "el_particle_set_use_custom_color");
        }

        public static bool GetUseCustomColor(IntPtr effect, int index)
        {
            Check(el_particle_get_use_custom_color(effect, index, out int useCustomColor), "el_particle_get_use_custom_color");
            return useCustomColor != 0;
        }

        /// <summary>
        /// The custom colour, straight (not premultiplied) RGBA; alpha is the
        /// motes' opacity. Read only with UseCustomColor on.
        /// </summary>
        public static void SetCustomColor(IntPtr effect, int index, float r, float g, float b, float a = 1f)
        {
            Check(el_particle_set_custom_color(effect, index, r, g, b, a), "el_particle_set_custom_color");
        }

        public static void GetCustomColor(IntPtr effect, int index, out float r, out float g, out float b, out float a)
        {
            Check(el_particle_get_custom_color(effect, index, out r, out g, out b, out a), "el_particle_get_custom_color");
        }

        /// <summary>How this arc's emitter spawns.</summary>
        public static void SetMode(IntPtr effect, int index, ElParticleMode mode)
        {
            Check(el_particle_set_mode(effect, index, mode), "el_particle_set_mode");
        }

        public static ElParticleMode GetMode(IntPtr effect, int index)
        {
            Check(el_particle_get_mode(effect, index, out ElParticleMode mode), "el_particle_get_mode");
            return mode;
        }

        /// <summary>Movement: density, arc px x 2 x this / 100 motes/s. HeadSparks: sparks/s.</summary>
        public static void SetEmissionRate(IntPtr effect, int index, float emissionRate)
        {
            Check(el_particle_set_emission_rate(effect, index, emissionRate), "el_particle_set_emission_rate");
        }

        public static float GetEmissionRate(IntPtr effect, int index)
        {
            Check(el_particle_get_emission_rate(effect, index, out float emissionRate), "el_particle_get_emission_rate");
            return emissionRate;
        }

        /// <summary>Seconds - the longest any mote of this arc lives.</summary>
        public static void SetLifetime(IntPtr effect, int index, float lifetime)
        {
            Check(el_particle_set_lifetime(effect, index, lifetime), "el_particle_set_lifetime");
        }

        public static float GetLifetime(IntPtr effect, int index)
        {
            Check(el_particle_get_lifetime(effect, index, out float lifetime), "el_particle_get_lifetime");
            return lifetime;
        }

        /// <summary>Movement: drift multiplier. HeadSparks: launch speed, px/s.</summary>
        public static void SetSpeed(IntPtr effect, int index, float speed)
        {
            Check(el_particle_set_speed(effect, index, speed), "el_particle_set_speed");
        }

        public static float GetSpeed(IntPtr effect, int index)
        {
            Check(el_particle_get_speed(effect, index, out float speed), "el_particle_get_speed");
            return speed;
        }

        /// <summary>Exponential drag, 1/s.</summary>
        public static void SetDrag(IntPtr effect, int index, float drag)
        {
            Check(el_particle_set_drag(effect, index, drag), "el_particle_set_drag");
        }

        public static float GetDrag(IntPtr effect, int index)
        {
            Check(el_particle_get_drag(effect, index, out float drag), "el_particle_get_drag");
            return drag;
        }

        /// <summary>Point size at birth, px.</summary>
        public static void SetSizeStart(IntPtr effect, int index, float sizeStart)
        {
            Check(el_particle_set_size_start(effect, index, sizeStart), "el_particle_set_size_start");
        }

        public static float GetSizeStart(IntPtr effect, int index)
        {
            Check(el_particle_get_size_start(effect, index, out float sizeStart), "el_particle_get_size_start");
            return sizeStart;
        }

        /// <summary>Live motes this arc may hold; the renderer clamps it to 4096.</summary>
        public static void SetMaxParticles(IntPtr effect, int index, int maxParticles)
        {
            Check(el_particle_set_max_particles(effect, index, maxParticles), "el_particle_set_max_particles");
        }

        public static int GetMaxParticles(IntPtr effect, int index)
        {
            Check(el_particle_get_max_particles(effect, index, out int maxParticles), "el_particle_get_max_particles");
            return maxParticles;
        }

        /// <summary>Movement: spawn skew, below 1 toward the head, above 1 toward the tail.</summary>
        public static void SetHeadBias(IntPtr effect, int index, float headBias)
        {
            Check(el_particle_set_head_bias(effect, index, headBias), "el_particle_set_head_bias");
        }

        public static float GetHeadBias(IntPtr effect, int index)
        {
            Check(el_particle_get_head_bias(effect, index, out float headBias), "el_particle_get_head_bias");
            return headBias;
        }

        /// <summary>How far from the line a mote may be born, px.</summary>
        public static void SetScatterRadius(IntPtr effect, int index, float scatterRadius)
        {
            Check(el_particle_set_scatter_radius(effect, index, scatterRadius), "el_particle_set_scatter_radius");
        }

        public static float GetScatterRadius(IntPtr effect, int index)
        {
            Check(el_particle_get_scatter_radius(effect, index, out float scatterRadius), "el_particle_get_scatter_radius");
            return scatterRadius;
        }

        /// <summary>Movement: size at the tail as a fraction of SizeStart.</summary>
        public static void SetTailSizeFraction(IntPtr effect, int index, float tailSizeFraction)
        {
            Check(el_particle_set_tail_size_fraction(effect, index, tailSizeFraction), "el_particle_set_tail_size_fraction");
        }

        public static float GetTailSizeFraction(IntPtr effect, int index)
        {
            Check(el_particle_get_tail_size_fraction(effect, index, out float tailSizeFraction), "el_particle_get_tail_size_fraction");
            return tailSizeFraction;
        }

        /// <summary>HeadSparks: full cone angle, degrees.</summary>
        public static void SetSpreadDeg(IntPtr effect, int index, float spreadDeg)
        {
            Check(el_particle_set_spread_deg(effect, index, spreadDeg), "el_particle_set_spread_deg");
        }

        public static float GetSpreadDeg(IntPtr effect, int index)
        {
            Check(el_particle_get_spread_deg(effect, index, out float spreadDeg), "el_particle_get_spread_deg");
            return spreadDeg;
        }

        /// <summary>HeadSparks: 0 = along the sweep, 1 = straight out of the rect.</summary>
        public static void SetOutwardBias(IntPtr effect, int index, float outwardBias)
        {
            Check(el_particle_set_outward_bias(effect, index, outwardBias), "el_particle_set_outward_bias");
        }

        public static float GetOutwardBias(IntPtr effect, int index)
        {
            Check(el_particle_get_outward_bias(effect, index, out float outwardBias), "el_particle_get_outward_bias");
            return outwardBias;
        }

        internal static void Check(ElResult result, string function)
        {
            if (result != ElResult.Success)
            {
                throw new InvalidOperationException($"{function} failed: {result} (the native log says why)");
            }
        }
    }
}
