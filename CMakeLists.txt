cmake_minimum_required(VERSION 3.20)
project(ZDMV VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Dependencies (downloaded at configure time) -----------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)

FetchContent_Declare(glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG 1.0.1
  GIT_SHALLOW TRUE)

# The next three have no CMake target we want; SOURCE_SUBDIR points at a
# directory that doesn't exist so FetchContent only downloads them.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _none)

FetchContent_Declare(tinyobjloader
  GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
  GIT_TAG v2.0.0rc13
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _none)

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 6b5799bae8a1fe3792691ef4b72ae26434c3e799
  SOURCE_SUBDIR _none)

FetchContent_MakeAvailable(glfw glm imgui tinyobjloader stb)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# --- ZDMV --------------------------------------------------------------------
add_executable(zdmv
  src/main.cpp
  src/browser.cpp
  src/image.cpp
  src/normals.cpp
  src/source/content.cpp
  src/source/phy.cpp
  src/source/vmt.cpp
  src/source/vtf.cpp
  src/loaders/mdl_loader.cpp
  src/renderer.cpp
  src/loaders/loader.cpp
  src/loaders/obj_loader.cpp)

target_include_directories(zdmv PRIVATE src third_party/glad/include)
target_include_directories(zdmv SYSTEM PRIVATE ${tinyobjloader_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(zdmv PRIVATE glfw glm::glm imgui)

if(WIN32)
  # Windowed app: no console window behind the viewer. Keep main() as the entry point.
  set_target_properties(zdmv PROPERTIES WIN32_EXECUTABLE ON)
  if(MSVC)
    target_link_options(zdmv PRIVATE /ENTRY:mainCRTStartup)
  elseif(MINGW)
    # Self-contained exe: no libstdc++/libgcc/winpthread DLLs to ship alongside it.
    target_link_options(zdmv PRIVATE -static)
  endif()
endif()

if(MSVC)
  target_compile_options(zdmv PRIVATE /W4)
else()
  target_compile_options(zdmv PRIVATE -Wall -Wextra)
endif()
