#pragma once

// CONFIRMED schema/defaults: reverse/knowledge/environment_settings.md.
// This authoring parser rejects malformed values; untouched source bytes survive saves.
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <limits>

namespace environment {
struct Property {
    const char* name;
    const char* initial;
    int count;
    bool integer = false;
    bool color = false;
    int ambient = -1;
};
inline const std::vector<Property> schema = {
    {"AmbientLightColor", ".20 .21 .28",3,false,true,0},
    {"AmbientLightColor", ".175 .175 .105",3,false,true,1},
    {"AmbientLightColor", ".05 .08 .10",3,false,true,2},
    {"AmbientLightColor", ".18 .18 .10",3,false,true,3},
    {"DirectionalLightColor", ".376 .376 .376",3,false,true},
    {"DirectionalLightAngles", "40.514386 -45",2},
    {"EnvironmentCubeMap", "\"env_cube_01\"",0},
    {"FogColor", "0 0 0",3,false,true},
    {"FogDepthRanges", "140 50",2},
    {"FogHeightRanges", "-2000 32",2},
    {"SkyColors", "0 0 0 0 0 0",6},
    {"SkyPositionOffset", "0 0 0",3},
    {"SkySunColors", "0 0 0 0 0 0",6},
    {"SkySunExponents", "0 0",2},
    {"SkySunAngles", "0 0",2},
    {"SnowFieldMinMaxOffsets", "-3 -2 -3 3 2 3",6,true},
    {"SnowFieldLowerYLimit", "-3",1,true},
    {"SnowFieldSampleDims", "10 18 10",3},
    {"SnowFieldEffect", "\"snow_01.fxt\"",0},
    {"EffectNearClipping", "1 2",2},
    {"LevelDeadZoneHeightStart", "-1",1},
    {"DemoCameraOffset", "0 1.3 -6.03",3},
    {"DemoCameraTargetOffset", "2 3.03 -1.03",3},
    {"DemoCameraRotationParameter", ".3 .4 4.5",3}
};
inline std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
inline bool parse(const Property& p, std::string value, std::string& normalized) {
    if (!p.count) {
        const size_t first = value.find_first_not_of(" \t\r\n"), last = value.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || last <= first+1 || value[first] != '"' || value[last] != '"') return false;
        const std::string name = value.substr(first+1, last-first-1);
        if (name.find_first_of("\r\n\"") != std::string::npos) return false;
        normalized = "\"" + name + "\"";
        return true;
    }
    for (char& c : value) if (c == ',' || c == ';') c = ' ';
    std::istringstream in(value);
    std::vector<double> numbers;
    std::string token;
    while (in >> token) {
        char* end = nullptr;
        double n = std::strtod(token.c_str(), &end);
        if (end == token.c_str() || *end || !std::isfinite(n) ||
            std::fabs(n) > std::numeric_limits<float>::max()) return false;
        if (p.integer && (std::trunc(n) != n || n < -2147483648.0 || n > 2147483647.0)) return false;
        numbers.push_back(n);
    }
    if (numbers.size() != (size_t)p.count && !(p.color && numbers.size() == (size_t)p.count + 1)) return false;
    if (numbers.size() > (size_t)p.count) {
        for (int i = 0; i < p.count; ++i) numbers[i] *= numbers.back();
        numbers.pop_back();
    }
    std::ostringstream out;
    out << std::setprecision(9);
    for (size_t i = 0; i < numbers.size(); ++i) {
        if (!std::isfinite(numbers[i]) || std::fabs(numbers[i]) > std::numeric_limits<float>::max()) return false;
        if (i) out << ' ';
        out << numbers[i];
    }
    normalized = out.str();
    return true;
}
inline bool read(const std::filesystem::path& path, std::string& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream out;
    out << file.rdbuf();
    if (file.bad()) return false;
    bytes = out.str();
    return true;
}
struct Document {
    std::vector<std::string> values, inherited;
    std::vector<bool> local;
    std::map<size_t, std::string> edits;
    std::string original, path;
    std::vector<std::string> diagnostics;
    bool existed = false;
    bool readable = true;

