#pragma once

// D-Bus (https://dbus.freedesktop.org/doc/dbus-specification.html): the
// wire format of a message with every type it has, the handshake a bus asks
// for, a connection over the bus's socket, and the service side — objects
// exported at paths, their method calls dispatched and answered, the
// standard interfaces (Properties, Introspectable, Peer), signals emitted
// and match rules. Written here, as the Wayland and the sound server's
// protocols are, so that the browser links nothing for it.
//
// What is not here: file descriptors are passed (SCM_RIGHTS) but a received
// one is not taken from the socket; a message in big-endian order is passed
// over (every bus on this machine writes in its own order).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::platform::dbus {

// --- Names and signatures (§ "Valid Signatures", "Valid Names") ---------------

// A signature: a sequence of complete types, at most 255 bytes, arrays
// and structs each nested at most 32 deep, a dict entry only as an array's
// element and keyed by a basic type.
bool valid_signature(std::string_view signature);
// One complete type and nothing else (a variant's signature).
bool single_complete_type(std::string_view signature);
// The length of the complete type that starts at `at` in a signature that
// is valid; 0 when none starts there.
std::size_t complete_type_length(std::string_view signature, std::size_t at);
// The alignment a value of the type whose code starts a signature has.
std::size_t alignment_of(char code);
bool valid_object_path(std::string_view path);
bool valid_interface_name(std::string_view name);
bool valid_member_name(std::string_view name);
bool valid_bus_name(std::string_view name);
// UTF-8 as the specification wants a string's bytes: well formed, no
// surrogate, no NUL.
bool valid_utf8(std::string_view text);

// A value of any D-Bus type: what a signature describes, as a tree.
struct Value {
    // The value's single complete type ("u", "as", "a{sv}", "(so)", "v").
    std::string type;
    // A number of any width, its bits (a signed one sign-extended); a
    // boolean is 0 or 1.
    std::uint64_t number = 0;
    double real = 0;
    std::string text; // a string, an object path, a signature
    // An array's elements; a struct's fields; a dict entry's key then value;
    // a variant's one value.
    std::vector<Value> items;

    static Value byte(std::uint8_t);
    static Value boolean(bool);
    static Value int16(std::int16_t);
    static Value uint16(std::uint16_t);
    static Value int32(std::int32_t);
    static Value uint32(std::uint32_t);
    static Value int64(std::int64_t);
    static Value uint64(std::uint64_t);
    static Value float64(double);
    static Value string(std::string);
    static Value object_path(std::string);
    static Value signature(std::string);
    static Value variant(Value inner);
    // An array of values of `element_type` (which an empty one needs).
    static Value array(std::string element_type, std::vector<Value> elements);
    static Value structure(std::vector<Value> fields);
    static Value dict_entry(Value key, Value value);

    std::int64_t as_int() const { return static_cast<std::int64_t>(number); }
    // A dict (`a{..}`): the entry whose key is this string; null when none.
    Value const* find(std::string_view key) const;
};

// A message body, or part of one, being written: each value at the
// alignment its type asks for, counted from the start of the body.
class Writer {
public:
    void byte(std::uint8_t value);
    void boolean(bool value);
    void int16(std::int16_t value);
    void uint16(std::uint16_t value);
    void int32(std::int32_t value);
    void uint32(std::uint32_t value);
    void int64(std::int64_t value);
    void uint64(std::uint64_t value);
    void float64(double value);
    void string(std::string_view text); // also an object path
    void object_path(std::string_view path) { string(path); }
    void signature(std::string_view text);
    // Any value, as its type says; false (and nothing coherent written)
    // when the value does not match its own type.
    bool value(Value const& value);
    // A variant holding `value`: its signature, then it.
    bool variant(Value const& value);
    // A struct of an object reference (`(so)`): a bus name and a path.
    void reference(std::string_view bus_name, std::string_view path);
    // An array of bytes (`ay`), copied whole.
    void byte_array(std::span<std::uint8_t const> values);
    // An array: `fill` writes its elements, each aligned to
    // `element_alignment` (1 byte, 4 a number or a string, 8 a struct or a
    // dictionary entry).
    template <typename Fill>
    void array(std::size_t element_alignment, Fill&& fill)
    {
        std::size_t const length_at = begin_array(element_alignment);
        fill();
        end_array(length_at, element_alignment);
    }
    // The start of a struct or a dictionary entry.
    void align(std::size_t alignment);
    // A variant holding one value of a basic type.
    void variant_string(std::string_view text);
    void variant_boolean(bool value);
    void variant_byte(std::uint8_t value);
    void variant_uint32(std::uint32_t value);

    std::vector<std::uint8_t> const& bytes() const { return m_bytes; }
    std::vector<std::uint8_t> take() { return std::move(m_bytes); }

private:
    std::size_t begin_array(std::size_t element_alignment);
    void end_array(std::size_t length_at, std::size_t element_alignment);
    std::vector<std::uint8_t> m_bytes;
};

