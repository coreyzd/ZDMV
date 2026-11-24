#include "browser.h"

#include <algorithm>
#include <cctype>

#include "loaders/loader.h"

namespace fs = std::filesystem;

namespace {
bool lessNoCase(const Browser::Entry& a, const Browser::Entry& b) {
    return std::lexicographical_compare(
        a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
        [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
}
} // namespace

bool Browser::open(const fs::path& dir) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(dir, ec);
    if (ec || !fs::is_directory(canonical, ec)) return false;

    fs::directory_iterator it(canonical, fs::directory_options::skip_permission_denied, ec);
    if (ec) return false;

    std::vector<Entry> folders, models;
    for (fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        Entry e;
        e.path = it->path();
        e.name = e.path.filename().string();
        if (e.name.empty() || e.name[0] == '.') continue; // hidden

        std::error_code typeEc;
        if (it->is_directory(typeEc)) {
            e.isDirectory = true;
            folders.push_back(std::move(e));
        } else if (it->is_regular_file(typeEc) && isSupportedModel(e.path)) {
            models.push_back(std::move(e));
        }
    }
    std::sort(folders.begin(), folders.end(), lessNoCase);
    std::sort(models.begin(), models.end(), lessNoCase);

    directory_ = std::move(canonical);
    folders_ = std::move(folders);
    models_ = std::move(models);
    return true;
}
