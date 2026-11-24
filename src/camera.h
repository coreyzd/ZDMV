#pragma once

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Orbit camera: rotates around `target` at `distance`.
struct OrbitCamera {
    glm::vec3 target{0.0f};
    float distance = 5.0f;
    float yaw = glm::radians(35.0f);
    float pitch = glm::radians(20.0f);
    float fovY = glm::radians(45.0f);
    float sceneRadius = 1.0f;

    glm::vec3 eye() const {
        const glm::vec3 dir(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
        return target + dir * distance;
    }
    glm::mat4 view() const { return glm::lookAt(eye(), target, glm::vec3(0.0f, 1.0f, 0.0f)); }
    glm::mat4 projection(float aspect) const {
        const float reach = distance + sceneRadius * 4.0f;
        return glm::perspective(fovY, aspect, std::max(reach * 0.0005f, distance * 0.01f), reach * 4.0f);
    }

    // Fit a bounding sphere in view, keeping the current orientation.
    void frame(const glm::vec3& center, float radius) {
        sceneRadius = std::max(radius, 1e-4f);
        target = center;
        distance = sceneRadius / std::sin(fovY * 0.5f) * 1.1f;
    }
    void resetOrientation() {
        yaw = glm::radians(35.0f);
        pitch = glm::radians(20.0f);
    }
    void orbit(float dxPixels, float dyPixels) {
        yaw -= dxPixels * 0.008f;
        pitch += dyPixels * 0.008f;
        const float limit = glm::radians(89.0f);
        pitch = std::clamp(pitch, -limit, limit);
    }
    void pan(float dxPixels, float dyPixels, float viewportHeight) {
        const glm::mat4 v = view();
        const glm::vec3 right(v[0][0], v[1][0], v[2][0]);
        const glm::vec3 up(v[0][1], v[1][1], v[2][1]);
        const float unitsPerPixel = 2.0f * distance * std::tan(fovY * 0.5f) / std::max(viewportHeight, 1.0f);
        target += (-right * dxPixels + up * dyPixels) * unitsPerPixel;
    }
    void zoom(float wheel) {
        distance *= std::pow(0.88f, wheel);
        distance = std::clamp(distance, sceneRadius * 0.01f, sceneRadius * 100.0f);
    }
};
