// ElParticle.cs - C# (P/Invoke) binding for the particle layer of the
// EdgeLighting C API, lib/capi/el-particle.h.
//
// ElParticleConfig mirrors el_particle_config_t FIELD FOR FIELD: 20 four-byte
// fields, 80 bytes. A field added there goes at the same place here - keep
// the two in the same order.
//
// Start from ElParticleConfig.CreateDefault(), the library's defaults.
// `new ElParticleConfig()` is all zeros - lifetime 0 - which emits nothing.
//
// Where the arcs come from:
//   - SetUseNeonArc(effect, true) (the default): the neon's arcs. Their count
//     is the neon's (el_effect_set_arc_count); config i drives neon arc i, and
//     a neon arc past the last config takes the last config.
//   - SetUseNeonArc(effect, false): the layer's own arcs. Their count is the
//     number of configs - SetArcCount, or SetArcConfig at a new index - and
//     each sits at its config's ArcStart / ArcLength.
//
// Two ways to write configs:
//   - SetConfig(effect, cfg), no index: ONE config for the whole layer. With
//     neon arcs it drives every neon arc, however many there are.
//   - SetArcConfig(effect, index, cfg): one arc's own config.
//
// Every setter writes the effect's STAGING config: nothing reaches the screen
// until the next el_effect_update.
//
//     var sparks = ElParticleConfig.CreateDefault();
//     sparks.Mode = ElParticleMode.HeadSparks;
//     sparks.EmissionRate = 300f;                  // sparks per second
//     sparks.SetCustomColor(0f, 1f, 0f);           // always green
//
//     ElParticle.SetRendererEnabled(effect, true);
//     ElParticle.SetConfig(effect, sparks);        // one config: every neon arc
//
//     var trail = ElParticleConfig.CreateDefault(); // Movement
//     ElParticle.SetArcConfig(effect, 1, trail);   // now neon arc 1 differs
//
//     // Two own arcs, one on each side:
//     ElParticle.SetUseNeonArc(effect, false);
//     var left = ElParticleConfig.CreateDefault();
//     left.ArcStart = 0.03f; left.ArcLength = 0.12f;
//     var right = left;
//     right.ArcStart = 0.53f;
//     ElParticle.SetConfigs(effect, left, right);  // arc count 2
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
    /// el_particle_config_t: one arc's particle settings (C++ ParticleArcConfig).
    /// Blittable - the two booleans are stored as the C side's 4-byte el_bool_t
    /// and exposed through <see cref="Enable"/> and <see cref="UseCustomColor"/>.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ElParticleConfig
    {
        private int enable;              //  0
        /// <summary>Own arc's tail, perimeter position in [0, 1). Read only while UseNeonArc is off.</summary>
        public float ArcStart;           //  4
        /// <summary>Own arc's length, perimeter fraction. Read only while UseNeonArc is off.</summary>
        public float ArcLength;          //  8
        private int useCustomColor;      // 12
        /// <summary>Straight (not premultiplied) RGBA, read only with UseCustomColor.</summary>
        public float CustomColorR;       // 16
        public float CustomColorG;       // 20
        public float CustomColorB;       // 24
        /// <summary>The motes' opacity.</summary>
        public float CustomColorA;       // 28
        public ElParticleMode Mode;      // 32
        /// <summary>Movement: density, arc px x 2 x this / 100 motes/s. HeadSparks: sparks/s.</summary>
        public float EmissionRate;       // 36
        /// <summary>Seconds - the longest any mote of this arc lives.</summary>
        public float Lifetime;           // 40
        /// <summary>Movement: drift multiplier. HeadSparks: launch speed, px/s.</summary>
        public float Speed;              // 44
        /// <summary>Exponential drag, 1/s.</summary>
        public float Drag;               // 48
        /// <summary>Point size at birth, px.</summary>
        public float SizeStart;          // 52
        /// <summary>Live motes this arc may hold; the renderer clamps it to 4096.</summary>
        public int MaxParticles;         // 56
        /// <summary>Movement: spawn skew, below 1 toward the head, above 1 toward the tail.</summary>
        public float HeadBias;           // 60
        /// <summary>How far from the line a mote may be born, px.</summary>
        public float ScatterRadius;      // 64
        /// <summary>Movement: size at the tail as a fraction of SizeStart.</summary>
        public float TailSizeFraction;   // 68
        /// <summary>HeadSparks: full cone angle, degrees.</summary>
        public float SpreadDeg;          // 72
        /// <summary>HeadSparks: 0 = along the sweep, 1 = straight out of the rect.</summary>
        public float OutwardBias;        // 76

        /// <summary>This arc's emitter. Off clears the arc's motes at once.</summary>
        public bool Enable
        {
            get => enable != 0;
            set => enable = value ? 1 : 0;
        }

        /// <summary>Paint every mote the custom colour instead of taking the neon's.</summary>
        public bool UseCustomColor
        {
            get => useCustomColor != 0;
            set => useCustomColor = value ? 1 : 0;
        }

        /// <summary>Switch the custom colour on and set it (straight RGBA).</summary>
        public void SetCustomColor(float r, float g, float b, float a = 1f)
        {
            UseCustomColor = true;
            CustomColorR = r;
            CustomColorG = g;
            CustomColorB = b;
            CustomColorA = a;
        }

        /// <summary>The library's defaults, ready to edit and hand to <see cref="ElParticle.SetConfig"/>.</summary>
        public static ElParticleConfig CreateDefault()
        {
            ElParticle.Check(ElParticle.el_particle_config_init(out ElParticleConfig config), "el_particle_config_init");
            return config;
        }
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
        // Configs pass by `ref` rather than `in`: every runtime (Mono, .NET
        // Framework, .NET) marshals it as a plain pointer. The setters only read it.

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_particle_config_init(out ElParticleConfig config);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_set_particle_renderer_enabled(IntPtr effect, int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_get_particle_renderer_enabled(IntPtr effect, out int enabled);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_set_particle_use_neon_arc(IntPtr effect, int useNeonArc);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_get_particle_use_neon_arc(IntPtr effect, out int useNeonArc);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_set_particle_config(IntPtr effect, ref ElParticleConfig config);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_get_particle_config(IntPtr effect, out ElParticleConfig config);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_set_particle_arc_config(IntPtr effect, int index, ref ElParticleConfig config);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_get_particle_arc_config(IntPtr effect, int index, out ElParticleConfig config);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_set_particle_arc_count(IntPtr effect, int count);

        [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)]
        public static extern ElResult el_effect_get_particle_arc_count(IntPtr effect, out int count);

        // ---- Convenience wrappers: throw on anything but Success -------------

        /// <summary>Switch the layer on or off. Off clears every mote and frees the layer's memory.</summary>
        public static void SetRendererEnabled(IntPtr effect, bool enabled)
        {
            Check(el_effect_set_particle_renderer_enabled(effect, enabled ? 1 : 0), "el_effect_set_particle_renderer_enabled");
        }

        public static bool GetRendererEnabled(IntPtr effect)
        {
            Check(el_effect_get_particle_renderer_enabled(effect, out int enabled), "el_effect_get_particle_renderer_enabled");
            return enabled != 0;
        }

        /// <summary>True: emit from the neon's arcs. False: from the layer's own, one per config.</summary>
        public static void SetUseNeonArc(IntPtr effect, bool useNeonArc)
        {
            Check(el_effect_set_particle_use_neon_arc(effect, useNeonArc ? 1 : 0), "el_effect_set_particle_use_neon_arc");
        }

        public static bool GetUseNeonArc(IntPtr effect)
        {
            Check(el_effect_get_particle_use_neon_arc(effect, out int useNeonArc), "el_effect_get_particle_use_neon_arc");
            return useNeonArc != 0;
        }

        /// <summary>
        /// ONE config for the whole layer: the list becomes exactly
        /// <paramref name="config"/>. With neon arcs it drives every neon arc;
        /// with own arcs the layer has one arc, at its ArcStart / ArcLength.
        /// </summary>
        public static void SetConfig(IntPtr effect, ElParticleConfig config)
        {
            Check(el_effect_set_particle_config(effect, ref config), "el_effect_set_particle_config");
        }

        /// <summary>The first config - the one <see cref="SetConfig"/> wrote.</summary>
        public static ElParticleConfig GetConfig(IntPtr effect)
        {
            Check(el_effect_get_particle_config(effect, out ElParticleConfig config), "el_effect_get_particle_config");
            return config;
        }

        /// <summary>
        /// Arc <paramref name="index"/>'s own config, 0 to 7. The list grows to
        /// the index with copies of its last config if it is shorter - which
        /// with own arcs is how an arc is added.
        /// </summary>
        public static void SetArcConfig(IntPtr effect, int index, ElParticleConfig config)
        {
            Check(el_effect_set_particle_arc_config(effect, index, ref config), "el_effect_set_particle_arc_config");
        }

        /// <summary>Arc <paramref name="index"/>'s config as the staging config holds it.</summary>
        public static ElParticleConfig GetArcConfig(IntPtr effect, int index)
        {
            Check(el_effect_get_particle_arc_config(effect, index, out ElParticleConfig config), "el_effect_get_particle_arc_config");
            return config;
        }

        /// <summary>One config per arc, in order: arc i gets configs[i], and the arc count becomes configs.Length.</summary>
        public static void SetConfigs(IntPtr effect, params ElParticleConfig[] configs)
        {
            if (configs == null)
            {
                throw new ArgumentNullException(nameof(configs));
            }
            Check(el_effect_set_particle_arc_count(effect, configs.Length), "el_effect_set_particle_arc_count");
            for (int i = 0; i < configs.Length; i++)
            {
                Check(el_effect_set_particle_arc_config(effect, i, ref configs[i]), "el_effect_set_particle_arc_config");
            }
        }

        /// <summary>
        /// How many arc configs the layer holds, 0 to 8. With own arcs this IS
        /// the arc count; with neon arcs it is how many of them have a config
        /// of their own. Growing copies the last config.
        /// </summary>
        public static void SetArcCount(IntPtr effect, int count)
        {
            Check(el_effect_set_particle_arc_count(effect, count), "el_effect_set_particle_arc_count");
        }

        public static int GetArcCount(IntPtr effect)
        {
            Check(el_effect_get_particle_arc_count(effect, out int count), "el_effect_get_particle_arc_count");
            return count;
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
