#include "source/vtf.h"

#include <algorithm>

#include "source/binary_reader.h"

namespace {

// IMAGE_FORMAT_* values from the Source SDK.
enum VtfFormat {
    RGBA8888 = 0, ABGR8888 = 1, RGB888 = 2, BGR888 = 3, RGB565 = 4, I8 = 5, IA88 = 6, A8 = 8,
    RGB888_BLUESCREEN = 9, BGR888_BLUESCREEN = 10, ARGB8888 = 11, BGRA8888 = 12, DXT1 = 13, DXT3 = 14,
    DXT5 = 15, BGRX8888 = 16, BGR565 = 17, BGRX5551 = 18, BGRA4444 = 19, DXT1_ONEBITALPHA = 20, BGRA5551 = 21,
};

// Bytes per pixel for uncompressed formats, 0 if unsupported/compressed.
int bytesPerPixel(int format) {
    switch (format) {
        case RGBA8888: case ABGR8888: case ARGB8888: case BGRA8888: case BGRX8888: return 4;
        case RGB888: case BGR888: case RGB888_BLUESCREEN: case BGR888_BLUESCREEN: return 3;
        case RGB565: case BGR565: case IA88: case BGRX5551: case BGRA4444: case BGRA5551: return 2;
        case I8: case A8: return 1;
        default: return 0;
    }
}

bool isDxt(int format) { return format == DXT1 || format == DXT3 || format == DXT5 || format == DXT1_ONEBITALPHA; }

// Size in bytes of one image of this format; -1 if the format is unsupported.
std::int64_t imageSize(int format, int width, int height) {
    if (isDxt(format)) {
        const std::int64_t blocks = std::int64_t(std::max(1, (width + 3) / 4)) * std::max(1, (height + 3) / 4);
        return blocks * ((format == DXT1 || format == DXT1_ONEBITALPHA) ? 8 : 16);
    }
    const int bpp = bytesPerPixel(format);
    return bpp ? std::int64_t(width) * height * bpp : -1;
}

void unpack565(std::uint16_t c, std::uint8_t* rgb) {
    rgb[0] = static_cast<std::uint8_t>(((c >> 11) & 31) * 255 / 31);
    rgb[1] = static_cast<std::uint8_t>(((c >> 5) & 63) * 255 / 63);
    rgb[2] = static_cast<std::uint8_t>((c & 31) * 255 / 31);
}

void decodeDxt(const std::uint8_t* src, int format, int width, int height, std::vector<std::uint8_t>& rgba) {
    const bool dxt1 = format == DXT1 || format == DXT1_ONEBITALPHA;
    const int blocksX = std::max(1, (width + 3) / 4), blocksY = std::max(1, (height + 3) / 4);
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            std::uint8_t alpha[16];
            std::fill(alpha, alpha + 16, std::uint8_t(255));
            if (format == DXT3) {
                for (int i = 0; i < 16; ++i) alpha[i] = static_cast<std::uint8_t>(((src[i / 2] >> ((i % 2) * 4)) & 15) * 17);
                src += 8;
            } else if (format == DXT5) {
                std::uint8_t a[8] = {src[0], src[1]};
                for (int i = 1; i < 7; ++i) {
                    a[i + 1] = a[0] > a[1] ? static_cast<std::uint8_t>(((7 - i) * a[0] + i * a[1]) / 7)
                                           : (i < 5 ? static_cast<std::uint8_t>(((5 - i) * a[0] + i * a[1]) / 5)
                                                    : static_cast<std::uint8_t>(i == 5 ? 0 : 255));
                }
                std::uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= std::uint64_t(src[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
                src += 8;
            }

            const std::uint16_t c0 = static_cast<std::uint16_t>(src[0] | (src[1] << 8));
            const std::uint16_t c1 = static_cast<std::uint16_t>(src[2] | (src[3] << 8));
            std::uint8_t colors[4][4];
            unpack565(c0, colors[0]);
            unpack565(c1, colors[1]);
            colors[0][3] = colors[1][3] = colors[2][3] = colors[3][3] = 255;
            if (!dxt1 || c0 > c1) {
                for (int k = 0; k < 3; ++k) {
                    colors[2][k] = static_cast<std::uint8_t>((2 * colors[0][k] + colors[1][k]) / 3);
                    colors[3][k] = static_cast<std::uint8_t>((colors[0][k] + 2 * colors[1][k]) / 3);
                }
            } else {
                for (int k = 0; k < 3; ++k) {
                    colors[2][k] = static_cast<std::uint8_t>((colors[0][k] + colors[1][k]) / 2);
                    colors[3][k] = 0;
                }
                if (format == DXT1_ONEBITALPHA) colors[3][3] = 0;
            }
            const std::uint32_t lookup = src[4] | (src[5] << 8) | (src[6] << 16) | (std::uint32_t(src[7]) << 24);
            src += 8;

            for (int i = 0; i < 16; ++i) {
                const int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= width || y >= height) continue;
                const std::uint8_t* c = colors[(lookup >> (2 * i)) & 3];
                std::uint8_t* dst = &rgba[(std::size_t(y) * width + x) * 4];
                dst[0] = c[0];
                dst[1] = c[1];
                dst[2] = c[2];
                dst[3] = dxt1 ? c[3] : alpha[i];
            }
        }
    }
}

