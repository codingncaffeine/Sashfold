#include "platform/Notifier.h"

// Windows' toasts (Windows.UI.Notifications), as Chrome and Firefox show a
// page's notifications there: a ToastNotifier made for the browser's
// application identity shows each notice from the XML of the ToastGeneric
// template (platform::toast_xml), with the site on the attribution line and
// the page's picture as the toast's logo. A press is the toast's Activated
// event, a dismissal its Dismissed with the reader's or our own reason;
// one that only timed out into the notification centre stays there and can
// still be pressed.
//
// The runtime is reached through combase.dll, looked up when the notifier
// starts — a Windows without it (before 10, or Wine) has no notifier, and
// pages are answered "denied" — and everything of it lives on one thread
// of the notifier's own in the multithreaded apartment, whatever apartment
// the window's thread is in; the events come on the system's threads and
// are only handed over from there.
//
// An unpackaged program names itself to the shell by its application user
// model ID, registered for this user under
// HKCU\Software\Classes\AppUserModelId with the name and the icon a toast
// shows (what Windows documents for a program with no package and no
// shortcut).

#include "core/Png.h"

#include <windows.h>

#include <shobjidl.h>
#include <winstring.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace sashfold::platform {

namespace {

// The seven interfaces of the runtime this needs, declared here as the
// Windows metadata lays out their vtables (IUnknown's three, IInspectable's
// three, then the interface's own, in order) — only the slots called are
// typed. MinGW's headers for them do not compile as C++ (their
// windows.foundation.h defines IReference twice over, boolean and UINT8
// being one type), in either of their forms.
template <typename Self>
struct InspectableSlots {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(Self*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(Self*);
    ULONG(STDMETHODCALLTYPE* Release)(Self*);
    void* GetIids;
    void* GetRuntimeClassName;
    void* GetTrustLevel;
};
template <typename Table>
struct Object {
    Table const* lpVtbl;
};

struct EventToken {
    std::int64_t value = 0;
};
enum DismissalReason : std::int32_t {
    dismissed_by_reader = 0, // ToastDismissalReason_UserCanceled
    hidden_by_us = 1, // ToastDismissalReason_ApplicationHidden
    timed_out = 2, // ToastDismissalReason_TimedOut
};

struct InspectableTable;
using IInspectableObject = Object<InspectableTable>;
struct InspectableTable {
    InspectableSlots<IInspectableObject> base;
};
struct XmlDocumentTable;
using IXmlDocument = Object<XmlDocumentTable>;
struct XmlDocumentTable {
    InspectableSlots<IXmlDocument> base;
};
struct XmlDocumentIOTable;
using IXmlDocumentIO = Object<XmlDocumentIOTable>;
struct XmlDocumentIOTable {
    InspectableSlots<IXmlDocumentIO> base;
    HRESULT(STDMETHODCALLTYPE* LoadXml)(IXmlDocumentIO*, HSTRING);
    void* LoadXmlWithSettings;
    void* SaveToFileAsync;
};
struct ToastDismissedEventArgsTable;
using IToastDismissedEventArgs = Object<ToastDismissedEventArgsTable>;
struct ToastDismissedEventArgsTable {
    InspectableSlots<IToastDismissedEventArgs> base;
    HRESULT(STDMETHODCALLTYPE* get_Reason)(IToastDismissedEventArgs*, DismissalReason*);
};
struct ToastNotificationTable;
using IToastNotification = Object<ToastNotificationTable>;
struct ToastNotificationTable {
    InspectableSlots<IToastNotification> base;
    void* get_Content;
    void* put_ExpirationTime;
    void* get_ExpirationTime;
    HRESULT(STDMETHODCALLTYPE* add_Dismissed)(IToastNotification*, void* handler, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_Dismissed)(IToastNotification*, EventToken);
    HRESULT(STDMETHODCALLTYPE* add_Activated)(IToastNotification*, void* handler, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_Activated)(IToastNotification*, EventToken);
    HRESULT(STDMETHODCALLTYPE* add_Failed)(IToastNotification*, void* handler, EventToken*);
    HRESULT(STDMETHODCALLTYPE* remove_Failed)(IToastNotification*, EventToken);
};
struct ToastNotificationFactoryTable;
using IToastNotificationFactory = Object<ToastNotificationFactoryTable>;
struct ToastNotificationFactoryTable {
    InspectableSlots<IToastNotificationFactory> base;
    HRESULT(STDMETHODCALLTYPE* CreateToastNotification)(IToastNotificationFactory*, IXmlDocument*, IToastNotification**);
};
struct ToastNotifierTable;
using IToastNotifier = Object<ToastNotifierTable>;
struct ToastNotifierTable {
    InspectableSlots<IToastNotifier> base;
    HRESULT(STDMETHODCALLTYPE* Show)(IToastNotifier*, IToastNotification*);
    HRESULT(STDMETHODCALLTYPE* Hide)(IToastNotifier*, IToastNotification*);
};
struct ToastNotificationManagerStaticsTable;
using IToastNotificationManagerStatics = Object<ToastNotificationManagerStaticsTable>;
struct ToastNotificationManagerStaticsTable {
    InspectableSlots<IToastNotificationManagerStatics> base;
    void* CreateToastNotifier;
    HRESULT(STDMETHODCALLTYPE* CreateToastNotifierWithId)(IToastNotificationManagerStatics*, HSTRING, IToastNotifier**);
    void* GetTemplateContent;
};

// The interfaces' identities, as the Windows metadata gives them (written
// here, so that nothing is linked for them).
constexpr GUID iid_manager_statics = { 0x50ac103f, 0xd235, 0x4598, { 0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4 } };
constexpr GUID iid_toast_factory = { 0x04124b20, 0x82c6, 0x4229, { 0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53 } };
constexpr GUID iid_xml_document = { 0xf7f3a506, 0x1e87, 0x42d6, { 0xbc, 0xfb, 0xb8, 0xc8, 0x09, 0xfa, 0x54, 0x94 } };
constexpr GUID iid_xml_document_io = { 0x6cd0e74e, 0xee65, 0x4489, { 0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37 } };
constexpr GUID iid_activated_handler = { 0xab54de2d, 0x97d9, 0x5528, { 0xb6, 0xad, 0x10, 0x5a, 0xfe, 0x15, 0x65, 0x30 } };
constexpr GUID iid_dismissed_handler = { 0x61c2402f, 0x0ed0, 0x5a18, { 0xab, 0x69, 0x59, 0xf4, 0xaa, 0x99, 0xa3, 0x68 } };
constexpr GUID iid_failed_handler = { 0x95e3e803, 0xc969, 0x5e3a, { 0x97, 0x53, 0xea, 0x2a, 0xd2, 0x2a, 0x9a, 0x33 } };
constexpr GUID iid_agile_object = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };
constexpr GUID iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

constexpr wchar_t app_id[] = L"Sashfold.Browser";

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    int const length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// combase.dll's entry points, looked up rather than imported.
struct Runtime {
    HMODULE module = nullptr;
    HRESULT(WINAPI* initialize)(int) = nullptr; // RO_INIT_MULTITHREADED is 1
    void(WINAPI* uninitialize)() = nullptr;
    HRESULT(WINAPI* activation_factory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* activate_instance)(HSTRING, IInspectableObject**) = nullptr;
    HRESULT(WINAPI* create_string)(PCNZWCH, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* delete_string)(HSTRING) = nullptr;

