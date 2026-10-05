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
     * Every arc has settings of its own (@c EdgeLighting::ParticleArcConfig),
     * written and read one field at a time: el_particle_set_<field>(effect,
     * index, value) / el_particle_get_<field>(effect, index, &value). No
     * struct crosses the ABI.
     *
     * Which indices exist depends on one switch,
     * @ref el_particle_set_use_neon_arc:
     *   - on (the default): one arc per NEON arc. The count is the neon's own,
     *     set with @ref el_effect_set_arc_count, and each arc is where
     *     @ref el_effect_set_arc puts it. @ref el_particle_set_arc_count is
     *     refused - the count is not the layer's to set.
     *   - off: the layer's OWN arcs. The count is set with
     *     @ref el_particle_set_arc_count, and each arc sits at its own
     *     arc_start / arc_length.
     * Either way @ref el_particle_get_arc_count returns it, capped at 8, and
     * an index from 0 to that count minus 1 is valid; any other is refused
     * with @ref EL_ERROR_INVALID_PARAMETER. The range is checked against the
     * staging config at the time of the call, so set the neon's arc count and
     * the switch BEFORE writing the arcs' fields.
     *
     * Each index is one arc: writing a field on arc i changes arc i alone. A
     * new effect has one arc either way (the neon starts with one arc, the
     * layer with one), on the library's defaults, so index 0 is writable at
     * once.
     *
     * With neon arcs, the layer holds settings for as many arcs as it has
     * been written for, and a neon arc past them - one the neon has just
     * gained - shows the LAST one's; the getters return exactly that. The
     * first write after the neon gains arcs gives each of them a copy of
     * those settings, so nothing changes on screen but the field written. The
     * one exception is a layer whose own arc count was set to 0 before
     * switching to neon arcs: it emits nothing, and reads as the defaults,
     * until a field is written.
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

    /** @name Particles - the layer
     *  @{ */

    /** @brief Switch the particle layer on or off.
     *  @details Off stops it outright: every mote is cleared and the layer
     *           gives its memory back; switched on again it starts from
     *           nothing. Default off. */
    EL_API el_result_e el_particle_set_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled);
    EL_API el_result_e el_particle_get_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled);

    /** @brief Emit from the neon's arcs (non-zero, the default) or from the
     *         layer's own (zero) - see the section note for what that does to
     *         the arc count. */
    EL_API el_result_e el_particle_set_use_neon_arc(el_effect_handle_t effect, el_bool_t useNeonArc);
    EL_API el_result_e el_particle_get_use_neon_arc(el_effect_handle_t effect, el_bool_t *outUseNeonArc);

    /** @brief Set how many OWN arcs the layer emits from, 0 to 8.
     *  @details Own arcs only (use_neon_arc off): with neon arcs the count is
     *           the neon's (@ref el_effect_set_arc_count) and this is refused.
     *
     *           Growing adds copies of the last arc (the defaults when there
     *           is none), so a new arc starts on top of it - move it with
     *           @ref el_particle_set_arc_start. Shrinking drops arcs from the
     *           end. */
    EL_API el_result_e el_particle_set_arc_count(el_effect_handle_t effect, int32_t count);

    /** @brief How many arcs the layer emits from - the number of valid
     *         indices: the neon's arc count with neon arcs, the own arc count
     *         without, capped at 8 either way. */
    EL_API el_result_e el_particle_get_arc_count(el_effect_handle_t effect, int32_t *outCount);

    /** @brief Put arc @p index back on the library's defaults
     *         (@c EdgeLighting::ParticleArcConfig{}), every field at once. */
    EL_API el_result_e el_particle_reset_arc(el_effect_handle_t effect, int32_t index);

    /** @} */

    /** @name Particles - one arc's fields
     *  Every call takes the arc's @p index, 0 to
     *  @ref el_particle_get_arc_count minus 1.
     *  @{ */

    /** @brief This arc's emitter. Off clears the arc's motes at once.
     *         Default on. */
    EL_API el_result_e el_particle_set_enabled(el_effect_handle_t effect, int32_t index, el_bool_t enabled);
    EL_API el_result_e el_particle_get_enabled(el_effect_handle_t effect, int32_t index, el_bool_t *outEnabled);

    /** @brief Own arc's tail, a perimeter position in [0, 1). Default 0.
     *  @details Read only while use_neon_arc is off. */
    EL_API el_result_e el_particle_set_arc_start(el_effect_handle_t effect, int32_t index, float arcStart);
    EL_API el_result_e el_particle_get_arc_start(el_effect_handle_t effect, int32_t index, float *outArcStart);

    /** @brief Own arc's length, a perimeter fraction. Default 0.25.
     *  @details Read only while use_neon_arc is off. At or below 0.001 the
     *           arc emits nothing. */
    EL_API el_result_e el_particle_set_arc_length(el_effect_handle_t effect, int32_t index, float arcLength);
    EL_API el_result_e el_particle_get_arc_length(el_effect_handle_t effect, int32_t index, float *outArcLength);

    /** @brief Paint every mote of this arc the custom colour instead of
     *         taking the neon's. Default off. */
    EL_API el_result_e el_particle_set_use_custom_color(el_effect_handle_t effect, int32_t index,
                                                        el_bool_t useCustomColor);
    EL_API el_result_e el_particle_get_use_custom_color(el_effect_handle_t effect, int32_t index,
                                                        el_bool_t *outUseCustomColor);

    /** @brief The custom colour, straight (not premultiplied) RGBA; @p a is
     *         the motes' opacity. Default (1, 1, 1, 1).
     *  @details Read only with use_custom_color on. */
    EL_API el_result_e el_particle_set_custom_color(el_effect_handle_t effect, int32_t index,
                                                    float r, float g, float b, float a);
    EL_API el_result_e el_particle_get_custom_color(el_effect_handle_t effect, int32_t index,
                                                    float *outR, float *outG, float *outB, float *outA);

    /** @brief How this arc's emitter spawns. Default
     *         @ref EL_PARTICLE_MODE_MOVEMENT.
     *  @details A value outside @ref el_particle_mode_e is refused. */
    EL_API el_result_e el_particle_set_mode(el_effect_handle_t effect, int32_t index, el_particle_mode_e mode);
    EL_API el_result_e el_particle_get_mode(el_effect_handle_t effect, int32_t index, el_particle_mode_e *outMode);

    /** @brief MOVEMENT: spawn density - the arc spawns arc px x 2 x this / 100
     *         motes per second. HEAD_SPARKS: sparks per second. Default 400. */
    EL_API el_result_e el_particle_set_emission_rate(el_effect_handle_t effect, int32_t index, float emissionRate);
    EL_API el_result_e el_particle_get_emission_rate(el_effect_handle_t effect, int32_t index,
                                                     float *outEmissionRate);

    /** @brief Seconds - the longest any mote of this arc lives. Default 0.7. */
    EL_API el_result_e el_particle_set_lifetime(el_effect_handle_t effect, int32_t index, float lifetime);
    EL_API el_result_e el_particle_get_lifetime(el_effect_handle_t effect, int32_t index, float *outLifetime);

    /** @brief MOVEMENT: drift multiplier. HEAD_SPARKS: launch speed, px/s.
     *         Default 90. */
    EL_API el_result_e el_particle_set_speed(el_effect_handle_t effect, int32_t index, float speed);
    EL_API el_result_e el_particle_get_speed(el_effect_handle_t effect, int32_t index, float *outSpeed);

    /** @brief Exponential drag, 1/s - larger slows the motes down faster.
     *         Default 2.5. */
    EL_API el_result_e el_particle_set_drag(el_effect_handle_t effect, int32_t index, float drag);
    EL_API el_result_e el_particle_get_drag(el_effect_handle_t effect, int32_t index, float *outDrag);

    /** @brief Point size at birth, px; shrinks with the remaining life.
     *         Default 7. */
    EL_API el_result_e el_particle_set_size_start(el_effect_handle_t effect, int32_t index, float sizeStart);
    EL_API el_result_e el_particle_get_size_start(el_effect_handle_t effect, int32_t index, float *outSizeStart);

    /** @brief Live motes this arc may hold; the renderer clamps it to 4096.
     *         Default 4096. */
    EL_API el_result_e el_particle_set_max_particles(el_effect_handle_t effect, int32_t index,
                                                     int32_t maxParticles);
    EL_API el_result_e el_particle_get_max_particles(el_effect_handle_t effect, int32_t index,
                                                     int32_t *outMaxParticles);

    /** @brief MOVEMENT: spawn skew along the arc - below 1 toward the head,
     *         above 1 toward the tail. Default 1. */
    EL_API el_result_e el_particle_set_head_bias(el_effect_handle_t effect, int32_t index, float headBias);
    EL_API el_result_e el_particle_get_head_bias(el_effect_handle_t effect, int32_t index, float *outHeadBias);

    /** @brief How far from the line a mote may be born, px. Default 20. */
    EL_API el_result_e el_particle_set_scatter_radius(el_effect_handle_t effect, int32_t index,
                                                      float scatterRadius);
    EL_API el_result_e el_particle_get_scatter_radius(el_effect_handle_t effect, int32_t index,
                                                      float *outScatterRadius);

    /** @brief MOVEMENT: size at the tail as a fraction of size_start.
     *         Default 1. */
    EL_API el_result_e el_particle_set_tail_size_fraction(el_effect_handle_t effect, int32_t index,
                                                          float tailSizeFraction);
    EL_API el_result_e el_particle_get_tail_size_fraction(el_effect_handle_t effect, int32_t index,
                                                          float *outTailSizeFraction);

    /** @brief HEAD_SPARKS: full cone angle, degrees. Default 50. */
    EL_API el_result_e el_particle_set_spread_deg(el_effect_handle_t effect, int32_t index, float spreadDeg);
    EL_API el_result_e el_particle_get_spread_deg(el_effect_handle_t effect, int32_t index, float *outSpreadDeg);

    /** @brief HEAD_SPARKS: launch direction, 0 = along the sweep, 1 = straight
     *         out of the rect. Default 0.6. */
    EL_API el_result_e el_particle_set_outward_bias(el_effect_handle_t effect, int32_t index, float outwardBias);
    EL_API el_result_e el_particle_get_outward_bias(el_effect_handle_t effect, int32_t index,
                                                    float *outOutwardBias);

    /** @} */

#ifdef __cplusplus
} // extern "C"
#endif

#endif // _EL_PARTICLE_H_
