#include "settings_model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace dqix::linux_ui {
namespace {

enum class RuleKind { Boolean, Integer, Enumeration, RenderScale, String, Float, Rectangle, MacAddress };

struct Rule {
    std::string_view key;
    RuleKind kind;
    long long minimum = 0;
    long long maximum = 0;
    std::string_view choices;
    std::size_t max_length = 0;
    bool spaces_allowed = false;
};

constexpr Rule kRules[] = {
    {"update_check", RuleKind::Boolean},
    {"confirm_quit", RuleKind::Boolean},
    {"fast_forward", RuleKind::Enumeration, 0, 0, "toggle|hold"},
    {"update_skip", RuleKind::String, 0, 0, "", 31, false},
    {"session_port", RuleKind::Integer, 1, 65535},
    {"session_consoles", RuleKind::Enumeration, 0, 0, "thread|processes"},
    {"mode", RuleKind::Enumeration, 0, 0, "classic|enhanced"},
    {"resolution_3d", RuleKind::Integer, 1, 4},
    {"render_scale", RuleKind::RenderScale, 1, 12, "auto"},
    {"window_scale", RuleKind::Integer, 1, 8},
    {"layout", RuleKind::Enumeration, 0, 0, "stacked|side_by_side|corner|wide|wide_dynamic|two_windows"},
    {"single_screen", RuleKind::Boolean},
    {"small_screen_corner", RuleKind::Integer, 0, 3},
    {"small_screen_size", RuleKind::Integer, 0, 2},
    {"small_screen_hidden", RuleKind::Boolean},
    {"small_screen_opacity_chosen", RuleKind::Boolean},
    {"volume", RuleKind::Integer, 0, 100},
    {"mute", RuleKind::Boolean},
    {"swap_screens", RuleKind::Boolean},
    {"widescreen_popout", RuleKind::Boolean},
    {"widescreen_battle_hud", RuleKind::Boolean},
    {"smooth_textures", RuleKind::Boolean},
    {"outlines", RuleKind::Enumeration, 0, 0, "thin|medium|ds"},
    {"soft_shadows", RuleKind::Boolean},
    {"smooth_shading", RuleKind::Boolean},
    {"hd_text", RuleKind::Boolean},
    {"texture_packs", RuleKind::Boolean},
    {"music_packs", RuleKind::Boolean},
    {"music_song_names", RuleKind::Boolean},
    {"music_pack_volume", RuleKind::Integer, 0, 200},
    {"discord", RuleKind::Boolean},
    {"discord_app_id", RuleKind::String, 0, 0, "", 31, false},
    {"upscale_textures", RuleKind::Boolean},
    {"hd_text_shadows", RuleKind::Boolean},
    {"small_screen_opacity", RuleKind::Integer, 10, 100},
    {"second_window", RuleKind::Rectangle},
    {"second_window_fullscreen", RuleKind::Boolean},
    {"walk_any_direction", RuleKind::Boolean},
    {"fullscreen", RuleKind::Boolean},
    {"dslite_filter", RuleKind::Boolean},
    {"lcd_colors", RuleKind::Boolean},
    {"pixel_grid", RuleKind::Boolean},
    {"motion_blur", RuleKind::Boolean},
    {"backlight", RuleKind::Enumeration, 0, 0, "low|medium|high|max"},
    {"screen_gap", RuleKind::Boolean},
    {"console", RuleKind::Boolean},
    {"console_color", RuleKind::String, 0, 0, "", 63, false},
    {"console_color_chosen", RuleKind::Boolean},
    {"integer_scaling", RuleKind::Boolean},
    {"actual_size", RuleKind::Boolean},
    {"monitor_width_mm", RuleKind::Float, 0, 5000},
    {"downloadable_content", RuleKind::Boolean},
    {"language", RuleKind::Enumeration, 0, 0, "english|french|german|italian|spanish"},
    {"wireless", RuleKind::Boolean},
    {"wfc_server", RuleKind::String, 0, 0, "", 63, false},
    {"wireless_mac", RuleKind::MacAddress},
    {"player_name", RuleKind::String, 0, 0, "", 31, true},
};

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return value;
}

