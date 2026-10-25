#include "capi-internal.h"

// ==========================================================================
// Particles (Config::particle)
// ==========================================================================

namespace
{
    /// C -> C++: every field of the struct into one ParticleArcConfig.
    EdgeLighting::ParticleArcConfig ToArcConfig(const el_particle_config_t &in)
    {
        EdgeLighting::ParticleArcConfig out;
        out.enable = in.enable != 0;
        out.arcStart = in.arcStart;
        out.arcLength = in.arcLength;
        out.useCustomColor = in.useCustomColor != 0;
        out.customColor = glm::vec4(in.customColorR, in.customColorG, in.customColorB, in.customColorA);
        out.mode = static_cast<EdgeLighting::ParticleMode>(in.mode);
        out.emissionRate = in.emissionRate;
        out.lifetime = in.lifetime;
        out.speed = in.speed;
        out.drag = in.drag;
        out.sizeStart = in.sizeStart;
        out.maxParticles = in.maxParticles;
        out.headBias = in.headBias;
        out.scatterRadius = in.scatterRadius;
        out.tailSizeFraction = in.tailSizeFraction;
        out.spreadDeg = in.spreadDeg;
        out.outwardBias = in.outwardBias;
        return out;
    }

    /// C++ -> C: the same fields back.
    el_particle_config_t FromArcConfig(const EdgeLighting::ParticleArcConfig &in)
    {
        el_particle_config_t out;
        out.enable = in.enable ? 1 : 0;
        out.arcStart = in.arcStart;
        out.arcLength = in.arcLength;
        out.useCustomColor = in.useCustomColor ? 1 : 0;
        out.customColorR = in.customColor.r;
        out.customColorG = in.customColor.g;
        out.customColorB = in.customColor.b;
        out.customColorA = in.customColor.a;
        out.mode = static_cast<int32_t>(in.mode);
        out.emissionRate = in.emissionRate;
        out.lifetime = in.lifetime;
        out.speed = in.speed;
        out.drag = in.drag;
        out.sizeStart = in.sizeStart;
        out.maxParticles = in.maxParticles;
        out.headBias = in.headBias;
        out.scatterRadius = in.scatterRadius;
        out.tailSizeFraction = in.tailSizeFraction;
        out.spreadDeg = in.spreadDeg;
        out.outwardBias = in.outwardBias;
        return out;
    }

    /// What a list grows with: a copy of its last config, the defaults when
    /// it is empty. With neon arcs a neon arc past the list already showed
    /// the last config, so growing changes nothing on screen.
    EdgeLighting::ParticleArcConfig GrowthConfig(const std::vector<EdgeLighting::ParticleArcConfig> &arcs)
    {
        return arcs.empty() ? EdgeLighting::ParticleArcConfig{} : arcs.back();
    }

    /// An incoming config the layer can take: present, with a known mode.
    bool IsValidConfig(const el_particle_config_t *config, const char *who)
    {
        if (config == nullptr)
        {
            LOG_E("%s: config is null", who);
            return false;
        }
        if (config->mode < EL_PARTICLE_MODE_MOVEMENT || config->mode > EL_PARTICLE_MODE_HEAD_SPARKS)
        {
            LOG_E("%s: mode %d is not an el_particle_mode_e", who, config->mode);
            return false;
        }
        return true;
    }
}

el_result_e el_particle_config_init(el_particle_config_t *config)
{
    VALIDATE_OUT_PTR(config, "el_particle_config_init");
    *config = FromArcConfig(EdgeLighting::ParticleArcConfig{});
    return EL_SUCCESS;
}

el_result_e el_effect_set_particle_renderer_enabled(el_effect_handle_t effect, el_bool_t enabled)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_set_particle_renderer_enabled");
    SET_AND_LOG(effect->config.particle.enable, enabled != 0,
                "effect=%p, enabled=%d", (void *)effect, (int)enabled);
}

el_result_e el_effect_get_particle_renderer_enabled(el_effect_handle_t effect, el_bool_t *outEnabled)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_get_particle_renderer_enabled");
    VALIDATE_OUT_PTR(outEnabled, "el_effect_get_particle_renderer_enabled");
    *outEnabled = effect->config.particle.enable ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_effect_set_particle_use_neon_arc(el_effect_handle_t effect, el_bool_t useNeonArc)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_set_particle_use_neon_arc");
    SET_AND_LOG(effect->config.particle.useNeonArc, useNeonArc != 0,
                "effect=%p, useNeonArc=%d", (void *)effect, (int)useNeonArc);
}

