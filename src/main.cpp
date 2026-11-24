// ZDMV - a small model viewer for browsing folders of 3D models.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "browser.h"
#include "camera.h"
#include "loaders/loader.h"
#include "normals.h"
#include "renderer.h"
#include "source/content.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Settings that persist between runs (just the last folder, for now).

fs::path configDir() {
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA")) return fs::path(appData) / "zdmv";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) return fs::path(xdg) / "zdmv";
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".config" / "zdmv";
#endif
    return fs::current_path();
}

fs::path readLastDirectory() {
    std::ifstream in(configDir() / "last_dir");
    std::string line;
    if (in && std::getline(in, line) && !line.empty()) return fs::path(line);
    return {};
}

void writeLastDirectory(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::ofstream out(configDir() / "last_dir", std::ios::trunc);
    if (out) out << dir.string() << '\n';
}

// Extra Source content folders, one per line.
void readContentPaths() {
    std::ifstream in(configDir() / "content_paths");
    std::string line;
    extraContentPaths().clear();
    while (in && std::getline(in, line)) {
        if (!line.empty()) extraContentPaths().emplace_back(line);
    }
}

void writeContentPaths() {
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::ofstream out(configDir() / "content_paths", std::ios::trunc);
    for (const auto& p : extraContentPaths()) out << p.string() << '\n';
}

std::string expandHome(std::string path) {
    if (!path.empty() && path[0] == '~') {
        if (const char* home = std::getenv("HOME")) path = std::string(home) + path.substr(1);
    }
    return path;
}

bool containsNoCase(const std::string& haystack, const char* needle) {
    const std::size_t n = std::strlen(needle);
    if (n == 0) return true;
    auto it = std::search(haystack.begin(), haystack.end(), needle, needle + n, [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    });
    return it != haystack.end();
}

// ---------------------------------------------------------------------------

struct App {
    Browser browser;
    Renderer renderer;
    OrbitCamera camera;

    Model model;
    bool hasModel = false;
    fs::path currentFile;
    std::string error;
    double loadMs = 0.0;

    ShadingMode shading = ShadingMode::Solid;
    bool zUp = false;
    bool cullBackfaces = false;
    bool showTextures = true;
    bool showGrid = true;
    bool showPhysics = false;
    NormalMode normalMode = NormalMode::Auto;
    float creaseAngle = 75.0f;
    bool smoothingActive = false; // what Auto resolved to for this model
    bool modelIsFaceted = false;

    char contentPathsInput[8192] = "";
    char pathInput[4096] = "";
    char filter[128] = "";
    bool scrollToSelection = false;
    std::vector<fs::path> dropped;

    bool navigate(const fs::path& dir) {
        if (!browser.open(dir)) {
            error = "Can't open folder: " + dir.string();
            std::snprintf(pathInput, sizeof(pathInput), "%s", browser.directory().string().c_str());
            return false;
        }
        error.clear();
        filter[0] = '\0';
        std::snprintf(pathInput, sizeof(pathInput), "%s", browser.directory().string().c_str());
        writeLastDirectory(browser.directory());
        return true;
    }

