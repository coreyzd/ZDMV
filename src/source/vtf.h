// Valve Texture Format (.vtf) decoder: first frame, largest mip, as RGBA8.
#pragma once

#include <filesystem>
#include <string>

#include "image.h"

bool loadVtf(const std::filesystem::path& path, ImageData& out, std::string& error);
