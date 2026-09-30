/**
 * @file el-particle.h
 * @brief The particle layer (Config::particle) for the EdgeLighting C API:
 *        point-sprite motes emitted from the neon's arcs or the layer's own.
 */
#ifndef _EL_PARTICLE_H_
#define _EL_PARTICLE_H_

#include "el-types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ======================================================================
     * Particles (Config::particle)
     *
     * The layer is registered by @ref el_effect_init (and by
     * @ref el_effect_init_with_renderers when the mask carries
     * @ref EL_RENDERER_PARTICLES) and starts switched off.
     *
     * It keeps a LIST of @ref el_particle_config_t, one per arc, written two
     * ways:
     *   - @ref el_effect_set_particle_config, no index: ONE config for the
     *     whole layer - the list becomes exactly that config. With neon arcs
     *     it drives every neon arc.
     *   - @ref el_effect_set_particle_arc_config, by index: one arc's own
     *     config.
     *
     * How many arcs the layer emits from, and where they are, depends on one
     * switch, @ref el_effect_set_particle_use_neon_arc:
     *   - on (the default): the NEON's arcs. The arc count is the neon's own,
     *     set with @ref el_effect_set_arc_count, and each arc is where
     *     @ref el_effect_set_arc puts it. Config i drives neon arc i; a neon
     *     arc past the end of the list takes the LAST config - which is why a
     *     single config drives every neon arc.
     *   - off: the layer's OWN arcs. The arc count is the length of the list,
     *     set with @ref el_effect_set_particle_arc_count - or grown by writing
     *     a config at a new index - and each arc sits at its own config's
     *     arcStart / arcLength.
     *
     * Like every other setter, these write the effect's staging config: the
     * next @ref el_effect_update hands it to the renderer. A setter handed the
     * value already held does nothing.
     * ==================================================================== */

    /** @brief How one arc's emitter spawns.
     *  @details Mirrors @c EdgeLighting::ParticleMode 1:1; parity is pinned
     *           by static_asserts in capi-internal.h. */
    typedef enum el_particle_mode_e
    {
        EL_PARTICLE_MODE_MOVEMENT = 0,   /**< Trail motes scattered along the arc, settling back onto the line. */
        EL_PARTICLE_MODE_HEAD_SPARKS = 1 /**< Sparks shot from the arc's leading edge. */
    } el_particle_mode_e;

    /** @brief One arc's particle settings - @c EdgeLighting::ParticleArcConfig
     *         across the ABI.
     *  @details 20 fields of 4 bytes each, 80 bytes, no padding: a C# host
     *           declares the same fields in the same order with
     *           [StructLayout(LayoutKind.Sequential)] - see
     *           lib/capi/csharp/ElParticle.cs. Start from
     *           @ref el_particle_config_init rather than from zeros: an
     *           all-zero struct is lifetime 0, which emits nothing. */
    typedef struct el_particle_config_t
    {
        el_bool_t enable;         /**<  0: this arc's emitter. Off clears the arc's motes at once. */
        float arcStart;           /**<  4: own arc's tail, perimeter position in [0, 1). Read only while useNeonArc is off. */
        float arcLength;          /**<  8: own arc's length, perimeter fraction. Read only while useNeonArc is off. */
        el_bool_t useCustomColor; /**< 12: paint every mote customColor instead of taking the neon's colour. */
        float customColorR;       /**< 16: straight (not premultiplied) RGBA, read only with useCustomColor. */
        float customColorG;       /**< 20 */
        float customColorB;       /**< 24 */
        float customColorA;       /**< 28: the motes' opacity. */
        int32_t mode;             /**< 32: an el_particle_mode_e, as a fixed-width int. */
        float emissionRate;       /**< 36: MOVEMENT: density, arc px x 2 x this / 100 motes/s. HEAD_SPARKS: sparks/s. */
        float lifetime;           /**< 40: seconds - the longest any mote of this arc lives. */
        float speed;              /**< 44: MOVEMENT: drift multiplier. HEAD_SPARKS: launch speed, px/s. */
        float drag;               /**< 48: exponential drag, 1/s. */
        float sizeStart;          /**< 52: point size at birth, px. */
        int32_t maxParticles;     /**< 56: live motes this arc may hold; the renderer clamps it to 4096. */
        float headBias;           /**< 60: MOVEMENT: spawn skew, below 1 toward the head, above 1 toward the tail. */
        float scatterRadius;      /**< 64: how far from the line a mote may be born, px. */
        float tailSizeFraction;   /**< 68: MOVEMENT: size at the tail as a fraction of sizeStart. */
        float spreadDeg;          /**< 72: HEAD_SPARKS: full cone angle, degrees. */
        float outwardBias;        /**< 76: HEAD_SPARKS: 0 = along the sweep, 1 = straight out of the rect. */
    } el_particle_config_t;

    /** @name Particles
     *  @{ */

    /** @brief Fill @p config with the library's defaults
     *         (@c EdgeLighting::ParticleArcConfig{}), to edit from. */
    EL_API el_result_e el_particle_config_init(el_particle_config_t *config);

    /** @brief Switch the particle layer on or off.
     *  @details Off stops it outright: every mote is cleared and the layer
     *           gives its memory back; switched on again it starts from
     *           nothing. Default off. */
    EL_API el_result_e el_effect_set_particle_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_effect_get_particle_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Emit from the neon's arcs (non-zero, the default) or from the
     *         layer's own (zero) - see the section note for where the arc
     *         count comes from in each case. */
    EL_API el_result_e el_effect_set_particle_use_neon_arc(el_effect_handle_t effect, el_bool_t useNeonArc);
    EL_API el_result_e el_effect_get_particle_use_neon_arc(el_effect_handle_t effect, el_bool_t *outUseNeonArc);

    /** @brief ONE config for the whole layer: the list becomes exactly
     *         @p config, dropping any per-arc configs.
     *  @details With neon arcs it drives every neon arc, however many there
     *           are. With own arcs the layer has one arc, at the config's
     *           arcStart / arcLength. A mode outside @ref el_particle_mode_e is
     *           refused. */
    EL_API el_result_e el_effect_set_particle_config(el_effect_handle_t effect,
                                                     const el_particle_config_t *config);

    /** @brief Read the first config back - the one
     *         @ref el_effect_set_particle_config wrote. */
    EL_API el_result_e el_effect_get_particle_config(el_effect_handle_t effect, el_particle_config_t *outConfig);

    /** @brief Write arc @p index's own config, 0 to 7.
     *  @details An index past the end grows the list to it, the new configs
     *           being copies of the last one - with neon arcs the picture does
     *           not change until that index is written; with own arcs that is
     *           how an arc is added. Any other index, or a mode outside
     *           @ref el_particle_mode_e, is refused. */
    EL_API el_result_e el_effect_set_particle_arc_config(el_effect_handle_t effect, int32_t index,
                                                         const el_particle_config_t *config);

    /** @brief Read arc @p index's config back from the staging config. */
    EL_API el_result_e el_effect_get_particle_arc_config(el_effect_handle_t effect, int32_t index,
                                                         el_particle_config_t *outConfig);

    /** @brief Set how many arc configs the layer holds, 0 to 8.
     *  @details With own arcs (useNeonArc off) this IS the number of arcs the
     *           layer emits from. With neon arcs the arc count is the neon's
     *           (@ref el_effect_set_arc_count) and this is only how many of
     *           them have a config of their own.
     *
     *           Growing copies the last config (the defaults when the list is
     *           empty); shrinking drops configs from the end. */
    EL_API el_result_e el_effect_set_particle_arc_count(el_effect_handle_t effect, int32_t count);
    EL_API el_result_e el_effect_get_particle_arc_count(el_effect_handle_t effect, int32_t *outCount);

    /** @} */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_PARTICLE_H_
