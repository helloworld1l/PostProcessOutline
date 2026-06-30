#include "AircraftPostProcessOutlineComponent.hpp"

#include "GResourceManager.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
#include "SceneObject.hpp"
#include "ShaderProgram.hpp"

#include <algorithm>
#include <cstddef>
#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace {

constexpr int kOutlineRenderPriority = 1600;
constexpr float kMinimumThicknessPixels = 1.0f;
constexpr int kMaxOutlineKernelRadiusPixels = 8;

struct FullscreenQuadVertex {
    glm::vec2 position = glm::vec2(0.0f);
    glm::vec2 texCoord = glm::vec2(0.0f);
};

class FullscreenQuadRenderer {
public:
    ~FullscreenQuadRenderer()
    {
        clearGL();
    }

    void ensureInitialized()
    {
        if (m_vao != 0 && m_vbo != 0) {
            return;
        }

        static constexpr std::array<FullscreenQuadVertex, 4> kVertices = {{
            {{-1.0f, -1.0f}, {0.0f, 0.0f}},
            {{ 1.0f, -1.0f}, {1.0f, 0.0f}},
            {{-1.0f,  1.0f}, {0.0f, 1.0f}},
            {{ 1.0f,  1.0f}, {1.0f, 1.0f}},
        }};

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);

