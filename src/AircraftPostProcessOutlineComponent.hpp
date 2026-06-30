#ifndef AIRCRAFTPOSTPROCESSOUTLINECOMPONENT_HPP
#define AIRCRAFTPOSTPROCESSOUTLINECOMPONENT_HPP

#pragma once

#include "Component.hpp"
#include "GraphicsTypes.hpp"
#include "RenderUnit.hpp"

#include <glm/glm.hpp>
#include <functional>
#include <utility>
#include <vector>

enum class AircraftPostProcessOutlineMode {
    DilateOnly,
    SobelOnly,
    DilateSobelHybrid,
    BlenderStyle
};

struct AircraftPostProcessOutlineConfig {
    float thicknessPixels = 5.0f;
    bool occlusionAware = true;
    AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid;
};

// Supplies the model-to-world matrix for one mesh at draw time.
// Returning an empty optional (nullptr function) means "use the component's own ctx.model".
using MatrixProvider = std::function<glm::mat4()>;

class AircraftPostProcessOutlineComponent : public Component {
    RTTR_ENABLE(Component)
public:
    AircraftPostProcessOutlineComponent(const MeshPtr &sourceMesh,
                                        const glm::vec3 &outlineColor,
                                        float thicknessPixels = 5.0f,
                                        AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid);
    AircraftPostProcessOutlineComponent(const std::vector<MeshPtr> &sourceMeshes,
                                        const glm::vec3 &outlineColor,
                                        float thicknessPixels = 5.0f,
                                        AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid);
    // Renders meshes from multiple SceneObjects into one shared mask FBO.
    // Each MatrixProvider returns the world matrix of its mesh's SceneObject.
    AircraftPostProcessOutlineComponent(const std::vector<std::pair<MeshPtr, MatrixProvider>> &sourcePairs,
                                        const glm::vec3 &outlineColor,
                                        float thicknessPixels = 5.0f,
                                        AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid);
    // Dynamic hierarchy root: traverses the SceneObject tree at draw time so that
    // child objects (e.g. missiles mounted after init) are included automatically.
    AircraftPostProcessOutlineComponent(const SceneObjectWeakPtr &rootObject,
                                        const glm::vec3 &outlineColor,
                                        float thicknessPixels = 5.0f,
                                        AircraftPostProcessOutlineMode mode = AircraftPostProcessOutlineMode::DilateSobelHybrid);
    ~AircraftPostProcessOutlineComponent() override;

    void initialize() override;
    void update(float deltaTime) override;

    std::vector<RenderUnit> getAllRenderUnits() override { return {m_renderUnit}; }

    void setOutlineColor(const glm::vec3 &outlineColor);
    glm::vec3 getOutlineColor() const { return m_outlineColor; }

    void setThicknessPixels(float thicknessPixels);
    float getThicknessPixels() const { return m_config->thicknessPixels; }

    void setOutlineMode(AircraftPostProcessOutlineMode mode);
    AircraftPostProcessOutlineMode getOutlineMode() const { return m_config->mode; }

    void setOcclusionAware(bool enabled);
    bool isOcclusionAware() const { return m_config->occlusionAware; }

protected:
    void onSetEnabled(bool enabled) override;

private:
    std::vector<std::pair<MeshPtr, MatrixProvider>> m_sourceMeshes;
    std::shared_ptr<IRenderable> m_outlineRenderable;
    MaterialPtr m_material;
    RenderUnit m_renderUnit;
    std::shared_ptr<glm::mat4> m_modelMat;
    std::shared_ptr<AircraftPostProcessOutlineConfig> m_config;
    glm::vec3 m_outlineColor = glm::vec3(1.0f);
};

#endif // AIRCRAFTPOSTPROCESSOUTLINECOMPONENT_HPP
