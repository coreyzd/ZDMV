#include "loaders/loader.h"

#include <algorithm>
#include <cctype>

namespace {

std::vector<std::unique_ptr<ModelLoader>>& registry() {
    static std::vector<std::unique_ptr<ModelLoader>> loaders = [] {
        std::vector<std::unique_ptr<ModelLoader>> v;
        v.push_back(makeObjLoader());
        v.push_back(makeMdlLoader());
        return v;
    }();
    return loaders;
}

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

ModelLoader* findLoader(const std::filesystem::path& path) {
    const std::string ext = lowerExtension(path);
    if (ext.empty()) return nullptr;
    for (auto& loader : registry()) {
        for (const auto& e : loader->extensions()) {
            if (e == ext) return loader.get();
        }
    }
    return nullptr;
}

bool loadModel(const std::filesystem::path& path, Model& out, std::string& error) {
    ModelLoader* loader = findLoader(path);
    if (!loader) {
        error = "Unsupported file type: " + path.extension().string();
        return false;
    }
    out = Model{};
    out.source = path;
    try {
        if (!loader->load(path, out, error)) return false;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    out.meshes.erase(std::remove_if(out.meshes.begin(), out.meshes.end(),
                                    [](const Mesh& m) { return m.indices.empty(); }),
                     out.meshes.end());
    if (out.meshes.empty()) {
        error = "File contains no geometry.";
        return false;
    }
    out.computeBounds();
    return true;
}