        glGenBuffers(1, &m_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(sizeof(FullscreenQuadVertex) * kVertices.size()),
                     kVertices.data(),
                     GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0,
                              2,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(FullscreenQuadVertex),
                              reinterpret_cast<void *>(offsetof(FullscreenQuadVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1,
                              2,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(FullscreenQuadVertex),
                              reinterpret_cast<void *>(offsetof(FullscreenQuadVertex, texCoord)));

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    void draw() const
    {
        if (m_vao == 0) {
            return;
        }

        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }

private:
    void clearGL()
    {
        if (m_vbo != 0) {
            glDeleteBuffers(1, &m_vbo);
            m_vbo = 0;
        }
        if (m_vao != 0) {
            glDeleteVertexArrays(1, &m_vao);
            m_vao = 0;
        }
    }

    GLuint m_vao = 0;
    GLuint m_vbo = 0;
};

class OutlineMaskFramebuffer {
public:
    ~OutlineMaskFramebuffer()
    {
        release();
    }

    bool ensureSize(int width, int height)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (m_width == width && m_height == height && m_fbo != 0 && m_maskTexture != 0 && m_selectedDepthTexture != 0 && m_sceneDepthTexture != 0) {
            return true;
        }

        release();

        m_width = width;
        m_height = height;

        glGenFramebuffers(1, &m_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

        glGenTextures(1, &m_maskTexture);
        glBindTexture(GL_TEXTURE_2D, m_maskTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, m_width, m_height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_maskTexture, 0);

        glGenTextures(1, &m_selectedDepthTexture);
        glBindTexture(GL_TEXTURE_2D, m_selectedDepthTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_width, m_height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_selectedDepthTexture, 0);

        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);

        const bool framebufferComplete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        glGenTextures(1, &m_sceneDepthTexture);
        glBindTexture(GL_TEXTURE_2D, m_sceneDepthTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, m_width, m_height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (!framebufferComplete) {
            release();
            return false;
        }
        return true;
    }

    void copySceneDepthFromCurrentFramebuffer(int viewportX, int viewportY)
    {
        if (m_sceneDepthTexture == 0 || m_width <= 0 || m_height <= 0) {
            return;
        }

        glBindTexture(GL_TEXTURE_2D, m_sceneDepthTexture);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewportX, viewportY, m_width, m_height);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void bindMaskPass() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    }

    GLuint maskTexture() const { return m_maskTexture; }
    GLuint selectedDepthTexture() const { return m_selectedDepthTexture; }
    GLuint sceneDepthTexture() const { return m_sceneDepthTexture; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    void release()
    {
        if (m_sceneDepthTexture != 0) {
            glDeleteTextures(1, &m_sceneDepthTexture);
            m_sceneDepthTexture = 0;
        }
        if (m_selectedDepthTexture != 0) {
            glDeleteTextures(1, &m_selectedDepthTexture);
            m_selectedDepthTexture = 0;
        }
        if (m_maskTexture != 0) {
            glDeleteTextures(1, &m_maskTexture);
            m_maskTexture = 0;
        }
        if (m_fbo != 0) {
            glDeleteFramebuffers(1, &m_fbo);
            m_fbo = 0;
        }
        m_width = 0;
        m_height = 0;
    }

    GLuint m_fbo = 0;
    GLuint m_maskTexture = 0;
    GLuint m_selectedDepthTexture = 0;
    GLuint m_sceneDepthTexture = 0;
    int m_width = 0;
    int m_height = 0;
};

static void collectMeshesFromHierarchy(const SceneObjectPtr &object,
                                       std::vector<std::pair<MeshPtr, glm::mat4>> &out)
{
    if (!object || !object->visible()) {
        return;
    }
    const glm::mat4 worldMatrix = object->getWorldMatrix();
    for (const auto &unit : object->getRenderUnit()) {
        auto mesh = std::dynamic_pointer_cast<Mesh>(unit.renderable);
        if (mesh && mesh->getBounds().isValid()) {
            out.emplace_back(mesh, worldMatrix);
        }
    }
    for (const auto &child : object->getChildren()) {
        collectMeshesFromHierarchy(child, out);
    }
}

class AircraftPostProcessOutlineRenderable : public IRenderable {
public:
    // Fixed mesh list with per-mesh matrix providers.
    AircraftPostProcessOutlineRenderable(std::vector<std::pair<MeshPtr, MatrixProvider>> sourceMeshes,
                                         std::shared_ptr<AircraftPostProcessOutlineConfig> config)
        : m_sourceMeshes(std::move(sourceMeshes))
        , m_config(std::move(config))
    {
    }

    // Dynamic root: traverses the SceneObject hierarchy at draw time.
    AircraftPostProcessOutlineRenderable(SceneObjectWeakPtr rootObject,
                                         std::shared_ptr<AircraftPostProcessOutlineConfig> config)
        : m_rootObject(std::move(rootObject))
        , m_config(std::move(config))
    {
    }

    void draw(const RenderContext &ctx, const ShaderProgram &shader) override
    {
        if (!isEnabled() || m_config == nullptr) {
            return;
        }

        // Build the active mesh list for this frame.
        std::vector<std::pair<MeshPtr, glm::mat4>> activeMeshes;
        auto root = m_rootObject.lock();
        if (root) {
            collectMeshesFromHierarchy(root, activeMeshes);
        } else {
            activeMeshes.reserve(m_sourceMeshes.size());
            for (const auto &[mesh, provider] : m_sourceMeshes) {
                if (!mesh) { continue; }
                activeMeshes.emplace_back(mesh, provider ? provider() : ctx.model);
            }
        }
        if (activeMeshes.empty()) {
            return;
        }

        const int viewportX = static_cast<int>(ctx.viewport.x);
        const int viewportY = static_cast<int>(ctx.viewport.y);
        const int viewportWidth = std::max(static_cast<int>(ctx.viewport.z), 1);
        const int viewportHeight = std::max(static_cast<int>(ctx.viewport.w), 1);
        if (!m_maskFramebuffer.ensureSize(viewportWidth, viewportHeight)) {
            return;
        }

        auto maskShader = GResourceManager::getShaderProgram("aircraft_postprocess_mask");
        if (maskShader == nullptr) {
            return;
        }

        m_fullscreenQuad.ensureInitialized();

        GLint previousDrawFramebuffer = 0;
        GLint previousReadFramebuffer = 0;
        GLint previousViewport[4] = {0, 0, 0, 0};
        GLint previousActiveTexture = GL_TEXTURE0;
        GLint previousCullFaceMode = GL_BACK;
        GLint previousBlendSrcRgb = GL_ONE;
        GLint previousBlendDstRgb = GL_ZERO;
        GLint previousBlendSrcAlpha = GL_ONE;
        GLint previousBlendDstAlpha = GL_ZERO;
        GLboolean depthTestWasEnabled = glIsEnabled(GL_DEPTH_TEST);
        GLboolean cullFaceWasEnabled = glIsEnabled(GL_CULL_FACE);
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLboolean depthMaskWasEnabled = GL_TRUE;

        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        glGetIntegerv(GL_VIEWPORT, previousViewport);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMaskWasEnabled);
        glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFaceMode);
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousBlendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousBlendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousBlendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &previousBlendDstAlpha);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousDrawFramebuffer);
        m_maskFramebuffer.copySceneDepthFromCurrentFramebuffer(viewportX, viewportY);

        m_maskFramebuffer.bindMaskPass();
        glViewport(0, 0, viewportWidth, viewportHeight);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        maskShader->use();
        for (const auto &[mesh, worldMatrix] : activeMeshes) {
            RenderContext meshCtx = ctx;
            meshCtx.model = worldMatrix;
            mesh->draw(meshCtx, *maskShader);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, previousDrawFramebuffer);
        glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
        if (blendWasEnabled == GL_TRUE) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
        glBlendFuncSeparate(previousBlendSrcRgb,
                            previousBlendDstRgb,
                            previousBlendSrcAlpha,
                            previousBlendDstAlpha);
        if (cullFaceWasEnabled == GL_TRUE) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
        glCullFace(previousCullFaceMode);

        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);

        shader.use();
        shader.setUniform("maskTexture", 0);
        shader.setUniform("sceneDepthTexture", 1);
        shader.setUniform("selectedDepthTexture", 2);
        shader.setUniform("outlineScreenParams",
                          glm::vec4(static_cast<float>(viewportWidth),
                                    static_cast<float>(viewportHeight),
                                    1.0f / static_cast<float>(viewportWidth),
                                    1.0f / static_cast<float>(viewportHeight)));
        shader.setUniform("outlineThicknessPixels",
                          std::min(std::max(m_config->thicknessPixels, kMinimumThicknessPixels),
                                   static_cast<float>(kMaxOutlineKernelRadiusPixels)));
        shader.setUniform("outlineOcclusionAware", m_config->occlusionAware ? 1 : 0);
        shader.setUniform("outlineMode", static_cast<int>(m_config->mode));

        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, m_maskFramebuffer.maskTexture());
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, m_maskFramebuffer.sceneDepthTexture());
        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, m_maskFramebuffer.selectedDepthTexture());
        m_fullscreenQuad.draw();

        glActiveTexture(GL_TEXTURE0 + 2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0 + 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(previousActiveTexture);

        if (depthTestWasEnabled == GL_TRUE) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        glDepthMask(depthMaskWasEnabled);
        if (cullFaceWasEnabled == GL_TRUE) {
            glEnable(GL_CULL_FACE);
        } else {
            glDisable(GL_CULL_FACE);
        }
        glCullFace(previousCullFaceMode);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFramebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFramebuffer);
    }

