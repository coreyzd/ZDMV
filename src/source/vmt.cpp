#include "source/vmt.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "util.h"

namespace {

struct Token {
    std::string text;
    bool quoted = false;
};

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '"') {
            const std::size_t end = s.find('"', i + 1);
            const std::size_t stop = end == std::string::npos ? s.size() : end;
            tokens.push_back({s.substr(i + 1, stop - i - 1), true});
            i = stop + 1;
        } else if (c == '{' || c == '}') {
            tokens.push_back({std::string(1, c), false});
            ++i;
        } else {
            std::size_t end = i;
            while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end])) && s[end] != '"' &&
                   s[end] != '{' && s[end] != '}') {
                ++end;
            }
            tokens.push_back({s.substr(i, end - i), false});
            i = end;
        }
    }
    return tokens;
}

struct Block {
    std::map<std::string, std::string> values;
    std::map<std::string, Block> blocks;
};

bool isBrace(const Token& t, char c) { return !t.quoted && t.text.size() == 1 && t.text[0] == c; }

// Parses the contents of a { ... } block; `pos` is just past the '{'.
Block parseBlock(const std::vector<Token>& tokens, std::size_t& pos, int depth) {
    Block block;
    while (pos < tokens.size()) {
        if (isBrace(tokens[pos], '}')) {
            ++pos;
            break;
        }
        const std::string key = toLower(tokens[pos++].text);
        if (pos >= tokens.size()) break;
        if (isBrace(tokens[pos], '{')) {
            ++pos;
            Block child = parseBlock(tokens, pos, depth + 1);
            if (depth < 8) block.blocks[key] = std::move(child);
        } else {
            block.values[key] = tokens[pos++].text;
            // Platform conditionals such as [$X360] may follow a value.
            if (pos < tokens.size() && !tokens[pos].quoted && tokens[pos].text.size() > 2 &&
                tokens[pos].text.front() == '[' && tokens[pos].text.back() == ']') {
                ++pos;
            }
        }
    }
    return block;
}

bool loadVmtRecursive(const std::filesystem::path& path, const SourceContent& content, VmtMaterial& out, int depth) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();

    const std::vector<Token> tokens = tokenize(buffer.str());
    if (tokens.size() < 2 || !isBrace(tokens[1], '{')) return false;
    std::size_t pos = 2;
    Block root = parseBlock(tokens, pos, 0);
    out.shader = toLower(tokens[0].text);

    if (out.shader == "patch") {
        // "patch" { "include" "materials/x.vmt"  "insert"/"replace" { ... } }
        auto include = root.values.find("include");
        if (include != root.values.end() && depth < 4) {
            const std::filesystem::path base = content.find(include->second);
            if (!base.empty()) loadVmtRecursive(base, content, out, depth + 1);
        }
        for (const char* section : {"insert", "replace"}) {
            auto it = root.blocks.find(section);
            if (it == root.blocks.end()) continue;
            for (const auto& [key, value] : it->second.values) out.params[key] = value;
        }
    } else {
        out.params = std::move(root.values);
    }
    return true;
}

} // namespace

bool loadVmt(const std::filesystem::path& path, const SourceContent& content, VmtMaterial& out) {
    out = VmtMaterial{};
    return loadVmtRecursive(path, content, out, 0);
}
