// A real TLS 1.3 handshake, driven end to end against the openssl command's
// own server on the loopback interface: the engine negotiates a suite,
// checks the server's signature over the transcript, finishes, and then
// exchanges application data over protected records. Both suites are driven,
// a server that shares none of ours must be refused, and a server that asks
// for another hello (RFC 8446 §4.1.4) must be answered. Nothing here reaches
// the network — the peer is a child process on 127.0.0.1 — and the whole
// live half SKIPs with a printed reason when openssl is not on PATH, when no
// certificate can be made, or when no loopback port can be bound.
//
// The hand-written cases come first and need no openssl at all: they drive
// the retry-request path with crafted records, including every way §4.1.4
// says a retry must be refused, which no cooperating server would produce.
#include "Test.h"

#include "net/tls/Tls13.h"
#include "platform/Net.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace sashfold;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t record_handshake = 22;
constexpr std::uint8_t record_alert = 21;
constexpr std::uint8_t record_change_cipher_spec = 20;
constexpr std::uint16_t suite_aes_128_gcm = 0x1301;
constexpr std::uint16_t suite_chacha20 = 0x1303;
constexpr std::uint16_t group_x25519 = 0x001d;
constexpr std::uint16_t group_secp256r1 = 0x0017;

std::string hex(std::span<std::uint8_t const> bytes)
{
    std::string out;
    char buffer[3];
    for (std::uint8_t const b : bytes) {
        std::snprintf(buffer, sizeof buffer, "%02x", b);
        out += buffer;
    }
    return out;
}

void put16(Bytes& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append(Bytes& out, Bytes const& more)
{
    out.insert(out.end(), more.begin(), more.end());
}

Bytes extension(std::uint16_t type, Bytes const& data)
{
    Bytes out;
    put16(out, type);
    put16(out, static_cast<std::uint16_t>(data.size()));
    append(out, data);
    return out;
}

Bytes supported_versions()
{
    Bytes body;
    put16(body, 0x0304);
    return extension(43, body);
}

Bytes key_share(std::uint16_t group)
{
    Bytes body;
    put16(body, group);
    return extension(51, body);
}

Bytes cookie_value(std::string const& text)
{
    Bytes body;
    put16(body, static_cast<std::uint16_t>(text.size()));
    body.insert(body.end(), text.begin(), text.end());
    return body;
}

// §4.1.3: a ServerHello whose random is this value is a retry request.
Bytes retry_request(Bytes const& session_id, std::uint16_t suite, Bytes const& extensions)
{
    static constexpr std::uint8_t retry_random[32] = {
        0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c, 0x02, 0x1e, 0x65, 0xb8, 0x91,
        0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb, 0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c,
    };
    Bytes body;
    put16(body, 0x0303);
    body.insert(body.end(), std::begin(retry_random), std::end(retry_random));
    body.push_back(static_cast<std::uint8_t>(session_id.size()));
    append(body, session_id);
    put16(body, suite);
    body.push_back(0);
    put16(body, static_cast<std::uint16_t>(extensions.size()));
    append(body, extensions);

    Bytes message;
    message.push_back(2); // server_hello
    message.push_back(0);
    put16(message, static_cast<std::uint16_t>(body.size()));
    append(message, body);

    Bytes record;
    record.push_back(record_handshake);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(message.size()));
    append(record, message);
    return record;
}

// A plain ServerHello that is not a retry, for the rules that apply to the
// hello after one.
Bytes server_hello(Bytes const& session_id, std::uint16_t suite, Bytes const& extensions)
{
    Bytes body;
    put16(body, 0x0303);
    for (std::size_t i = 0; i < 32; ++i)
        body.push_back(static_cast<std::uint8_t>(0x30 + i));
    body.push_back(static_cast<std::uint8_t>(session_id.size()));
    append(body, session_id);
    put16(body, suite);
    body.push_back(0);
    put16(body, static_cast<std::uint16_t>(extensions.size()));
    append(body, extensions);

    Bytes message;
    message.push_back(2);
    message.push_back(0);
    put16(message, static_cast<std::uint16_t>(body.size()));
    append(message, body);

    Bytes record;
    record.push_back(record_handshake);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(message.size()));
    append(record, message);
    return record;
}

// A 1.2 ServerHello: no supported_versions among its extensions, and, when
// asked, the downgrade sentinel of RFC 8446 §4.1.3 in the last eight bytes
// of its random.
Bytes server_hello_12(Bytes const& session_id, std::uint16_t suite, Bytes const& extensions, bool downgrade_sentinel)
{
    static constexpr std::uint8_t sentinel[8] = { 0x44, 0x4f, 0x57, 0x4e, 0x47, 0x52, 0x44, 0x01 };
    Bytes body;
    put16(body, 0x0303);
    for (std::size_t i = 0; i < 32; ++i)
        body.push_back(downgrade_sentinel && i >= 24 ? sentinel[i - 24] : static_cast<std::uint8_t>(0x30 + i));
    body.push_back(static_cast<std::uint8_t>(session_id.size()));
    append(body, session_id);
    put16(body, suite);
    body.push_back(0);
    put16(body, static_cast<std::uint16_t>(extensions.size()));
    append(body, extensions);

    Bytes message;
    message.push_back(2);
    message.push_back(0);
    put16(message, static_cast<std::uint16_t>(body.size()));
    append(message, body);

    Bytes record;
    record.push_back(record_handshake);
    put16(record, 0x0303);
    put16(record, static_cast<std::uint16_t>(message.size()));
    append(record, message);
    return record;
}

