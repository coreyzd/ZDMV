// The pose a Source model is shown in: frame 0 of its first sequence.
//
// Vertices in the .vvd are in the reference (bind) pose, but the engine never
// draws that directly: it draws boneToModel(frame) * poseToBone. The two
// differ whenever the first frame moves a bone, and for most props it does:
// studiomdl turns non-static models 90 degrees about Z, and stores that turn
// in the root bone's animation rather than in the vertices. The collision
// model, on the other hand, is compiled with the turn already applied, so
// showing the bind pose leaves the two a quarter turn apart.
#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "source/binary_reader.h"

struct MdlPose {
    std::vector<std::string> boneNames;
    std::vector<glm::mat4> boneToModel; // each bone's posed transform
    std::vector<glm::mat4> skin;        // bind-pose model space -> posed model space
    // False if the first frame couldn't be read (it lives in an .ani file,
    // say) and this is the reference pose instead.
    bool animated = false;
};

// Never throws: a model whose bones or animation can't be read comes back
// with what could be read, in the reference pose.
MdlPose readMdlPose(const BinaryFile& mdl);
