// Normal regeneration, independent of file format.
#pragma once

#include "model.h"

enum class NormalMode {
    Auto,   // file normals, unless the model turns out to be faceted
    File,   // exactly what the loader produced
    Smooth, // regenerate, smoothing across edges below the crease angle
    Flat,   // one normal per triangle
};

// True if (nearly) every triangle has the same normal at all three corners,
// i.e. the file would render with a faceted look.
bool isFaceted(const Model& model);

// Returns a copy of `source` with regenerated normals. Vertices are matched
// by position rather than by index, so this also smooths models whose faces
// don't share vertices. Edges sharper than `creaseDegrees` stay hard.
Model withSmoothNormals(const Model& source, float creaseDegrees);
Model withFlatNormals(const Model& source);