struct Hello {
    Bytes random;
    Bytes session_id;
    std::vector<std::uint16_t> suites;
    std::vector<std::pair<std::uint16_t, Bytes>> extensions;

    bool has(std::uint16_t type) const
    {
        for (auto const& entry : extensions)
            if (entry.first == type)
                return true;
        return false;
    }

    Bytes data(std::uint16_t type) const
    {
        for (auto const& entry : extensions)
            if (entry.first == type)
                return entry.second;
        return {};
    }
};

// The body of the first ClientHello handshake message in a flight of records.
Bytes client_hello_body(Bytes const& records)
{
    std::size_t offset = 0;
    while (offset + 5 <= records.size()) {
        std::size_t const length = (static_cast<std::size_t>(records[offset + 3]) << 8) | records[offset + 4];
        if (offset + 5 + length > records.size())
            break;
        if (records[offset] == record_handshake && length >= 4 && records[offset + 5] == 1) {
            std::size_t const body_length = (static_cast<std::size_t>(records[offset + 6]) << 16)
                | (static_cast<std::size_t>(records[offset + 7]) << 8) | records[offset + 8];
            if (9 + body_length <= 5 + length)
                return Bytes(records.begin() + static_cast<std::ptrdiff_t>(offset + 9),
                    records.begin() + static_cast<std::ptrdiff_t>(offset + 9 + body_length));
            break;
        }
        offset += 5 + length;
    }
    return {};
}

bool parse_hello(Bytes const& body, Hello& hello)
{
    std::size_t offset = 0;
    auto need = [&](std::size_t count) { return offset + count <= body.size(); };
    if (!need(2 + 32 + 1))
        return false;
    offset += 2;
    hello.random.assign(body.begin() + 2, body.begin() + 34);
    offset = 34;
    std::size_t const session_length = body[offset++];
    if (!need(session_length))
        return false;
    hello.session_id.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
        body.begin() + static_cast<std::ptrdiff_t>(offset + session_length));
    offset += session_length;
    if (!need(2))
        return false;
    std::size_t const suites_length = (static_cast<std::size_t>(body[offset]) << 8) | body[offset + 1];
    offset += 2;
    if (!need(suites_length) || suites_length % 2 != 0)
        return false;
    for (std::size_t i = 0; i < suites_length; i += 2)
        hello.suites.push_back(static_cast<std::uint16_t>((body[offset + i] << 8) | body[offset + i + 1]));
    offset += suites_length;
    if (!need(1))
        return false;
    std::size_t const compression_length = body[offset++];
    if (!need(compression_length))
        return false;
    offset += compression_length;
    if (!need(2))
        return false;
    std::size_t const extensions_length = (static_cast<std::size_t>(body[offset]) << 8) | body[offset + 1];
    offset += 2;
    if (!need(extensions_length))
        return false;
    std::size_t const end = offset + extensions_length;
    while (offset + 4 <= end) {
        std::uint16_t const type = static_cast<std::uint16_t>((body[offset] << 8) | body[offset + 1]);
        std::size_t const length = (static_cast<std::size_t>(body[offset + 2]) << 8) | body[offset + 3];
        offset += 4;
        if (offset + length > end)
            return false;
        hello.extensions.emplace_back(type,
            Bytes(body.begin() + static_cast<std::ptrdiff_t>(offset),
                body.begin() + static_cast<std::ptrdiff_t>(offset + length)));
        offset += length;
    }
    return offset == end;
}

// The description of the last alert in a flight, or −1 when there is none.
int alert_description(Bytes const& records)
{
    int description = -1;
    std::size_t offset = 0;
    while (offset + 5 <= records.size()) {
        std::size_t const length = (static_cast<std::size_t>(records[offset + 3]) << 8) | records[offset + 4];
        if (offset + 5 + length > records.size())
            break;
        if (records[offset] == record_alert && length == 2)
            description = records[offset + 6];
        offset += 5 + length;
    }
    return description;
}

tls::TlsConfig test_config()
{
    tls::TlsConfig config;
    config.server_name = "localhost";
    for (std::size_t i = 0; i < config.private_key.size(); ++i)
        config.private_key[i] = static_cast<std::uint8_t>(0x40 + i);
    for (std::size_t i = 0; i < config.p256_private_key.size(); ++i)
        config.p256_private_key[i] = static_cast<std::uint8_t>(0x60 + i);
    for (std::size_t i = 0; i < config.client_random.size(); ++i)
        config.client_random[i] = static_cast<std::uint8_t>(0x10 + i);
    for (std::size_t i = 0; i < config.session_id.size(); ++i)
        config.session_id[i] = static_cast<std::uint8_t>(0x80 + i);
    // The chain is the validator's business and has its own tests; what is
    // under test here is the handshake.
    config.verify_chain = [](std::vector<tls::Certificate> const&, std::string&) { return true; };
    return config;
}

Bytes session_id_of(tls::TlsConfig const& config)
{
    return Bytes(config.session_id.begin(), config.session_id.end());
}

// ---- the retry request, against crafted records

