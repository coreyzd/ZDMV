#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Directory listing for the side panel: sub-folders plus model files that
// have a registered loader.
class Browser {
public:
    struct Entry {
        std::filesystem::path path;
        std::string name;
        bool isDirectory = false;
    };

    // Returns false (and leaves the listing untouched) if `dir` can't be read.
    bool open(const std::filesystem::path& dir);
    void refresh() { open(directory_); }

    const std::filesystem::path& directory() const { return directory_; }
    const std::vector<Entry>& folders() const { return folders_; }
    const std::vector<Entry>& models() const { return models_; }

private:
    std::filesystem::path directory_;
    std::vector<Entry> folders_;
    std::vector<Entry> models_;
};
