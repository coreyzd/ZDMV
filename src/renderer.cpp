#include "renderer.h"

#include <cstddef>
#include <map>

#define GLAD_GL_IMPLEMENTATION
#include <glad/gl.h>
#include <glm/gtc/type_ptr.hpp>

#include "image.h"

namespace {

const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uMvp;
uniform mat3 uNormalMatrix; // model -> view space
out vec3 vNormal;
out vec2 vUV;
void main() {
    vNormal = uNormalMatrix * aNormal;
    vUV = aUV;
    gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

const char* kFragmentShader = R"(#version 330 core
in vec3 vNormal;
in vec2 vUV;
uniform vec3 uColor;
uniform bool uUseTexture;
uniform bool uUnlit;
uniform float uOpacity;     // for unlit overlays
uniform int uAlphaMode;     // 0 opaque, 1 alpha test, 2 blend
uniform float uAlphaCutoff;
uniform sampler2D uTexture;
out vec4 fragColor;
void main() {
    vec4 base = vec4(uColor, 1.0);
    if (uUseTexture) {
        base *= texture(uTexture, vUV);
        if (uAlphaMode == 1 && base.a < uAlphaCutoff) discard;
        if (uAlphaMode == 2 && base.a < 0.004) discard;
    }
    float alpha = (uUseTexture && uAlphaMode == 2) ? base.a : 1.0;
    if (uUnlit) {
        fragColor = vec4(base.rgb, uOpacity);
        return;
    }
    // Headlight slightly above and to the side of the camera; both faces lit.
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    float diffuse = max(dot(n, normalize(vec3(0.35, 0.5, 1.0))), 0.0);
    float fill = max(dot(n, normalize(vec3(-0.6, -0.2, 0.4))), 0.0) * 0.15;
    fragColor = vec4(base.rgb * (0.22 + 0.78 * diffuse + fill), alpha);
}
)";

unsigned compile(GLenum type, const char* source, std::string& error) {
    const unsigned shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        error = std::string("Shader compile failed: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

unsigned loadTexture(const std::filesystem::path& path) {
    ImageData image;
    std::string error;
    if (!loadImage(path, image, error)) return 0;

    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return texture;
}

} // namespace

bool Renderer::init(std::string& error) {
    const unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader, error);
    if (!vs) return false;
    const unsigned fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, error);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        error = std::string("Shader link failed: ") + log;
        return false;
    }
    uMvp_ = glGetUniformLocation(program_, "uMvp");
    uNormalMatrix_ = glGetUniformLocation(program_, "uNormalMatrix");
    uColor_ = glGetUniformLocation(program_, "uColor");
    uUseTexture_ = glGetUniformLocation(program_, "uUseTexture");
    uUnlit_ = glGetUniformLocation(program_, "uUnlit");
    uTexture_ = glGetUniformLocation(program_, "uTexture");
    uAlphaMode_ = glGetUniformLocation(program_, "uAlphaMode");
    uOpacity_ = glGetUniformLocation(program_, "uOpacity");
    uAlphaCutoff_ = glGetUniformLocation(program_, "uAlphaCutoff");

    glGenVertexArrays(1, &gridVao_);
    glGenBuffers(1, &gridVbo_);
    return true;
}

void Renderer::shutdown() {
    clearModel();
    if (gridVbo_) glDeleteBuffers(1, &gridVbo_);
    if (gridVao_) glDeleteVertexArrays(1, &gridVao_);
    if (program_) glDeleteProgram(program_);
    gridVbo_ = gridVao_ = program_ = 0;
}

void Renderer::clearGeometry() {
    for (auto& m : meshes_) {
        glDeleteBuffers(1, &m.vbo);
        glDeleteBuffers(1, &m.ebo);
        glDeleteVertexArrays(1, &m.vao);
    }
    meshes_.clear();
}

