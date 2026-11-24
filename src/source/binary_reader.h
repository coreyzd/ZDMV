// Bounds-checked access to a binary file held in memory. Every read throws
// std::runtime_error instead of running off the end, so a truncated or
// corrupt file becomes a load error rather than a crash.
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

class BinaryFile {
public:
    bool open(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return false;
        const std::streamoff size = in.tellg();
        if (size < 0) return false;
        data_.resize(static_cast<std::size_t>(size));
        in.seekg(0);
        if (size > 0) in.read(reinterpret_cast<char*>(data_.data()), size);
        name_ = path.filename().string();
        return static_cast<bool>(in);
    }

    std::size_t size() const { return data_.size(); }
    const std::uint8_t* data() const { return data_.data(); }

    void require(std::int64_t offset, std::int64_t length) const {
        if (offset < 0 || length < 0 || offset + length > static_cast<std::int64_t>(data_.size())) {
            throw std::runtime_error(name_ + " is truncated or corrupt.");
        }
    }

    template <typename T>
    T read(std::int64_t offset) const {
        require(offset, sizeof(T));
        T value;
        std::memcpy(&value, data_.data() + offset, sizeof(T));
        return value;
    }
    std::int32_t i32(std::int64_t offset) const { return read<std::int32_t>(offset); }
    std::uint16_t u16(std::int64_t offset) const { return read<std::uint16_t>(offset); }
    std::uint8_t u8(std::int64_t offset) const { return read<std::uint8_t>(offset); }
    float f32(std::int64_t offset) const { return read<float>(offset); }

    // NUL-terminated string starting at `offset`.
    std::string cstring(std::int64_t offset) const {
        require(offset, 0);
        std::string s;
        for (std::size_t i = static_cast<std::size_t>(offset); i < data_.size() && data_[i] != 0; ++i) {
            s.push_back(static_cast<char>(data_[i]));
        }
        return s;
    }

private:
    std::vector<std::uint8_t> data_;
    std::string name_;
};
