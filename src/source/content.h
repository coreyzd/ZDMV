// Finding Source Engine content on disk.
//
// A model only stores names like "models/alyx/alyx_sheet"; the engine finds
// the actual files by looking under every mounted game folder. We don't have
// a gameinfo.txt to go by, so the search path is worked out from where the
// model file sits (see SourceContent's constructor) plus any folders the
// user has added in the viewer.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Extra folders to search, set from the UI. Each may be a game folder
// (containing materials/) or a folder of game folders.
std::vector<std::filesystem::path>& extraContentPaths();

// Resolves `relative` under `base` ignoring case and slash direction, the way
// the engine does on Windows. Returns an empty path if nothing matches.
std::filesystem::path resolveNoCase(const std::filesystem::path& base, const std::string& relative);

class SourceContent {
public:
    explicit SourceContent(const std::filesystem::path& modelFile);

    // Game folders searched, highest priority first.
    const std::vector<std::filesystem::path>& roots() const { return roots_; }

    // First match for a game-relative path such as
    // "materials/models/alyx/alyx_sheet.vmt"; empty if not found anywhere.
    std::filesystem::path find(const std::string& relative) const;

private:
    void addRoot(const std::filesystem::path& dir);
    void addChildRoots(const std::filesystem::path& dir);

    std::vector<std::filesystem::path> roots_;
};