void Renderer::clearModel() {
    clearGeometry();
    if (!textures_.empty()) glDeleteTextures(static_cast<GLsizei>(textures_.size()), textures_.data());
    textures_.clear();
    materialTextures_.clear();
    failedTextures_.clear();
    if (physicsVao_) {
        glDeleteBuffers(1, &physicsVbo_);
        glDeleteBuffers(1, &physicsEbo_);
        glDeleteVertexArrays(1, &physicsVao_);
    }
    physicsVao_ = physicsVbo_ = physicsEbo_ = 0;
    physicsIndexCount_ = 0;
    gridVertexCount_ = 0;
}

void Renderer::setModel(const Model& model) {
    clearModel();

    // Several materials commonly share one texture file.
    std::map<std::filesystem::path, unsigned> textureCache;
    materialTextures_.assign(model.materials.size(), 0);
    for (std::size_t i = 0; i < model.materials.size(); ++i) {
        const auto& path = model.materials[i].diffuseTexture;
        if (path.empty()) continue;
        auto it = textureCache.find(path);
        if (it == textureCache.end()) {
            const unsigned tex = loadTexture(path);
            if (tex) textures_.push_back(tex);
            else failedTextures_.push_back(path.filename().string());
            it = textureCache.emplace(path, tex).first;
        }
        materialTextures_[i] = it->second;
    }
    updateGeometry(model);

    if (!model.physics.indices.empty()) {
        const Mesh& p = model.physics;
        physicsIndexCount_ = static_cast<int>(p.indices.size());
        glGenVertexArrays(1, &physicsVao_);
        glBindVertexArray(physicsVao_);
        glGenBuffers(1, &physicsVbo_);
        glBindBuffer(GL_ARRAY_BUFFER, physicsVbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(p.vertices.size() * sizeof(Vertex)), p.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &physicsEbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, physicsEbo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(p.indices.size() * sizeof(std::uint32_t)), p.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glBindVertexArray(0);
    }
}

void Renderer::updateGeometry(const Model& model) {
    clearGeometry();
    for (const auto& mesh : model.meshes) {
        if (mesh.indices.empty()) continue;
        GpuMesh gpu;
        gpu.indexCount = static_cast<int>(mesh.indices.size());
        if (mesh.material >= 0 && mesh.material < static_cast<int>(model.materials.size())) {
            gpu.color = model.materials[mesh.material].diffuse;
            if (mesh.material < static_cast<int>(materialTextures_.size())) gpu.texture = materialTextures_[mesh.material];
            gpu.alphaMode = model.materials[mesh.material].alphaMode;
            gpu.alphaCutoff = model.materials[mesh.material].alphaCutoff;
        }
        glGenVertexArrays(1, &gpu.vao);
        glBindVertexArray(gpu.vao);
        glGenBuffers(1, &gpu.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(Vertex)),
                     mesh.vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &gpu.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(std::uint32_t)),
                     mesh.indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        meshes_.push_back(gpu);
    }
    glBindVertexArray(0);
}