void decodeUncompressed(const std::uint8_t* s, int format, std::size_t pixels, std::vector<std::uint8_t>& rgba) {
    const int bpp = bytesPerPixel(format);
    for (std::size_t i = 0; i < pixels; ++i, s += bpp) {
        std::uint8_t* d = &rgba[i * 4];
        const std::uint16_t w = bpp == 2 ? static_cast<std::uint16_t>(s[0] | (s[1] << 8)) : std::uint16_t(0);
        d[3] = 255;
        switch (format) {
            case RGBA8888: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
            case ABGR8888: d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0]; break;
            case ARGB8888: d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0]; break;
            case BGRA8888: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
            case BGRX8888: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; break;
            case RGB888: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; break;
            case BGR888: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; break;
            case RGB888_BLUESCREEN:
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                if (s[0] == 0 && s[1] == 0 && s[2] == 255) d[3] = 0;
                break;
            case BGR888_BLUESCREEN:
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                if (s[2] == 0 && s[1] == 0 && s[0] == 255) d[3] = 0;
                break;
            case I8: d[0] = d[1] = d[2] = s[0]; break;
            case IA88: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case A8: d[0] = d[1] = d[2] = 255; d[3] = s[0]; break;
            case BGR565: unpack565(w, d); break;
            case RGB565: unpack565(w, d); std::swap(d[0], d[2]); break;
            case BGRX5551: case BGRA5551:
                d[0] = static_cast<std::uint8_t>(((w >> 10) & 31) * 255 / 31);
                d[1] = static_cast<std::uint8_t>(((w >> 5) & 31) * 255 / 31);
                d[2] = static_cast<std::uint8_t>((w & 31) * 255 / 31);
                if (format == BGRA5551) d[3] = (w & 0x8000) ? 255 : 0;
                break;
            case BGRA4444:
                d[0] = static_cast<std::uint8_t>(((w >> 8) & 15) * 17);
                d[1] = static_cast<std::uint8_t>(((w >> 4) & 15) * 17);
                d[2] = static_cast<std::uint8_t>((w & 15) * 17);
                d[3] = static_cast<std::uint8_t>(((w >> 12) & 15) * 17);
                break;
            default: break;
        }
    }
}

} // namespace

bool loadVtf(const std::filesystem::path& path, ImageData& out, std::string& error) {
    BinaryFile file;
    if (!file.open(path)) {
        error = "could not open file";
        return false;
    }
    try {
        if (file.size() < 64 || std::memcmp(file.data(), "VTF\0", 4) != 0) {
            error = "not a VTF file";
            return false;
        }
        const std::uint32_t major = file.read<std::uint32_t>(4), minor = file.read<std::uint32_t>(8);
        const std::uint32_t headerSize = file.read<std::uint32_t>(12);
        const int width = file.u16(16), height = file.u16(18);
        const std::uint32_t flags = file.read<std::uint32_t>(20);
        const int frames = std::max<int>(1, file.u16(24));
        const std::uint16_t firstFrame = file.u16(26);
        const int format = file.i32(52);
        const int mipCount = std::max<int>(1, file.u8(56));
        const int lowFormat = file.i32(57);
        const int lowWidth = file.u8(61), lowHeight = file.u8(62);
        const int depth = (minor >= 2) ? std::max<int>(1, file.u16(63)) : 1;

        if (major != 7) {
            error = "unsupported VTF version";
            return false;
        }
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
            error = "invalid VTF dimensions";
            return false;
        }
        if (depth > 1) {
            error = "volume textures are not supported";
            return false;
        }
        const std::int64_t topSize = imageSize(format, width, height);
        if (topSize < 0) {
            error = "unsupported VTF pixel format " + std::to_string(format);
            return false;
        }

        // Cube maps store 6 faces per frame (7 with the old sphere map).
        int faces = 1;
        if (flags & 0x4000) faces = (minor < 5 && firstFrame != 0xFFFF) ? 7 : 6;

        // Where the image data starts: 7.3+ has a resource directory, older
        // versions put it right after the header and low-res thumbnail.
        std::int64_t offset = -1;
        if (minor >= 3) {
            const std::uint32_t resources = file.read<std::uint32_t>(68);
            for (std::uint32_t i = 0; i < resources && i < 64; ++i) {
                const std::int64_t entry = 80 + std::int64_t(i) * 8;
                if (file.u8(entry) == 0x30 && file.u8(entry + 1) == 0 && file.u8(entry + 2) == 0) {
                    offset = file.read<std::uint32_t>(entry + 4);
                }
            }
        }
        if (offset < 0) {
            offset = headerSize;
            if (lowFormat >= 0 && lowWidth > 0 && lowHeight > 0) {
                offset += std::max<std::int64_t>(0, imageSize(lowFormat, lowWidth, lowHeight));
            }
        }
        // Mips are stored smallest first; skip to the largest.
        for (int mip = mipCount - 1; mip >= 1; --mip) {
            offset += imageSize(format, std::max(1, width >> mip), std::max(1, height >> mip)) * frames * faces;
        }
        if (offset + topSize > static_cast<std::int64_t>(file.size())) {
            // Header didn't add up; the largest mip is always last in the file.
            offset = static_cast<std::int64_t>(file.size()) - topSize * frames * faces;
        }
        file.require(offset, topSize);

        out.width = width;
        out.height = height;
        out.rgba.assign(static_cast<std::size_t>(width) * height * 4, 255);
        if (isDxt(format)) decodeDxt(file.data() + offset, format, width, height, out.rgba);
        else decodeUncompressed(file.data() + offset, format, static_cast<std::size_t>(width) * height, out.rgba);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
