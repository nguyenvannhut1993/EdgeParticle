#include "capi-internal.h"

// ==========================================================================
// Particles (Config::particle)
// ==========================================================================

namespace
{
    /// What a list grows with: a copy of its last config, the defaults when
    /// it is empty. With neon arcs a neon arc past the list already showed
    /// the last config, so growing changes nothing on screen.
    EdgeLighting::ParticleArcConfig GrowthConfig(const std::vector<EdgeLighting::ParticleArcConfig> &arcs)
    {
        return arcs.empty() ? EdgeLighting::ParticleArcConfig{} : arcs.back();
    }

    /// How many arcs the layer emits from, which is how many indices are
    /// valid. With neon arcs it is the NEON's count, whatever the length of
    /// the config list; with own arcs it is the list. Capped as the renderer
    /// caps it.
    size_t ArcCount(el_effect_handle_t effect)
    {
        const auto &pc = effect->config.particle;
        const size_t count = pc.useNeonArc ? effect->config.neon.arcs.size() : pc.arcs.size();
        return std::min(count, static_cast<size_t>(EdgeLighting::ParticleConfig::MAX_ARCS_CAP));
    }

    /// Arc @p index's settings as the renderer reads them: its own entry, the
    /// LAST entry for a neon arc past the end of the list, the defaults when
    /// the list is empty. Null, logged, when @p index is not an arc.
    const EdgeLighting::ParticleArcConfig *ArcInUse(el_effect_handle_t effect, int32_t index, const char *who)
    {
        const size_t count = ArcCount(effect);
        if (index < 0 || static_cast<size_t>(index) >= count)
        {
            LOG_E("%s: index %d out of range (arc count=%zu)", who, index, count);
            return nullptr;
        }
        const auto &arcs = effect->config.particle.arcs;
        if (arcs.empty())
        {
            static const EdgeLighting::ParticleArcConfig DEFAULTS{};
            return &DEFAULTS;
        }
        return &arcs[std::min(static_cast<size_t>(index), arcs.size() - 1)];
    }

    /// Arc @p index's own entry, to write into. With neon arcs the list first
    /// grows to every neon arc, each new entry a copy of the last - what that
    /// arc already showed - so a write on arc i changes arc i alone rather
    /// than every neon arc past the end of the list. Call only with an index
    /// @ref ArcInUse accepted.
    EdgeLighting::ParticleArcConfig &OwnArc(el_effect_handle_t effect, int32_t index)
    {
        auto &arcs = effect->config.particle.arcs;
        const size_t count = ArcCount(effect);
        if (arcs.size() < count)
        {
            arcs.resize(count, GrowthConfig(arcs));
        }
        return arcs[static_cast<size_t>(index)];
    }

    el_result_e SetArcFloat(el_effect_handle_t effect, int32_t index,
                            float EdgeLighting::ParticleArcConfig::*field, float value, const char *who)
    {
        VALIDATE_EFFECT_PTR(effect, who);
        const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, who);
        if (!arc)
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        if (arc->*field == value)
        {
            return EL_SUCCESS;
        }
        LOG_I("%s: effect=%p, index=%d, value=%f", who, (void *)effect, index, value);
        OwnArc(effect, index).*field = value;
        return EL_SUCCESS;
    }

    el_result_e SetArcBool(el_effect_handle_t effect, int32_t index,
                           bool EdgeLighting::ParticleArcConfig::*field, el_bool_t value, const char *who)
    {
        VALIDATE_EFFECT_PTR(effect, who);
        const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, who);
        if (!arc)
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        const bool v = (value != 0);
        if (arc->*field == v)
        {
            return EL_SUCCESS;
        }
        LOG_I("%s: effect=%p, index=%d, value=%d", who, (void *)effect, index, (int)value);
        OwnArc(effect, index).*field = v;
        return EL_SUCCESS;
    }

    /// One field read back, for the fields whose C type is the C++ one.
    template <typename T>
    el_result_e GetArcField(el_effect_handle_t effect, int32_t index,
                            T EdgeLighting::ParticleArcConfig::*field, T *outValue, const char *who)
    {
        VALIDATE_EFFECT_PTR(effect, who);
        VALIDATE_OUT_PTR(outValue, who);
        const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, who);
        if (!arc)
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        *outValue = arc->*field;
        return EL_SUCCESS;
    }

    el_result_e GetArcBool(el_effect_handle_t effect, int32_t index,
                           bool EdgeLighting::ParticleArcConfig::*field, el_bool_t *outValue, const char *who)
    {
        VALIDATE_EFFECT_PTR(effect, who);
        VALIDATE_OUT_PTR(outValue, who);
        const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, who);
        if (!arc)
        {
            return EL_ERROR_INVALID_PARAMETER;
        }
        *outValue = (arc->*field) ? 1 : 0;
        return EL_SUCCESS;
    }
}