private:
    SceneObjectWeakPtr m_rootObject;
    std::vector<std::pair<MeshPtr, MatrixProvider>> m_sourceMeshes;
    std::shared_ptr<AircraftPostProcessOutlineConfig> m_config;
    OutlineMaskFramebuffer m_maskFramebuffer;
    FullscreenQuadRenderer m_fullscreenQuad;
};

} // namespace

static void initOutlineComponent(AircraftPostProcessOutlineComponent &self,
                                  std::vector<std::pair<MeshPtr, MatrixProvider>> sourceMeshes,
                                  const glm::vec3 &outlineColor,
                                  float thicknessPixels,
                                  AircraftPostProcessOutlineMode mode,
                                  std::vector<std::pair<MeshPtr, MatrixProvider>> &outMeshes,
                                  std::shared_ptr<IRenderable> &outRenderable,
                                  MaterialPtr &outMaterial,
                                  RenderUnit &outRenderUnit,
                                  std::shared_ptr<glm::mat4> &outModelMat,
                                  std::shared_ptr<AircraftPostProcessOutlineConfig> &outConfig,
                                  glm::vec3 &outColor)
{
    outMeshes = std::move(sourceMeshes);
    outModelMat = std::make_shared<glm::mat4>(1.0f);
    outConfig = std::make_shared<AircraftPostProcessOutlineConfig>();
    outColor = outlineColor;

    outConfig->thicknessPixels = std::max(kMinimumThicknessPixels, thicknessPixels);
    outConfig->occlusionAware = true;
    outConfig->mode = mode;

    auto shader = GResourceManager::getShaderProgram("aircraft_postprocess_outline");
    if (shader != nullptr) {
        outMaterial = std::make_shared<Material>(shader);
        outMaterial->setUniform("outlineColor", outColor);
        outMaterial->setUniform("outlineThicknessPixels", outConfig->thicknessPixels);
        outMaterial->setUniform("outlineScreenParams", glm::vec4(1.0f));
        outMaterial->setUniform("outlineOcclusionAware", outConfig->occlusionAware ? 1 : 0);
        outMaterial->setUniform("outlineMode", static_cast<int>(outConfig->mode));
        outMaterial->setTransparent(true);
        outMaterial->setBlendMode(BlendMode::Alpha);
    }

    if (!outMeshes.empty()) {
        outRenderable = std::make_shared<AircraftPostProcessOutlineRenderable>(outMeshes, outConfig);
    }

    outRenderUnit = RenderUnit(outRenderable,
                               outMaterial,
                               std::static_pointer_cast<const glm::mat4>(outModelMat));
    outRenderUnit.layer = RenderLayer::Layer3D;
    outRenderUnit.priority = kOutlineRenderPriority;
}