bool valid_key(std::string_view key) {
    if (key.empty()) return false;
    for (const char c : key) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

const Rule* find_rule(std::string_view key) {
    for (const auto& rule : kRules) if (rule.key == key) return &rule;
    return nullptr;
}

bool parse_integer(std::string_view value, long long& number) {
    if (value.empty()) return false;
    const char* first = value.data();
    const char* last = first + value.size();
    const auto result = std::from_chars(first, last, number, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

bool one_of(std::string_view value, std::string_view choices) {
    while (!choices.empty()) {
        const auto separator = choices.find('|');
        const auto choice = choices.substr(0, separator);
        if (value == choice) return true;
        if (separator == std::string_view::npos) break;
        choices.remove_prefix(separator + 1);
    }
    return false;
}

bool valid_rectangle(std::string_view value) {
    long long parts[4]{};
    for (int i = 0; i < 4; ++i) {
        const auto comma = value.find(',');
        const auto piece = trim(value.substr(0, comma));
        if (!parse_integer(piece, parts[i])) return false;
        if (i == 3) {
            if (comma != std::string_view::npos) return false;
        } else {
            if (comma == std::string_view::npos) return false;
            value.remove_prefix(comma + 1);
        }
        if (parts[i] < std::numeric_limits<int>::min() ||
            parts[i] > std::numeric_limits<int>::max()) return false;
    }
    return parts[2] > 0 && parts[3] > 0;
}

bool valid_mac_address(std::string_view value) {
    if (value.size() != 17) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if ((i + 1) % 3 == 0) {
            if (value[i] != ':') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
    }
    return true;
}

std::string system_error(const char* operation, const std::filesystem::path& path) {
    std::ostringstream stream;
    stream << operation << " '" << path.string() << "': " << std::strerror(errno);
    return stream.str();
}

bool write_all(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto count = ::write(fd, data.data() + offset, data.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (count == 0) {
            errno = EIO;
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

} // namespace

IniDocument IniDocument::parse(std::string_view text) {
    IniDocument document;
    document.final_newline_ = !text.empty() && text.back() == '\n';

    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        std::string raw(text.substr(start, newline == std::string_view::npos
                                             ? text.size() - start
                                             : newline - start));
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        document.lines_.push_back(parse_line(std::move(raw)));
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    return document;
}

IniDocument::Line IniDocument::parse_line(std::string raw) {
    Line line;
    line.raw = std::move(raw);
    const std::string_view view(line.raw);
    const auto first = trim(view);
    if (first.empty() || first.front() == '#' || first.front() == ';') return line;

    const auto equals = view.find('=');
    if (equals == std::string_view::npos) return line;
    const auto key = trim(view.substr(0, equals));
    if (!valid_key(key)) return line;

    line.key.assign(key);
    line.value.assign(trim(view.substr(equals + 1)));
    line.assignment = true;
    return line;
}

bool IniDocument::load_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = system_error("cannot open settings file", path);
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        error = "failed while reading settings file '" + path.string() + "'";
        return false;
    }

    IniDocument loaded = parse(buffer.str());
    *this = std::move(loaded);
    error.clear();
    return true;
}

std::optional<std::string> IniDocument::get(std::string_view key) const {
    std::optional<std::string> found;
    for (const auto& line : lines_) {
        if (line.assignment && line.key == key) found = line.value;
    }
    return found;
}

bool IniDocument::set(std::string_view key, std::string_view value, std::string& error) {
    if (!valid_key(key)) {
        error = "invalid setting key";
        return false;
    }
    for (const char c : value) {
        if (c == '\n' || c == '\r' || c == '\0') {
            error = "setting values cannot contain newline or NUL characters";
            return false;
        }
    }

    bool found = false;
    for (auto& line : lines_) {
        if (!line.assignment || line.key != key) continue;
        const auto equals = line.raw.find('=');
        if (equals == std::string::npos) continue;
        std::size_t leading = equals + 1;
        while (leading < line.raw.size() &&
               std::isspace(static_cast<unsigned char>(line.raw[leading]))) ++leading;
        std::size_t trailing = line.raw.size();
        while (trailing > leading &&
               std::isspace(static_cast<unsigned char>(line.raw[trailing - 1]))) --trailing;

        line.raw = line.raw.substr(0, leading) + std::string(value) + line.raw.substr(trailing);
        line = parse_line(std::move(line.raw));
        found = true;
    }

    if (!found) {
        lines_.push_back(parse_line(std::string(key) + " = " + std::string(value)));
    }
    final_newline_ = true;
    error.clear();
    return true;
}

bool IniDocument::set_validated(std::string_view key, std::string_view value, std::string& error) {
    if (!validate_setting(key, value, error)) return false;
    return set(key, value, error);
}

std::string IniDocument::serialize() const {
    std::string output;
    std::size_t reserve = 1;
    for (const auto& line : lines_) reserve += line.raw.size() + 1;
    output.reserve(reserve);
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i != 0) output.push_back('\n');
        output += lines_[i].raw;
    }
    if (final_newline_ && !lines_.empty()) output.push_back('\n');
    return output;
}

bool IniDocument::save_atomic(const std::filesystem::path& path, std::string& error) const {
    struct stat old_stat {};
    mode_t mode = 0600;
    if (::lstat(path.c_str(), &old_stat) == 0) {
        if (!S_ISREG(old_stat.st_mode)) {
            error = "refusing to replace a non-regular settings path '" + path.string() + "'";
            return false;
        }
        mode = old_stat.st_mode & 0777;
    } else if (errno != ENOENT) {
        error = system_error("cannot inspect settings file", path);
        return false;
    }

    static std::atomic<unsigned long> sequence{0};
    std::filesystem::path temporary;
    int fd = -1;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary = path;
        temporary += ".tmp." + std::to_string(static_cast<long long>(::getpid())) + "." +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (fd >= 0) break;
        if (errno != EEXIST) {
            error = system_error("cannot create temporary settings file", temporary);
            return false;
        }
    }
    if (fd < 0) {
        error = "could not allocate a unique temporary settings file beside '" + path.string() + "'";
        return false;
    }
    if (::fchmod(fd, mode) != 0) {
        error = system_error("cannot set temporary settings permissions", temporary);
        ::close(fd);
        ::unlink(temporary.c_str());
        return false;
    }

    const auto data = serialize();
    bool ok = write_all(fd, data);
    if (!ok) error = system_error("cannot write temporary settings file", temporary);
    if (ok && ::fsync(fd) != 0) {
        ok = false;
        error = system_error("cannot sync temporary settings file", temporary);
    }
    if (::close(fd) != 0 && ok) {
        ok = false;
        error = system_error("cannot close temporary settings file", temporary);
    }
    if (!ok) {
        ::unlink(temporary.c_str());
        return false;
    }

    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        error = system_error("cannot atomically replace settings file", path);
        ::unlink(temporary.c_str());
        return false;
    }

    error.clear();
    return true;
}

