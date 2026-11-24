#include "source/phy.h"

#include <cstring>
#include <map>

#include "source/binary_reader.h"

namespace {

// The physics engine (IVP/Havok) works in metres with Y pointing down.
constexpr float kInchesPerMetre = 1.0f / 0.0254f;

glm::vec3 toSourceSpace(float x, float y, float z) { return glm::vec3(x, z, -y) * kInchesPerMetre; }

constexpr int kCollideHeaderSize = 28;  // "VPHY" compactsurfaceheader_t
constexpr int kSurfaceHeaderSize = 48;  // IVP_Compact_Surface
constexpr int kLedgeHeaderSize = 16;    // IVP_Compact_Ledge
constexpr int kTriangleSize = 16;       // IVP_Compact_Triangle
constexpr int kPointSize = 16;          // IVP_Compact_Poly_Point (xyz + padding)

void readSolid(const BinaryFile& file, std::int64_t start, std::int64_t size, PhySolid& solid) {
    const std::int64_t end = start + size;
    std::int64_t pos = start;
    if (size >= 8 && std::memcmp(file.data() + start, "VPHY", 4) == 0) {
        const int modelType = file.read<std::int16_t>(start + 6);
        if (modelType != 0) return; // not a convex-poly solid (e.g. a ball)
        pos += kCollideHeaderSize;
    }
    pos += kSurfaceHeaderSize;

    // Convex pieces ("ledges") follow one another until the point data that
    // the first one refers to begins.
    std::int64_t pointsStart = -1;
    while (pos + kLedgeHeaderSize <= end && (pointsStart < 0 || pos < pointsStart)) {
        const std::int64_t points = pos + file.i32(pos);
        const int triangleCount = file.read<std::int16_t>(pos + 12);
        if (pointsStart < 0) pointsStart = points;
        if (triangleCount < 0 || points < start || points > end) throw std::runtime_error("corrupt .phy solid");
        std::int64_t triangle = pos + kLedgeHeaderSize;
        if (triangle + std::int64_t(triangleCount) * kTriangleSize > end) throw std::runtime_error("corrupt .phy solid");

        // A ledge flagged as having children isn't a collision piece: it is
        // the convex hull of several pieces, kept by the physics engine as a
        // quick first test. Skip it, or the model appears shrink-wrapped.
        const bool isBoundingHull = (file.u8(pos + 8) & 0x03) != 0;
        if (isBoundingHull) {
            pos = triangle + std::int64_t(triangleCount) * kTriangleSize;
            continue;
        }

        std::map<int, std::uint32_t> remap; // point index -> output vertex
        for (int t = 0; t < triangleCount; ++t, triangle += kTriangleSize) {
            for (int e = 0; e < 3; ++e) {
                // Each edge's low 16 bits are the index of its start point.
                const int pointIndex = file.u16(triangle + 4 + e * 4);
                auto it = remap.find(pointIndex);
                if (it == remap.end()) {
                    const std::int64_t at = points + std::int64_t(pointIndex) * kPointSize;
                    if (at + 12 > end) throw std::runtime_error("corrupt .phy solid");
                    it = remap.emplace(pointIndex, static_cast<std::uint32_t>(solid.vertices.size())).first;
                    solid.vertices.push_back(toSourceSpace(file.f32(at), file.f32(at + 4), file.f32(at + 8)));
                }
                solid.indices.push_back(it->second);
            }
        }
        ++solid.convexCount;
        pos = triangle;
    }
}

// The text after the solids is KeyValues; all we want is each
// solid { "index" "N" "name" "..." } pair.
void readSolidNames(const std::string& text, std::vector<PhySolid>& solids) {
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            std::size_t close = text.find('"', i + 1);
            if (close == std::string::npos) close = text.size();
            tokens.push_back(text.substr(i + 1, close - i - 1));
            i = close + 1;
        } else if (c == '{' || c == '}') {
            tokens.push_back(std::string(1, c));
            ++i;
        } else if (std::isspace(static_cast<unsigned char>(c)) || c == '\0') {
            ++i;
        } else {
            std::size_t stop = i;
            while (stop < text.size() && !std::isspace(static_cast<unsigned char>(text[stop])) && text[stop] != '{' &&
                   text[stop] != '}' && text[stop] != '"' && text[stop] != '\0') {
                ++stop;
            }
            tokens.push_back(text.substr(i, stop - i));
            i = stop;
        }
    }
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] != "solid" || tokens[i + 1] != "{") continue;
        int index = -1;
        std::string name;
        std::size_t j = i + 2;
        for (; j + 1 < tokens.size() && tokens[j] != "}"; j += 2) {
            if (tokens[j] == "index") index = std::atoi(tokens[j + 1].c_str());
            else if (tokens[j] == "name") name = tokens[j + 1];
        }
        if (index >= 0 && index < static_cast<int>(solids.size())) solids[index].name = name;
        i = j;
    }
}

} // namespace

bool loadPhy(const std::filesystem::path& path, std::vector<PhySolid>& out, std::string& error) {
    out.clear();
    BinaryFile file;
    if (!file.open(path)) {
        error = "could not open file";
        return false;
    }
    try {
        const int headerSize = file.i32(0);
        const int solidCount = file.i32(8);
        if (headerSize < 16 || solidCount < 0 || solidCount > 4096) {
            error = "not a .phy file";
            return false;
        }
        std::int64_t pos = headerSize;
        out.resize(static_cast<std::size_t>(solidCount));
        for (int i = 0; i < solidCount; ++i) {
            const int size = file.i32(pos);
            file.require(pos + 4, size);
            readSolid(file, pos + 4, size, out[i]);
            pos += 4 + std::int64_t(size);
        }
        if (pos < static_cast<std::int64_t>(file.size())) {
            readSolidNames(std::string(reinterpret_cast<const char*>(file.data()) + pos, file.size() - pos), out);
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        out.clear();
        return false;
    }
}
