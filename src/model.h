// Format-independent, CPU-side model data. Every loader (OBJ today, Source
// Engine MDL later) fills in one of these; the renderer only knows this type.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
};

enum class AlphaMode {
    Opaque, // ignore the texture's alpha channel (Source often keeps masks there)
    Test,   // discard texels below Material::alphaCutoff
    Blend,  // alpha blend
};

struct Material {
    std::string name;
    glm::vec3 diffuse{0.8f};
    // Absolute path to the diffuse texture, empty if none.
    std::filesystem::path diffuseTexture;
    AlphaMode alphaMode = AlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    // Set by the loader when the texture couldn't be located: what was
    // missing, for display to the user.
    std::string problem;
};

// One draw call: triangles sharing a single material.
struct Mesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices; // triangle list
    int material = -1;                  // index into Model::materials, -1 = default
};

struct Model {
    std::filesystem::path source;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
    // Source Engine content is Z-up; loaders for such formats set this so the
    // viewer can default its up-axis toggle sensibly.
    bool zUp = false;
    // Collision geometry, if the format has any (Source .phy): triangles in
    // model space, made of `physicsParts` convex pieces. Normals/UVs unused.
    Mesh physics;
    int physicsParts = 0;
    // Non-fatal problems reported by the loader (missing MTL, etc).
    std::string warnings;
    // Folders the loader searched for materials, in priority order (formats
    // whose textures live outside the model's own folder, i.e. Source).
    std::vector<std::filesystem::path> contentRoots;

    std::size_t vertexCount() const {
        std::size_t n = 0;
        for (const auto& m : meshes) n += m.vertices.size();
        return n;
    }
    std::size_t triangleCount() const {
        std::size_t n = 0;
        for (const auto& m : meshes) n += m.indices.size() / 3;
        return n;
    }
    void computeBounds() {
        bool first = true;
        for (const auto& m : meshes) {
            for (const auto& v : m.vertices) {
                if (first) { boundsMin = boundsMax = v.position; first = false; }
                boundsMin = glm::min(boundsMin, v.position);
                boundsMax = glm::max(boundsMax, v.position);
            }
        }
        if (first) boundsMin = boundsMax = glm::vec3(0.0f);
    }
};
