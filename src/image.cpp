#include "image.h"

#include "source/vtf.h"
#include "util.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

bool loadImage(const std::filesystem::path& path, ImageData& out, std::string& error) {
    if (toLower(path.extension().string()) == ".vtf") return loadVtf(path, out, error);

    stbi_set_flip_vertically_on_load(1); // OBJ UVs have their origin at the bottom-left
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "could not decode image";
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}