// --------------------------------------------------------------------------
// The layer
// --------------------------------------------------------------------------

el_result_e el_particle_set_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_renderer_enabled");
    SET_AND_LOG(effect->config.particle.enable, enabled != 0,
                "effect=%p, enabled=%d", (void *)effect, (int)enabled);
}

el_result_e el_particle_get_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_get_renderer_enabled");
    VALIDATE_OUT_PTR(outEnabled, "el_particle_get_renderer_enabled");
    *outEnabled = effect->config.particle.enable ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_particle_set_use_neon_arc(el_effect_handle_t effect, el_bool_t useNeonArc)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_use_neon_arc");
    SET_AND_LOG(effect->config.particle.useNeonArc, useNeonArc != 0,
                "effect=%p, useNeonArc=%d", (void *)effect, (int)useNeonArc);
}

el_result_e el_particle_get_use_neon_arc(el_effect_handle_t effect, el_bool_t *outUseNeonArc)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_get_use_neon_arc");
    VALIDATE_OUT_PTR(outUseNeonArc, "el_particle_get_use_neon_arc");
    *outUseNeonArc = effect->config.particle.useNeonArc ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_particle_set_arc_count(el_effect_handle_t effect, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_arc_count");
    if (effect->config.particle.useNeonArc)
    {
        LOG_E("el_particle_set_arc_count: the arc count follows the neon while useNeonArc is on - "
              "use el_effect_set_arc_count");
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (count < 0 || count > EdgeLighting::ParticleConfig::MAX_ARCS_CAP)
    {
        LOG_E("el_particle_set_arc_count: count %d - use 0 to %d",
              count, EdgeLighting::ParticleConfig::MAX_ARCS_CAP);
        return EL_ERROR_INVALID_PARAMETER;
    }
    auto &arcs = effect->config.particle.arcs;
    if (arcs.size() == static_cast<size_t>(count))
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, count=%d (was %zu)", (void *)effect, count, arcs.size());
    arcs.resize(static_cast<size_t>(count), GrowthConfig(arcs));
    return EL_SUCCESS;
}

el_result_e el_particle_get_arc_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_get_arc_count");
    VALIDATE_OUT_PTR(outCount, "el_particle_get_arc_count");
    *outCount = static_cast<int32_t>(ArcCount(effect));
    return EL_SUCCESS;
}

el_result_e el_particle_reset_arc(el_effect_handle_t effect, int32_t index)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_reset_arc");
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_reset_arc");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    const EdgeLighting::ParticleArcConfig defaults{};
    if (*arc == defaults)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d", (void *)effect, index);
    OwnArc(effect, index) = defaults;
    return EL_SUCCESS;
}

// --------------------------------------------------------------------------
// One arc's fields
// --------------------------------------------------------------------------

el_result_e el_particle_set_enabled(el_effect_handle_t effect, int32_t index, el_bool_t enabled)
{
    return SetArcBool(effect, index, &EdgeLighting::ParticleArcConfig::enable, enabled,
                      "el_particle_set_enabled");
}

el_result_e el_particle_get_enabled(el_effect_handle_t effect, int32_t index, el_bool_t *outEnabled)
{
    return GetArcBool(effect, index, &EdgeLighting::ParticleArcConfig::enable, outEnabled,
                      "el_particle_get_enabled");
}

el_result_e el_particle_set_arc_start(el_effect_handle_t effect, int32_t index, float arcStart)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::arcStart, arcStart,
                       "el_particle_set_arc_start");
}