el_result_e el_effect_get_particle_use_neon_arc(el_effect_handle_t effect, el_bool_t *outUseNeonArc)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_get_particle_use_neon_arc");
    VALIDATE_OUT_PTR(outUseNeonArc, "el_effect_get_particle_use_neon_arc");
    *outUseNeonArc = effect->config.particle.useNeonArc ? 1 : 0;
    return EL_SUCCESS;
}

el_result_e el_effect_set_particle_config(el_effect_handle_t effect, const el_particle_config_t *config)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_set_particle_config");
    if (!IsValidConfig(config, "el_effect_set_particle_config"))
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    // One config for the whole layer. With neon arcs it drives every one of
    // them: a neon arc past the end of the list takes the last config.
    const EdgeLighting::ParticleArcConfig arc = ToArcConfig(*config);
    auto &arcs = effect->config.particle.arcs;
    if (arcs.size() == 1 && arcs[0] == arc)
    {
        return EL_SUCCESS;
    }
    arcs.assign(1, arc);
    LOG_I("effect=%p, one config for every arc: mode=%d, emissionRate=%f, lifetime=%f",
          (void *)effect, config->mode, arc.emissionRate, arc.lifetime);
    return EL_SUCCESS;
}

el_result_e el_effect_get_particle_config(el_effect_handle_t effect, el_particle_config_t *outConfig)
{
    return el_effect_get_particle_arc_config(effect, 0, outConfig);
}

el_result_e el_effect_set_particle_arc_config(el_effect_handle_t effect, int32_t index,
                                              const el_particle_config_t *config)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_set_particle_arc_config");
    if (!IsValidConfig(config, "el_effect_set_particle_arc_config"))
    {
        return EL_ERROR_INVALID_PARAMETER;
    }
    if (index < 0 || index >= EdgeLighting::ParticleConfig::MAX_ARCS_CAP)
    {
        LOG_E("el_effect_set_particle_arc_config: index %d - use 0 to %d",
              index, EdgeLighting::ParticleConfig::MAX_ARCS_CAP - 1);
        return EL_ERROR_INVALID_PARAMETER;
    }
    const EdgeLighting::ParticleArcConfig arc = ToArcConfig(*config);
    auto &arcs = effect->config.particle.arcs;
    const size_t i = static_cast<size_t>(index);
    if (i < arcs.size() && arcs[i] == arc)
    {
        return EL_SUCCESS;
    }
    if (i >= arcs.size())
    {
        arcs.resize(i + 1, GrowthConfig(arcs));
    }
    arcs[i] = arc;
    LOG_I("effect=%p, index=%d (of %zu): mode=%d, emissionRate=%f, lifetime=%f",
          (void *)effect, index, arcs.size(), config->mode, arc.emissionRate, arc.lifetime);
    return EL_SUCCESS;
}

el_result_e el_effect_get_particle_arc_config(el_effect_handle_t effect, int32_t index,
                                              el_particle_config_t *outConfig)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_get_particle_arc_config");
    VALIDATE_OUT_PTR(outConfig, "el_effect_get_particle_arc_config");
    const auto &arcs = effect->config.particle.arcs;
    if (index < 0 || static_cast<size_t>(index) >= arcs.size())
    {
        LOG_E("el_effect_get_particle_arc_config: index %d out of range (size=%zu)", index, arcs.size());
        return EL_ERROR_INVALID_PARAMETER;
    }
    *outConfig = FromArcConfig(arcs[static_cast<size_t>(index)]);
    return EL_SUCCESS;
}

el_result_e el_effect_set_particle_arc_count(el_effect_handle_t effect, int32_t count)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_set_particle_arc_count");
    if (count < 0 || count > EdgeLighting::ParticleConfig::MAX_ARCS_CAP)
    {
        LOG_E("el_effect_set_particle_arc_count: count %d - use 0 to %d",
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

el_result_e el_effect_get_particle_arc_count(el_effect_handle_t effect, int32_t *outCount)
{
    VALIDATE_EFFECT_PTR(effect, "el_effect_get_particle_arc_count");
    VALIDATE_OUT_PTR(outCount, "el_effect_get_particle_arc_count");
    *outCount = static_cast<int32_t>(effect->config.particle.arcs.size());
    return EL_SUCCESS;
}
