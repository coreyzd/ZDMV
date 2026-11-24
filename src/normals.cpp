#include "normals.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace {

struct Key3 {
    std::int64_t x, y, z;
    bool operator==(const Key3& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct Key3Hash {
    std::size_t operator()(const Key3& k) const {
        std::size_t h = std::hash<std::int64_t>()(k.x);
        h ^= std::hash<std::int64_t>()(k.y) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= std::hash<std::int64_t>()(k.z) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

Key3 quantize(const glm::vec3& v, float cell) {
    return {std::llround(v.x / cell), std::llround(v.y / cell), std::llround(v.z / cell)};
}

// A triangle corner's contribution to the normal at its position.
struct Contribution {
    glm::vec3 faceNormal;
    float weight; // corner angle: independent of how the surface is triangulated
};

float cornerAngle(const glm::vec3& at, const glm::vec3& b, const glm::vec3& c) {
    const glm::vec3 e0 = b - at, e1 = c - at;
    const float l0 = glm::length(e0), l1 = glm::length(e1);
    if (l0 <= 0.0f || l1 <= 0.0f) return 0.0f;
    return std::acos(glm::clamp(glm::dot(e0, e1) / (l0 * l1), -1.0f, 1.0f));
}

bool faceNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, glm::vec3& out) {
    const glm::vec3 n = glm::cross(b - a, c - a);
    const float len = glm::length(n);
    if (!(len > 0.0f)) return false; // degenerate triangle
    out = n / len;
    return true;
}

// Rebuilds a mesh from per-corner normals, re-sharing vertices that end up identical.
class MeshBuilder {
public:
    explicit MeshBuilder(const Mesh& source) : source_(source) {
        out_.name = source.name;
        out_.material = source.material;
        out_.indices.reserve(source.indices.size());
    }
    void addCorner(std::uint32_t sourceIndex, const glm::vec3& normal) {
        const Key3 n = quantize(normal, 1e-4f);
        const Key key{sourceIndex, n};
        auto it = seen_.find(key);
        if (it == seen_.end()) {
            Vertex v = source_.vertices[sourceIndex];
            v.normal = normal;
            it = seen_.emplace(key, static_cast<std::uint32_t>(out_.vertices.size())).first;
            out_.vertices.push_back(v);
        }
        out_.indices.push_back(it->second);
    }
    Mesh take() { return std::move(out_); }

private:
    struct Key {
        std::uint32_t index;
        Key3 normal;
        bool operator==(const Key& o) const { return index == o.index && normal == o.normal; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const { return Key3Hash()(k.normal) * 31u + k.index; }
    };
    const Mesh& source_;
    Mesh out_;
    std::unordered_map<Key, std::uint32_t, KeyHash> seen_;
};

Model copyWithoutMeshes(const Model& source) {
    Model out;
    out.source = source.source;
    out.materials = source.materials;
    out.boundsMin = source.boundsMin;
    out.boundsMax = source.boundsMax;
    out.zUp = source.zUp;
    out.warnings = source.warnings;
    out.contentRoots = source.contentRoots;
    out.physics = source.physics;
    out.physicsParts = source.physicsParts;
    return out;
}

} // namespace

bool isFaceted(const Model& model) {
    std::size_t triangles = 0, flat = 0;
    for (const auto& mesh : model.meshes) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const glm::vec3& a = mesh.vertices[mesh.indices[i]].normal;
            const glm::vec3& b = mesh.vertices[mesh.indices[i + 1]].normal;
            const glm::vec3& c = mesh.vertices[mesh.indices[i + 2]].normal;
            ++triangles;
            if (glm::dot(a, b) > 0.9999f && glm::dot(a, c) > 0.9999f) ++flat;
        }
    }
    return triangles > 0 && flat * 10 >= triangles * 9;
}

Model withSmoothNormals(const Model& source, float creaseDegrees) {
    // Positions closer than this are treated as the same point.
    const float diagonal = glm::length(source.boundsMax - source.boundsMin);
    const float cell = diagonal > 0.0f ? diagonal * 1e-5f : 1e-6f;
    const float creaseCos = std::cos(glm::radians(glm::clamp(creaseDegrees, 0.0f, 180.0f)));

    // Pass 1: every triangle corner, grouped by welded position.
    std::unordered_map<Key3, std::vector<Contribution>, Key3Hash> byPosition;
    for (const auto& mesh : source.meshes) {
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const glm::vec3 p[3] = {mesh.vertices[mesh.indices[i]].position,
                                    mesh.vertices[mesh.indices[i + 1]].position,
                                    mesh.vertices[mesh.indices[i + 2]].position};
            glm::vec3 fn;
            if (!faceNormal(p[0], p[1], p[2], fn)) continue;
            for (int c = 0; c < 3; ++c) {
                const float angle = cornerAngle(p[c], p[(c + 1) % 3], p[(c + 2) % 3]);
                byPosition[quantize(p[c], cell)].push_back({fn, angle});
            }
        }
    }

    // Pass 2: each corner averages the faces around it that are within the
    // crease angle of its own face.
    Model out = copyWithoutMeshes(source);
    for (const auto& mesh : source.meshes) {
        MeshBuilder builder(mesh);
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const std::uint32_t idx[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
            const glm::vec3 p[3] = {mesh.vertices[idx[0]].position, mesh.vertices[idx[1]].position,
                                    mesh.vertices[idx[2]].position};
            glm::vec3 fn;
            if (!faceNormal(p[0], p[1], p[2], fn)) continue; // drop degenerate triangles
            for (int c = 0; c < 3; ++c) {
                glm::vec3 sum(0.0f);
                for (const Contribution& other : byPosition[quantize(p[c], cell)]) {
                    if (glm::dot(fn, other.faceNormal) >= creaseCos) sum += other.faceNormal * other.weight;
                }
                const float len = glm::length(sum);
                builder.addCorner(idx[c], len > 0.0f ? sum / len : fn);
            }
        }
        out.meshes.push_back(builder.take());
    }
    return out;
}

Model withFlatNormals(const Model& source) {
    Model out = copyWithoutMeshes(source);
    for (const auto& mesh : source.meshes) {
        MeshBuilder builder(mesh);
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const std::uint32_t idx[3] = {mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]};
            glm::vec3 fn;
            if (!faceNormal(mesh.vertices[idx[0]].position, mesh.vertices[idx[1]].position,
                            mesh.vertices[idx[2]].position, fn)) continue;
            for (std::uint32_t index : idx) builder.addCorner(index, fn);
        }
        out.meshes.push_back(builder.take());
    }
    return out;
}
