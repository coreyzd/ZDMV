// Wavefront OBJ (+ MTL) loader built on tinyobjloader.
#include "loaders/loader.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

namespace {

namespace fs = std::filesystem;

// MTL files written on Windows often use backslashes and odd casing.
fs::path resolveTexture(const fs::path& baseDir, std::string name) {
    if (name.empty()) return {};
    std::replace(name.begin(), name.end(), '\\', '/');
    fs::path p(name);
    if (p.is_relative()) p = baseDir / p;
    std::error_code ec;
    if (fs::exists(p, ec)) return p;

    // Fall back to a case-insensitive match on the file name.
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string want = lower(p.filename().string());
    for (fs::directory_iterator it(p.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (lower(it->path().filename().string()) == want) return it->path();
    }
    return {};
}

struct IndexKey {
    int v, n, t;
    bool operator==(const IndexKey& o) const { return v == o.v && n == o.n && t == o.t; }
};
struct IndexKeyHash {
    std::size_t operator()(const IndexKey& k) const {
        std::size_t h = std::hash<int>()(k.v);
        h ^= std::hash<int>()(k.n) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.t) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class ObjLoader final : public ModelLoader {
public:
    std::vector<std::string> extensions() const override { return {".obj"}; }

    bool load(const fs::path& path, Model& out, std::string& error) override {
        tinyobj::ObjReaderConfig config;
        config.triangulate = true;
        config.mtl_search_path = path.parent_path().string();

        tinyobj::ObjReader reader;
        if (!reader.ParseFromFile(path.string(), config)) {
            error = reader.Error().empty() ? "Failed to parse OBJ file." : reader.Error();
            return false;
        }
        out.warnings = reader.Warning();

        const tinyobj::attrib_t& attrib = reader.GetAttrib();
        const auto& shapes = reader.GetShapes();
        const auto& materials = reader.GetMaterials();
        const fs::path baseDir = path.parent_path();

        for (const auto& m : materials) {
            Material mat;
            mat.name = m.name;
            mat.diffuse = glm::vec3(m.diffuse[0], m.diffuse[1], m.diffuse[2]);
            mat.diffuseTexture = resolveTexture(baseDir, m.diffuse_texname);
            // A textured material with a black Kd is almost always meant to
            // show the texture unmodified.
            if (!mat.diffuseTexture.empty()) mat.diffuse = glm::vec3(1.0f);
            if (!m.diffuse_texname.empty() && mat.diffuseTexture.empty()) mat.problem = "texture not found: " + m.diffuse_texname;
            mat.alphaMode = AlphaMode::Test; // cut-out PNGs are common
            mat.alphaCutoff = 0.1f;
            out.materials.push_back(std::move(mat));
        }

        const std::size_t positionCount = attrib.vertices.size() / 3;
        auto position = [&](int i) {
            return glm::vec3(attrib.vertices[3 * i], attrib.vertices[3 * i + 1], attrib.vertices[3 * i + 2]);
        };

        // Smooth normals for faces that don't provide their own.
        std::vector<glm::vec3> generated;
        auto generatedNormals = [&]() -> const std::vector<glm::vec3>& {
            if (!generated.empty()) return generated;
            generated.assign(positionCount, glm::vec3(0.0f));
            for (const auto& shape : shapes) {
                const auto& idx = shape.mesh.indices;
                for (std::size_t i = 0; i + 2 < idx.size(); i += 3) {
                    const int a = idx[i].vertex_index, b = idx[i + 1].vertex_index, c = idx[i + 2].vertex_index;
                    // Cross product length is twice the triangle area, so
                    // this is an area-weighted average.
                    const glm::vec3 n = glm::cross(position(b) - position(a), position(c) - position(a));
                    generated[a] += n;
                    generated[b] += n;
                    generated[c] += n;
                }
            }
            for (auto& n : generated) {
                const float len = glm::length(n);
                n = len > 0.0f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
            }
            return generated;
        };

        // One mesh per material so each is a single draw call.
        std::map<int, std::size_t> meshForMaterial;
        std::vector<std::unordered_map<IndexKey, std::uint32_t, IndexKeyHash>> dedup;

        for (const auto& shape : shapes) {
            const auto& idx = shape.mesh.indices;
            const std::size_t faceCount = idx.size() / 3;
            for (std::size_t f = 0; f < faceCount; ++f) {
                int matId = f < shape.mesh.material_ids.size() ? shape.mesh.material_ids[f] : -1;
                if (matId < 0 || matId >= static_cast<int>(out.materials.size())) matId = -1;

                auto found = meshForMaterial.find(matId);
                if (found == meshForMaterial.end()) {
                    found = meshForMaterial.emplace(matId, out.meshes.size()).first;
                    Mesh mesh;
                    mesh.material = matId;
                    mesh.name = matId >= 0 ? out.materials[matId].name : "default";
                    out.meshes.push_back(std::move(mesh));
                    dedup.emplace_back();
                }
                Mesh& mesh = out.meshes[found->second];
                auto& seen = dedup[found->second];

                for (int corner = 0; corner < 3; ++corner) {
                    const tinyobj::index_t& i = idx[3 * f + corner];
                    if (i.vertex_index < 0 || static_cast<std::size_t>(i.vertex_index) >= positionCount) {
                        error = "OBJ references a vertex that does not exist.";
                        return false;
                    }
                    const IndexKey key{i.vertex_index, i.normal_index, i.texcoord_index};
                    auto it = seen.find(key);
                    if (it == seen.end()) {
                        Vertex v;
                        v.position = position(i.vertex_index);
                        if (i.normal_index >= 0 && 3 * static_cast<std::size_t>(i.normal_index) + 2 < attrib.normals.size()) {
                            v.normal = glm::vec3(attrib.normals[3 * i.normal_index],
                                                 attrib.normals[3 * i.normal_index + 1],
                                                 attrib.normals[3 * i.normal_index + 2]);
                        } else {
                            v.normal = generatedNormals()[i.vertex_index];
                        }
                        if (i.texcoord_index >= 0 && 2 * static_cast<std::size_t>(i.texcoord_index) + 1 < attrib.texcoords.size()) {
                            v.uv = glm::vec2(attrib.texcoords[2 * i.texcoord_index],
                                             attrib.texcoords[2 * i.texcoord_index + 1]);
                        }
                        it = seen.emplace(key, static_cast<std::uint32_t>(mesh.vertices.size())).first;
                        mesh.vertices.push_back(v);
                    }
                    mesh.indices.push_back(it->second);
                }
            }
        }
        return true;
    }
};

} // namespace

std::unique_ptr<ModelLoader> makeObjLoader() { return std::make_unique<ObjLoader>(); }
