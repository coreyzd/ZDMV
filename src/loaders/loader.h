// Model loader registry. To add a format (e.g. Source Engine .mdl):
//   1. Implement ModelLoader in loaders/<format>_loader.cpp
//   2. Register it in loader.cpp
// The file browser and viewer pick up the new extension automatically.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "model.h"

class ModelLoader {
public:
    virtual ~ModelLoader() = default;
    // Lower-case extensions including the dot, e.g. {".obj"}.
    virtual std::vector<std::string> extensions() const = 0;
    // Returns false and sets `error` on failure.
    virtual bool load(const std::filesystem::path& path, Model& out, std::string& error) = 0;
};

// Loader for this file based on its extension, or nullptr if unsupported.
ModelLoader* findLoader(const std::filesystem::path& path);
inline bool isSupportedModel(const std::filesystem::path& path) { return findLoader(path) != nullptr; }

// Loads `path` with the matching loader and computes bounds.
bool loadModel(const std::filesystem::path& path, Model& out, std::string& error);

std::unique_ptr<ModelLoader> makeObjLoader();
std::unique_ptr<ModelLoader> makeMdlLoader();
