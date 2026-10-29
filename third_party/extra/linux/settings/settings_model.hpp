#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dqix::linux_ui {

// Line-preserving INI document for editing the existing game's settings.ini.
// Unknown keys, comments, blank lines, and entry ordering are retained.
class IniDocument {
public:
    static IniDocument parse(std::string_view text);

    bool load_file(const std::filesystem::path& path, std::string& error);
    bool save_atomic(const std::filesystem::path& path, std::string& error) const;

    std::optional<std::string> get(std::string_view key) const;
    bool set(std::string_view key, std::string_view value, std::string& error);
    bool set_validated(std::string_view key, std::string_view value, std::string& error);

    std::string serialize() const;
    bool empty() const noexcept { return lines_.empty(); }

private:
    struct Line {
        std::string raw;
        std::string key;
        std::string value;
        bool assignment = false;
    };

    static Line parse_line(std::string raw);
    std::vector<Line> lines_;
    bool final_newline_ = false;
};

// The schema intentionally models the keys used by the existing runtime.
// Unrecognized keys may be read and preserved, but cannot be edited through
// set_validated() until their type and accepted values have been audited.
bool validate_setting(std::string_view key, std::string_view value, std::string& error);
const std::vector<std::string_view>& known_setting_keys();

} // namespace dqix::linux_ui