void Renderer::setGrid(const glm::vec3& center, float halfExtent, float spacing) {
    std::vector<glm::vec3> lines;
    if (spacing > 0.0f && halfExtent > 0.0f) {
        const int n = static_cast<int>(halfExtent / spacing);
        const float extent = n * spacing;
        for (int i = -n; i <= n; ++i) {
            const float o = i * spacing;
            lines.emplace_back(center.x + o, center.y, center.z - extent);
            lines.emplace_back(center.x + o, center.y, center.z + extent);
            lines.emplace_back(center.x - extent, center.y, center.z + o);
            lines.emplace_back(center.x + extent, center.y, center.z + o);
        }
    }
    gridVertexCount_ = static_cast<int>(lines.size());
    glBindVertexArray(gridVao_);
    glBindBuffer(GL_ARRAY_BUFFER, gridVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(lines.size() * sizeof(glm::vec3)), lines.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glBindVertexArray(0);
}

void Renderer::draw(const DrawSettings& s) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glUseProgram(program_);
    glUniform1i(uTexture_, 0);
    glUniform1f(uOpacity_, 1.0f);
    glActiveTexture(GL_TEXTURE0);

    const glm::mat4 viewProjection = s.projection * s.view;

    if (s.showGrid && gridVertexCount_ > 0) {
        glUniformMatrix4fv(uMvp_, 1, GL_FALSE, glm::value_ptr(viewProjection));
        glUniform1i(uUnlit_, 1);
        glUniform1i(uUseTexture_, 0);
        glUniform3f(uColor_, 0.32f, 0.33f, 0.36f);
        glBindVertexArray(gridVao_);
        glDrawArrays(GL_LINES, 0, gridVertexCount_);
    }

    const glm::mat4 mvp = viewProjection * s.model;
    const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(s.view * s.model)));
    glUniformMatrix4fv(uMvp_, 1, GL_FALSE, glm::value_ptr(mvp));
    glUniformMatrix3fv(uNormalMatrix_, 1, GL_FALSE, glm::value_ptr(normalMatrix));

    if (s.cullBackfaces) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);

    if (s.shading != ShadingMode::Wireframe) {
        const bool overlay = s.shading == ShadingMode::SolidWireframe;
        if (overlay) {
            // Push the fill back a little so the wireframe drawn on top doesn't z-fight.
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(1.0f, 1.0f);
        }
        glUniform1i(uUnlit_, 0);
        // Opaque and alpha-tested meshes first, then blended ones on top
        // without writing depth.
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                glEnable(GL_BLEND);
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
            }
            for (const auto& m : meshes_) {
                const bool textured = s.showTextures && m.texture != 0;
                const bool blended = textured && m.alphaMode == AlphaMode::Blend;
                if (blended != (pass == 1)) continue;
                glUniform1i(uUseTexture_, textured ? 1 : 0);
                glUniform1i(uAlphaMode_, static_cast<int>(m.alphaMode));
                glUniform1f(uAlphaCutoff_, m.alphaCutoff);
                glUniform3fv(uColor_, 1, glm::value_ptr(m.color));
                if (textured) glBindTexture(GL_TEXTURE_2D, m.texture);
                glBindVertexArray(m.vao);
                glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
            }
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        if (overlay) glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (s.shading != ShadingMode::Solid) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform1i(uUnlit_, 1);
        glUniform1i(uUseTexture_, 0);
        if (s.shading == ShadingMode::SolidWireframe) glUniform3f(uColor_, 0.05f, 0.05f, 0.06f);
        else glUniform3f(uColor_, 0.85f, 0.88f, 0.92f);
        for (const auto& m : meshes_) {
            glBindVertexArray(m.vao);
            glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, nullptr);
        }
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Collision model: translucent cyan hull with its edges drawn on top.
    // Edges hidden behind the model still show faintly, since the hull is
    // often smaller than the visible mesh.
    if (s.showPhysics && physicsIndexCount_ > 0) {
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUniform1i(uUnlit_, 1);
        glUniform1i(uUseTexture_, 0);
        glUniform3f(uColor_, 0.0f, 0.95f, 0.95f);
        glBindVertexArray(physicsVao_);

        glUniform1f(uOpacity_, 0.28f);
        glDrawElements(GL_TRIANGLES, physicsIndexCount_, GL_UNSIGNED_INT, nullptr);

        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDisable(GL_DEPTH_TEST);
        glUniform1f(uOpacity_, 0.22f);
        glDrawElements(GL_TRIANGLES, physicsIndexCount_, GL_UNSIGNED_INT, nullptr);
        glEnable(GL_DEPTH_TEST);
        glUniform1f(uOpacity_, 1.0f);
        glDrawElements(GL_TRIANGLES, physicsIndexCount_, GL_UNSIGNED_INT, nullptr);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    glBindVertexArray(0);
    glDisable(GL_CULL_FACE);
    glUseProgram(0);
}