    glm::mat4 modelMatrix() const {
        // Z-up -> Y-up: (x, y, z) becomes (x, z, -y).
        return zUp ? glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) : glm::mat4(1.0f);
    }

    // Fit the camera and ground grid to the current model.
    void frameModel() {
        if (!hasModel) return;
        const glm::mat4 m = modelMatrix();
        glm::vec3 lo(0.0f), hi(0.0f);
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? model.boundsMax.x : model.boundsMin.x,
                                   (i & 2) ? model.boundsMax.y : model.boundsMin.y,
                                   (i & 4) ? model.boundsMax.z : model.boundsMin.z);
            const glm::vec3 p = glm::vec3(m * glm::vec4(corner, 1.0f));
            if (i == 0) lo = hi = p;
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        const glm::vec3 center = (lo + hi) * 0.5f;
        const float radius = std::max(glm::length(hi - lo) * 0.5f, 1e-4f);
        camera.frame(center, radius);

        float spacing = std::pow(10.0f, std::floor(std::log10(radius)));
        if (radius / spacing < 2.5f) spacing /= 5.0f;
        renderer.setGrid(glm::vec3(center.x, lo.y, center.z), radius * 2.0f, spacing);
    }

    // Send the model to the GPU with normals according to the current mode.
    // `geometryOnly` keeps the already-loaded textures.
    void uploadModel(bool geometryOnly) {
        if (!hasModel) return;
        const bool smooth = normalMode == NormalMode::Smooth || (normalMode == NormalMode::Auto && modelIsFaceted);
        smoothingActive = smooth;
        const Model* display = &model;
        Model regenerated;
        if (smooth) {
            regenerated = withSmoothNormals(model, creaseAngle);
            display = &regenerated;
        } else if (normalMode == NormalMode::Flat) {
            regenerated = withFlatNormals(model);
            display = &regenerated;
        }
        if (geometryOnly) renderer.updateGeometry(*display);
        else renderer.setModel(*display);
    }

    void loadFile(const fs::path& path) {
        const auto start = std::chrono::steady_clock::now();
        Model loaded;
        std::string loadError;
        currentFile = path;
        if (!loadModel(path, loaded, loadError)) {
            error = path.filename().string() + ": " + loadError;
            hasModel = false;
            renderer.clearModel();
            return;
        }
        error.clear();
        model = std::move(loaded);
        hasModel = true;
        zUp = model.zUp;
        modelIsFaceted = isFaceted(model);
        uploadModel(false);
        frameModel();
        loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    // Open a file or folder given on the command line or dropped on the window.
    void openPath(const fs::path& path) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            navigate(path);
        } else if (fs::is_regular_file(path, ec)) {
            navigate(fs::absolute(path, ec).parent_path());
            if (isSupportedModel(path)) {
                // Use the browser's spelling of the path so the list highlights it.
                fs::path target = fs::weakly_canonical(path, ec);
                loadFile(target);
                scrollToSelection = true;
            } else {
                error = "Unsupported file type: " + path.filename().string();
            }
        } else {
            error = "Not found: " + path.string();
        }
    }
};

void dropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app && count > 0) app->dropped.emplace_back(paths[0]);
}

