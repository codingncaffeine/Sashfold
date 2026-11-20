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
//            [retries=<n>] [cm=none|preview] [capacity=<n>] [uv] [passkey=<site>/<name>/<display name>]...
//   key-touch                 every key plugged in is touched from now on
//   key-unplug                every key is pulled out
//   key-power-cycle           every key loses power for a moment (its token goes)
//   assert-key-credentials <n>   what the first key holds
//   assert-key-pin-retries <n>
// and for the main menu's key settings (ui/KeySettings.h):
//   assert-key-settings <what they show>   waited for, as the reader waits for the key
//   assert-key-settings-focus <where the keyboard is>
//   click-key-settings <button> | row <n>
#include "SoftAuthenticator.h"

#include "core/Unicode.h"
#include "crypto/Ec.h"
#include "crypto/Sha2.h"
#include "net/Url.h"
#include "platform/Clipboard.h"
#include "platform/Random.h"
#include "text/FontManager.h"
#include "ui/Browser.h"
#include "ui/Script.h"
#include "ui/Theme.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
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

    std::optional<std::string> command(ui::Browser& browser, std::string const& name, std::string const& argument)
    {
        std::istringstream words(argument);
        std::vector<std::string> args;
        for (std::string word; words >> word;)
            args.push_back(word);
        if (auto const settings = settings_command(browser, name, argument, args))
            return settings;
        if (name == "key-plug") {
            test::SoftOptions options;
            options.client_pin = true;
            test::TouchPlan touch = test::TouchPlan::never();
            std::vector<std::string> passkeys;
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
                else if (arg.starts_with("retries="))
                    options.pin_retries = std::atoi(arg.c_str() + 8);
                else if (arg == "cm=none")
                    options.credential_management = test::SoftOptions::CredentialManagement::None;
                else if (arg == "cm=preview")
                    options.credential_management = test::SoftOptions::CredentialManagement::Preview;
                else if (arg.starts_with("capacity="))
                    options.discoverable_capacity = static_cast<std::size_t>(std::atoi(arg.c_str() + 9));
                else if (arg == "uv")
                    options.built_in_uv = true;
                else if (arg.starts_with("passkey="))
                    passkeys.push_back(arg.substr(8));
                else
                    return "not understood: " + arg;
            }
            auto key = transport->plug(options);
            key->set_touch(touch);
            for (std::string const& passkey : passkeys) {
                if (!add_passkey(*key, passkey))
                    return "not understood: passkey=" + passkey;
            }
            plugged.push_back(std::move(key));
            return "";
        }
        if (name == "key-power-cycle") {
            for (auto const& key : plugged)
                key->power_cycle();
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

    // `<site>/<name>/<display name>`: a discoverable credential on the key,
    // as a site's registration would have left it.
    bool add_passkey(test::SoftAuthenticator& key, std::string const& spec)
    {
        std::size_t const first = spec.find('/');
        std::size_t const second = first == std::string::npos ? std::string::npos : spec.find('/', first + 1);
        if (second == std::string::npos)
            return false;
        test::SoftAuthenticator::Credential credential;
        credential.rp_id = spec.substr(0, first);
        credential.rp_id_hash = crypto::Sha256::hash(
            std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(credential.rp_id.data()), credential.rp_id.size()));
        credential.user.name = spec.substr(first + 1, second - first - 1);
        credential.user.has_name = true;
        credential.user.display_name = spec.substr(second + 1);
        credential.user.has_display_name = !credential.user.display_name.empty();
        std::uint8_t const serial = static_cast<std::uint8_t>(++m_passkeys);
        credential.user.id = { serial, 0x75 };
        credential.id = { 0xc4, serial, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
        credential.discoverable = true;
        do {
            platform::fill_random(credential.private_key);
        } while (!crypto::ec_private_scalar_valid(crypto::CurveId::P256, credential.private_key));
        key.add_credential(std::move(credential));
        return true;
    }

    // The key settings' commands.
    std::optional<std::string> settings_command(ui::Browser& browser, std::string const& name, std::string const& argument,
        std::vector<std::string> const& args)
    {
        if (name == "assert-key-settings") {
            // The settings' session runs on threads of its own: what they
            // show is waited for (a generous bound: a slow machine is slow,
            // not wrong), the window ticking meanwhile.
            auto const limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (browser.key_settings_shows() != argument && std::chrono::steady_clock::now() < limit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                browser.tick();
                browser.run_scripts();
            }
            std::string const got = browser.key_settings_shows();
            if (got != argument)
                return "expected \"" + argument + "\", got \"" + got + "\"";
            return "";
        }
        if (name == "assert-key-settings-focus") {
            std::string const got = browser.key_settings_focus();
            if (got != argument)
                return "expected \"" + argument + "\", got \"" + got + "\"";
            return "";
        }
        if (name == "click-key-settings") {
            std::optional<Rect> box;
            if (args.size() == 2 && args[0] == "row")
                box = browser.key_settings_box({}, static_cast<std::size_t>(std::atoi(args[1].c_str())));
            else
                box = browser.key_settings_box(argument);
            if (!box)
                return "no " + argument + " on the key settings";
            int const x = box->x + box->width / 2;
            int const y = box->y + box->height / 2;
            browser.mouse_move(x, y);
            browser.mouse_down(x, y, 1);
            browser.mouse_up(x, y, 1);
            browser.tick();
            return "";
        }
        return std::nullopt;
    }

    int m_passkeys = 0;
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
#ifdef _WIN32
    _putenv_s("PULSE_SERVER", "unix:/nonexistent/sashfold-headless");
#else
    setenv("PULSE_SERVER", "unix:/nonexistent/sashfold-headless", 1);
#endif
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
            [&keys, &browser](std::string const& command, std::string const& argument) { return keys.command(browser, command, argument); });
        // The window's requests end before the keys go.
        return result.ok();
    };
    if (!run(false, update))
        return 1;
    std::cout << "again, each tab's page in an engine of its own:\n";
    return run(true, false) ? 0 : 1;
}