void test_retry_request()
{
    tls::TlsConfig const config = test_config();
    Bytes const session_id = session_id_of(config);

    // A cookie is a change the client can make, so this retry is legal. The
    // second hello repeats the first exactly and adds the cookie.
    {
        tls::TlsEngine engine(config);
        Hello first;
        CHECK(parse_hello(client_hello_body(engine.start()), first));
        CHECK(!first.has(44));
        CHECK(first.has(51)); // a key share went out, so only a cookie can change the hello

        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("a-server-cookie")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        CHECK_EQ(engine.hello_retry_requests(), 1);
        CHECK(engine.state() == tls::TlsState::WaitServerHello);
        CHECK_EQ(engine.error(), std::string());
        // §D.4: the dummy record leads the second flight.
        CHECK(!out.to_send.empty() && out.to_send[0] == record_change_cipher_spec);

        Hello second;
        CHECK(parse_hello(client_hello_body(out.to_send), second));
        CHECK_EQ(hex(second.random), hex(first.random));
        CHECK_EQ(hex(second.session_id), hex(first.session_id));
        CHECK(second.suites == first.suites);
        CHECK(second.has(44));
        CHECK_EQ(hex(second.data(44)), hex(cookie_value("a-server-cookie")));
        CHECK_EQ(hex(second.data(51)), hex(first.data(51))); // the same share, as §4.1.4 requires
    }

    // A hello that carries no key share at all asks the server to name the
    // group (§4.2.8); the second hello then carries the share it asked for.
    {
        tls::TlsConfig empty = test_config();
        empty.empty_key_share = true;
        tls::TlsEngine engine(empty);
        Hello first;
        CHECK(parse_hello(client_hello_body(engine.start()), first));
        CHECK(first.has(51));
        CHECK_EQ(hex(first.data(51)), std::string("0000")); // an empty list of shares

        Bytes extensions = supported_versions();
        append(extensions, key_share(group_x25519));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        CHECK_EQ(engine.hello_retry_requests(), 1);
        Hello second;
        CHECK(parse_hello(client_hello_body(out.to_send), second));
        CHECK_EQ(second.data(51).size(), std::size_t(2 + 2 + 2 + 32)); // one x25519 share
        CHECK(!second.has(44)); // no cookie was sent, so none comes back
    }

    // Every retry §4.1.4 forbids, with the alert it names.
    auto refusal = [&](Bytes const& extensions, std::uint16_t suite) {
        tls::TlsEngine engine(config);
        engine.start();
        tls::TlsOutput out;
        bool const ok = engine.feed(retry_request(session_id, suite, extensions), out);
        CHECK(!ok);
        CHECK(engine.state() == tls::TlsState::Failed);
        CHECK(!engine.error().empty());
        return alert_description(out.to_send);
    };
    // Nothing would change: no group, no cookie.
    CHECK_EQ(refusal(supported_versions(), suite_chacha20), 47);
    // The group this hello already carried a share for.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share(group_x25519));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    // A group that was never offered: secp384r1, which no hello of ours lists.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share(0x0018));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    // The second group offered, secp256r1, asked for by a retry: the next
    // hello carries a P-256 share in its place, 65 bytes, uncompressed.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, key_share(group_secp256r1));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        CHECK_EQ(engine.hello_retry_requests(), 1);
        Hello second;
        CHECK(parse_hello(client_hello_body(out.to_send), second));
        CHECK_EQ(second.data(51).size(), std::size_t(2 + 2 + 2 + 65));
        CHECK_EQ(hex(second.data(51)).substr(0, 14), std::string("00450017004104"));
    }
    // No version at all; an extension this client knows but that has no
    // place in a retry (renegotiation_info, offered for 1.2); and one it
    // never offered at all (status_request) — §4.2 names a different alert
    // for each of the last two.
    CHECK_EQ(refusal(extension(44, cookie_value("no-version")), suite_chacha20), 109);
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(0xff01, Bytes { 0x00 }));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(5, Bytes {}));
        CHECK_EQ(refusal(extensions, suite_chacha20), 110);
    }
    // The same extension twice in one block.
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("twice")));
        append(extensions, extension(44, cookie_value("twice")));
        CHECK_EQ(refusal(extensions, suite_chacha20), 47);
    }
    // A suite that was never offered (0x1302 is AES-256-GCM, which is not ours).
    {
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("wrong-suite")));
        CHECK_EQ(refusal(extensions, 0x1302), 47);
    }

    // A second retry is fatal, and it is unexpected_message that says so.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("first")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        tls::TlsOutput second;
        CHECK(!engine.feed(retry_request(session_id, suite_chacha20, extensions), second));
        CHECK_EQ(alert_description(second.to_send), 10);
        CHECK(engine.state() == tls::TlsState::Failed);
    }

    // The hello after a retry may not change the suite the retry named.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, extension(44, cookie_value("first")));
        tls::TlsOutput out;
        CHECK(engine.feed(retry_request(session_id, suite_chacha20, extensions), out));
        Bytes hello_extensions = supported_versions();
        append(hello_extensions, key_share(group_x25519));
        tls::TlsOutput second;
        CHECK(!engine.feed(server_hello(session_id, suite_aes_128_gcm, hello_extensions), second));
        CHECK_EQ(alert_description(second.to_send), 47);
    }
}

// ---- the extension rules of §4.2, against crafted hellos

// A ServerHello's key share: one entry, the group and its key.
Bytes key_share_entry(std::uint16_t group, Bytes const& key)
{
    Bytes body;
    put16(body, group);
    put16(body, static_cast<std::uint16_t>(key.size()));
    append(body, key);
    return extension(51, body);
}