void drawSidePanel(App& app, float& panelWidth) {
    const ImGuiIO& io = ImGui::GetIO();
    const Browser& browser = app.browser;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y), ImGuiCond_Always);
    ImGui::Begin("Browser", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);

    fs::path pendingFolder;
    fs::path pendingFile;

    // --- Folder path -------------------------------------------------------
    if (ImGui::Button("Up")) pendingFolder = browser.directory().parent_path();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##path", app.pathInput, sizeof(app.pathInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
        pendingFolder = expandHome(app.pathInput);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "Filter models...", app.filter, sizeof(app.filter));

    // --- Models matching the filter, and keyboard stepping -----------------
    std::vector<const Browser::Entry*> visible;
    for (const auto& e : browser.models()) {
        if (containsNoCase(e.name, app.filter)) visible.push_back(&e);
    }
    const bool modalOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (!io.WantTextInput && !modalOpen && !visible.empty()) {
        int current = -1;
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
            if (visible[i]->path == app.currentFile) { current = i; break; }
        }
        const int last = static_cast<int>(visible.size()) - 1;
        int next = current;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) next = std::min(current + 1, last);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) next = current < 0 ? last : std::max(current - 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) next = std::min(current + 10, last);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) next = std::max(current - 10, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) next = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) next = last;
        if (next != current && next >= 0) {
            pendingFile = visible[next]->path;
            app.scrollToSelection = true;
        }
    }
    if (!io.WantTextInput && !modalOpen && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        pendingFolder = browser.directory().parent_path();
    }

    // --- File list ---------------------------------------------------------
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 9.0f + ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    ImGui::BeginChild("files", ImVec2(0, std::max(80.0f, ImGui::GetContentRegionAvail().y - footerHeight)),
                      ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.78f, 1.0f, 1.0f));
    for (const auto& e : browser.folders()) {
        const std::string label = e.name + "/";
        if (ImGui::Selectable(label.c_str(), false)) pendingFolder = e.path;
    }
    ImGui::PopStyleColor();
    for (const Browser::Entry* e : visible) {
        const bool selected = e->path == app.currentFile;
        if (ImGui::Selectable(e->name.c_str(), selected)) pendingFile = e->path;
        if (selected && app.scrollToSelection) {
            ImGui::SetScrollHereY(0.5f);
            app.scrollToSelection = false;
        }
    }
    if (browser.folders().empty() && browser.models().empty()) {
        ImGui::TextDisabled("No models or folders here.");
    } else if (visible.empty() && !browser.models().empty()) {
        ImGui::TextDisabled("No models match the filter.");
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%d of %d models", static_cast<int>(visible.size()), static_cast<int>(browser.models().size()));

    // --- View options ------------------------------------------------------
    ImGui::SeparatorText("View");
    int shading = static_cast<int>(app.shading);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##shading", &shading, "Solid\0Solid + wireframe\0Wireframe\0")) {
        app.shading = static_cast<ShadingMode>(shading);
    }
    ImGui::Checkbox("Textures", &app.showTextures);
    ImGui::SameLine();
    ImGui::Checkbox("Grid", &app.showGrid);
    int normals = static_cast<int>(app.normalMode);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::Combo("Normals", &normals, "Auto\0From file\0Smooth\0Flat\0")) {
        app.normalMode = static_cast<NormalMode>(normals);
        app.uploadModel(true);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto uses the file's normals, but smooths models\nthat would otherwise render faceted.");
    }
    ImGui::BeginDisabled(app.normalMode != NormalMode::Auto && app.normalMode != NormalMode::Smooth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    if (ImGui::SliderFloat("Crease", &app.creaseAngle, 0.0f, 180.0f, "%.0f deg") && app.smoothingActive) {
        app.uploadModel(true);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Edges sharper than this stay hard when smoothing.");
    }
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Z-up", &app.zUp)) app.frameModel();
    ImGui::SameLine();
    ImGui::Checkbox("Cull backfaces", &app.cullBackfaces);
    const bool hasPhysics = app.hasModel && !app.model.physics.indices.empty();
    ImGui::BeginDisabled(!hasPhysics);
    ImGui::Checkbox("Physics model", &app.showPhysics);
    ImGui::EndDisabled();
    if (!hasPhysics && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(app.hasModel ? "This model has no collision data (.phy)." : "No model loaded.");
    }
    if (ImGui::Button("Reset view (F)")) {
        app.camera.resetOrientation();
        app.frameModel();
    }
    ImGui::SameLine();
    if (ImGui::Button("Content paths...")) {
        std::string text;
        for (const auto& p : extraContentPaths()) text += p.string() + "\n";
        std::snprintf(app.contentPathsInput, sizeof(app.contentPathsInput), "%s", text.c_str());
        ImGui::OpenPopup("Source content paths");
    }
    bool reloadForContentPaths = false;
    ImGui::SetNextWindowSize(ImVec2(std::min(720.0f, io.DisplaySize.x - 40.0f), 0.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Source content paths", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Source models (.mdl) name their materials relative to a game folder. ZDMV searches the "
                           "model's own game folder, the folders above it, and its neighbours. Add more folders here, "
                           "one per line: either a game folder (one that contains materials/) or a folder of them.");
        ImGui::InputTextMultiline("##paths", app.contentPathsInput, sizeof(app.contentPathsInput),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f));
        if (app.hasModel && !app.model.contentRoots.empty()) {
            ImGui::SeparatorText("Searched for the current model, in order");
            ImGui::BeginChild("roots", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8.0f), ImGuiChildFlags_Borders);
            for (const auto& root : app.model.contentRoots) ImGui::TextUnformatted(root.string().c_str());
            ImGui::EndChild();
        }
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            extraContentPaths().clear();
            std::string text = app.contentPathsInput, line;
            for (std::size_t start = 0; start <= text.size();) {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                line = text.substr(start, end - start);
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(line.begin());
                if (!line.empty()) extraContentPaths().emplace_back(expandHome(line));
                start = end + 1;
            }
            writeContentPaths();
            reloadForContentPaths = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // --- Model info --------------------------------------------------------
    ImGui::SeparatorText("Model");
    if (!app.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s", app.error.c_str());
        ImGui::PopStyleColor();
    } else if (app.hasModel) {
        const glm::vec3 size = app.model.boundsMax - app.model.boundsMin;
        ImGui::TextUnformatted(app.currentFile.filename().string().c_str());
        ImGui::Text("%zu triangles, %zu vertices", app.model.triangleCount(), app.model.vertexCount());
        int problems = 0;
        for (const auto& mat : app.model.materials) problems += mat.problem.empty() ? 0 : 1;
        if (problems > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%zu meshes, %zu materials (%d missing)",
                               app.model.meshes.size(), app.model.materials.size(), problems);
        } else {
            ImGui::Text("%zu meshes, %zu materials", app.model.meshes.size(), app.model.materials.size());
        }
        if (!app.model.materials.empty() && ImGui::IsItemHovered()) {
            // Where each material's texture came from, or why it has none.
            ImGui::BeginTooltip();
            for (const auto& mat : app.model.materials) {
                if (!mat.problem.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%s: %s", mat.name.c_str(), mat.problem.c_str());
                } else if (!mat.diffuseTexture.empty()) {
                    ImGui::Text("%s", mat.name.c_str());
                    ImGui::TextDisabled("    %s", mat.diffuseTexture.string().c_str());
                } else {
                    ImGui::Text("%s (no texture)", mat.name.c_str());
                }
            }
            ImGui::EndTooltip();
        }
        ImGui::Text("Size %.3g x %.3g x %.3g", size.x, size.y, size.z);
        if (!app.model.physics.indices.empty()) {
            ImGui::Text("Physics: %d convex part%s, %zu triangles", app.model.physicsParts,
                        app.model.physicsParts == 1 ? "" : "s", app.model.physics.indices.size() / 3);
        }
        ImGui::TextDisabled("Loaded in %.0f ms%s", app.loadMs,
                            app.normalMode == NormalMode::Auto && app.smoothingActive ? ", normals smoothed" : "");
        if (!app.renderer.failedTextures().empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "%zu texture(s) could not be decoded",
                               app.renderer.failedTextures().size());
            if (ImGui::IsItemHovered()) {
                std::string names;
                for (const auto& name : app.renderer.failedTextures()) names += name + "\n";
                ImGui::SetTooltip("%s", names.c_str());
            }
        }
        if (!app.model.warnings.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Loader warnings (hover)");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", app.model.warnings.c_str());
        }
    } else {
        ImGui::TextDisabled("No model loaded.");
    }

    // Drag the right edge to resize the panel.
    const float grip = 5.0f;
    const ImVec2 edgeMin(panelWidth - grip, 0), edgeMax(panelWidth + grip, io.DisplaySize.y);
    static bool resizing = false;
    const bool overEdge = ImGui::IsMouseHoveringRect(edgeMin, edgeMax, false);
    if (overEdge && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) resizing = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) resizing = false;
    if (overEdge || resizing) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (resizing) panelWidth = std::clamp(io.MousePos.x, 180.0f, std::max(180.0f, io.DisplaySize.x - 100.0f));

    ImGui::End();

    // Apply navigation after the UI is done iterating the listing.
    if (!pendingFolder.empty()) app.navigate(pendingFolder);
    if (!pendingFile.empty()) app.loadFile(pendingFile);
    else if (reloadForContentPaths && !app.currentFile.empty()) app.loadFile(app.currentFile);
}

} // namespace

