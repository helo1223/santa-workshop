#pragma once
#include "lvlx_format.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cctype>

namespace mod_project {
namespace fs = std::filesystem;

inline bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64 || !std::isalnum((unsigned char)id[0])) return false;
    for (unsigned char c : id)
        if (c > 127 || !(std::isalnum(c) || c == '_' || c == '-')) return false;
    std::string upper = id;
    for (char& c : upper) c = (char)std::toupper((unsigned char)c);
    return upper != "CON" && upper != "PRN" && upper != "AUX" && upper != "NUL" &&
        !(upper.size() == 4 && (upper.substr(0,3) == "COM" || upper.substr(0,3) == "LPT") &&
          upper[3] >= '0' && upper[3] <= '9');
}

// Find shared resources independently of the selected level's directory.
inline fs::path findBin(const fs::path& level) {
    const auto absolute = fs::absolute(level);
    // A mod may contain an additive settings/elements.txt. It is not a full
    // resource root: prefer the containing game's stock/extracted bank, then
    // let EditorMain append the mod definitions.
    for (auto p = absolute.parent_path(); !p.empty(); p = p.parent_path()) {
        std::string leaf=p.filename().string();
        for(char& c:leaf)c=(char)std::tolower((unsigned char)c);
        if(leaf=="mods") {
            const auto game=p.parent_path();
            if(fs::is_regular_file(game/"bin_win32/settings/elements.txt"))return game/"bin_win32";
            if(fs::is_regular_file(game/"settings/elements.txt"))return game;
            break;
        }
        if (p == p.parent_path()) break;
    }
    for (auto p = absolute.parent_path(); !p.empty(); p = p.parent_path()) {
        if (fs::is_regular_file(p / "settings/elements.txt")) return p;
        if (fs::is_regular_file(p / "bin_win32/settings/elements.txt")) return p / "bin_win32";
        if (p == p.parent_path()) break;
    }
    return {};
}

inline void writeText(const fs::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary);
    out.write(text.data(), (std::streamsize)text.size());
    out.close();
    if (!out) throw std::runtime_error("Could not write " + file.string());
}

// Publish only a complete new project. Existing projects/resources are never replaced.
inline bool create(const fs::path& bin, const std::string& modId,
        const std::string& levelId, const LvlxLevel& source,
        const std::string& environment, bool blank, const fs::path& sourcePath,
        fs::path& output, std::string& error) {
    fs::path staging;
    bool ownsStaging = false;
    try {
        if (!validId(modId) || !validId(levelId))
            throw std::runtime_error("Use 1-64 ASCII letters, digits, underscores or hyphens; start with a letter/digit.");
        if (!fs::is_regular_file(bin / "settings/elements.txt"))
            throw std::runtime_error("Game settings/elements.txt was not found.");
        if (source.version != 3 || !source.has_spawn_data || source.trailing_size != 24)
            throw std::runtime_error("Mod export requires a reviewed LVLX v3 level with a 24-byte trailer.");
        const auto root = bin.filename() == "bin_win32" ? bin.parent_path() : bin;
        const auto destination = root / "mods" / modId;
        if (fs::exists(destination)) throw std::runtime_error("That mod already exists; choose a new mod ID.");
        fs::create_directories(root / "mods");
        staging = root / "mods" / ("." + modId + ".staging");
        if (!fs::create_directory(staging)) throw std::runtime_error("An export staging folder already exists.");
        ownsStaging = true;
        fs::create_directory(staging / "levels");
        LvlxLevel exported = source; // Borrow source storage; never free this view.
        if (blank) {
            exported.element_count = 0; exported.elements = nullptr;
            exported.waylist_count = 0; exported.waylists = nullptr;
        }
        if (!lvlx_save_level((staging / "levels" / (levelId + ".dat")).string().c_str(), &exported))
            throw std::runtime_error("Could not write level.");
        writeText(staging / "levels" / (levelId + ".txt"), environment);
        auto lmd = sourcePath; lmd.replace_extension(".lmd");
        if (!blank && fs::is_regular_file(lmd))
            fs::copy_file(lmd, staging / "levels" / (levelId + ".lmd"));
        writeText(staging / "mod.json", "{\n  \"schemaVersion\": 1,\n  \"id\": \"" + modId +
            "\",\n  \"title\": \"" + modId + "\",\n  \"levelsDirectory\": \"levels\"\n}\n");
        fs::rename(staging, destination);
        ownsStaging = false;
        output = destination / "levels" / (levelId + ".dat");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (ownsStaging) { std::error_code ignored; fs::remove_all(staging, ignored); }
        return false;
    }
}
}