el_result_e el_particle_get_arc_start(el_effect_handle_t effect, int32_t index, float *outArcStart)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::arcStart, outArcStart,
                       "el_particle_get_arc_start");
}

el_result_e el_particle_set_arc_length(el_effect_handle_t effect, int32_t index, float arcLength)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::arcLength, arcLength,
                       "el_particle_set_arc_length");
}

el_result_e el_particle_get_arc_length(el_effect_handle_t effect, int32_t index, float *outArcLength)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::arcLength, outArcLength,
                       "el_particle_get_arc_length");
}

el_result_e el_particle_set_use_custom_color(el_effect_handle_t effect, int32_t index, el_bool_t useCustomColor)
{
    return SetArcBool(effect, index, &EdgeLighting::ParticleArcConfig::useCustomColor, useCustomColor,
                      "el_particle_set_use_custom_color");
}

el_result_e el_particle_get_use_custom_color(el_effect_handle_t effect, int32_t index,
                                             el_bool_t *outUseCustomColor)
{
    return GetArcBool(effect, index, &EdgeLighting::ParticleArcConfig::useCustomColor, outUseCustomColor,
                      "el_particle_get_use_custom_color");
}

el_result_e el_particle_set_custom_color(el_effect_handle_t effect, int32_t index,
                                         float r, float g, float b, float a)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_custom_color");
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_set_custom_color");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    const glm::vec4 color(r, g, b, a);
    if (arc->customColor == color)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, rgba=(%f, %f, %f, %f)", (void *)effect, index, r, g, b, a);
    OwnArc(effect, index).customColor = color;
    return EL_SUCCESS;
}

el_result_e el_particle_get_custom_color(el_effect_handle_t effect, int32_t index,
                                         float *outR, float *outG, float *outB, float *outA)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_get_custom_color");
    VALIDATE_OUT_PTR(outR, "el_particle_get_custom_color");
    VALIDATE_OUT_PTR(outG, "el_particle_get_custom_color");
    VALIDATE_OUT_PTR(outB, "el_particle_get_custom_color");
    VALIDATE_OUT_PTR(outA, "el_particle_get_custom_color");
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_get_custom_color");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outR = arc->customColor.r;
    *outG = arc->customColor.g;
    *outB = arc->customColor.b;
    *outA = arc->customColor.a;
    return EL_SUCCESS;
}

el_result_e el_particle_set_mode(el_effect_handle_t effect, int32_t index, el_particle_mode_e mode)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_mode");
    // As an int: a host across the ABI can hand over any value, and the
    // compiler may assume an enum holds only its enumerators.
    if (static_cast<int>(mode) < EL_PARTICLE_MODE_MOVEMENT || static_cast<int>(mode) > EL_PARTICLE_MODE_HEAD_SPARKS)
    {
        LOG_E("el_particle_set_mode: mode %d is not an el_particle_mode_e", (int)mode);
        return EL_ERROR_INVALID_PARAMETER;
    }
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_set_mode");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    const auto m = static_cast<EdgeLighting::ParticleMode>(mode);
    if (arc->mode == m)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, mode=%d", (void *)effect, index, (int)mode);
    OwnArc(effect, index).mode = m;
    return EL_SUCCESS;
}

el_result_e el_particle_get_mode(el_effect_handle_t effect, int32_t index, el_particle_mode_e *outMode)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_get_mode");
    VALIDATE_OUT_PTR(outMode, "el_particle_get_mode");
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_get_mode");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outMode = static_cast<el_particle_mode_e>(arc->mode);
    return EL_SUCCESS;
}

el_result_e el_particle_set_emission_rate(el_effect_handle_t effect, int32_t index, float emissionRate)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::emissionRate, emissionRate,
                       "el_particle_set_emission_rate");
}

el_result_e el_particle_get_emission_rate(el_effect_handle_t effect, int32_t index, float *outEmissionRate)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::emissionRate, outEmissionRate,
                       "el_particle_get_emission_rate");
}

el_result_e el_particle_set_lifetime(el_effect_handle_t effect, int32_t index, float lifetime)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::lifetime, lifetime,
                       "el_particle_set_lifetime");
}

el_result_e el_particle_get_lifetime(el_effect_handle_t effect, int32_t index, float *outLifetime)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::lifetime, outLifetime,
                       "el_particle_get_lifetime");
}