int main(int argc, char** argv) {
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, description);
    });
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1400, 860, "ZDMV", nullptr, nullptr);
    if (!window) {
        // Some drivers refuse multisampled default framebuffers.
        glfwWindowHint(GLFW_SAMPLES, 0);
        window = glfwCreateWindow(1400, 860, "ZDMV", nullptr, nullptr);
    }
    if (!window) {
        std::fprintf(stderr, "Could not create an OpenGL 3.3 window.\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "Could not load OpenGL.\n");
        return 1;
    }

    App app;
    glfwSetWindowUserPointer(window, &app);
    glfwSetDropCallback(window, dropCallback);

    std::string error;
    if (!app.renderer.init(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // the layout is fixed, nothing worth saving
    float scaleX = 1.0f, scaleY = 1.0f;
    glfwGetWindowContentScale(window, &scaleX, &scaleY);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scaleX);
    io.FontGlobalScale = scaleX;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    readContentPaths();

    // Start folder: command line argument, else last session's, else cwd.
    if (argc > 1) {
        app.openPath(argv[1]);
    } else {
        const fs::path last = readLastDirectory();
        if (last.empty() || !app.navigate(last)) app.navigate(fs::current_path());
    }
    if (app.browser.directory().empty()) app.navigate(fs::current_path());

    float panelWidth = 320.0f * scaleX;
    std::string title;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            glfwWaitEvents();
            continue;
        }

        if (!app.dropped.empty()) {
            app.openPath(app.dropped.back());
            app.dropped.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawSidePanel(app, panelWidth);

        // --- Camera input (only when the mouse isn't over the UI) ---------
        if (!io.WantCaptureMouse) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) app.camera.orbit(io.MouseDelta.x, io.MouseDelta.y);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
                app.camera.pan(io.MouseDelta.x, io.MouseDelta.y, io.DisplaySize.y);
            }
            if (io.MouseWheel != 0.0f) app.camera.zoom(io.MouseWheel);
        }
        if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            app.camera.resetOrientation();
            app.frameModel();
        }

        if (!app.hasModel) {
            const char* hint = "Pick a model from the list, or drop a file or folder here";
            const ImVec2 textSize = ImGui::CalcTextSize(hint);
            const ImVec2 pos(panelWidth + (io.DisplaySize.x - panelWidth - textSize.x) * 0.5f, io.DisplaySize.y * 0.5f);
            ImGui::GetBackgroundDrawList()->AddText(pos, IM_COL32(150, 155, 165, 255), hint);
        }

        const std::string wantedTitle = app.hasModel ? "ZDMV - " + app.currentFile.filename().string() : "ZDMV";
        if (wantedTitle != title) {
            title = wantedTitle;
            glfwSetWindowTitle(window, title.c_str());
        }

        // --- Render -------------------------------------------------------
        ImGui::Render();
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.13f, 0.14f, 0.16f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // The 3D view fills the area to the right of the side panel.
        const float pixelsPerPoint = io.DisplaySize.x > 0.0f ? fbWidth / io.DisplaySize.x : 1.0f;
        const int viewX = static_cast<int>(panelWidth * pixelsPerPoint);
        const int viewWidth = fbWidth - viewX;
        if (app.hasModel && viewWidth > 0 && fbHeight > 0) {
            glViewport(viewX, 0, viewWidth, fbHeight);
            DrawSettings settings;
            settings.view = app.camera.view();
            settings.projection = app.camera.projection(static_cast<float>(viewWidth) / static_cast<float>(fbHeight));
            settings.model = app.modelMatrix();
            settings.shading = app.shading;
            settings.cullBackfaces = app.cullBackfaces;
            settings.showTextures = app.showTextures;
            settings.showGrid = app.showGrid;
            settings.showPhysics = app.showPhysics;
            app.renderer.draw(settings);
            glViewport(0, 0, fbWidth, fbHeight);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    app.renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