void test_extension_rules()
{
    tls::TlsConfig const config = test_config();
    Bytes const session_id = session_id_of(config);
    Bytes const x25519_key(32, 0x09); // the base point: a valid share
    auto plain_refusal = [&](tls::TlsConfig const& with, Bytes const& extensions, std::string& error) {
        tls::TlsEngine engine(with);
        engine.start();
        tls::TlsOutput out;
        bool const ok = engine.feed(server_hello(session_id, suite_chacha20, extensions), out);
        CHECK(!ok);
        CHECK(engine.state() == tls::TlsState::Failed);
        error = engine.error();
        return alert_description(out.to_send);
    };
    std::string error;

    // §4.2.8: a hello that carried an empty key-share list asked the server
    // to name a group, which only a retry request can do; a plain hello
    // answering it with a share is refused, before its point is looked at.
    {
        tls::TlsConfig empty = config;
        empty.empty_key_share = true;
        Bytes extensions = supported_versions();
        append(extensions, key_share_entry(group_x25519, x25519_key));
        CHECK_EQ(plain_refusal(empty, extensions, error), 47);
        CHECK(error.find("retry") != std::string::npos);
    }
    // The same answer to a hello that did carry a share goes through.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = supported_versions();
        append(extensions, key_share_entry(group_x25519, x25519_key));
        tls::TlsOutput out;
        CHECK(engine.feed(server_hello(session_id, suite_chacha20, extensions), out));
        CHECK(engine.state() == tls::TlsState::WaitEncryptedExtensions);
    }
    // §4.2: an extension twice in a ServerHello.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share_entry(group_x25519, x25519_key));
        append(extensions, supported_versions());
        CHECK_EQ(plain_refusal(config, extensions, error), 47);
        CHECK(error.find("twice") != std::string::npos);
    }
    // §4.2: server_name was offered, but a 1.3 ServerHello is not where its
    // acknowledgement belongs (EncryptedExtensions is): illegal_parameter.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share_entry(group_x25519, x25519_key));
        append(extensions, extension(0, Bytes {}));
        CHECK_EQ(plain_refusal(config, extensions, error), 47);
        CHECK(error.find("no place") != std::string::npos);
    }
    // §4.2: an extension never offered (status_request) in a ServerHello.
    {
        Bytes extensions = supported_versions();
        append(extensions, key_share_entry(group_x25519, x25519_key));
        append(extensions, extension(5, Bytes {}));
        CHECK_EQ(plain_refusal(config, extensions, error), 110);
        CHECK(error.find("not offered") != std::string::npos);
    }
    // The 1.2 answer under the same rules: renegotiation_info twice, and a
    // server_name acknowledgement when no name was sent (an IP literal).
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = extension(0xff01, Bytes { 0x00 });
        append(extensions, extension(0xff01, Bytes { 0x00 }));
        tls::TlsOutput out;
        CHECK(!engine.feed(server_hello_12(session_id, 0xc02f, extensions, false), out));
        CHECK_EQ(alert_description(out.to_send), 47);
        CHECK(engine.error().find("twice") != std::string::npos);
    }
    {
        tls::TlsConfig literal = config;
        literal.server_name.clear();
        tls::TlsEngine engine(literal);
        engine.start();
        Bytes extensions = extension(0xff01, Bytes { 0x00 });
        append(extensions, extension(0, Bytes {}));
        tls::TlsOutput out;
        CHECK(!engine.feed(server_hello_12(session_id, 0xc02f, extensions, false), out));
        CHECK_EQ(alert_description(out.to_send), 47);
        CHECK(engine.error().find("no place") != std::string::npos);
    }
    // And with the name sent, the acknowledgement is taken.
    {
        tls::TlsEngine engine(config);
        engine.start();
        Bytes extensions = extension(0xff01, Bytes { 0x00 });
        append(extensions, extension(0, Bytes {}));
        tls::TlsOutput out;
        CHECK(engine.feed(server_hello_12(session_id, 0xc02f, extensions, false), out));
        CHECK(engine.state() == tls::TlsState::WaitCertificate);
    }
}

// ---- the choice of version, against crafted answers