bool validate_setting(std::string_view key, std::string_view value, std::string& error) {
    const Rule* rule = find_rule(key);
    if (!rule) {
        error = "setting is not yet in the audited Linux UI schema: " + std::string(key);
        return false;
    }

    value = trim(value);
    switch (rule->kind) {
    case RuleKind::Boolean: {
        long long number = 0;
        if (!parse_integer(value, number) || number < 0 || number > 1) {
            error = "setting '" + std::string(key) + "' must be 0 or 1";
            return false;
        }
        break;
    }
    case RuleKind::Integer: {
        long long number = 0;
        if (!parse_integer(value, number) || number < rule->minimum || number > rule->maximum) {
            error = "setting '" + std::string(key) + "' is outside its accepted integer range";
            return false;
        }
        break;
    }
    case RuleKind::Enumeration:
        if (!one_of(value, rule->choices)) {
            error = "setting '" + std::string(key) + "' has an unsupported value";
            return false;
        }
        break;
    case RuleKind::RenderScale: {
        if (value != "auto") {
            long long number = 0;
            if (!parse_integer(value, number) || number < rule->minimum || number > rule->maximum) {
                error = "render_scale must be 'auto' or an integer in the supported range";
                return false;
            }
        }
        break;
    }
    case RuleKind::String:
        if (value.size() > rule->max_length) {
            error = "setting '" + std::string(key) + "' is too long for the existing runtime field";
            return false;
        }
        for (const char c : value) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7f ||
                (!rule->spaces_allowed && std::isspace(byte))) {
                error = "setting '" + std::string(key) + "' contains unsupported whitespace/control characters";
                return false;
            }
        }
        break;
    case RuleKind::Float: {
        if (value.empty()) {
            error = "setting '" + std::string(key) + "' must be numeric";
            return false;
        }
        const std::string owned(value);
        char* end = nullptr;
        errno = 0;
        const float number = std::strtof(owned.c_str(), &end);
        if (errno == ERANGE || end != owned.c_str() + owned.size() || !std::isfinite(number) ||
            number < static_cast<float>(rule->minimum) || number > static_cast<float>(rule->maximum)) {
            error = "setting '" + std::string(key) + "' is outside its accepted numeric range";
            return false;
        }
        break;
    }
    case RuleKind::Rectangle:
        if (!valid_rectangle(value)) {
            error = "second_window must be x,y,width,height with integer coordinates and positive dimensions";
            return false;
        }
        break;
    case RuleKind::MacAddress:
        if (!valid_mac_address(value)) {
            error = "wireless_mac must contain six hexadecimal byte pairs separated by colons";
            return false;
        }
        break;
    }

    error.clear();
    return true;
}

const std::vector<std::string_view>& known_setting_keys() {
    static const std::vector<std::string_view> keys = [] {
        std::vector<std::string_view> result;
        result.reserve(sizeof(kRules) / sizeof(kRules[0]));
        for (const auto& rule : kRules) result.push_back(rule.key);
        return result;
    }();
    return keys;
}

} // namespace dqix::linux_ui
