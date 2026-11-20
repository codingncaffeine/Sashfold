// The shell's Web Authentication prompts, driven by a --script file as the
// browser's own --script mode drives it, with software security keys
// plugged into the window's transport. The keys live here, in the test
// program: the shipped browser has no way to make one.
//
//   shell_webauthn --script <file> --theme <themes/default.json> [--update-goldens]
//
// The page is served at https://webauthn.test/ from the script's folder
// (a secure context with an origin of its own; a file: page has none). As
// the browser's --script does, the script is run by a whole shell and then
// by one whose pages are in engines, and passes when both runs do.
//
// The commands added to the script's:
//   key-plug [pin=<pin>] [touch=now|never] [protocol=ctap21|ctap20|u2f] [no-rk] [min-pin=<n>] [force-pin-change]
//   key-touch                 every key plugged in is touched from now on
//   key-unplug                every key is pulled out
//   assert-key-credentials <n>   what the first key holds
//   assert-key-pin-retries <n>
#include "SoftAuthenticator.h"

#include "core/Unicode.h"
#include "net/Url.h"
#include "platform/Clipboard.h"
#include "text/FontManager.h"
#include "ui/Browser.h"
#include "ui/Script.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace sashfold;

namespace {

constexpr std::string_view site = "webauthn.test";

// Serves https://webauthn.test/<name> from a folder, and nothing else.
class SiteLoader final : public ui::Loader {
public:
    explicit SiteLoader(std::filesystem::path folder)
        : m_folder(std::move(folder))
    {
    }
    net::FetchResult load(net::Url const& url, std::string const&, bool, std::string_view) override
    {
        if (url.scheme != "https" || url.host != site)
            return { std::nullopt, "the test serves only https://webauthn.test/" };
        std::string name = url.serialize_path();
        while (!name.empty() && name.front() == '/')
            name.erase(name.begin());
        if (name.empty() || name.find("..") != std::string::npos)
            return { std::nullopt, "not found" };
        std::ifstream file(m_folder / name, std::ios::binary);
        if (!file)
            return { std::nullopt, "not found" };
        net::FetchResponse response;
        response.status = 200;
        response.status_text = "OK";
        response.headers.push_back({ "Content-Type", "text/html; charset=utf-8" });
        response.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        response.final_url = url;
        return { std::move(response), "" };
    }

private:
    std::filesystem::path m_folder;
};

std::vector<ui::Browser::ThemePreset> presets_beside(std::string const& theme_path)
{
    std::vector<ui::Browser::ThemePreset> presets;
    std::error_code error;
    for (auto const& entry : std::filesystem::directory_iterator(std::filesystem::path(theme_path).parent_path(), error)) {
        if (entry.path().extension() != ".json")
            continue;
        std::vector<std::string> problems;
        if (std::optional<ui::Theme> const theme = ui::Theme::load(entry.path().string(), &problems); theme && problems.empty() && !theme->name.empty())
            presets.push_back({ theme->name, entry.path().string() });
    }
    std::sort(presets.begin(), presets.end(), [](auto const& a, auto const& b) { return a.name < b.name; });
    return presets;
}

// The keys a script plugs in, kept for the run.
struct Keys {
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::vector<std::shared_ptr<test::SoftAuthenticator>> plugged;

    std::optional<std::string> command(std::string const& name, std::string const& argument)
    {
        std::istringstream words(argument);
        std::vector<std::string> args;
        for (std::string word; words >> word;)
            args.push_back(word);
        if (name == "key-plug") {
            test::SoftOptions options;
            options.client_pin = true;
            test::TouchPlan touch = test::TouchPlan::never();
            for (std::string const& arg : args) {
                if (arg.starts_with("pin="))
                    options.pin = arg.substr(4);
                else if (arg == "touch=now")
                    touch = test::TouchPlan::immediately();
                else if (arg == "touch=never")
                    touch = test::TouchPlan::never();
                else if (arg == "protocol=u2f")
                    options.protocol = test::SoftOptions::Protocol::U2fOnly;
                else if (arg == "protocol=ctap20")
                    options.protocol = test::SoftOptions::Protocol::Ctap20;
                else if (arg == "no-rk")
                    options.resident_keys = false;
                else if (arg.starts_with("min-pin="))
                    options.min_pin_length = static_cast<std::size_t>(std::atoi(arg.c_str() + 8));
                else if (arg == "force-pin-change")
                    options.force_pin_change = true;
                else
                    return "not understood: " + arg;
            }
            auto key = transport->plug(options);
            key->set_touch(touch);
            plugged.push_back(std::move(key));
            return "";
        }
        if (name == "key-touch") {
            for (auto const& key : plugged)
                key->set_touch(test::TouchPlan::immediately());
            return "";
        }
        if (name == "key-unplug") {
            for (auto const& key : plugged)
                transport->unplug(key);
            plugged.clear();
            return "";
        }
        if (name == "assert-key-credentials" || name == "assert-key-pin-retries") {
            if (plugged.empty())
                return std::string("no key is plugged in");
            int const got = name == "assert-key-credentials" ? static_cast<int>(plugged.front()->credentials().size()) : plugged.front()->pin_retries();
            if (std::to_string(got) != argument)
                return "expected " + argument + ", got " + std::to_string(got);
            return "";
        }
        return std::nullopt;
    }
};

}

int main(int argc, char** argv)
{
    std::string script;
    std::string theme_path;
    bool update = false;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        if (arg == "--script" && i + 1 < argc)
            script = argv[++i];
        else if (arg == "--theme" && i + 1 < argc)
            theme_path = argv[++i];
        else if (arg == "--update-goldens")
            update = true;
    }
    if (script.empty()) {
        std::cerr << "usage: shell_webauthn --script <file> [--theme <file>] [--update-goldens]\n";
        return 2;
    }
    // As the browser's --script: the built-in face alone, so the pictures
    // are the same on every machine; the process's own clipboard; and no
    // sound server to reach.
    text::FontManager::instance().set_system_fonts(false);
    platform::use_process_clipboard(true);
    setenv("PULSE_SERVER", "unix:/nonexistent/sashfold-headless", 1);
    ui::Theme theme;
    if (!theme_path.empty()) {
        std::vector<std::string> problems;
        theme = ui::Theme::load(theme_path, &problems).value_or(ui::Theme {});
    }
    auto const run = [&](bool in_engines, bool bless) {
        SiteLoader loader(std::filesystem::absolute(std::filesystem::path(script)).parent_path());
        ui::Browser browser(loader, theme, 1024, 720);
        browser.set_pages_in_engines(in_engines);
        platform::write_clipboard_text({});
        if (!theme_path.empty())
            browser.set_theme_presets(presets_beside(theme_path));
        Keys keys;
        browser.set_security_key_transport(keys.transport);
        ui::ScriptResult const result = ui::run_script(browser, script, bless, std::cout,
            [&keys](std::string const& command, std::string const& argument) { return keys.command(command, argument); });
        // The window's requests end before the keys go.
        return result.ok();
    };
    if (!run(false, update))
        return 1;
    std::cout << "again, each tab's page in an engine of its own:\n";
    return run(true, false) ? 0 : 1;
}