    template <typename Function>
    bool find(Function& into, char const* name)
    {
        FARPROC const found = GetProcAddress(module, name);
        static_assert(sizeof found == sizeof into);
        std::memcpy(&into, &found, sizeof into);
        return found != nullptr;
    }

    bool load()
    {
        module = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module && find(initialize, "RoInitialize") && find(uninitialize, "RoUninitialize")
            && find(activation_factory, "RoGetActivationFactory") && find(activate_instance, "RoActivateInstance")
            && find(create_string, "WindowsCreateString") && find(delete_string, "WindowsDeleteString");
    }
};

// A runtime string, kept for as long as the call that takes it.
class Text {
public:
    Text(Runtime const& runtime, std::wstring_view text)
        : m_runtime(runtime)
    {
        if (FAILED(runtime.create_string(text.data(), static_cast<UINT32>(text.size()), &m_string)))
            m_string = nullptr;
    }
    ~Text()
    {
        if (m_string)
            m_runtime.delete_string(m_string);
    }
    Text(Text const&) = delete;
    Text& operator=(Text const&) = delete;
    HSTRING get() const { return m_string; }

private:
    Runtime const& m_runtime;
    HSTRING m_string = nullptr;
};

// A COM reference, released when it goes.
template <typename Interface>
class Ref {
public:
    Ref() = default;
    ~Ref() { reset(); }
    Ref(Ref const& other)
        : m_pointer(other.m_pointer)
    {
        if (m_pointer)
            m_pointer->lpVtbl->base.AddRef(m_pointer);
    }
    Ref& operator=(Ref other)
    {
        std::swap(m_pointer, other.m_pointer);
        return *this;
    }
    void reset()
    {
        if (m_pointer)
            m_pointer->lpVtbl->base.Release(m_pointer);
        m_pointer = nullptr;
    }
    Interface** put()
    {
        reset();
        return &m_pointer;
    }
    void** put_void() { return reinterpret_cast<void**>(put()); }
    Interface* operator->() const { return m_pointer; }
    Interface* get() const { return m_pointer; }
    explicit operator bool() const { return m_pointer != nullptr; }

private:
    Interface* m_pointer = nullptr;
};

// An event handler the toast calls on the system's threads: an object of
// our own behind the vtable every TypedEventHandler has (QueryInterface,
// AddRef, Release, Invoke(sender, args)), freed by its last Release.
struct HandlerObject;
struct HandlerTable {
    HRESULT(STDMETHODCALLTYPE* query_interface)(HandlerObject*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(HandlerObject*);
    ULONG(STDMETHODCALLTYPE* release)(HandlerObject*);
    HRESULT(STDMETHODCALLTYPE* invoke)(HandlerObject*, void* sender, void* args);
};
struct HandlerObject {
    HandlerTable const* table; // first: where COM looks for the vtable
    std::atomic<ULONG> references { 1 };
    GUID iid;
    std::function<void(void* args)> call;
};

HRESULT STDMETHODCALLTYPE handler_query_interface(HandlerObject* self, REFIID id, void** out)
{
    if (IsEqualGUID(id, iid_unknown) || IsEqualGUID(id, self->iid) || IsEqualGUID(id, iid_agile_object)) {
        *out = self;
        ++self->references;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE handler_add_ref(HandlerObject* self) { return ++self->references; }
ULONG STDMETHODCALLTYPE handler_release(HandlerObject* self)
{
    ULONG const left = --self->references;
    if (left == 0)
        delete self;
    return left;
}
HRESULT STDMETHODCALLTYPE handler_invoke(HandlerObject* self, void*, void* args)
{
    self->call(args);
    return S_OK;
}
constexpr HandlerTable handler_table = { handler_query_interface, handler_add_ref, handler_release, handler_invoke };

// A new handler, for the toast's add_ call.
void* make_handler(GUID const& iid, std::function<void(void* args)> call)
{
    return new HandlerObject { &handler_table, 1, iid, std::move(call) };
}

void release(void* object) { handler_release(static_cast<HandlerObject*>(object)); }

// The folder the program was started from, and its icon there, if the
// download put one beside it.
std::wstring shipped_icon()
{
    wchar_t path[MAX_PATH] {};
    DWORD const length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::error_code error;
    std::filesystem::path const folder = std::filesystem::path(path).parent_path();
    for (std::filesystem::path const& base : { folder, folder.parent_path() }) {
        std::filesystem::path const icon = base / "assets" / "icon.png";
        if (std::filesystem::exists(icon, error))
            return icon.wstring();
    }
    return {};
}

// The browser's name and icon for the toasts, for this user.
void register_identity()
{
    std::wstring const key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + app_id;
    HKEY opened = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &opened, nullptr) != ERROR_SUCCESS)
        return;
    auto const put = [opened](wchar_t const* name, std::wstring const& value) {
        RegSetValueExW(opened, name, 0, REG_SZ, reinterpret_cast<BYTE const*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    put(L"DisplayName", L"Sashfold");
    if (std::wstring const icon = shipped_icon(); !icon.empty())
        put(L"IconUri", icon);
    RegCloseKey(opened);
}

class ToastNotifier;

// Where a toast's handlers send what they hear: the notifier, until it
// goes. The handlers may be called on the system's threads after that (a
// toast hidden at the end says so a moment later), and then find no one.
struct Sink {
    std::mutex mutex;
    ToastNotifier* owner = nullptr; // under the mutex
};

class ToastNotifier final : public Notifier {
public:
    ToastNotifier(std::string application, std::function<void()> wake)
        : m_application(std::move(application))
        , m_wake(std::move(wake))
    {
        m_sink->owner = this;
        m_thread = std::thread([this] { run(); });
    }

    ~ToastNotifier() override
    {
        {
            std::lock_guard const lock(m_mutex);
            m_stop = true;
        }
        m_asked.notify_one();
        if (m_thread.joinable())
            m_thread.join();
        // A handler still running finishes first; any later finds no one.
        std::lock_guard const lock(m_sink->mutex);
        m_sink->owner = nullptr;
    }

    // From a toast's handler, through the sink.
    static void heard(std::shared_ptr<Sink> const& sink, Event::Kind kind, std::uint64_t id)
    {
        std::lock_guard const lock(sink->mutex);
        if (sink->owner)
            sink->owner->say(kind, id);
    }

    bool available() const override { return m_available; }

    std::uint64_t show(Notice const& notice, std::uint64_t replaces) override
    {
        std::uint64_t const id = m_next_id++;
        {
            std::lock_guard const lock(m_mutex);
            if (!m_available) {
                m_events.push_back(Event { Event::Kind::Failed, id, {} });
            } else {
                m_requests.push_back(Request { id, replaces, notice, false });
            }
        }
        if (m_available)
            m_asked.notify_one();
        else if (m_wake)
            m_wake();
        return id;
    }

    void close(std::uint64_t id) override
    {
        {
            std::lock_guard const lock(m_mutex);
            m_requests.push_back(Request { id, 0, {}, true });
        }
        m_asked.notify_one();
    }

    std::vector<Event> take_events() override
    {
        std::lock_guard const lock(m_mutex);
        return std::exchange(m_events, {});
    }

private:
    struct Request {
        std::uint64_t id = 0;
        std::uint64_t replaces = 0;
        Notice notice;
        bool close = false;
    };
    struct Shown {
        Ref<IToastNotification> toast;
        std::filesystem::path picture; // its file, deleted with it
    };

    void say(Event::Kind kind, std::uint64_t id)
    {
        {
            std::lock_guard const lock(m_mutex);
            m_events.push_back(Event { kind, id, {} });
            if (kind == Event::Kind::Closed || kind == Event::Kind::Failed)
                m_finished.push_back(id);
        }
        // The thread tidies what is gone; the window hears.
        m_asked.notify_one();
        if (m_wake)
            m_wake();
    }

    void gone()
    {
        std::deque<Request> left;
        {
            std::lock_guard const lock(m_mutex);
            m_available = false;
            left.swap(m_requests);
        }
        for (Request const& request : left) {
            if (!request.close)
                say(Event::Kind::Failed, request.id);
        }
        if (m_wake)
            m_wake();
    }

    // The page's picture as a file the toast can name.
    std::filesystem::path write_picture(std::uint64_t id, Bitmap const& picture)
    {
        wchar_t temporary[MAX_PATH + 1] {};
        DWORD const length = GetTempPathW(MAX_PATH + 1, temporary);
        if (length == 0 || length > MAX_PATH)
            return {};
        std::error_code error;
        std::filesystem::path const folder = std::filesystem::path(temporary) / "sashfold-notices";
        std::filesystem::create_directories(folder, error);
        std::filesystem::path const file = folder / (std::to_string(GetCurrentProcessId()) + "-" + std::to_string(id) + ".png");
        std::vector<std::uint8_t> const png = encode_png(picture);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<char const*>(png.data()), static_cast<std::streamsize>(png.size()));
        return out ? file : std::filesystem::path();
    }

    void forget(std::uint64_t id)
    {
        auto const found = m_shown.find(id);
        if (found == m_shown.end())
            return;
        std::error_code error;
        if (!found->second.picture.empty())
            std::filesystem::remove(found->second.picture, error);
        m_shown.erase(found);
    }

    bool put_up(Runtime const& runtime, IToastNotifier& notifier, IToastNotificationFactory& factory, Request const& request)
    {
        Shown shown;
        std::string picture_uri;
        if (request.notice.picture) {
            shown.picture = write_picture(request.id, *request.notice.picture);
            if (!shown.picture.empty()) {
                // In UTF-8 whatever the system's code page, a space written
                // as a URI writes it.
                std::u8string const path = shown.picture.generic_u8string();
                picture_uri = "file:///";
                for (char8_t const c : path) {
                    if (c == u8' ')
                        picture_uri += "%20";
                    else
                        picture_uri += static_cast<char>(c);
                }
            }
        }
        std::wstring const xml = wide(toast_xml(request.notice, picture_uri));
        Ref<IInspectableObject> made;
        Text const document_class(runtime, L"Windows.Data.Xml.Dom.XmlDocument");
        Ref<IXmlDocumentIO> loader;
        Ref<IXmlDocument> document;
        Text const source(runtime, xml);
        Ref<IToastNotification> toast;
        if (FAILED(runtime.activate_instance(document_class.get(), made.put()))
            || FAILED(made->lpVtbl->base.QueryInterface(made.get(), iid_xml_document_io, loader.put_void()))
            || FAILED(loader->lpVtbl->LoadXml(loader.get(), source.get()))
            || FAILED(made->lpVtbl->base.QueryInterface(made.get(), iid_xml_document, document.put_void()))
            || FAILED(factory.lpVtbl->CreateToastNotification(&factory, document.get(), toast.put()))) {
            std::error_code error;
            if (!shown.picture.empty())
                std::filesystem::remove(shown.picture, error);
            return false;
        }
        std::uint64_t const id = request.id;
        EventToken token {};
        std::shared_ptr<Sink> const sink = m_sink;
        void* const activated = make_handler(iid_activated_handler, [sink, id](void*) {
            heard(sink, Event::Kind::Clicked, id);
        });
        toast->lpVtbl->add_Activated(toast.get(), activated, &token);
        release(activated);
        void* const dismissed = make_handler(iid_dismissed_handler, [sink, id](void* args) {
            auto* const given = static_cast<IToastDismissedEventArgs*>(args);
            DismissalReason reason = dismissed_by_reader;
            if (given)
                given->lpVtbl->get_Reason(given, &reason);
            // Timed out, it waits in the notification centre and may still be
            // pressed: not gone.
            if (reason != timed_out)
                heard(sink, Event::Kind::Closed, id);
        });
        toast->lpVtbl->add_Dismissed(toast.get(), dismissed, &token);
        release(dismissed);
        void* const failed = make_handler(iid_failed_handler, [sink, id](void*) {
            heard(sink, Event::Kind::Failed, id);
        });
        toast->lpVtbl->add_Failed(toast.get(), failed, &token);
        release(failed);
        // One put in another's place: the other goes first.
        if (auto const before = m_shown.find(request.replaces); request.replaces != 0 && before != m_shown.end())
            notifier.lpVtbl->Hide(&notifier, before->second.toast.get());
        if (FAILED(notifier.lpVtbl->Show(&notifier, toast.get()))) {
            std::error_code error;
            if (!shown.picture.empty())
                std::filesystem::remove(shown.picture, error);
            return false;
        }
        shown.toast = toast;
        m_shown[id] = std::move(shown);
        return true;
    }

    void run()
    {
        Runtime runtime;
        if (!runtime.load() || FAILED(runtime.initialize(1))) {
            gone();
            return;
        }
        register_identity();
        SetCurrentProcessExplicitAppUserModelID(app_id);
        Ref<IToastNotificationManagerStatics> manager;
        Ref<IToastNotifier> notifier;
        Ref<IToastNotificationFactory> factory;
        {
            Text const manager_class(runtime, L"Windows.UI.Notifications.ToastNotificationManager");
            Text const toast_class(runtime, L"Windows.UI.Notifications.ToastNotification");
            Text const id(runtime, app_id);
            if (FAILED(runtime.activation_factory(manager_class.get(), iid_manager_statics, manager.put_void()))
                || FAILED(manager->lpVtbl->CreateToastNotifierWithId(manager.get(), id.get(), notifier.put()))
                || FAILED(runtime.activation_factory(toast_class.get(), iid_toast_factory, factory.put_void()))) {
                notifier.reset();
                manager.reset();
                gone();
                runtime.uninitialize();
                return;
            }
        }
        while (true) {
            std::deque<Request> requests;
            std::vector<std::uint64_t> finished;
            {
                std::unique_lock lock(m_mutex);
                m_asked.wait(lock, [this] { return m_stop || !m_requests.empty() || !m_finished.empty(); });
                if (m_stop)
                    break;
                requests.swap(m_requests);
                finished.swap(m_finished);
            }
            for (std::uint64_t const id : finished)
                forget(id);
            for (Request const& request : requests) {
                if (request.close) {
                    // Hidden by us: its Dismissed comes, and says it is closed.
                    if (auto const shown = m_shown.find(request.id); shown != m_shown.end())
                        notifier->lpVtbl->Hide(notifier.get(), shown->second.toast.get());
                    continue;
                }
                if (put_up(runtime, *notifier.get(), *factory.get(), request))
                    say(Event::Kind::Shown, request.id);
                else
                    say(Event::Kind::Failed, request.id);
            }
        }
        // A page's notices go with the browser: none is left in the
        // notification centre for a page that is no longer there.
        for (auto& [id, shown] : m_shown) {
            notifier->lpVtbl->Hide(notifier.get(), shown.toast.get());
            std::error_code error;
            if (!shown.picture.empty())
                std::filesystem::remove(shown.picture, error);
        }
        m_shown.clear();
        factory.reset();
        notifier.reset();
        manager.reset();
        runtime.uninitialize();
    }

    std::string m_application;
    std::function<void()> m_wake;
    std::shared_ptr<Sink> m_sink = std::make_shared<Sink>();
    std::atomic<bool> m_available { true }; // until it is known that there are no toasts
    std::atomic<std::uint64_t> m_next_id { 1 };
    std::mutex m_mutex;
    std::condition_variable m_asked;
    bool m_stop = false; // under the mutex
    std::deque<Request> m_requests; // under the mutex
    std::vector<std::uint64_t> m_finished; // under the mutex: gone, to be tidied
    std::vector<Event> m_events; // under the mutex
    std::map<std::uint64_t, Shown> m_shown; // the thread's alone
    std::thread m_thread;
};

}

std::unique_ptr<Notifier> open_notifier(std::string application, std::function<void()> wake)
{
    return std::make_unique<ToastNotifier>(std::move(application), std::move(wake));
}

}