// A message body being read. A value that is not there — the body ends, a
// length runs past it — makes every later read answer nothing.
class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> bytes)
        : m_bytes(bytes)
    {
    }
    std::optional<std::uint8_t> byte();
    std::optional<bool> boolean(); // 0 or 1; anything else is a failure
    std::optional<std::int16_t> int16();
    std::optional<std::uint16_t> uint16();
    std::optional<std::int32_t> int32();
    std::optional<std::uint32_t> uint32();
    std::optional<std::int64_t> int64();
    std::optional<std::uint64_t> uint64();
    std::optional<double> float64();
    std::optional<std::string> string(); // valid UTF-8 with no NUL inside
    std::optional<std::string> object_path();
    std::optional<std::string> signature();
    std::optional<std::vector<std::string>> string_array();
    // One value of a single complete type, read whole and checked: a
    // container nested deeper than the specification allows (32 arrays, 32
    // structs, 64 in all with the variants), an array longer than 2^26
    // bytes or whose elements do not fill it exactly, a variant whose
    // signature is not one complete type, all fail.
    std::optional<Value> value(std::string_view type);
    // Every value of a signature, in order.
    std::optional<std::vector<Value>> values(std::string_view signature);
    bool align(std::size_t alignment);
    bool skip(std::size_t count);
    std::size_t position() const { return m_at; }
    bool failed() const { return m_failed; }
    bool at_end() const { return m_at >= m_bytes.size(); }

private:
    std::optional<Value> value_at(std::string_view type, int arrays, int structs, int depth);
    template <typename T>
    std::optional<T> fixed(std::size_t size);
    std::span<std::uint8_t const> m_bytes;
    std::size_t m_at = 0;
    bool m_failed = false;
};

enum class MessageType : std::uint8_t {
    MethodCall = 1,
    MethodReturn = 2,
    Error = 3,
    Signal = 4,
};

struct Message {
    MessageType type = MessageType::MethodCall;
    bool no_reply_expected = false;
    std::uint32_t serial = 0;
    std::uint32_t reply_serial = 0; // of a return or an error: the call it answers
    std::string path;
    std::string interface;
    std::string member;
    std::string error_name;
    std::string destination;
    std::string sender;
    std::string signature; // of the body
    std::vector<std::uint8_t> body;
    // File descriptors passed with the message (SCM_RIGHTS): a body value
    // of type 'h' is a uint32 index into these. The caller keeps them open
    // until the message is sent.
    std::vector<int> fds;
};

// The message as it goes on the wire, little-endian.
std::vector<std::uint8_t> encode(Message const& message);

// The first message in `bytes` and how many bytes it took: nothing with
// `used` 0 when the bytes do not yet hold a whole message, nothing with
// `used` set past it when it is whole and cannot be read (big-endian, a
// header that does not parse) — the caller drops those bytes and goes on.
// `malformed` is set when what is there cannot be the start of a message
// at all, and the connection is not worth keeping.
std::optional<Message> decode(std::span<std::uint8_t const> bytes, std::size_t& used, bool& malformed);

// Where the session's bus listens: the socket's path, or its name in the
// abstract namespace (`abstract` true), from an address as
// DBUS_SESSION_BUS_ADDRESS gives it ("unix:path=/run/user/1000/bus", several
// separated by ';' — the first that names a unix socket is taken).
struct SocketAddress {
    std::string path;
    bool abstract = false;
};
std::optional<SocketAddress> parse_address(std::string_view address);
// The session bus's address: the environment's, else the socket the user's
// runtime folder holds; nothing when neither is there.
std::optional<SocketAddress> session_bus_address();

// A connection to a bus, authenticated as this user and said hello to.
// One thread reads it; send() may be called from any.
class Connection {
public:
    // Nothing, with the reason, when the socket cannot be reached, the bus
    // refuses this user or the hello is not answered within a few seconds.
    static std::unique_ptr<Connection> open(SocketAddress const& address, std::string* why_not = nullptr);
    ~Connection();
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    // Sends the message under a serial of its own, which is answered: the
    // reply's reply_serial. 0 when the socket is gone.
    std::uint32_t send(Message message);
    // The next message, waiting up to `timeout_ms` for one (negative: for
    // as long as it takes; `also_fd` readable ends the wait too, with
    // nothing). Nothing also when the bus has gone — closed() says which.
    std::optional<Message> receive(int timeout_ms, int also_fd = -1);
    bool closed() const { return m_closed; }
    // The name the bus gave this connection (":1.42").
    std::string const& unique_name() const { return m_unique_name; }
    // Whether the bus agreed to carry file descriptors (NEGOTIATE_UNIX_FD).
    bool passes_fds() const { return m_passes_fds; }

private:
    Connection() = default;
    int m_fd = -1;
    bool m_passes_fds = false;
    std::atomic<bool> m_closed { false };
    std::uint32_t m_next_serial = 1;
    std::string m_unique_name;
    std::vector<std::uint8_t> m_pending; // read and not yet a whole message
    struct Lock;
    std::unique_ptr<Lock> m_send_lock;
};

