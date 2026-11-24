#include "source/content.h"

#include <algorithm>

#include "util.h"

namespace fs = std::filesystem;

std::vector<fs::path>& extraContentPaths() {
    static std::vector<fs::path> paths;
    return paths;
}

namespace {

// Child of `dir` whose name matches ignoring case, or empty.
fs::path childNoCase(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    const fs::path exact = dir / name;
    if (fs::exists(exact, ec)) return exact;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (equalsNoCase(it->path().filename().string(), name)) return it->path();
    }
    return {};
}

bool hasMaterials(const fs::path& dir) {
    std::error_code ec;
    const fs::path m = childNoCase(dir, "materials");
    return !m.empty() && fs::is_directory(m, ec);
}

} // namespace

fs::path resolveNoCase(const fs::path& base, const std::string& relative) {
    fs::path current = base;
    std::size_t start = 0;
    while (start <= relative.size()) {
        std::size_t end = relative.find_first_of("/\\", start);
        if (end == std::string::npos) end = relative.size();
        const std::string part = relative.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return {}; // never wander out of the content folder
        current = childNoCase(current, part);
        if (current.empty()) return {};
    }
    return current;
}

void SourceContent::addRoot(const fs::path& dir) {
    if (dir.empty() || !hasMaterials(dir)) return;
    if (std::find(roots_.begin(), roots_.end(), dir) == roots_.end()) roots_.push_back(dir);
}

void SourceContent::addChildRoots(const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> children;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code typeEc;
        if (it->is_directory(typeEc)) children.push_back(it->path());
    }
    std::sort(children.begin(), children.end());
    for (const auto& child : children) addRoot(child);
}

SourceContent::SourceContent(const fs::path& modelFile) {
    // 1. The model's own game folder: the parent of the nearest "models"
    //    directory above the file (.../hl2/models/props/x.mdl -> .../hl2).
    fs::path gameRoot;
    for (fs::path dir = modelFile.parent_path(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
        if (equalsNoCase(dir.filename().string(), "models")) {
            gameRoot = dir.parent_path();
            break;
        }
    }
    if (gameRoot.empty()) gameRoot = modelFile.parent_path(); // loose file: look beside it
    addRoot(gameRoot);

    // 2. Folders above it that are themselves game folders (content packs
    //    nested inside a larger one).
    std::vector<fs::path> ancestors;
    for (fs::path dir = gameRoot.parent_path(); ancestors.size() < 3 && !dir.empty() && dir != dir.parent_path();
         dir = dir.parent_path()) {
        ancestors.push_back(dir);
    }
    for (const auto& dir : ancestors) addRoot(dir);

    // 3. Folders the user added, and game folders directly inside them.
    for (const auto& dir : extraContentPaths()) {
        addRoot(dir);
        addChildRoots(dir);
    }

    // 4. Neighbouring game folders (hl2 next to episodic, or the packs in an
    //    assets collection), then packs nested inside this one, including the
    //    engine's own <game>/custom/<pack> convention.
    if (!ancestors.empty()) addChildRoots(ancestors.front());
    addChildRoots(gameRoot);
    const fs::path custom = childNoCase(gameRoot, "custom");
    if (!custom.empty()) addChildRoots(custom);
}

fs::path SourceContent::find(const std::string& relative) const {
    for (const auto& root : roots_) {
        fs::path hit = resolveNoCase(root, relative);
        std::error_code ec;
        if (!hit.empty() && fs::is_regular_file(hit, ec)) return hit;
    }
    return {};
}