void test_version_choice()
{
    tls::TlsConfig const config = test_config();
    Bytes const session_id = session_id_of(config);
    Bytes const renegotiation = extension(0xff01, Bytes { 0x00 });
    // The hello offers both versions: its suites are the 1.3 ones and then
    // the 1.2 ones, and its extensions carry the 1.2 extras.
    {
        tls::TlsEngine engine(config);
        Hello hello;
        CHECK(parse_hello(client_hello_body(engine.start()), hello));
        CHECK_EQ(hello.suites.size(), std::size_t(6));
        CHECK_EQ(hex(hello.data(43)), std::string("0403040303"));
        CHECK(hello.has(11) && hello.has(23) && hello.has(0xff01));
        CHECK_EQ(hex(hello.data(10)), std::string("0004001d0017"));
    }
    // A 1.2 answer — no supported_versions — is taken as TLS 1.2.
    {
        tls::TlsEngine engine(config);
        engine.start();
        tls::TlsOutput out;
        CHECK(engine.feed(server_hello_12(session_id, 0xc02f, renegotiation, false), out));
        CHECK_EQ(engine.version(), std::uint16_t(0x0303));
        CHECK(engine.state() == tls::TlsState::WaitCertificate);
        CHECK(out.to_send.empty());
    }
    // The same answer whose random carries the downgrade sentinel: a server
    // that could have spoken 1.3 chose not to, which is refused (§4.1.3).
    {
        tls::TlsEngine engine(config);
        engine.start();
        tls::TlsOutput out;
        CHECK(!engine.feed(server_hello_12(session_id, 0xc02f, renegotiation, true), out));
        CHECK_EQ(alert_description(out.to_send), 47);
        CHECK(engine.error().find("downgrade") != std::string::npos);
    }
    // A 1.2 answer naming a 1.3 suite is malformed.
    {
        tls::TlsEngine engine(config);
        engine.start();
        tls::TlsOutput out;
        CHECK(!engine.feed(server_hello_12(session_id, 0x1303, renegotiation, false), out));
        CHECK_EQ(alert_description(out.to_send), 47);
    }
    // A hello for 1.3 alone carries no 1.2 extras, and a 1.2 answer to it is
    // refused as the wrong version.
    {
        tls::TlsConfig only13 = config;
        only13.offer_tls12 = false;
        tls::TlsEngine engine(only13);
        Hello hello;
        CHECK(parse_hello(client_hello_body(engine.start()), hello));
        CHECK_EQ(hello.suites.size(), std::size_t(2));
        CHECK_EQ(hex(hello.data(43)), std::string("020304"));
        CHECK(!hello.has(11) && !hello.has(23) && !hello.has(0xff01));
        tls::TlsOutput out;
        CHECK(!engine.feed(server_hello_12(session_id, 0x1303, {}, false), out));
        CHECK_EQ(alert_description(out.to_send), 70);
    }
    // A hello for 1.2 alone carries no key share and no 1.3 modes.
    {
        tls::TlsConfig only12 = config;
        only12.offer_tls13 = false;
        tls::TlsEngine engine(only12);
        Hello hello;
        CHECK(parse_hello(client_hello_body(engine.start()), hello));
        CHECK_EQ(hello.suites.size(), std::size_t(4));
        CHECK_EQ(hex(hello.data(43)), std::string("020303"));
        CHECK(!hello.has(51) && !hello.has(45));
        CHECK(hello.has(11) && hello.has(23));
    }
}

// ---- a whole handshake against openssl s_server

#ifndef _WIN32

std::string g_openssl;
std::string g_directory;
std::string g_certificate;
std::string g_key;
std::string g_ec_certificate;
std::string g_ec_key;
std::string g_setup_log;

// The name openssl prints for a suite on its -www page: the IANA name for
// 1.3, its own for 1.2.
std::string openssl_name(tls::CipherSuite suite)
{
    switch (suite) {
    case tls::CipherSuite::EcdheEcdsaAes128GcmSha256:
        return "ECDHE-ECDSA-AES128-GCM-SHA256";
    case tls::CipherSuite::EcdheRsaAes128GcmSha256:
        return "ECDHE-RSA-AES128-GCM-SHA256";
    case tls::CipherSuite::EcdheRsaChaCha20Poly1305Sha256:
        return "ECDHE-RSA-CHACHA20-POLY1305";
    case tls::CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256:
        return "ECDHE-ECDSA-CHACHA20-POLY1305";
    default:
        return tls::cipher_suite_name(suite);
    }
}

void on_alarm(int)
{
    char const message[] = "FAIL test_tls_handshake: a wait passed its deadline\n";
    ssize_t const written = ::write(2, message, sizeof message - 1);
    static_cast<void>(written);
    ::_exit(2);
}

std::string read_file(std::string const& path)
{
    std::string out;
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
        return out;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0)
        out.append(buffer, read);
    std::fclose(file);
    return out;
}