// --- Being a service --------------------------------------------------------------

// A method call's answer: a return with its body, or an error by name.
struct Reply {
    std::string signature;
    Writer body;
    std::string error; // set: the reply is this error, with `error_text`
    std::string error_text;
    static Reply failure(std::string name, std::string text);
};

// The errors the specification names for a call that cannot be answered.
inline constexpr std::string_view error_unknown_method = "org.freedesktop.DBus.Error.UnknownMethod";
inline constexpr std::string_view error_unknown_object = "org.freedesktop.DBus.Error.UnknownObject";
inline constexpr std::string_view error_unknown_interface = "org.freedesktop.DBus.Error.UnknownInterface";
inline constexpr std::string_view error_unknown_property = "org.freedesktop.DBus.Error.UnknownProperty";
inline constexpr std::string_view error_property_read_only = "org.freedesktop.DBus.Error.PropertyReadOnly";
inline constexpr std::string_view error_invalid_args = "org.freedesktop.DBus.Error.InvalidArgs";
inline constexpr std::string_view error_failed = "org.freedesktop.DBus.Error.Failed";

// An interface an object offers: its methods (one function answers them
// all, by member), its properties, and the XML Introspect gives for it.
struct Interface {
    std::string name;
    // The interface's <interface> element, as Introspect lists it.
    std::string introspection;
    struct Property {
        std::string name;
        std::string signature;
        bool writable = false;
    };
    std::vector<Property> properties;
    // A method call to this interface at `call.path`: its reply, or nothing
    // when the interface has no such method (UnknownMethod is answered).
    std::function<std::optional<Reply>(Message const& call)> call;
    // A property's value at `path`, of the property's own signature;
    // nothing when the object has it not.
    std::function<std::optional<Value>(std::string const& path, std::string_view property)> get;
    // Setting a writable property: an error reply, or nothing on success.
    std::function<std::optional<Reply>(std::string const& path, std::string_view property, Value const& value)> set;
};

// Which interfaces an object path has: nothing when there is no object.
using ObjectResolver = std::function<std::optional<std::vector<std::shared_ptr<Interface const>>>(std::string_view path)>;

// A service on a connection: its objects, what answers their calls, its
// signals, its names and match rules, and calls of its own that wait for
// their reply while the calls that come meanwhile are kept for later. Used
// from the one thread that reads the connection.
class Service {
public:
    explicit Service(Connection& connection);

    // An object at this exact path; and a subtree of objects under a
    // prefix ("/org/a11y/atspi/accessible/" answers for every path below
    // it), whose objects the resolver says. An exact path wins.
    void add_object(std::string path, std::vector<std::shared_ptr<Interface const>> interfaces);
    void add_subtree(std::string prefix, ObjectResolver resolver);
    void remove_object(std::string const& path);

    // Answers a method call — the object's own interfaces, Properties
    // (Get, GetAll, Set), Introspectable and Peer (Ping, GetMachineId) — or
    // the error the specification gives. False when `message` was no call.
    bool dispatch(Message const& message);

    // A call of this service's, and its reply (a return or an error), waiting
    // at most `timeout_ms`; what arrives meanwhile is kept for next().
    std::optional<Message> call(Message message, int timeout_ms);
    // The next message: one kept by call() first, else the connection's.
    std::optional<Message> next(int timeout_ms, int also_fd = -1);
    bool has_kept() const { return !m_kept.empty(); }

    // A signal from `path` (`destination` empty: broadcast).
    std::uint32_t emit(std::string path, std::string interface, std::string member, std::string signature,
        Writer body, std::string destination = {});
    // The bus's own methods: a well-known name (true when this connection
    // is its primary owner), and a match rule for the signals to hear.
    bool request_name(std::string const& name, std::uint32_t flags = 4, int timeout_ms = 2000);
    bool add_match(std::string const& rule, int timeout_ms = 2000);
    bool remove_match(std::string const& rule, int timeout_ms = 2000);

    Connection& connection() { return *m_connection; }

    // A message calling `member` of `interface` at `destination`'s `path`.
    static Message method_call(std::string destination, std::string path, std::string interface, std::string member,
        std::string signature = {}, Writer body = {});

private:
    std::optional<std::vector<std::shared_ptr<Interface const>>> interfaces_at(std::string_view path) const;
    std::string introspect(std::string const& path) const;
    void answer(Message const& call, Reply reply);
    Connection* m_connection;
    std::map<std::string, std::vector<std::shared_ptr<Interface const>>, std::less<>> m_objects;
    std::vector<std::pair<std::string, ObjectResolver>> m_subtrees;
    std::deque<Message> m_kept;
};

}