    void overlay(const std::string& text, bool companion) {
        std::istringstream lines(text);
        std::string line;
        int lineNumber = 0;
        while (std::getline(lines, line)) {
            ++lineNumber;
            bool quoted = false;
            for (size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '"') quoted = !quoted;
                if (!quoted && (line[i] == '#' || (line[i] == '/' && i+1 < line.size() && line[i+1] == '/'))) {
                    line.resize(i); break;
                }
            }
            std::istringstream in(line);
            std::string name;
            if (!(in >> name)) continue;
            int index = -1;
            if (lower(name) == "ambientlightcolor" && !(in >> index)) index = -2;
            std::string value;
            std::getline(in, value);
            bool found = false;
            for (size_t i = 0; i < schema.size(); ++i) {
                const auto& p = schema[i];
                if (lower(p.name) != lower(name) || p.ambient != index) continue;
                found = true;
                std::string parsed;
                if (parse(p, value, parsed)) {
                    values[i] = parsed;
                    if (companion) local[i] = true;
                } else diagnostics.push_back(name + ": invalid value at line " + std::to_string(lineNumber));
                break;
            }
            if (!found) diagnostics.push_back(name + ": unsupported property/index preserved at line " + std::to_string(lineNumber));
        }
    }
    void load(const std::string& global, const std::string& companion) {
        *this = Document{};
        path = companion;
        for (const auto& p : schema) {
            std::string value;
            parse(p, p.initial, value);
            values.push_back(value);
        }
        local.resize(schema.size(), false);
        std::string bytes;
        if (read(global, bytes)) overlay(bytes, false);
        else diagnostics.push_back("Global environment unavailable: using constructor defaults");
        inherited = values;
        std::error_code ec;
        existed = std::filesystem::exists(path, ec);
        if (ec || (existed && !read(path, original))) {
            readable = false;
            diagnostics.push_back("Companion could not be read; saving disabled");
        } else if (existed) overlay(original, true);
    }
    std::string value(size_t i) const {
        auto it = edits.find(i);
        return it == edits.end() ? values.at(i) : it->second;
    }
    std::array<float, 6> numbers(size_t i) const {
        std::array<float, 6> result{};
        std::istringstream in(value(i));
        for (int n = 0; n < schema[i].count; ++n) in >> result[n];
        return result;
    }
    bool set(size_t i, const std::string& text) {
        std::string parsed;
        if (i >= schema.size() || !parse(schema[i], text, parsed)) return false;
        if (parsed == values[i]) edits.erase(i);
        else edits[i] = parsed;
        return true;
    }
    std::string serialize() const {
        std::string bytes = original;
        for (const auto& [i, value] : edits) {
            if (!bytes.empty() && bytes.back() != '\n') bytes += "\r\n";
            bytes += schema[i].name;
            if (schema[i].ambient >= 0) bytes += " " + std::to_string(schema[i].ambient);
            bytes += " " + value + "\r\n";
        }
        return bytes;
    }
    bool save(std::string& message) {
        if (edits.empty()) return true;
        if (!readable) { message = "Environment save blocked: unreadable source"; return false; }
        std::error_code ec;
        const bool present = std::filesystem::exists(path, ec);
        std::string current;
        if (ec || present != existed || (present && (!read(path, current) || current != original))) {
            message = "Environment changed on disk; save blocked to preserve external edits"; return false;
        }
        std::string temp = path + ".santamapper.tmp";
        if (std::filesystem::exists(temp, ec) || ec) { message = "Environment temporary file already exists"; return false; }
        std::string backup = path + ".santamapper.bak";
        for (unsigned n = 1; std::filesystem::exists(backup, ec) && !ec; ++n)
            backup = path + ".santamapper.bak." + std::to_string(n);
        if (ec) { message = "Cannot inspect environment backup path"; return false; }
        const std::string bytes = serialize();
        {
            std::ofstream out(temp, std::ios::binary);
            out.write(bytes.data(), (std::streamsize)bytes.size());
            out.close();
            if (!out) { std::filesystem::remove(temp, ec); message = "Environment write failed"; return false; }
        }
        if (present) std::filesystem::rename(path, backup, ec);
        if (!ec) std::filesystem::rename(temp, path, ec);
        if (ec) {
            std::error_code restore;
            if (present && !std::filesystem::exists(path, restore)) std::filesystem::rename(backup, path, restore);
            message = "Environment replacement failed; original/backup and temporary file retained";
            return false;
        }
        original = bytes;
        for (const auto& [i, value] : edits) { values[i] = value; local[i] = true; }
        edits.clear();
        existed = true;
        message = "Environment saved" + (present ? "; backup: " + backup : "");
        return true;
    }
};
} // namespace environment