AircraftPostProcessOutlineComponent::AircraftPostProcessOutlineComponent(const MeshPtr &sourceMesh,
                                                                         const glm::vec3 &outlineColor,
                                                                         float thicknessPixels,
                                                                         AircraftPostProcessOutlineMode mode)
{
    std::vector<std::pair<MeshPtr, MatrixProvider>> entries;
    if (sourceMesh != nullptr) {
        entries.push_back({sourceMesh, nullptr});
    }
    initOutlineComponent(*this, std::move(entries), outlineColor, thicknessPixels, mode,
                         m_sourceMeshes, m_outlineRenderable, m_material,
                         m_renderUnit, m_modelMat, m_config, m_outlineColor);
}

AircraftPostProcessOutlineComponent::AircraftPostProcessOutlineComponent(const std::vector<MeshPtr> &sourceMeshes,
                                                                         const glm::vec3 &outlineColor,
                                                                         float thicknessPixels,
                                                                         AircraftPostProcessOutlineMode mode)
{
    std::vector<std::pair<MeshPtr, MatrixProvider>> entries;
    entries.reserve(sourceMeshes.size());
    for (const auto &mesh : sourceMeshes) {
        entries.push_back({mesh, nullptr});
    }
    initOutlineComponent(*this, std::move(entries), outlineColor, thicknessPixels, mode,
                         m_sourceMeshes, m_outlineRenderable, m_material,
                         m_renderUnit, m_modelMat, m_config, m_outlineColor);
}

AircraftPostProcessOutlineComponent::AircraftPostProcessOutlineComponent(const std::vector<std::pair<MeshPtr, MatrixProvider>> &sourcePairs,
                                                                         const glm::vec3 &outlineColor,
                                                                         float thicknessPixels,
                                                                         AircraftPostProcessOutlineMode mode)
{
    initOutlineComponent(*this, std::vector<std::pair<MeshPtr, MatrixProvider>>(sourcePairs), outlineColor, thicknessPixels, mode,
                         m_sourceMeshes, m_outlineRenderable, m_material,
                         m_renderUnit, m_modelMat, m_config, m_outlineColor);
}

