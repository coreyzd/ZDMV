#include "source/pose.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>

#include <glm/gtc/quaternion.hpp>

namespace {

constexpr int kMdlBoneSize = 216;     // mstudiobone_t
constexpr int kMdlAnimDescSize = 100; // mstudioanimdesc_t
constexpr int kMdlSeqDescSize = 212;  // mstudioseqdesc_t
constexpr int kSeqDelta = 0x04;       // STUDIO_DELTA: adds to another pose, isn't one itself
constexpr int kMaxBones = 256;        // an animation addresses bones with one byte

// mstudioanim_t flags.
constexpr int kAnimRawPos = 0x01;   // Vector48
constexpr int kAnimRawRot = 0x02;   // Quaternion48
constexpr int kAnimAnimPos = 0x04;  // run-length encoded position channels
constexpr int kAnimAnimRot = 0x08;  // run-length encoded Euler channels
constexpr int kAnimDelta = 0x10;
constexpr int kAnimRawRot2 = 0x20;  // Quaternion64

// A bone's local pose (what an animation changes) and its bind-pose inverse.
struct Bone {
    int parent = -1;
    glm::vec3 pos{0.0f}, rot{0.0f}, posScale{0.0f}, rotScale{0.0f};
    glm::quat quat{1.0f, 0.0f, 0.0f, 0.0f};
    glm::mat4 poseToBone{1.0f};
};

// Source's RadianEuler (rotations about x, y, z) -> quaternion.
glm::quat eulerToQuat(const glm::vec3& e) {
    const float sr = std::sin(e.x * 0.5f), cr = std::cos(e.x * 0.5f);
    const float sp = std::sin(e.y * 0.5f), cp = std::cos(e.y * 0.5f);
    const float sy = std::sin(e.z * 0.5f), cy = std::cos(e.z * 0.5f);
    return glm::quat(cr * cp * cy + sr * sp * sy,  // w
                     sr * cp * cy - cr * sp * sy,  // x
                     cr * sp * cy + sr * cp * sy,  // y
                     cr * cp * sy - sr * sp * cy); // z
}

float halfToFloat(std::uint16_t h) {
    const int sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v;
    if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31) v = 0.0f; // infinity / NaN: not a position
    else v = std::ldexp(static_cast<float>(mantissa | 1024), exponent - 25);
    return sign ? -v : v;
}

std::uint64_t readBits(const BinaryFile& mdl, std::int64_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(mdl.u8(at + i)) << (8 * i);
    return v;
}

// One animated channel at `frame`. The values (mstudioanimvalue_t) are run
// length encoded: {valid, total}, then `valid` values covering `total` frames,
// the last one repeating.
float animChannel(const BinaryFile& mdl, std::int64_t at, int frame) {
    int k = std::max(0, frame);
    for (int guard = 0; guard < 4096; ++guard) {
        const int valid = mdl.u8(at), total = mdl.u8(at + 1);
        if (total == 0) return 0.0f;
        if (k < total) {
            if (valid == 0) return 0.0f;
            return static_cast<float>(mdl.read<std::int16_t>(at + 2 + 2 * std::int64_t(std::min(k, valid - 1))));
        }
        k -= total;
        at += 2 * std::int64_t(valid + 1);
    }
    return 0.0f;
}

std::vector<Bone> readBones(const BinaryFile& mdl, std::vector<std::string>& names) {
    const int boneCount = mdl.i32(156), boneOffset = mdl.i32(160);
    if (boneCount <= 0 || boneCount > kMaxBones) return {};
    std::vector<Bone> bones(static_cast<std::size_t>(boneCount));
    for (int i = 0; i < boneCount; ++i) {
        const std::int64_t at = boneOffset + std::int64_t(i) * kMdlBoneSize;
        Bone& bone = bones[static_cast<std::size_t>(i)];
        names.push_back(mdl.cstring(at + mdl.i32(at)));
        bone.parent = mdl.i32(at + 4);
        for (int k = 0; k < 3; ++k) {
            bone.pos[k] = mdl.f32(at + 32 + 4 * k);
            bone.rot[k] = mdl.f32(at + 60 + 4 * k);
            bone.posScale[k] = mdl.f32(at + 72 + 4 * k);
            bone.rotScale[k] = mdl.f32(at + 84 + 4 * k);
        }
        bone.quat = glm::quat(mdl.f32(at + 56), mdl.f32(at + 44), mdl.f32(at + 48), mdl.f32(at + 52));
        // poseToBone is a row-major 3x4 taking model space to bone space.
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col) bone.poseToBone[col][row] = mdl.f32(at + 96 + (row * 4 + col) * 4);
        }
    }
    return bones;
}