el_result_e el_particle_set_speed(el_effect_handle_t effect, int32_t index, float speed)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::speed, speed,
                       "el_particle_set_speed");
}

el_result_e el_particle_get_speed(el_effect_handle_t effect, int32_t index, float *outSpeed)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::speed, outSpeed,
                       "el_particle_get_speed");
}

el_result_e el_particle_set_drag(el_effect_handle_t effect, int32_t index, float drag)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::drag, drag,
                       "el_particle_set_drag");
}

el_result_e el_particle_get_drag(el_effect_handle_t effect, int32_t index, float *outDrag)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::drag, outDrag,
                       "el_particle_get_drag");
}

el_result_e el_particle_set_size_start(el_effect_handle_t effect, int32_t index, float sizeStart)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::sizeStart, sizeStart,
                       "el_particle_set_size_start");
}

el_result_e el_particle_get_size_start(el_effect_handle_t effect, int32_t index, float *outSizeStart)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::sizeStart, outSizeStart,
                       "el_particle_get_size_start");
}

el_result_e el_particle_set_max_particles(el_effect_handle_t effect, int32_t index, int32_t maxParticles)
{
    VALIDATE_EFFECT_PTR(effect, "el_particle_set_max_particles");
    const EdgeLighting::ParticleArcConfig *arc = ArcInUse(effect, index, "el_particle_set_max_particles");
    if (!arc)
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (arc->maxParticles == maxParticles)
    {
        return EL_SUCCESS;
    }
    LOG_I("effect=%p, index=%d, maxParticles=%d", (void *)effect, index, maxParticles);
    OwnArc(effect, index).maxParticles = maxParticles;
    return EL_SUCCESS;
}

el_result_e el_particle_get_max_particles(el_effect_handle_t effect, int32_t index, int32_t *outMaxParticles)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::maxParticles, outMaxParticles,
                       "el_particle_get_max_particles");
}

el_result_e el_particle_set_head_bias(el_effect_handle_t effect, int32_t index, float headBias)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::headBias, headBias,
                       "el_particle_set_head_bias");
}

el_result_e el_particle_get_head_bias(el_effect_handle_t effect, int32_t index, float *outHeadBias)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::headBias, outHeadBias,
                       "el_particle_get_head_bias");
}

el_result_e el_particle_set_scatter_radius(el_effect_handle_t effect, int32_t index, float scatterRadius)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::scatterRadius, scatterRadius,
                       "el_particle_set_scatter_radius");
}

el_result_e el_particle_get_scatter_radius(el_effect_handle_t effect, int32_t index, float *outScatterRadius)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::scatterRadius, outScatterRadius,
                       "el_particle_get_scatter_radius");
}

el_result_e el_particle_set_tail_size_fraction(el_effect_handle_t effect, int32_t index, float tailSizeFraction)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::tailSizeFraction, tailSizeFraction,
                       "el_particle_set_tail_size_fraction");
}

el_result_e el_particle_get_tail_size_fraction(el_effect_handle_t effect, int32_t index,
                                               float *outTailSizeFraction)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::tailSizeFraction, outTailSizeFraction,
                       "el_particle_get_tail_size_fraction");
}

el_result_e el_particle_set_spread_deg(el_effect_handle_t effect, int32_t index, float spreadDeg)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::spreadDeg, spreadDeg,
                       "el_particle_set_spread_deg");
}

el_result_e el_particle_get_spread_deg(el_effect_handle_t effect, int32_t index, float *outSpreadDeg)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::spreadDeg, outSpreadDeg,
                       "el_particle_get_spread_deg");
}

el_result_e el_particle_set_outward_bias(el_effect_handle_t effect, int32_t index, float outwardBias)
{
    return SetArcFloat(effect, index, &EdgeLighting::ParticleArcConfig::outwardBias, outwardBias,
                       "el_particle_set_outward_bias");
}

el_result_e el_particle_get_outward_bias(el_effect_handle_t effect, int32_t index, float *outOutwardBias)
{
    return GetArcField(effect, index, &EdgeLighting::ParticleArcConfig::outwardBias, outOutwardBias,
                       "el_particle_get_outward_bias");
}
