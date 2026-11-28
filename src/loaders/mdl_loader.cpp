// Source Engine studio model loader (MDL v44-49, i.e. Source 2006 - 2013).
//
// A compiled model is split over three files:
//   .mdl  structure: body parts, meshes, material names
//   .vvd  vertex data shared by all meshes
//   .vtx  optimised triangle lists that index into the VVD
// Only the highest LOD is loaded, posed at the first frame of the model's
// first sequence (which is how the game and Hammer show a prop that isn't
// playing anything); flexes are ignored.
#include "loaders/loader.h"

#include <cstring>
#include <map>
#include <unordered_map>

#include "source/binary_reader.h"
#include "source/content.h"
#include "source/phy.h"
#include "source/pose.h"
#include "source/vmt.h"
#include "util.h"

namespace {

namespace fs = std::filesystem;

// Struct sizes and field offsets from the Source SDK (studio.h, optimize.h).
constexpr int kMdlTextureSize = 64;
constexpr int kMdlBodyPartSize = 16;
constexpr int kMdlModelSize = 148;
constexpr int kMdlMeshSize = 116;
constexpr int kVvdVertexSize = 48;
constexpr int kVtxMeshSize = 9;
constexpr int kVtxVertexSize = 9;

// Companion file next to the .mdl, matched ignoring case (tools on Windows
// are careless about it: "Crate.dx90.vtx" beside "crate.mdl").
fs::path findCompanion(const fs::path& mdl, const std::vector<std::string>& suffixes) {
    const std::string stem = mdl.stem().string();
    for (const auto& suffix : suffixes) {
        fs::path hit = resolveNoCase(mdl.parent_path(), stem + suffix);
        if (!hit.empty()) return hit;
    }
    return {};
}

std::string normalizeSlashes(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string stripExtension(std::string s, const char* ext) {
    const std::size_t n = std::strlen(ext);
    if (s.size() > n && equalsNoCase(s.substr(s.size() - n), ext)) s.resize(s.size() - n);
    return s;
}

// "[r g b]" (0-1) or "{r g b}" (0-255).
bool parseColor(const std::string& text, glm::vec3& out) {
    if (text.empty()) return false;
    const bool bytes = text[0] == '{';
    std::string digits = text;
    for (char& c : digits) {
        if (c == '[' || c == ']' || c == '{' || c == '}') c = ' ';
    }
    float r = 0, g = 0, b = 0;
    const int n = std::sscanf(digits.c_str(), "%f %f %f", &r, &g, &b);
    if (n == 1) g = b = r;
    else if (n != 3) return false;
    out = glm::vec3(r, g, b) * (bytes ? 1.0f / 255.0f : 1.0f);
    return true;
}

// Locates the VMT for a model texture name and from it the base texture.
Material resolveMaterial(const std::string& textureName, const std::vector<std::string>& searchDirs,
                         const SourceContent& content) {
    Material mat;
    const std::string name = normalizeSlashes(textureName);
    mat.name = name;
    mat.diffuse = glm::vec3(0.75f);

    // $cdmaterials directories first, then the bare name (which may already
    // contain a path).
    std::vector<std::string> candidates;
    for (const auto& dir : searchDirs) candidates.push_back(normalizeSlashes(dir) + "/" + name);
    candidates.push_back(name);

    fs::path vmtPath;
    for (const auto& candidate : candidates) {
        vmtPath = content.find("materials/" + candidate + ".vmt");
        if (!vmtPath.empty()) break;
    }

    std::string baseTexture;
    if (!vmtPath.empty()) {
        VmtMaterial vmt;
        if (!loadVmt(vmtPath, content, vmt)) {
            mat.problem = "could not parse " + vmtPath.filename().string();
            return mat;
        }
        if (const std::string* v = vmt.get("$basetexture")) baseTexture = *v;
        else if (const std::string* iris = vmt.get("$iris")) baseTexture = *iris;
        if (vmt.flag("$translucent") || vmt.flag("$additive")) mat.alphaMode = AlphaMode::Blend;
        else if (vmt.flag("$alphatest")) mat.alphaMode = AlphaMode::Test;
        if (const std::string* ref = vmt.get("$alphatestreference")) {
            const float cutoff = static_cast<float>(std::atof(ref->c_str()));
            if (cutoff > 0.0f && cutoff < 1.0f) mat.alphaCutoff = cutoff;
        }
        glm::vec3 tint;
        const std::string* color = vmt.get("$color2");
        if (!color) color = vmt.get("$color");
        if (color && parseColor(*color, tint)) mat.diffuse = tint;
        else mat.diffuse = glm::vec3(1.0f);

        if (baseTexture.empty()) {
            mat.problem = vmtPath.filename().string() + " has no $basetexture (shader \"" + vmt.shader + "\")";
            mat.diffuse = glm::vec3(0.75f);
            return mat;
        }
    }

    // With no VMT, a VTF of the same name is a reasonable guess.
    std::vector<std::string> textureCandidates;
    if (!baseTexture.empty()) textureCandidates.push_back(stripExtension(normalizeSlashes(baseTexture), ".vtf"));
    else textureCandidates = candidates;

    for (const auto& candidate : textureCandidates) {
        mat.diffuseTexture = content.find("materials/" + candidate + ".vtf");
        if (!mat.diffuseTexture.empty()) break;
    }
    if (mat.diffuseTexture.empty()) {
        if (vmtPath.empty()) mat.problem = "no VMT found (looked for materials/" + candidates.front() + ".vmt)";
        else mat.problem = "texture not found: materials/" + textureCandidates.front() + ".vtf";
        mat.diffuse = glm::vec3(0.75f);
    } else if (vmtPath.empty()) {
        mat.diffuse = glm::vec3(1.0f);
    }
    return mat;
}

class MdlLoader final : public ModelLoader {
public:
    std::vector<std::string> extensions() const override { return {".mdl"}; }

    bool load(const fs::path& path, Model& out, std::string& error) override {
        BinaryFile mdl;
        if (!mdl.open(path)) {
            error = "Could not open file.";
            return false;
        }
        if (mdl.size() < 408 || std::memcmp(mdl.data(), "IDST", 4) != 0) {
            error = "Not a Source Engine model (missing IDST header).";
            return false;
        }
        const int version = mdl.i32(4);
        if (version == 10) {
            error = "GoldSrc (Half-Life 1 engine) models are not supported.";
            return false;
        }
        if (version < 44 || version > 49) {
            error = "Unsupported MDL version " + std::to_string(version) + " (supported: 44-49).";
            return false;
        }
        const int checksum = mdl.i32(8);

        // --- Companion files -------------------------------------------------
        const fs::path vvdPath = findCompanion(path, {".vvd"});
        const fs::path vtxPath = findCompanion(path, {".dx90.vtx", ".dx80.vtx", ".sw.vtx", ".vtx"});
        const int bodyPartCount = mdl.i32(232);
        if (vvdPath.empty() || vtxPath.empty()) {
            if (bodyPartCount == 0 || mdl.i32(204) == 0) {
                error = "No geometry (animation-only or include model).";
            } else {
                error = std::string("Missing ") + (vvdPath.empty() ? ".vvd" : ".vtx") + " file next to the model.";
            }
            return false;
        }
        BinaryFile vvd, vtx;
        if (!vvd.open(vvdPath) || !vtx.open(vtxPath)) {
            error = "Could not open the model's .vvd/.vtx files.";
            return false;
        }
        if (vvd.size() < 64 || std::memcmp(vvd.data(), "IDSV", 4) != 0 || vvd.i32(4) != 4) {
            error = "Unsupported or corrupt .vvd file.";
            return false;
        }
        if (vtx.i32(0) != 7) {
            error = "Unsupported .vtx version " + std::to_string(vtx.i32(0)) + ".";
            return false;
        }
        if (vvd.i32(8) != checksum || vtx.i32(16) != checksum) {
            out.warnings += "The .vvd/.vtx checksums don't match the .mdl; they may be from a different compile.\n";
        }

        // --- Vertices (VVD) --------------------------------------------------
        // With LODs, the file stores vertices grouped by the lowest-detail LOD
        // that still uses them, and a fixup table says how to reassemble the
        // list for a given LOD. For LOD 0 that is every fixup, in order.
        const int fixupCount = vvd.i32(48), fixupStart = vvd.i32(52), vertexStart = vvd.i32(56);
        std::vector<Vertex> vertices;
        std::vector<BoneWeights> weights;
        auto appendVertices = [&](std::int64_t first, std::int64_t count) {
            vvd.require(vertexStart + first * kVvdVertexSize, count * kVvdVertexSize);
            for (std::int64_t i = 0; i < count; ++i) {
                const std::int64_t weightsAt = vertexStart + (first + i) * kVvdVertexSize;
                BoneWeights w;
                w.count = std::min<int>(vvd.u8(weightsAt + 15), 3);
                for (int k = 0; k < 3; ++k) {
                    w.weight[k] = vvd.f32(weightsAt + k * 4);
                    w.bone[k] = vvd.u8(weightsAt + 12 + k);
                }
                weights.push_back(w);
                const std::int64_t at = weightsAt + 16;
                Vertex v;
                v.position = glm::vec3(vvd.f32(at), vvd.f32(at + 4), vvd.f32(at + 8));
                v.normal = glm::vec3(vvd.f32(at + 12), vvd.f32(at + 16), vvd.f32(at + 20));
                v.uv = glm::vec2(vvd.f32(at + 24), vvd.f32(at + 28));
                vertices.push_back(v);
            }
        };
        if (fixupCount > 0) {
            for (int i = 0; i < fixupCount; ++i) {
                const std::int64_t at = fixupStart + std::int64_t(i) * 12;
                appendVertices(vvd.i32(at + 4), vvd.i32(at + 8));
            }
        } else {
            appendVertices(0, vvd.i32(16)); // numLODVertexes[0]
        }

        // The file has the vertices in the reference pose; move them to the
        // pose the model is actually shown in (see source/pose.h).
        const MdlPose pose = readMdlPose(mdl);
        skinVertices(pose, weights, vertices);

        // --- Materials ---------------------------------------------------------
        const int textureCount = mdl.i32(204), textureOffset = mdl.i32(208);
        const int searchDirCount = mdl.i32(212), searchDirOffset = mdl.i32(216);
        const int skinRefCount = mdl.i32(220), skinFamilyCount = mdl.i32(224), skinOffset = mdl.i32(228);

        std::vector<std::string> searchDirs;
        for (int i = 0; i < searchDirCount; ++i) {
            std::string dir = normalizeSlashes(mdl.cstring(mdl.i32(searchDirOffset + std::int64_t(i) * 4)));
            while (!dir.empty() && dir.back() == '/') dir.pop_back();
            searchDirs.push_back(dir);
        }

        const SourceContent content(path);
        out.contentRoots = content.roots();
        for (int i = 0; i < textureCount; ++i) {
            const std::int64_t at = textureOffset + std::int64_t(i) * kMdlTextureSize;
            out.materials.push_back(resolveMaterial(mdl.cstring(at + mdl.i32(at)), searchDirs, content));
        }

        // --- Triangles (MDL structure + VTX indices) ---------------------------
        // CS:GO-era v49 files grew two of the VTX structs by 8 bytes without
        // changing the version number, so try the classic layout first and
        // fall back if it produces nonsense.
        std::string firstError;
        for (int extra : {0, 8}) {
            try {
                out.meshes.clear();
                buildMeshes(mdl, vtx, vertices, skinRefCount, skinFamilyCount, skinOffset, extra, out);
                firstError.clear();
                break;
            } catch (const std::exception& e) {
                if (firstError.empty()) firstError = e.what();
            }
        }
        if (!firstError.empty()) {
            error = firstError;
            return false;
        }

        loadPhysics(path, pose, out);

        out.zUp = true;
        return true;
    }

private:
    // mstudioboneweight_t: up to three bones a vertex follows.
    struct BoneWeights {
        float weight[3] = {0.0f, 0.0f, 0.0f};
        std::uint8_t bone[3] = {0, 0, 0};
        int count = 0;
    };

    // Moves reference-pose vertices to `pose`.
    static void skinVertices(const MdlPose& pose, const std::vector<BoneWeights>& weights,
                             std::vector<Vertex>& vertices) {
        if (pose.skin.empty()) return;
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            const BoneWeights& w = weights[i];
            glm::mat4 blend(0.0f);
            float total = 0.0f;
            for (int k = 0; k < w.count; ++k) {
                if (w.bone[k] >= pose.skin.size()) continue;
                blend += pose.skin[w.bone[k]] * w.weight[k];
                total += w.weight[k];
            }
            if (total > 1e-6f) blend /= total;
            else blend = pose.skin[0]; // no usable weights: follow the root
            Vertex& v = vertices[i];
            v.position = glm::vec3(blend * glm::vec4(v.position, 1.0f));
            const glm::vec3 normal = glm::mat3(blend) * v.normal;
            const float length = glm::length(normal);
            if (length > 1e-6f) v.normal = normal / length;
        }
    }

    // Collision model from the .phy, if there is one. Never fatal.
    static void loadPhysics(const fs::path& path, const MdlPose& pose, Model& out) {
        const fs::path phyPath = findCompanion(path, {".phy"});
        if (phyPath.empty()) return;
        std::vector<PhySolid> solids;
        std::string error;
        if (!loadPhy(phyPath, solids, error)) {
            out.warnings += "Could not read the .phy file: " + error + "\n";
            return;
        }
        // A lone solid is compiled in model space, as the model stands in the
        // game. With several (a ragdoll), each is in the space of the bone it
        // is named after, so it goes wherever the pose puts that bone.
        const bool jointed = solids.size() > 1;
        for (const PhySolid& solid : solids) {
            glm::mat4 boneToModel(1.0f);
            if (jointed) {
                for (std::size_t b = 0; b < pose.boneNames.size(); ++b) {
                    if (!equalsNoCase(pose.boneNames[b], solid.name)) continue;
                    boneToModel = pose.boneToModel[b];
                    break;
                }
            }
            const std::uint32_t base = static_cast<std::uint32_t>(out.physics.vertices.size());
            for (const glm::vec3& p : solid.vertices) {
                Vertex v;
                v.position = glm::vec3(boneToModel * glm::vec4(p, 1.0f));
                out.physics.vertices.push_back(v);
            }
            for (std::uint32_t index : solid.indices) out.physics.indices.push_back(base + index);
            out.physicsParts += solid.convexCount;
        }
    }

    static void buildMeshes(const BinaryFile& mdl, const BinaryFile& vtx, const std::vector<Vertex>& vertices,
                            int skinRefCount, int skinFamilyCount, int skinOffset, int vtxExtraBytes, Model& out) {
        const int stripGroupSize = 25 + vtxExtraBytes, stripSize = 27 + vtxExtraBytes;
        const int bodyPartCount = mdl.i32(232), bodyPartOffset = mdl.i32(236);
        const int vtxBodyPartCount = vtx.i32(28), vtxBodyPartOffset = vtx.i32(32);
        if (vtxBodyPartCount != bodyPartCount) throw std::runtime_error("The .vtx file doesn't match the .mdl.");

        std::map<int, std::size_t> meshForMaterial;
        std::vector<std::unordered_map<std::uint32_t, std::uint32_t>> remap;

        for (int b = 0; b < bodyPartCount; ++b) {
            const std::int64_t bodyPart = bodyPartOffset + std::int64_t(b) * kMdlBodyPartSize;
            const std::int64_t vtxBodyPart = vtxBodyPartOffset + std::int64_t(b) * 8;
            // A body part is a set of interchangeable sub-models (bodygroups);
            // show the default, which is the first.
            if (mdl.i32(bodyPart + 4) < 1 || vtx.i32(vtxBodyPart) < 1) continue;
            const std::int64_t model = bodyPart + mdl.i32(bodyPart + 12);
            const std::int64_t vtxModel = vtxBodyPart + vtx.i32(vtxBodyPart + 4);

            const int meshCount = mdl.i32(model + 72);
            const std::int64_t meshes = model + mdl.i32(model + 76);
            const std::int64_t modelFirstVertex = mdl.i32(model + 84) / kVvdVertexSize;

            if (vtx.i32(vtxModel) < 1) continue; // no LODs
            const std::int64_t vtxLod = vtxModel + vtx.i32(vtxModel + 4); // LOD 0
            if (vtx.i32(vtxLod) != meshCount) throw std::runtime_error("The .vtx file doesn't match the .mdl.");
            const std::int64_t vtxMeshes = vtxLod + vtx.i32(vtxLod + 4);

            for (int m = 0; m < meshCount; ++m) {
                const std::int64_t mesh = meshes + std::int64_t(m) * kMdlMeshSize;
                const std::int64_t vtxMesh = vtxMeshes + std::int64_t(m) * kVtxMeshSize;
                const std::int64_t meshFirstVertex = modelFirstVertex + mdl.i32(mesh + 12);

                // Skin family 0 maps the mesh's material slot to a texture.
                int material = mdl.i32(mesh);
                if (skinFamilyCount > 0 && material >= 0 && material < skinRefCount) {
                    material = mdl.read<std::int16_t>(skinOffset + std::int64_t(material) * 2);
                }
                if (material < 0 || material >= static_cast<int>(out.materials.size())) material = -1;

                auto found = meshForMaterial.find(material);
                if (found == meshForMaterial.end()) {
                    found = meshForMaterial.emplace(material, out.meshes.size()).first;
                    Mesh created;
                    created.material = material;
                    created.name = material >= 0 ? out.materials[material].name : "default";
                    out.meshes.push_back(std::move(created));
                    remap.emplace_back();
                }
                Mesh& dest = out.meshes[found->second];
                auto& seen = remap[found->second];

                auto emit = [&](std::int64_t stripGroupVertices, int vertexCount, int index) {
                    if (index < 0 || index >= vertexCount) throw std::runtime_error("The .vtx file is corrupt.");
                    const std::int64_t id = meshFirstVertex + vtx.u16(stripGroupVertices + std::int64_t(index) * kVtxVertexSize + 4);
                    if (id < 0 || id >= static_cast<std::int64_t>(vertices.size())) {
                        throw std::runtime_error("The .vtx file refers to vertices missing from the .vvd.");
                    }
                    auto it = seen.find(static_cast<std::uint32_t>(id));
                    if (it == seen.end()) {
                        it = seen.emplace(static_cast<std::uint32_t>(id), static_cast<std::uint32_t>(dest.vertices.size())).first;
                        dest.vertices.push_back(vertices[static_cast<std::size_t>(id)]);
                    }
                    dest.indices.push_back(it->second);
                };

                const int stripGroupCount = vtx.i32(vtxMesh);
                const std::int64_t stripGroups = vtxMesh + vtx.i32(vtxMesh + 4);
                for (int g = 0; g < stripGroupCount; ++g) {
                    const std::int64_t group = stripGroups + std::int64_t(g) * stripGroupSize;
                    const int vertexCount = vtx.i32(group);
                    const std::int64_t groupVertices = group + vtx.i32(group + 4);
                    const int indexCount = vtx.i32(group + 8);
                    const std::int64_t groupIndices = group + vtx.i32(group + 12);
                    const int stripCount = vtx.i32(group + 16);
                    const std::int64_t strips = group + vtx.i32(group + 20);
                    if (vertexCount < 0 || indexCount < 0 || stripCount < 0 || stripCount > 65536) {
                        throw std::runtime_error("The .vtx file is corrupt.");
                    }

                    for (int s = 0; s < stripCount; ++s) {
                        const std::int64_t strip = strips + std::int64_t(s) * stripSize;
                        const int count = vtx.i32(strip), first = vtx.i32(strip + 4);
                        const int flags = vtx.u8(strip + 18);
                        if (count < 0 || first < 0 || first + count > indexCount) {
                            throw std::runtime_error("The .vtx file is corrupt.");
                        }
                        auto index = [&](int i) { return static_cast<int>(vtx.u16(groupIndices + std::int64_t(first + i) * 2)); };
                        // Source winds triangles clockwise; swap two corners
                        // so front faces are counter-clockwise for OpenGL.
                        if (flags & 0x02) { // triangle strip
                            for (int i = 0; i + 2 < count; ++i) {
                                const int a = index(i), b = index(i + 1), c = index(i + 2);
                                if (a == b || b == c || a == c) continue;
                                emit(groupVertices, vertexCount, a);
                                emit(groupVertices, vertexCount, (i & 1) ? b : c);
                                emit(groupVertices, vertexCount, (i & 1) ? c : b);
                            }
                        } else { // triangle list
                            for (int i = 0; i + 2 < count; i += 3) {
                                emit(groupVertices, vertexCount, index(i));
                                emit(groupVertices, vertexCount, index(i + 2));
                                emit(groupVertices, vertexCount, index(i + 1));
                            }
                        }
                    }
                }
            }
        }
    }
};

} // namespace

std::unique_ptr<ModelLoader> makeMdlLoader() { return std::make_unique<MdlLoader>(); }