// Moves `bones` to `frame` of animation `anim`. False if the frame isn't in
// the .mdl itself (long animations can be split off into an .ani file).
bool applyAnimFrame(const BinaryFile& mdl, int anim, int frame, std::vector<Bone>& bones) {
    const int animCount = mdl.i32(180), animOffset = mdl.i32(184);
    if (anim < 0 || anim >= animCount) return false;
    const std::int64_t desc = animOffset + std::int64_t(anim) * kMdlAnimDescSize;
    frame = std::clamp(frame, 0, std::max(0, mdl.i32(desc + 16) - 1));
    int block = mdl.i32(desc + 52);
    std::int64_t dataOffset = mdl.i32(desc + 56);
    const int sectionFrames = mdl.i32(desc + 84);
    if (sectionFrames != 0) { // long animations come in sections of this many frames
        const int section = sectionFrames > 0 ? frame / sectionFrames : 0;
        const std::int64_t at = desc + mdl.i32(desc + 80) + std::int64_t(section) * 8;
        block = mdl.i32(at);
        dataOffset = mdl.i32(at + 4);
        if (sectionFrames > 0) frame -= section * sectionFrames;
    }
    if (block != 0) return false;

    // A list of mstudioanim_t, one per bone the animation moves; the rest
    // keep their bind pose.
    std::int64_t p = desc + dataOffset;
    for (int guard = 0; guard <= kMaxBones; ++guard) {
        const int boneId = mdl.u8(p), flags = mdl.u8(p + 1);
        const int next = mdl.read<std::int16_t>(p + 2);
        if (boneId < static_cast<int>(bones.size())) {
            Bone& bone = bones[static_cast<std::size_t>(boneId)];
            const bool delta = (flags & kAnimDelta) != 0;
            std::int64_t q = p + 4;
            if (flags & kAnimRawRot) {
                const std::uint64_t v = readBits(mdl, q, 6);
                const float x = (static_cast<float>(v & 0xFFFF) - 32768.0f) / 32768.0f;
                const float y = (static_cast<float>((v >> 16) & 0xFFFF) - 32768.0f) / 32768.0f;
                const float z = (static_cast<float>((v >> 32) & 0x7FFF) - 16384.0f) / 16384.0f;
                float w = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y - z * z));
                if ((v >> 47) & 1) w = -w;
                bone.quat = glm::quat(w, x, y, z);
                q += 6;
            }
            if (flags & kAnimRawRot2) {
                const std::uint64_t v = readBits(mdl, q, 8);
                const double x = (static_cast<double>(v & 0x1FFFFF) - 1048576.0) / 1048576.5;
                const double y = (static_cast<double>((v >> 21) & 0x1FFFFF) - 1048576.0) / 1048576.5;
                const double z = (static_cast<double>((v >> 42) & 0x1FFFFF) - 1048576.0) / 1048576.5;
                double w = std::sqrt(std::max(0.0, 1.0 - x * x - y * y - z * z));
                if (v >> 63) w = -w;
                bone.quat = glm::quat(static_cast<float>(w), static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(z));
                q += 8;
            }
            // mstudioanim_valueptr_t: per axis, the offset of its values from
            // the pointer block (0 = that axis isn't animated).
            auto channel = [&](std::int64_t ptrs, int axis) {
                const int offset = mdl.read<std::int16_t>(ptrs + 2 * axis);
                return offset > 0 ? animChannel(mdl, ptrs + offset, frame) : 0.0f;
            };
            if (flags & kAnimAnimRot) {
                glm::vec3 euler;
                for (int k = 0; k < 3; ++k) euler[k] = channel(q, k) * bone.rotScale[k] + (delta ? 0.0f : bone.rot[k]);
                bone.quat = eulerToQuat(euler);
                q += 6;
            }
            if (flags & kAnimRawPos) {
                for (int k = 0; k < 3; ++k) bone.pos[k] = halfToFloat(mdl.u16(q + 2 * k));
            } else if (flags & kAnimAnimPos) {
                for (int k = 0; k < 3; ++k) bone.pos[k] = channel(q, k) * bone.posScale[k] + (delta ? 0.0f : bone.pos[k]);
            }
        }
        if (next <= 0) break;
        p += next;
    }
    return true;
}

// The animation behind the model's first sequence, which is what a model
// that hasn't been told to play anything shows; -1 if it has none. That
// isn't always animation 0 (character models tend to start with a
// proportions delta), and sequences that only adjust another pose are
// passed over.
int defaultAnimation(const BinaryFile& mdl) {
    const int animCount = mdl.i32(180);
    const int seqCount = mdl.i32(188), seqOffset = mdl.i32(192);
    for (int i = 0; i < seqCount; ++i) {
        const std::int64_t seq = seqOffset + std::int64_t(i) * kMdlSeqDescSize;
        if (mdl.i32(seq + 12) & kSeqDelta) continue;
        const int anim = mdl.read<std::int16_t>(seq + mdl.i32(seq + 60)); // first of its blend grid
        if (anim >= 0 && anim < animCount) return anim;
    }
    return -1;
}

} // namespace

MdlPose readMdlPose(const BinaryFile& mdl) {
    MdlPose pose;
    std::vector<Bone> bones;
    try {
        bones = readBones(mdl, pose.boneNames);
    } catch (const std::exception&) {
        pose.boneNames.clear();
        return pose;
    }
    try {
        std::vector<Bone> moved = bones;
        if (applyAnimFrame(mdl, defaultAnimation(mdl), 0, moved)) {
            bones = std::move(moved);
            pose.animated = true;
        }
    } catch (const std::exception&) {
        // Unreadable animation: keep the reference pose.
    }

    pose.boneToModel.resize(bones.size());
    pose.skin.resize(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const glm::quat& q = bones[i].quat;
        const bool usable = glm::dot(q, q) > 1e-12f;
        glm::mat4 local = usable ? glm::mat4_cast(glm::normalize(q)) : glm::mat4(1.0f);
        local[3] = glm::vec4(bones[i].pos, 1.0f);
        const int parent = bones[i].parent;
        // Parents always come before their children.
        const bool hasParent = parent >= 0 && static_cast<std::size_t>(parent) < i;
        pose.boneToModel[i] = hasParent ? pose.boneToModel[static_cast<std::size_t>(parent)] * local : local;
        pose.skin[i] = pose.boneToModel[i] * bones[i].poseToBone;
    }
    return pose;
}