pid_t spawn(std::vector<std::string> const& args, std::string const& log)
{
    pid_t const child = ::fork();
    if (child != 0)
        return child;
    int const handle = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (handle >= 0) {
        ::dup2(handle, 1);
        ::dup2(handle, 2);
        ::close(handle);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string const& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    ::_exit(127);
}

// Waits for a child, killing it if it outstays the deadline; true when it
// exited cleanly of its own accord.
bool reap(pid_t child, int seconds, bool ask_first)
{
    if (child <= 0)
        return false;
    if (ask_first)
        ::kill(child, SIGTERM);
    for (int waited = 0; waited < seconds * 20; ++waited) {
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        ::usleep(50 * 1000);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    ::waitpid(child, &status, 0);
    return false;
}

bool run(std::vector<std::string> const& args, int seconds)
{
    return reap(spawn(args, g_setup_log), seconds, false);
}

// A loopback port, bound to learn its number and then released for the
// child. Losing that race is a SKIP, never a failure.
bool free_port(std::uint16_t& port)
{
    std::optional<platform::TcpListener> listener = platform::TcpListener::listen_loopback();
    if (!listener)
        return false;
    port = listener->port();
    return true;
}

std::optional<platform::TcpSocket> connect_loopback(std::uint16_t port, int seconds, int& attempts)
{
    for (attempts = 1; attempts <= seconds * 20; ++attempts) {
        std::optional<platform::TcpSocket> socket = platform::TcpSocket::connect("127.0.0.1", port);
        if (socket)
            return socket;
        ::usleep(50 * 1000);
    }
    return std::nullopt;
}

struct Exchange {
    bool connected = false;
    // The plumbing got as far as the peer: the socket connected and our first
    // flight went out. Without this, a refusal case cannot tell a client that
    // refused from a server that never came up.
    bool reached_peer = false;
    // The engine ended the handshake, rather than the socket or the child
    // process ending it for us.
    bool engine_failed = false;
    int retries = 0;
    std::uint16_t version = 0;
    tls::CipherSuite suite = tls::CipherSuite::ChaCha20Poly1305Sha256;
    std::string response;
    std::string error;
};

// The handshake and one request over it, on a socket of our own.
Exchange exchange(tls::TlsConfig config, std::uint16_t port, std::string const& request)
{
    Exchange result;
    int attempts = 0;
    std::optional<platform::TcpSocket> socket = connect_loopback(port, 10, attempts);
    if (!socket) {
        result.error = "no loopback connect in " + std::to_string(attempts) + " attempts";
        return result;
    }
    tls::TlsEngine engine(std::move(config));
    std::vector<std::uint8_t> const hello = engine.start();
    if (!socket->send_all(hello.data(), hello.size())) {
        result.error = "the first flight did not go out";
        return result;
    }
    result.reached_peer = true;
    std::uint8_t buffer[16384];
    while (!engine.connected() && engine.state() != tls::TlsState::Failed) {
        std::ptrdiff_t const received = socket->receive(buffer, sizeof buffer);
        if (received <= 0) {
            result.error = "the server went away during the handshake";
            break;
        }
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.to_send.empty() && !socket->send_all(out.to_send.data(), out.to_send.size())) {
            result.error = "a flight did not go out";
            break;
        }
        if (!ok)
            break;
    }
    result.retries = engine.hello_retry_requests();
    result.version = engine.version();
    result.suite = engine.cipher_suite();
    result.engine_failed = engine.state() == tls::TlsState::Failed;
    if (!engine.connected()) {
        if (!engine.error().empty())
            result.error = engine.error();
        else if (result.error.empty())
            result.error = "the handshake did not finish";
        return result;
    }
    result.connected = true;
    std::vector<std::uint8_t> const sealed = engine.seal(
        std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(request.data()), request.size()));
    if (!socket->send_all(sealed.data(), sealed.size())) {
        result.error = "the request did not go out";
        return result;
    }
    for (;;) {
        std::ptrdiff_t const received = socket->receive(buffer, sizeof buffer);
        if (received <= 0)
            break;
        tls::TlsOutput out;
        bool const ok = engine.feed(std::span<std::uint8_t const>(buffer, static_cast<std::size_t>(received)), out);
        if (!out.plaintext.empty())
            result.response.append(reinterpret_cast<char const*>(out.plaintext.data()), out.plaintext.size());
        if (!ok || engine.state() != tls::TlsState::Connected)
            break;
        if (result.response.find("</HTML>") != std::string::npos)
            break;
    }
    std::vector<std::uint8_t> const alert = engine.close_notify();
    if (!alert.empty())
        socket->send_all(alert.data(), alert.size());
    return result;
}

