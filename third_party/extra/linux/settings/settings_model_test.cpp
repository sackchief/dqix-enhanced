#include "settings_model.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>

using dqix::linux_ui::IniDocument;
using dqix::linux_ui::known_setting_keys;
using dqix::linux_ui::validate_setting;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_validation() {
    std::string error;
    require(validate_setting("mode", "classic", error), "classic mode must be accepted");
    require(validate_setting("mode", "enhanced", error), "enhanced mode must be accepted");
    require(!validate_setting("mode", "turbo", error), "unknown mode must be rejected");
    require(validate_setting("update_check", "1", error), "boolean 1 must be accepted");
    require(validate_setting("update_check", "0", error), "boolean 0 must be accepted");
    require(!validate_setting("update_check", "2", error), "boolean 2 must be rejected");
    require(validate_setting("render_scale", "auto", error), "auto render scale must be accepted");
    require(validate_setting("render_scale", "12", error), "maximum render scale must be accepted");
    require(!validate_setting("render_scale", "13", error), "out-of-range render scale must be rejected");
    require(validate_setting("player_name", "Hero the Brave", error), "player names may contain spaces");
    require(!validate_setting("player_name", std::string(32, 'x'), error), "player name must fit the runtime buffer");
    require(validate_setting("second_window", "-1,40,640,480", error), "signed second-window coordinates must be accepted");
    require(!validate_setting("second_window", "1,2,0,480", error), "zero-sized second window must be rejected");
    require(validate_setting("wireless_mac", "02:aa:B3:00:01:ff", error), "valid MAC address must be accepted");
    require(!validate_setting("wireless_mac", "02:aa:B3:00:01:xyz", error), "malformed MAC address must be rejected");
    require(!validate_setting("monitor_width_mm", "nan", error), "non-finite monitor width must be rejected");
    require(validate_setting("discord_app_id", "1564002016639062076", error),
        "normal Discord application IDs must be accepted");
    require(!validate_setting("discord_app_id", std::string(32, '1'), error),
        "Discord application ID must fit the runtime g_app_id buffer");
    require(!validate_setting("not_a_runtime_setting", "1", error), "unknown keys must not be editable");
}

void test_document_and_atomic_write() {
    const std::string initial =
        "# custom comment\n"
        "mode = classic\n"
        "private_plugin = keep me\n"
        "mode=classic\n"
        "player_name = Old name\n"
        "; another comment without changing it";
    IniDocument document = IniDocument::parse(initial);
    std::string error;

    require(document.get("mode").value_or("") == "classic", "last duplicate setting should be read");
    require(document.get("private_plugin").value_or("") == "keep me", "unknown values should be readable and preserved");

    require(document.set_validated("mode", "enhanced", error), "validated mode update failed: " + error);
    require(document.get("mode").value_or("") == "enhanced", "updated mode should be visible");
    require(document.set_validated("player_name", "Hero the Brave", error), "player name update failed: " + error);
    require(!document.set_validated("volume", "101", error), "out-of-range volume must not be saved");
    require(!document.set_validated("mode", "turbo", error), "invalid mode must not be saved");
    require(!document.set("invalid key", "1", error), "key syntax must be validated");
    require(!document.set("mode", "line one\nmode = classic", error), "newlines must not be injected into values");

    const auto serialized = document.serialize();
    require(serialized.find("# custom comment\n") != std::string::npos, "leading comments must be preserved");
    require(serialized.find("private_plugin = keep me") != std::string::npos, "unknown entries must be preserved");
    require(serialized.find("; another comment without changing it") != std::string::npos, "semicolon comments must be preserved");
    require(serialized.find("mode = enhanced") != std::string::npos, "first duplicate must be updated");
    require(serialized.find("mode=enhanced") != std::string::npos, "all duplicate assignments must be kept consistent");
    require(serialized.find("player_name = Hero the Brave") != std::string::npos, "spaces in player name must round-trip");

    const auto path = std::filesystem::temp_directory_path() /
        ("dqix-settings-model-" + std::to_string(static_cast<long long>(::getpid())) + ".ini");
    std::filesystem::remove(path);
    require(document.save_atomic(path, error), "atomic save failed: " + error);

    IniDocument roundtrip;
    require(roundtrip.load_file(path, error), "reload after atomic save failed: " + error);
    require(roundtrip.get("mode").value_or("") == "enhanced", "mode did not survive a round-trip");
    require(roundtrip.get("private_plugin").value_or("") == "keep me", "unknown setting did not survive a round-trip");
    require(roundtrip.get("player_name").value_or("") == "Hero the Brave", "player name did not survive a round-trip");
    std::filesystem::remove(path);
}

void test_runtime_schema_parity(const std::filesystem::path& runtime_source) {
    std::ifstream input(runtime_source, std::ios::binary);
    require(static_cast<bool>(input), "cannot open runtime source for schema parity: " + runtime_source.string());
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());

    const std::regex loader_key(R"REGEX(strcmp\(key,\s*"([a-z0-9_]+)"\))REGEX");
    const std::regex writer_key(R"REGEX(fprintf\(f,\s*"([a-z0-9_]+)\s*=)REGEX");
    std::set<std::string> loaded;
    std::set<std::string> written;

    for (auto it = std::sregex_iterator(source.begin(), source.end(), loader_key);
         it != std::sregex_iterator(); ++it) {
        loaded.insert((*it)[1].str());
    }

    const auto save_begin = source.find("static void settings_save(void) {");
    require(save_begin != std::string::npos, "could not find settings_save() in runtime source");
    const auto save_end = source.find("\n}\n", save_begin);
    require(save_end != std::string::npos, "could not find end of settings_save()");
    const std::string writer_source = source.substr(save_begin, save_end - save_begin);
    for (auto it = std::sregex_iterator(writer_source.begin(), writer_source.end(), writer_key);
         it != std::sregex_iterator(); ++it) {
        written.insert((*it)[1].str());
    }

    require(!loaded.empty(), "parity test did not find settings loader keys");
    require(!written.empty(), "parity test did not find settings writer keys");
    for (const auto& key : loaded) {
        bool in_schema = false;
        for (const auto candidate : known_setting_keys()) if (candidate == key) in_schema = true;
        require(in_schema, "runtime loader key is missing from the audited schema: " + key);
    }
    require(written.count("discord_app_id") != 0,
        "runtime must persist a custom discord_app_id after loading it");
    for (const auto key_view : known_setting_keys()) {
        const std::string key(key_view);
        require(loaded.count(key) != 0, "schema key is not recognized by runtime loader: " + key);
    }
    for (const auto& key : written) {
        bool in_schema = false;
        for (const auto candidate : known_setting_keys()) if (candidate == key) in_schema = true;
        require(in_schema, "runtime writes a key missing from the audited schema: " + key);
        require(loaded.count(key) != 0, "runtime writes a key not recognized by its loader: " + key);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: dqix-settings-model-tests /path/to/Linux/mirror/frontend_sdl.c");
        test_validation();
        test_document_and_atomic_write();
        test_runtime_schema_parity(argv[1]);
        std::cout << "Settings model tests and runtime schema parity passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Settings model test failed: " << exception.what() << '\n';
        return 1;
    }
}