AircraftPostProcessOutlineComponent::AircraftPostProcessOutlineComponent(const SceneObjectWeakPtr &rootObject,
                                                                         const glm::vec3 &outlineColor,
                                                                         float thicknessPixels,
                                                                         AircraftPostProcessOutlineMode mode)
{
    m_modelMat = std::make_shared<glm::mat4>(1.0f);
    m_config = std::make_shared<AircraftPostProcessOutlineConfig>();
    m_outlineColor = outlineColor;
    m_config->thicknessPixels = std::max(kMinimumThicknessPixels, thicknessPixels);
    m_config->occlusionAware = true;
    m_config->mode = mode;

    auto shader = GResourceManager::getShaderProgram("aircraft_postprocess_outline");
    if (shader != nullptr) {
        m_material = std::make_shared<Material>(shader);
        m_material->setUniform("outlineColor", m_outlineColor);
        m_material->setUniform("outlineThicknessPixels", m_config->thicknessPixels);
        m_material->setUniform("outlineScreenParams", glm::vec4(1.0f));
        m_material->setUniform("outlineOcclusionAware", m_config->occlusionAware ? 1 : 0);
        m_material->setUniform("outlineMode", static_cast<int>(m_config->mode));
        m_material->setTransparent(true);
        m_material->setBlendMode(BlendMode::Alpha);
    }

    m_outlineRenderable = std::make_shared<AircraftPostProcessOutlineRenderable>(rootObject, m_config);

    m_renderUnit = RenderUnit(m_outlineRenderable,
                              m_material,
                              std::static_pointer_cast<const glm::mat4>(m_modelMat));
    m_renderUnit.layer = RenderLayer::Layer3D;
    m_renderUnit.priority = kOutlineRenderPriority;
}

AircraftPostProcessOutlineComponent::~AircraftPostProcessOutlineComponent() = default;


void AircraftPostProcessOutlineComponent::initialize()
{
}

void AircraftPostProcessOutlineComponent::setOutlineColor(const glm::vec3 &outlineColor)
{
    m_outlineColor = outlineColor;
    if (m_material != nullptr) {
        m_material->setUniform("outlineColor", m_outlineColor);
    }
}

void AircraftPostProcessOutlineComponent::setThicknessPixels(float thicknessPixels)
{
    m_config->thicknessPixels = std::max(kMinimumThicknessPixels, thicknessPixels);
    if (m_material != nullptr) {
        m_material->setUniform("outlineThicknessPixels", m_config->thicknessPixels);
    }
}

void AircraftPostProcessOutlineComponent::setOutlineMode(AircraftPostProcessOutlineMode mode)
{
    m_config->mode = mode;
    if (m_material != nullptr) {
        m_material->setUniform("outlineMode", static_cast<int>(m_config->mode));
    }
}

void AircraftPostProcessOutlineComponent::setOcclusionAware(bool enabled)
{
    m_config->occlusionAware = enabled;
    if (m_material != nullptr) {
        m_material->setUniform("outlineOcclusionAware", m_config->occlusionAware ? 1 : 0);
    }
}

void AircraftPostProcessOutlineComponent::onSetEnabled(bool enabled)
{
    if (m_outlineRenderable != nullptr) {
        m_outlineRenderable->setEnabled(enabled);
    }
}

void AircraftPostProcessOutlineComponent::update(float /*deltaTime*/)
{
    *m_modelMat = glm::mat4(1.0f);

    if (m_material == nullptr || m_outlineRenderable == nullptr || m_config == nullptr || !isEnabled()) {
        return;
    }

    m_material->setUniform("outlineColor", m_outlineColor);
    m_material->setUniform("outlineThicknessPixels", m_config->thicknessPixels);
    m_material->setUniform("outlineOcclusionAware", m_config->occlusionAware ? 1 : 0);
    m_material->setUniform("outlineMode", static_cast<int>(m_config->mode));
}