// One case: a server with the given options (its versions among them),
// our client with the given configuration, and what both ends say about
// what happened — the version and suite the server believes it spoke are
// read off its own page, not our bookkeeping.
void live_case(std::string const& name, std::vector<std::string> const& server_options, tls::TlsConfig config,
    bool expect_connected, tls::CipherSuite expected_suite, int expected_retries,
    char const* expected_refusal = nullptr, std::uint16_t expected_version = 0x0304, bool ecdsa_server = false)
{
    std::uint16_t port = 0;
    if (!free_port(port)) {
        std::printf("SKIP %s: no loopback port could be bound\n", name.c_str());
        return;
    }
    std::string const log = g_directory + "/server-" + std::to_string(port) + ".log";
    std::vector<std::string> args = { g_openssl, "s_server", "-accept", "127.0.0.1:" + std::to_string(port),
        "-cert", ecdsa_server ? g_ec_certificate : g_certificate, "-key", ecdsa_server ? g_ec_key : g_key, "-naccept", "1", "-www" };
    args.insert(args.end(), server_options.begin(), server_options.end());
    pid_t const server = spawn(args, log);
    if (server < 0) {
        std::printf("SKIP %s: the server process could not be started\n", name.c_str());
        return;
    }

    ::alarm(60);
    Exchange const result = exchange(std::move(config), port, "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
    // -naccept 1 means the server leaves of its own accord once it has served
    // us; it is killed only if it overstays that.
    reap(server, 5, false);
    ::alarm(0);

    std::string const server_said = read_file(log);
    if (!expect_connected) {
        CHECK(!result.connected);
        // A refusal counts only if OUR client made it: the socket reached the
        // server, our hello went out, and the engine is what stopped. Asserting
        // a non-empty error alone passes on any infrastructure failure — both
        // "no loopback connect in N attempts" and "the server went away during
        // the handshake" satisfy it — so the case would go green on a machine
        // where openssl never started, which is the opposite of a test.
        CHECK(result.reached_peer);
        CHECK(result.engine_failed);
        CHECK(expected_refusal != nullptr);
        bool const named = expected_refusal != nullptr && result.error.find(expected_refusal) != std::string::npos;
        CHECK(named);
        if (!result.reached_peer || !result.engine_failed || !named)
            std::printf("  %s: refused, but not for the reason this case names: reached_peer=%d engine_failed=%d"
                        " error=\"%s\" wanted=\"%s\"\n",
                name.c_str(), result.reached_peer ? 1 : 0, result.engine_failed ? 1 : 0, result.error.c_str(),
                expected_refusal != nullptr ? expected_refusal : "(none given)");
        if (result.connected)
            std::printf("  %s: expected no handshake, got one\n", name.c_str());
        std::remove(log.c_str());
        return;
    }
    if (!result.connected) {
        sashfold::test::fail(name + ": the handshake failed: " + result.error, __FILE__, __LINE__);
        std::printf("  server said: %s\n", server_said.c_str());
        std::remove(log.c_str());
        return;
    }
    CHECK(result.connected);
    CHECK_EQ(result.retries, expected_retries);
    CHECK_EQ(result.version, expected_version);
    CHECK_EQ(std::string(tls::cipher_suite_name(result.suite)), std::string(tls::cipher_suite_name(expected_suite)));
    // Application data went both ways over protected records.
    CHECK(result.response.rfind("HTTP/1.0 200", 0) == 0);
    CHECK(result.response.find("</HTML>") != std::string::npos);
    // The page the server serves names the version and the suite it believes
    // it negotiated, which is not our own bookkeeping: they have to be the
    // ones we think we protected the records with, and not another suite.
    std::string const version_name = expected_version == 0x0304 ? "TLSv1.3" : "TLSv1.2";
    CHECK(result.response.find("New, " + version_name + ", Cipher is " + openssl_name(expected_suite)) != std::string::npos);
    if (result.response.find("New, " + version_name + ", Cipher is " + openssl_name(expected_suite)) == std::string::npos)
        std::printf("  %s: the server's page does not name %s with %s\n", name.c_str(), version_name.c_str(), openssl_name(expected_suite).c_str());
    std::string const other = expected_suite == tls::CipherSuite::Aes128GcmSha256 || expected_suite == tls::CipherSuite::EcdheRsaAes128GcmSha256
            || expected_suite == tls::CipherSuite::EcdheEcdsaAes128GcmSha256
        ? "CHACHA20"
        : "AES";
    CHECK(result.response.find("Cipher is " + other) == std::string::npos && result.response.find("Cipher is ECDHE-RSA-" + other) == std::string::npos
        && result.response.find("Cipher is ECDHE-ECDSA-" + other) == std::string::npos && result.response.find("Cipher is TLS_" + other) == std::string::npos);
    std::remove(log.c_str());
}

bool live_setup()
{
    char const* const base = std::getenv("TMPDIR");
    std::string pattern = (base != nullptr && base[0] != '\0' ? std::string(base) : std::string("/tmp"))
        + "/sashfold-tls-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
        std::printf("SKIP test_tls_handshake: no temporary directory could be made\n");
        return false;
    }
    g_directory = buffer.data();
    g_certificate = g_directory + "/server.pem";
    g_key = g_directory + "/server.key";
    g_ec_certificate = g_directory + "/server-ec.pem";
    g_ec_key = g_directory + "/server-ec.key";
    g_setup_log = g_directory + "/setup.log";
    // Throwaway keys and self-signed certificates for the child — one RSA,
    // one P-256 for the suites that name ECDSA — made here so that no
    // private key is ever committed. The chain is not what this test
    // judges; the signature over the handshake is.
    if (!run({ g_openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=localhost",
                 "-keyout", g_key, "-out", g_certificate },
            90)
        || !run({ g_openssl, "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "2",
                    "-subj", "/CN=localhost", "-keyout", g_ec_key, "-out", g_ec_certificate },
            90)) {
        std::printf("SKIP test_tls_handshake: openssl could not make a certificate:\n%s\n",
            read_file(g_setup_log).c_str());
        return false;
    }
    return true;
}

void live_teardown()
{
    // By name, one at a time.
    std::remove(g_certificate.c_str());
    std::remove(g_key.c_str());
    std::remove(g_ec_certificate.c_str());
    std::remove(g_ec_key.c_str());
    std::remove(g_setup_log.c_str());
    ::rmdir(g_directory.c_str());
}

