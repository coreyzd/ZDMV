// Valve Material Type (.vmt) reader: just enough to find a material's base
// texture and how its alpha channel is meant to be used.
#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "source/content.h"

struct VmtMaterial {
    std::string shader;                        // lower-case, e.g. "vertexlitgeneric"
    std::map<std::string, std::string> params; // lower-case keys, top level only

    const std::string* get(const std::string& key) const {
        auto it = params.find(key);
        return it == params.end() ? nullptr : &it->second;
    }
    bool flag(const std::string& key) const {
        const std::string* v = get(key);
        return v && !v->empty() && *v != "0";
    }
};

// Parses a VMT, following "patch" materials to the file they include.
bool loadVmt(const std::filesystem::path& path, const SourceContent& content, VmtMaterial& out);
