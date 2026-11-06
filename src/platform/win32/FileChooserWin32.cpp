// The common item dialog (IFileOpenDialog), shown from a thread of its own
// with COM in a single-threaded apartment there, as the shell asks.
#include "platform/FileChooser.h"

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace sashfold::platform {

namespace {

std::wstring wide(std::string const& text)
{
    if (text.empty())
        return {};
    int const length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string narrow(wchar_t const* text)
{
    int const length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string out(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), length, nullptr, nullptr);
    return out;
}

class DialogChooser final : public FileChooser {
public:
    DialogChooser(FileChoice choice, std::function<void()> wake)
        : m_wake(std::move(wake))
    {
        m_thread = std::thread([this, choice = std::move(choice)] { run(choice); });
    }
    ~DialogChooser() override
    {
        if (m_thread.joinable())
            m_thread.join();
    }
    std::optional<std::vector<std::string>> take_result() override
    {
        std::lock_guard<std::mutex> const lock(m_mutex);
        std::optional<std::vector<std::string>> out = std::move(m_result);
        m_result.reset();
        return out;
    }
    bool unavailable() const override { return m_unavailable; }

private:
    void finish(std::optional<std::vector<std::string>> result)
    {
        {
            std::lock_guard<std::mutex> const lock(m_mutex);
            if (result)
                m_result = std::move(result);
            else
                m_unavailable = true;
        }
        if (m_wake)
            m_wake();
    }

    void run(FileChoice const& choice)
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            finish(std::nullopt);
            return;
        }
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog)))) {
            CoUninitialize();
            finish(std::nullopt);
            return;
        }
        FILEOPENDIALOGOPTIONS options = 0;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        if (choice.multiple)
            options |= FOS_ALLOWMULTISELECT;
        if (choice.directory)
            options |= FOS_PICKFOLDERS;
        dialog->SetOptions(options);
        if (!choice.title.empty())
            dialog->SetTitle(wide(choice.title).c_str());
        // The accept filters, by pattern (the dialog filters by extension
        // alone), then everything.
        std::vector<std::wstring> names;
        std::vector<std::wstring> specs;
        for (FileFilter const& filter : choice.filters) {
            std::string spec;
            for (std::string const& pattern : filter.patterns)
                spec += (spec.empty() ? "" : ";") + pattern;
            if (spec.empty())
                continue;
            names.push_back(wide(filter.name));
            specs.push_back(wide(spec));
        }
        names.push_back(L"All files");
        specs.push_back(L"*.*");
        std::vector<COMDLG_FILTERSPEC> types;
        for (std::size_t i = 0; i < names.size(); ++i)
            types.push_back({ names[i].c_str(), specs[i].c_str() });
        if (!choice.directory)
            dialog->SetFileTypes(static_cast<UINT>(types.size()), types.data());
        HWND owner = nullptr;
        if (choice.parent_window.rfind("win32:", 0) == 0)
            owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::stoull(choice.parent_window.substr(6), nullptr, 16)));
        std::vector<std::string> paths;
        HRESULT const shown = dialog->Show(owner);
        if (SUCCEEDED(shown)) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dialog->GetResults(&items)) && items) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(i, &item)) || !item)
                        continue;
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        paths.push_back(narrow(path));
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
                items->Release();
            }
        }
        dialog->Release();
        CoUninitialize();
        finish(std::move(paths)); // cancelled: none chosen
    }

    std::function<void()> m_wake;
    std::thread m_thread;
    std::atomic<bool> m_unavailable { false };
    std::mutex m_mutex;
    std::optional<std::vector<std::string>> m_result;
};

}

std::unique_ptr<FileChooser> open_file_chooser(FileChoice choice, std::function<void()> wake)
{
    return std::make_unique<DialogChooser>(std::move(choice), std::move(wake));
}

}