void test_live_handshake()
{
    if (!live_setup())
        return;
    // AES-128-GCM in 1.3: the server will speak nothing else.
    live_case("aes-128-gcm", { "-tls1_3", "-ciphersuites", "TLS_AES_128_GCM_SHA256" }, test_config(), true,
        tls::CipherSuite::Aes128GcmSha256, 0);
    // ChaCha20-Poly1305, which the client has always had.
    live_case("chacha20-poly1305", { "-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256" }, test_config(), true,
        tls::CipherSuite::ChaCha20Poly1305Sha256, 0);
    // The negotiation is real: a client that offers only AES cannot talk to a
    // server that offers only ChaCha.
    {
        tls::TlsConfig only_aes = test_config();
        only_aes.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        // The server has nothing we offered, so it is the peer that refuses,
        // with an alert our engine reports: that string, not merely "something
        // went wrong", is what this case asserts.
        live_case("no-shared-suite", { "-tls1_3", "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256" }, only_aes, false,
            tls::CipherSuite::Aes128GcmSha256, 0, "fatal alert");
    }
    // A client that offers only AES and a server that speaks it: the suite is
    // forced from our side, not chosen by the peer.
    {
        tls::TlsConfig only_aes = test_config();
        only_aes.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        live_case("aes-only-client", { "-tls1_3" }, only_aes, true, tls::CipherSuite::Aes128GcmSha256, 0);
    }
    // A hello with no key share: the server answers with a retry request, and
    // the handshake finishes through it — transcript substitution and all.
    {
        tls::TlsConfig retry = test_config();
        retry.empty_key_share = true;
        retry.cipher_suites = { tls::CipherSuite::Aes128GcmSha256 };
        live_case("retry-then-aes", { "-tls1_3" }, retry, true, tls::CipherSuite::Aes128GcmSha256, 1);
    }
    {
        tls::TlsConfig retry = test_config();
        retry.empty_key_share = true;
        retry.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
        live_case("retry-then-chacha", { "-tls1_3" }, retry, true, tls::CipherSuite::ChaCha20Poly1305Sha256, 1);
    }
    // A 1.3 server that will only exchange keys over P-256: the retry names
    // the group, and the second hello carries the P-256 share.
    {
        tls::TlsConfig retry = test_config();
        retry.cipher_suites = { tls::CipherSuite::ChaCha20Poly1305Sha256 };
        live_case("retry-to-p256", { "-tls1_3", "-groups", "P-256" }, retry, true, tls::CipherSuite::ChaCha20Poly1305Sha256, 1);
    }

    // ---- TLS 1.2, against a server that speaks nothing newer.
    // ECDHE-RSA with AES-128-GCM: the RSA signature over the key exchange,
    // the extended master secret the server offers, and the 1.2 record
    // layer with its explicit nonces.
    live_case("tls12-ecdhe-rsa-aes128gcm", { "-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256" }, test_config(), true,
        tls::CipherSuite::EcdheRsaAes128GcmSha256, 0, nullptr, 0x0303);
    // ChaCha20-Poly1305 in 1.2: the same AEAD, the 1.3-shaped nonce.
    live_case("tls12-ecdhe-rsa-chacha20", { "-tls1_2", "-cipher", "ECDHE-RSA-CHACHA20-POLY1305" }, test_config(), true,
        tls::CipherSuite::EcdheRsaChaCha20Poly1305Sha256, 0, nullptr, 0x0303);
    // An ECDSA server: the suites that name it, and its signature.
    live_case("tls12-ecdhe-ecdsa-aes128gcm", { "-tls1_2", "-cipher", "ECDHE-ECDSA-AES128-GCM-SHA256" }, test_config(), true,
        tls::CipherSuite::EcdheEcdsaAes128GcmSha256, 0, nullptr, 0x0303, true);
    live_case("tls12-ecdhe-ecdsa-chacha20", { "-tls1_2", "-cipher", "ECDHE-ECDSA-CHACHA20-POLY1305" }, test_config(), true,
        tls::CipherSuite::EcdheEcdsaChaCha20Poly1305Sha256, 0, nullptr, 0x0303, true);
    // The key exchange over each group the hello offers, the server allowing
    // one at a time.
    live_case("tls12-x25519", { "-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "X25519" }, test_config(), true,
        tls::CipherSuite::EcdheRsaAes128GcmSha256, 0, nullptr, 0x0303);
    live_case("tls12-p256", { "-tls1_2", "-cipher", "ECDHE-RSA-AES128-GCM-SHA256", "-groups", "P-256" }, test_config(), true,
        tls::CipherSuite::EcdheRsaAes128GcmSha256, 0, nullptr, 0x0303);
    // A server that speaks both takes 1.3 from a hello that offers both, and
    // the 1.2 extras in that hello change nothing.
    live_case("both-offered-server-takes-1.3", { "-ciphersuites", "TLS_CHACHA20_POLY1305_SHA256" }, test_config(), true,
        tls::CipherSuite::ChaCha20Poly1305Sha256, 0, nullptr, 0x0304);
    // A hello for 1.2 alone against that server: 1.2, from its 1.2 list.
    {
        tls::TlsConfig only12 = test_config();
        only12.offer_tls13 = false;
        live_case("client-1.2-only", { "-cipher", "ECDHE-RSA-AES128-GCM-SHA256" }, only12, true,
            tls::CipherSuite::EcdheRsaAes128GcmSha256, 0, nullptr, 0x0303);
    }
    // ... and against a server that speaks 1.3 alone, the server refuses
    // with an alert the engine reports.
    {
        tls::TlsConfig only12 = test_config();
        only12.offer_tls13 = false;
        live_case("client-1.2-only-server-1.3-only", { "-tls1_3" }, only12, false,
            tls::CipherSuite::EcdheRsaAes128GcmSha256, 0, "fatal alert", 0x0303);
    }
    // A hello for 1.3 alone against a server that speaks 1.2 alone: the
    // server finds no version in common and says so.
    {
        tls::TlsConfig only13 = test_config();
        only13.offer_tls12 = false;
        live_case("client-1.3-only-server-1.2-only", { "-tls1_2" }, only13, false,
            tls::CipherSuite::ChaCha20Poly1305Sha256, 0, "fatal alert", 0x0304);
    }
    live_teardown();
}

#endif

}

int main(int argc, char** argv)
{
    test_retry_request();
    test_extension_rules();
    test_version_choice();
#ifndef _WIN32
    ::signal(SIGALRM, on_alarm);
    ::signal(SIGPIPE, SIG_IGN); // a peer that leaves early is a failed check, not a dead test
    if (argc < 2 || argv[1][0] == '\0' || ::access(argv[1], X_OK) != 0) {
        std::printf("SKIP test_tls_handshake live handshake: openssl is not on PATH\n");
        return sashfold::test::report("tls handshake");
    }
    g_openssl = argv[1];
    test_live_handshake();
#else
    static_cast<void>(argc);
    static_cast<void>(argv);
    std::printf("SKIP test_tls_handshake live handshake: no posix process control\n");
#endif
    return sashfold::test::report("tls handshake");
}
