// Decodes texture files to RGBA8 for the renderer.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ImageData {
    int width = 0;
    int height = 0;
    // RGBA8, in the row order the model's UVs expect when uploaded as-is:
    // bottom row first for ordinary images (OBJ, v up), top row first for
    // VTF (Source, v down).
    std::vector<std::uint8_t> rgba;
};

bool loadImage(const std::filesystem::path& path, ImageData& out, std::string& error);
