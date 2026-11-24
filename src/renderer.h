#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "model.h"

enum class ShadingMode { Solid, SolidWireframe, Wireframe };

struct DrawSettings {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 model{1.0f};
    ShadingMode shading = ShadingMode::Solid;
    bool cullBackfaces = false;
    bool showTextures = true;
    bool showGrid = true;
    bool showPhysics = false;
};

// Owns the GPU copy of the current model and draws it.
class Renderer {
public:
    bool init(std::string& error);
    void shutdown();

    void setModel(const Model& model);
    // Re-upload vertex data only (same materials); used when normals are regenerated.
    void updateGeometry(const Model& model);
    void clearModel();
    // Ground grid in world space: square of half-size `halfExtent` centred on
    // `center`, lines every `spacing` units.
    void setGrid(const glm::vec3& center, float halfExtent, float spacing);

    void draw(const DrawSettings& settings);

    // File names of textures that were found but couldn't be decoded.
    const std::vector<std::string>& failedTextures() const { return failedTextures_; }

private:
    void clearGeometry();

    struct GpuMesh {
        unsigned vao = 0, vbo = 0, ebo = 0;
        int indexCount = 0;
        glm::vec3 color{0.8f};
        unsigned texture = 0;
        AlphaMode alphaMode = AlphaMode::Opaque;
        float alphaCutoff = 0.5f;
    };

    unsigned program_ = 0;
    int uMvp_ = -1, uNormalMatrix_ = -1, uColor_ = -1, uUseTexture_ = -1, uUnlit_ = -1, uTexture_ = -1, uAlphaMode_ = -1, uOpacity_ = -1, uAlphaCutoff_ = -1;

    std::vector<GpuMesh> meshes_;
    std::vector<unsigned> textures_;
    std::vector<unsigned> materialTextures_; // per material, 0 = none
    std::vector<std::string> failedTextures_;

    unsigned physicsVao_ = 0, physicsVbo_ = 0, physicsEbo_ = 0;
    int physicsIndexCount_ = 0;

    unsigned gridVao_ = 0, gridVbo_ = 0;
    int gridVertexCount_ = 0;
};
