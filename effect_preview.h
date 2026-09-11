#pragma once

#include "raylib.h"
#include "lvlx_format.h"

#include <cstddef>
#include <memory>
#include <string>

class EffectPreview {
public:
    EffectPreview();
    ~EffectPreview();
    EffectPreview(const EffectPreview&) = delete;
    EffectPreview& operator=(const EffectPreview&) = delete;

    void BuildFrame(const LvlxLevel& level, const LvlxDefinitionTable& definitions,
        const Camera3D& camera, const std::string& effectsDirectory,
        const std::string& texturesDirectory,const std::string& fallbackEffectsDirectory,
        const std::string& fallbackTexturesDirectory,float elapsedSeconds,
        float nearClipStart, float nearClipEnd);
    // Receiver-local selection: camera-nearest lights can belong to unrelated objects.
    // Seven points plus the directional light fit the recovered eight-light shader.
    size_t ApplyPointLights(Shader shader, const BoundingBox& receiver) const;
    void Draw(const Camera3D& camera) const;
    void Unload();

    size_t LoadedTemplateCount() const;
    size_t FailedTemplateCount() const;
    size_t VisibleParticleCount() const;
    size_t VisibleLightCount() const;
    static bool ValidateTemplateFile(const std::string& path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
