// Source Engine collision model (.phy) reader.
//
// A .phy holds one "solid" per physics body (one for an ordinary prop, one
// per bone for a ragdoll). Each solid is a set of convex pieces stored as
// triangle lists in the physics engine's own units and axes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

struct PhySolid {
    // From the file's text section. For ragdolls this is the bone the solid
    // is attached to, and the vertices are in that bone's space; for a
    // single-solid model the vertices are already in model space (as the
    // model stands in the game, not its reference pose: see source/pose.h).
    std::string name;
    std::vector<glm::vec3> vertices;    // Source units and axes
    std::vector<std::uint32_t> indices; // triangle list
    int convexCount = 0;
};

bool loadPhy(const std::filesystem::path& path, std::vector<PhySolid>& out, std::string& error);
