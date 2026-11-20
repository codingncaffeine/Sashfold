#include "bindings/Internal.h"

// navigator.credentials (Credential Management Level 1) with the one
// credential type every browser ships, the public-key credential of Web
// Authentication Level 3 (https://www.w3.org/TR/webauthn-3/).
//
// create() and get() convert and check the page's options as §5.1.3 and
// §5.1.4 give them — the RP ID against the caller's origin, the challenge
// and user handle, the algorithms, the authenticator selection, the
// extensions — build the client data and its hash (auth/ClientData.h), and
// hand the ceremony to the host (HostHooks::security_key_ceremony), which
// finds the keys and asks the reader through its prompt, off the page's
// thread. What comes back is carried to the page's thread through the
// agent's remote tasks and becomes a PublicKeyCredential or the
// DOMException the specification names. The page's AbortSignal cancels the
// ceremony; so does the document unloading. The request's lifetime timer
// is a timer of the page's own (§5.1.3 step 22): when it fires the
// ceremony is cancelled and the page told NotAllowedError, and a request
// no key can serve — a platform authenticator asked for where there is
// none — waits for it, so that a page learns nothing of the reader's keys
// before the timeout or the reader's dismissal (§14.5.1).
//
// A frame of another origin may call get() when its container allows
// publickey-credentials-get, and create() when it allows
// publickey-credentials-create and the reader has just acted on the page
// (§5.9, §5.10, Permissions Policy's allow attribute; the features'
// default allowlist is 'self'). Requests go through the page's host, one
// at a time per document.
//
// Not here: conditional mediation (the autofill UI), related origin
// requests (§5.11), a credential store of the browser's own (the signal
// methods resolve with nothing to update), largeBlob reads and writes, and
// the platform authenticators Windows and macOS offer.

#include "auth/Client.h"
#include "auth/ClientData.h"
#include "crypto/Sha2.h"
#include "js/Runtime.h"
#include "media/ClearKey.h"
#include "net/Url.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sashfold::bindings {

using Bytes = std::vector<std::uint8_t>;

// What a call to create() or get() waits with: how to settle its promise,
// the signal that may abort it, and what the credential it makes is built
// from besides the authenticator's answer.
struct Realm::Internals::CredentialCall {
    std::uint64_t ceremony = 0; // the host's name for the ceremony; 0 for none
    js::Value resolve;
    js::Value reject;
    js::Value signal; // the AbortSignal given, or undefined
    auth::Operation operation = auth::Operation::Get;
    std::string client_data_json;
    // The extensions the page asked for, which shape the client outputs.
    auth::ExtensionInputs extensions;
    bool large_blob = false; // largeBlob was given
    bool large_blob_read = false;
    bool large_blob_write = false;
    bool get_cred_blob = false;
    // The realm's hooks that run the ceremony: the page's, for a frame too.
    Realm::Internals* host = nullptr;
};

namespace {

// ---------- Platform objects ---------------------------------------------------------------

// A PublicKeyCredential (§5.1): what the page gets from a ceremony.
class PublicKeyCredentialObject final : public js::Object {
public:
    PublicKeyCredentialObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype)
        , m_realm(realm)
    {
    }
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(raw_id_buffer);
        tracer.visit(response);
    }

    std::string id; // base64url of the raw ID
    Bytes raw_id;
    js::Value raw_id_buffer; // [SameObject]
    js::Value response; // the AuthenticatorResponse
    std::string attachment; // "cross-platform"; empty for null
    auth::Operation operation = auth::Operation::Get;
    auth::ExtensionOutputs outputs;
    auth::ExtensionInputs asked;
    bool large_blob = false;
    bool large_blob_read = false;
    bool large_blob_write = false;
    bool get_cred_blob = false;

private:
    js::RealmRecord* m_realm;
};

// An AuthenticatorAttestationResponse or AuthenticatorAssertionResponse
// (§5.2.1, §5.2.2): the bytes as the authenticator and the client gave
// them, and the ArrayBuffers handed out for them, the same each time.
class AuthenticatorResponseObject final : public js::Object {
public:
    AuthenticatorResponseObject(js::Object* prototype, js::RealmRecord* realm)
        : Object(prototype)
        , m_realm(realm)
    {
    }
    js::RealmRecord* home_realm() const override { return m_realm; }
    void trace(js::Tracer& tracer) override
    {
        Object::trace(tracer);
        tracer.visit(client_data_buffer);
        tracer.visit(attestation_buffer);
        tracer.visit(authenticator_data_buffer);
        tracer.visit(signature_buffer);
        tracer.visit(user_handle_buffer);
    }

    bool attestation = false;
    Bytes client_data_json;
    Bytes attestation_object; // attestation
    Bytes authenticator_data;
    Bytes signature; // assertion
    std::optional<Bytes> user_handle; // assertion
    std::vector<std::string> transports; // attestation
    std::optional<Bytes> public_key; // attestation: SubjectPublicKeyInfo, when expressible
    std::int64_t public_key_algorithm = 0;

    js::Value client_data_buffer;
    js::Value attestation_buffer;
    js::Value authenticator_data_buffer;
    js::Value signature_buffer;
    js::Value user_handle_buffer;

private:
    js::RealmRecord* m_realm;
};

template <class T>
T* object_of(js::Value const& value)
{
    return value.is_object() ? dynamic_cast<T*>(value.as_object()) : nullptr;
}

std::optional<PublicKeyCredentialObject*> this_credential(js::Interpreter& interp, js::Value const& this_value)
{
    if (auto* found = object_of<PublicKeyCredentialObject>(this_value))
        return found;
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

std::optional<AuthenticatorResponseObject*> this_response(js::Interpreter& interp, js::Value const& this_value, std::optional<bool> attestation)
{
    if (auto* found = object_of<AuthenticatorResponseObject>(this_value); found && (!attestation || found->attestation == *attestation))
        return found;
    interp.throw_type_error("Illegal invocation");
    return std::nullopt;
}

Native array_buffer(js::Interpreter& interp, std::span<std::uint8_t const> bytes)
{
    std::optional<js::ArrayBufferObject*> const buffer = js::allocate_array_buffer(interp, nullptr, static_cast<double>(bytes.size()), std::nullopt);
    if (!buffer)
        return std::nullopt;
    if (!bytes.empty())
        std::memcpy((*buffer)->data(), bytes.data(), bytes.size());
    return js::Value::object(*buffer);
}

// The buffer kept in `slot`, made the first time it is asked for.
Native same_buffer(js::Interpreter& interp, js::Value& slot, std::span<std::uint8_t const> bytes)
{
    if (slot.is_object())
        return slot;
    Native const made = array_buffer(interp, bytes);
    if (made)
        slot = *made;
    return made;
}

// ---------- Converting the page's dictionaries (WebIDL §3.2.17) ----------------------------

// Every exception in a promise-returning operation's steps is a rejection
// (WebIDL §3.7.7); a terminated script stays terminated.
Native rejecting(js::Interpreter& interp, Native const& result)
{
    if (result || !interp.has_exception() || interp.terminated())
        return result;
    return rejected_promise(interp, interp.take_exception());
}

Native reject_dom(Realm::Internals& in, std::string_view name, std::string_view message)
{
    return rejected_promise(in.interpreter, dom_exception_value(in, name, message));
}

// A dictionary member's value, undefined when it has none; nothing when
// reading it threw. A value that is neither undefined, null nor an object
// is no dictionary: a TypeError naming `what`.
struct Dictionary {
    js::Interpreter& interp;
    js::Value value;
    std::string what;

    static std::optional<Dictionary> of(js::Interpreter& interp, js::Value const& value, std::string what)
    {
        if (!value.is_nullish() && !value.is_object()) {
            interp.throw_type_error("The provided value is not of type '" + what + "'.");
            return std::nullopt;
        }
        return Dictionary { interp, value, std::move(what) };
    }

    std::optional<js::Value> get(std::string_view name) const
    {
        if (!value.is_object())
            return js::Value::undefined();
        std::optional<js::Value> got = interp.get(value, name);
        if (got)
            interp.root(*got);
        return got;
    }

    // A required member: a TypeError when it is undefined.
    std::optional<js::Value> required(std::string_view name) const
    {
        std::optional<js::Value> got = get(name);
        if (got && got->is_undefined()) {
            interp.throw_type_error("Failed to read the '" + std::string(name) + "' property from '" + what + "': Required member is undefined.");
            return std::nullopt;
        }
        return got;
    }
};

std::optional<std::string> dom_string(Realm::Internals& in, js::Value const& value) { return in.to_utf8(value); }

// A BufferSource member, copied: a TypeError for anything else.
std::optional<Bytes> buffer_source(js::Interpreter& interp, js::Value const& value, std::string_view what)
{
    std::optional<std::span<std::uint8_t const>> const bytes = buffer_source_bytes(value);
    if (!bytes) {
        interp.throw_type_error("The provided value for '" + std::string(what) + "' is not of type '(ArrayBuffer or ArrayBufferView)'.");
        return std::nullopt;
    }
    return Bytes(bytes->begin(), bytes->end());
}

// sequence<T> (WebIDL §3.2.18): an iterable object's values, each rooted.
std::optional<std::vector<js::Value>> sequence(js::Interpreter& interp, js::Value const& value, std::string_view what)
{
    if (!value.is_object()) {
        interp.throw_type_error("The provided value for '" + std::string(what) + "' cannot be converted to a sequence.");
        return std::nullopt;
    }
    std::optional<std::vector<js::Value>> list = interp.iterable_to_list(value);
    if (list) {
        for (js::Value const& one : *list)
            interp.root(one);
    }
    return list;
}

std::optional<std::vector<std::string>> string_sequence(Realm::Internals& in, js::Value const& value, std::string_view what)
{
    std::optional<std::vector<js::Value>> const list = sequence(in.interpreter, value, what);
    if (!list)
        return std::nullopt;
    std::vector<std::string> out;
    for (js::Value const& one : *list) {
        std::optional<std::string> text = dom_string(in, one);
        if (!text)
            return std::nullopt;
        out.push_back(std::move(*text));
    }
    return out;
}

std::optional<bool> boolean_member(Dictionary const& dictionary, std::string_view name, bool fallback)
{
    std::optional<js::Value> const value = dictionary.get(name);
    if (!value)
        return std::nullopt;
    return value->is_undefined() ? fallback : js::Interpreter::to_boolean(*value);
}

// A DOMString member, nullopt-in-optional when it is absent.
std::optional<std::optional<std::string>> string_member(Realm::Internals& in, Dictionary const& dictionary, std::string_view name)
{
    std::optional<js::Value> const value = dictionary.get(name);
    if (!value)
        return std::nullopt;
    if (value->is_undefined())
        return std::optional<std::string> {};
    std::optional<std::string> text = dom_string(in, *value);
    if (!text)
        return std::nullopt;
    return std::optional<std::string> { std::move(*text) };
}

// PublicKeyCredentialDescriptor (§5.10.3): type, id, transports. A
// descriptor of a type this client does not know is left out, as §5.1.3
// step 20 and §5.1.4 leave it out.
std::optional<std::optional<auth::CredentialDescriptor>> descriptor(Realm::Internals& in, js::Value const& value)
{
    std::optional<Dictionary> const dictionary = Dictionary::of(in.interpreter, value, "PublicKeyCredentialDescriptor");
    if (!dictionary)
        return std::nullopt;
    std::optional<js::Value> const id_value = dictionary->required("id");
    if (!id_value)
        return std::nullopt;
    std::optional<Bytes> id = buffer_source(in.interpreter, *id_value, "id");
    if (!id)
        return std::nullopt;
    std::optional<js::Value> const transports_value = dictionary->get("transports");
    if (!transports_value)
        return std::nullopt;
    std::vector<std::string> transports;
    if (!transports_value->is_undefined()) {
        std::optional<std::vector<std::string>> list = string_sequence(in, *transports_value, "transports");
        if (!list)
            return std::nullopt;
        transports = std::move(*list);
    }
    std::optional<js::Value> const type_value = dictionary->required("type");
    if (!type_value)
        return std::nullopt;
    std::optional<std::string> const type = dom_string(in, *type_value);
    if (!type)
        return std::nullopt;
    if (*type != "public-key")
        return std::optional<auth::CredentialDescriptor> {};
    return std::optional<auth::CredentialDescriptor> { auth::CredentialDescriptor { std::move(*id), std::move(transports) } };
}

std::optional<std::vector<auth::CredentialDescriptor>> descriptor_list(Realm::Internals& in, Dictionary const& dictionary, std::string_view name)
{
    std::optional<js::Value> const value = dictionary.get(name);
    if (!value)
        return std::nullopt;
    std::vector<auth::CredentialDescriptor> out;
    if (value->is_undefined())
        return out;
    std::optional<std::vector<js::Value>> const list = sequence(in.interpreter, *value, name);
    if (!list)
        return std::nullopt;
    for (js::Value const& one : *list) {
        std::optional<std::optional<auth::CredentialDescriptor>> made = descriptor(in, one);
        if (!made)
            return std::nullopt;
        if (*made)
            out.push_back(std::move(**made));
    }
    return out;
}

// AuthenticationExtensionsPRFValues (§10.1.4): first, and second if given.
std::optional<auth::PrfInputs> prf_values(js::Interpreter& interp, js::Value const& value)
{
    std::optional<Dictionary> const dictionary = Dictionary::of(interp, value, "AuthenticationExtensionsPRFValues");
    if (!dictionary)
        return std::nullopt;
    std::optional<js::Value> const first = dictionary->required("first");
    if (!first)
        return std::nullopt;
    auth::PrfInputs out;
    std::optional<Bytes> first_bytes = buffer_source(interp, *first, "first");
    if (!first_bytes)
        return std::nullopt;
    out.first = std::move(*first_bytes);
    std::optional<js::Value> const second = dictionary->get("second");
    if (!second)
        return std::nullopt;
    if (!second->is_undefined()) {
        std::optional<Bytes> bytes = buffer_source(interp, *second, "second");
        if (!bytes)
            return std::nullopt;
        out.second = std::move(*bytes);
    }
    return out;
}

// A refusal the client extension processing of §9 and §10 makes: a
// DOMException, not a TypeError.
struct Refusal {
    std::string name;
    std::string message;
};

// The client extension inputs both ceremonies share the reading of
// (§5.7.1, §9.3, §10). `refusal` is set, and true returned, for a
// DOMException; false with an exception pending for a conversion error.
bool read_extensions(Realm::Internals& in, js::Value const& value, auth::Operation operation, std::vector<auth::CredentialDescriptor> const& allow,
    Realm::Internals::CredentialCall& call, std::optional<Refusal>& refusal)
{
    js::Interpreter& interp = in.interpreter;
    std::optional<Dictionary> const dictionary = Dictionary::of(interp, value, "AuthenticationExtensionsClientInputs");
    if (!dictionary)
        return false;
    auth::ExtensionInputs& ext = call.extensions;
    bool const create = operation == auth::Operation::Create;
    // appid (§10.1.1, get) and appidExclude (§10.1.2, create): the AppID is
    // checked against the caller once the RP ID is known; kept here.
    if (!create) {
        std::optional<std::optional<std::string>> appid = string_member(in, *dictionary, "appid");
        if (!appid)
            return false;
        ext.appid = std::move(*appid);
    } else {
        std::optional<std::optional<std::string>> appid_exclude = string_member(in, *dictionary, "appidExclude");
        if (!appid_exclude)
            return false;
        ext.appid_exclude = std::move(*appid_exclude);
    }
    if (create) {
        // credBlob (CTAP 2.1 §12.2), as Chrome passes it.
        std::optional<js::Value> const blob = dictionary->get("credBlob");
        if (!blob)
            return false;
        if (!blob->is_undefined()) {
            std::optional<Bytes> bytes = buffer_source(interp, *blob, "credBlob");
            if (!bytes)
                return false;
            ext.cred_blob = std::move(*bytes);
        }
        // credProps (§10.1.3).
        std::optional<js::Value> const props = dictionary->get("credProps");
        if (!props)
            return false;
        ext.cred_props = js::Interpreter::to_boolean(*props);
        // credProtect (CTAP 2.1 §12.1), as Chrome takes it:
        // credentialProtectionPolicy and enforceCredentialProtectionPolicy.
        std::optional<std::optional<std::string>> policy = string_member(in, *dictionary, "credentialProtectionPolicy");
        if (!policy)
            return false;
        if (*policy) {
            if (**policy == "userVerificationOptional")
                ext.cred_protect = 1;
            else if (**policy == "userVerificationOptionalWithCredentialIDList")
                ext.cred_protect = 2;
            else if (**policy == "userVerificationRequired")
                ext.cred_protect = 3;
            else {
                interp.throw_type_error("The provided value '" + **policy + "' is not a valid enum value of type CredentialProtectionPolicy.");
                return false;
            }
        }
        std::optional<bool> const enforce = boolean_member(*dictionary, "enforceCredentialProtectionPolicy", false);
        if (!enforce)
            return false;
        ext.enforce_cred_protect = *enforce;
        if (ext.enforce_cred_protect && !ext.cred_protect) {
            refusal = Refusal { "NotSupportedError", "enforceCredentialProtectionPolicy needs a credentialProtectionPolicy." };
            return true;
        }
    } else {
        std::optional<bool> const get_blob = boolean_member(*dictionary, "getCredBlob", false);
        if (!get_blob)
            return false;
        ext.get_cred_blob = *get_blob;
        call.get_cred_blob = *get_blob;
    }
    if (create) {
        std::optional<bool> const hmac = boolean_member(*dictionary, "hmacCreateSecret", false);
        if (!hmac)
            return false;
        ext.hmac_create_secret = *hmac;
    }
    // largeBlob (§10.1.5).
    std::optional<js::Value> const large_blob = dictionary->get("largeBlob");
    if (!large_blob)
        return false;
    if (!large_blob->is_undefined()) {
        std::optional<Dictionary> const blob = Dictionary::of(interp, *large_blob, "AuthenticationExtensionsLargeBlobInputs");
        if (!blob)
            return false;
        std::optional<std::optional<bool>> read;
        std::optional<js::Value> const read_value = blob->get("read");
        if (!read_value)
            return false;
        std::optional<std::optional<std::string>> support = string_member(in, *blob, "support");
        if (!support)
            return false;
        std::optional<js::Value> const write_value = blob->get("write");
        if (!write_value)
            return false;
        if (!write_value->is_undefined() && !buffer_source(interp, *write_value, "write"))
            return false;
        bool const reads = !read_value->is_undefined();
        bool const writes = !write_value->is_undefined();
        if (create) {
            // §10.1.5 client extension processing (registration): read and
            // write belong to get().
            if (reads || writes) {
                refusal = Refusal { "NotSupportedError", "largeBlob read and write are not allowed in create()." };
                return true;
            }
            call.large_blob = true;
            ext.large_blob_key = true;
        } else {
            if (*support) {
                refusal = Refusal { "NotSupportedError", "largeBlob support is for create()." };
                return true;
            }
            if (reads && js::Interpreter::to_boolean(*read_value) && writes) {
                refusal = Refusal { "NotSupportedError", "largeBlob cannot read and write at once." };
                return true;
            }
            call.large_blob = true;
            call.large_blob_read = reads && js::Interpreter::to_boolean(*read_value);
            call.large_blob_write = writes;
        }
    }
    if (create) {
        // minPinLength (CTAP 2.1 §12.4).
        std::optional<bool> const min_pin = boolean_member(*dictionary, "minPinLength", false);
        if (!min_pin)
            return false;
        ext.min_pin_length = *min_pin;
    }
    // prf (§10.1.4).
    std::optional<js::Value> const prf = dictionary->get("prf");
    if (!prf)
        return false;
    if (!prf->is_undefined()) {
        std::optional<Dictionary> const inputs = Dictionary::of(interp, *prf, "AuthenticationExtensionsPRFInputs");
        if (!inputs)
            return false;
        ext.prf = true;
        std::optional<js::Value> const eval = inputs->get("eval");
        if (!eval)
            return false;
        if (!eval->is_undefined()) {
            std::optional<auth::PrfInputs> values = prf_values(interp, *eval);
            if (!values)
                return false;
            ext.prf_eval = std::move(*values);
        }
        std::optional<js::Value> const by_credential = inputs->get("evalByCredential");
        if (!by_credential)
            return false;
        if (!by_credential->is_undefined()) {
            if (!by_credential->is_object()) {
                interp.throw_type_error("The provided value for 'evalByCredential' is not a record.");
                return false;
            }
            std::vector<js::PropertyKey> const keys = by_credential->as_object()->own_keys();
            if (create && !keys.empty()) {
                refusal = Refusal { "NotSupportedError", "evalByCredential is for get()." };
                return true;
            }
            if (!keys.empty() && allow.empty()) {
                refusal = Refusal { "NotSupportedError", "evalByCredential needs allowCredentials." };
                return true;
            }
            for (js::PropertyKey const& key : keys) {
                if (!key.is_atom())
                    continue;
                std::string const name = key.as_atom()->to_utf8();
                std::optional<js::Value> const entry = interp.get(*by_credential, name);
                if (!entry)
                    return false;
                interp.root(*entry);
                std::optional<auth::PrfInputs> values = prf_values(interp, *entry);
                if (!values)
                    return false;
                std::optional<Bytes> const id = media::base64url_decode(name);
                if (!id || id->empty()) {
                    refusal = Refusal { "SyntaxError", "An evalByCredential key is not base64url." };
                    return true;
                }
                bool const listed = std::any_of(allow.begin(), allow.end(), [&](auth::CredentialDescriptor const& d) { return d.id == *id; });
                if (!listed) {
                    refusal = Refusal { "SyntaxError", "An evalByCredential key is not in allowCredentials." };
                    return true;
                }
                ext.prf_eval_by_credential.emplace_back(*id, std::move(*values));
            }
        }
    }
    return true;
}

// ---------- Where the caller stands ----------------------------------------------------------

Realm::Internals& top_of(Realm::Internals& in)
{
    Realm::Internals* top = &in;
    while (top->parent_realm != nullptr)
        top = top->parent_realm;
    return *top;
}

std::string origin_of(Realm::Internals const& in) { return in.origin_url.serialize_origin(); }

// Same origin with every ancestor (Credential Management §2.1.2).
bool same_origin_with_ancestors(Realm::Internals& in)
{
    std::string const mine = origin_of(in);
    if (mine == "null")
        return in.parent_realm == nullptr;
    for (Realm::Internals* up = in.parent_realm; up != nullptr; up = up->parent_realm) {
        if (origin_of(*up) != mine)
            return false;
    }
    return true;
}

// The origin an allowlist token names (Permissions Policy §9.5).
bool allowlist_matches(std::string_view token, std::string const& frame_origin, std::string const& container_origin)
{
    if (token == "*")
        return true;
    if (token == "'self'")
        return frame_origin == container_origin;
    if (token == "'src'")
        return frame_origin != "null";
    if (token == "'none'")
        return false;
    std::optional<net::Url> const url = net::parse_url(token);
    return url && url->serialize_origin() != "null" && url->serialize_origin() == frame_origin;
}

// Whether the container's allow attribute declares `feature` (§9.8 "parse
// policy directive"), and for which origins: nothing when it is silent.
std::optional<bool> declared_by_container(dom::Element const& container, std::string_view feature, std::string const& frame_origin,
    std::string const& container_origin)
{
    std::string const allow = attribute_or_empty(container, "allow");
    std::string_view rest = allow;
    while (!rest.empty()) {
        std::size_t const semicolon = rest.find(';');
        std::string_view const directive = rest.substr(0, semicolon);
        rest = semicolon == std::string_view::npos ? std::string_view() : rest.substr(semicolon + 1);
        std::vector<std::string> const tokens = split_tokens(directive);
        if (tokens.empty() || tokens.front() != feature)
            continue;
        if (tokens.size() == 1)
            return allowlist_matches("'src'", frame_origin, container_origin);
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (allowlist_matches(tokens[i], frame_origin, container_origin))
                return true;
        }
        return false;
    }
    return std::nullopt;
}

// Is `feature` enabled in the document (Permissions Policy §9.9), for a
// feature whose default allowlist is 'self' and a page that sends no
// Permissions-Policy header: the top's own document has it; a frame has it
// when its parent has it and its container's allow attribute names its
// origin, or, when the attribute is silent, when it is the parent's origin.
bool feature_enabled(Realm::Internals& in, std::string_view feature)
{
    if (in.parent_realm == nullptr)
        return true;
    Realm::Internals& parent = *in.parent_realm;
    if (!feature_enabled(parent, feature))
        return false;
    std::string const mine = origin_of(in);
    std::string const theirs = origin_of(parent);
    if (in.frame_element != nullptr) {
        if (std::optional<bool> const declared = declared_by_container(*in.frame_element, feature, mine, theirs))
            return *declared;
    }
    return mine != "null" && mine == theirs;
}

bool secure_context(Realm::Internals const& in)
{
    net::Url const& url = in.url;
    return url.scheme == "https" || url.scheme == "file" || url.scheme == "about" || url.host == "localhost";
}

// The FIDO AppID check (FIDO AppID and Facet Specification §3.1.2) as
// §10.1.1 runs it, without fetching a trusted facets list: an https AppID
// whose host is the caller's, or a registrable domain suffix of it.
bool appid_allowed(std::string const& appid, net::Url const& caller)
{
    std::optional<net::Url> const url = net::parse_url(appid);
    if (!url || url->scheme != "https")
        return false;
    return auth::is_registrable_suffix_or_equal(url->host, caller.host) || auth::is_registrable_suffix_or_equal(caller.host, url->host);
}

// ---------- Settling --------------------------------------------------------------------------

std::shared_ptr<Realm::Internals::CredentialCall> take_call(Realm::Internals& in, std::uint64_t id)
{
    auto const found = in.credential_calls.find(id);
    if (found == in.credential_calls.end())
        return nullptr;
    std::shared_ptr<Realm::Internals::CredentialCall> call = std::move(found->second);
    in.credential_calls.erase(found);
    return call;
}

void settle(Realm::Internals& in, Realm::Internals::CredentialCall const& call, bool resolved, js::Value const& value)
{
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    interp.root(value);
    js::Value const arguments[] = { value };
    static_cast<void>(interp.call(resolved ? call.resolve : call.reject, js::Value::undefined(), arguments));
}

void cancel_ceremony(Realm::Internals::CredentialCall const& call, bool by_page)
{
    if (call.ceremony != 0 && call.host != nullptr && call.host->hooks.cancel_security_key_ceremony)
        call.host->hooks.cancel_security_key_ceremony(call.ceremony, by_page);
}

// The client extension outputs (§5.1.3 step 22, §9.4) as the page reads them.
js::Value extension_results(Realm::Internals& in, PublicKeyCredentialObject const& credential, bool as_json)
{
    js::Interpreter& interp = in.interpreter;
    js::Heap::NoCollect const guard(interp.heap());
    js::Object* const out = interp.new_object();
    auto const put = [&](js::Object& target, std::string_view name, js::Value const& value) {
        static_cast<void>(interp.create_data_property(target, interp.key(name), value, false));
    };
    auto const bytes = [&](Bytes const& data) -> js::Value {
        if (as_json)
            return in.string(auth::base64url(data));
        Native const made = array_buffer(interp, data);
        return made ? *made : js::Value::undefined();
    };
    auth::ExtensionOutputs const& o = credential.outputs;
    auth::ExtensionInputs const& asked = credential.asked;
    bool const create = credential.operation == auth::Operation::Create;
    if (asked.appid && o.appid)
        put(*out, "appid", js::Value::boolean(*o.appid));
    if (asked.appid_exclude && o.appid_exclude)
        put(*out, "appidExclude", js::Value::boolean(*o.appid_exclude));
    if (create && asked.cred_blob && o.cred_blob_stored)
        put(*out, "credBlob", js::Value::boolean(*o.cred_blob_stored));
    if (create && asked.cred_props && o.cred_props_rk) {
        js::Object* const props = interp.new_object();
        put(*props, "rk", js::Value::boolean(*o.cred_props_rk));
        put(*out, "credProps", js::Value::object(props));
    }
    if (!create && credential.get_cred_blob)
        put(*out, "getCredBlob", bytes(o.cred_blob.value_or(Bytes {})));
    if (create && asked.hmac_create_secret && o.hmac_create_secret)
        put(*out, "hmacCreateSecret", js::Value::boolean(*o.hmac_create_secret));
    if (credential.large_blob) {
        js::Object* const blob = interp.new_object();
        if (create)
            put(*blob, "supported", js::Value::boolean(o.large_blob_key.has_value()));
        else if (credential.large_blob_write)
            put(*blob, "written", js::Value::boolean(false)); // authenticatorLargeBlobs is not spoken yet
        put(*out, "largeBlob", js::Value::object(blob));
    }
    if (asked.prf) {
        js::Object* const prf = interp.new_object();
        if (create)
            put(*prf, "enabled", js::Value::boolean(o.prf_enabled.value_or(false)));
        if (o.prf_first) {
            js::Object* const results = interp.new_object();
            put(*results, "first", bytes(*o.prf_first));
            if (o.prf_second)
                put(*results, "second", bytes(*o.prf_second));
            put(*prf, "results", js::Value::object(results));
        }
        put(*out, "prf", js::Value::object(prf));
    }
    return js::Value::object(out);
}

// What §5.1.3 step 22 and §5.1.4 step 20 make of the authenticator's answer.
Native new_credential(Realm::Internals& in, Realm::Internals::CredentialCall const& call, auth::Result const& result)
{
    js::Interpreter& interp = in.interpreter;
    js::Heap::NoCollect const guard(interp.heap());
    auto* const response = interp.heap().allocate<AuthenticatorResponseObject>(
        in.prototype(result.operation == auth::Operation::Create ? "AuthenticatorAttestationResponse" : "AuthenticatorAssertionResponse"), in.realm_record);
    response->client_data_json.assign(call.client_data_json.begin(), call.client_data_json.end());
    if (result.operation == auth::Operation::Create) {
        response->attestation = true;
        response->attestation_object = result.attestation_object;
        if (result.created) {
            response->authenticator_data = result.created->authenticator_data;
            response->public_key = result.created->public_key_spki;
            response->public_key_algorithm = result.created->public_key_algorithm;
            response->transports = result.created->transports;
        } else {
            response->transports = result.transports;
        }
    } else {
        response->authenticator_data = result.authenticator_data;
        response->signature = result.signature;
        response->user_handle = result.user_handle;
    }
    auto* const credential = interp.heap().allocate<PublicKeyCredentialObject>(in.prototype("PublicKeyCredential"), in.realm_record);
    credential->raw_id = result.credential_id;
    credential->id = auth::base64url(result.credential_id);
    credential->response = js::Value::object(response);
    credential->attachment = "cross-platform"; // every key reached is a roaming one
    credential->operation = result.operation;
    credential->outputs = result.extensions;
    credential->asked = call.extensions;
    credential->large_blob = call.large_blob;
    credential->large_blob_read = call.large_blob_read;
    credential->large_blob_write = call.large_blob_write;
    credential->get_cred_blob = call.get_cred_blob;
    return js::Value::object(credential);
}

void outcome_arrived(Realm::Internals& in, std::uint64_t id, auth::Outcome const& outcome)
{
    std::shared_ptr<Realm::Internals::CredentialCall> const call = take_call(in, id);
    if (!call)
        return;
    Realm::Internals::Entry const entry(in);
    js::Interpreter& interp = in.interpreter;
    js::Interpreter::Roots const roots(interp);
    if (outcome.result) {
        Native const made = new_credential(in, *call, *outcome.result);
        if (made) {
            settle(in, *call, true, interp.root(*made));
            return;
        }
    }
    auth::Error const error = outcome.error.value_or(auth::Error {});
    std::string_view message = "The operation either timed out or was not allowed.";
    if (error.kind == auth::ErrorKind::InvalidState)
        message = "The authenticator holds one of the credentials the page excluded.";
    else if (error.kind == auth::ErrorKind::Abort)
        message = "The operation was aborted.";
    // An abort the page asked for is told with the signal's reason
    // (Credential Management §2.5.4), when it gave one.
    if (error.kind == auth::ErrorKind::Abort) {
        if (auto* const signal = object_of<AbortSignalObject>(call->signal); signal && signal->aborted) {
            settle(in, *call, false, signal->reason);
            return;
        }
    }
    settle(in, *call, false, interp.root(dom_exception_value(in, error.dom_exception_name(), message)));
}

// The request's lifetime timer fired (§5.1.3 step 22, §5.1.4 step 18).
void lifetime_ended(Realm::Internals& in, std::uint64_t id)
{
    std::shared_ptr<Realm::Internals::CredentialCall> const call = take_call(in, id);
    if (!call)
        return;
    cancel_ceremony(*call, false);
    settle(in, *call, false, dom_exception_value(in, "NotAllowedError", "The operation either timed out or was not allowed."));
}

// ---------- create() and get() ----------------------------------------------------------------

// Credential Management §2.5.4 "Create a Credential" and §2.5.1 "Request
// a Credential", the parts before the credential type's own algorithm:
// the options dictionary, its signal and mediation, and which type.
struct CredentialOptions {
    std::string mediation = "optional";
    js::Value signal;
    js::Value public_key; // undefined when absent
    bool other_type = false; // a member naming a type not supported here
};

std::optional<CredentialOptions> credential_options(Realm::Internals& in, js::Value const& value, bool create)
{
    js::Interpreter& interp = in.interpreter;
    std::optional<Dictionary> const dictionary = Dictionary::of(interp, value, create ? "CredentialCreationOptions" : "CredentialRequestOptions");
    if (!dictionary)
        return std::nullopt;
    CredentialOptions out;
    out.signal = js::Value::undefined();
    out.public_key = js::Value::undefined();
    // The members a browser that ships only public keys still knows of: a
    // type it does not support is NotSupportedError, not ignored.
    for (std::string_view const other : { "digital", "federated", "identity", "otp", "password" }) {
        std::optional<js::Value> const member = dictionary->get(other);
        if (!member)
            return std::nullopt;
        if (!member->is_undefined())
            out.other_type = true;
    }
    std::optional<js::Value> const mediation = dictionary->get("mediation");
    if (!mediation)
        return std::nullopt;
    if (!mediation->is_undefined()) {
        std::optional<std::string> text = dom_string(in, *mediation);
        if (!text)
            return std::nullopt;
        if (*text != "silent" && *text != "optional" && *text != "conditional" && *text != "required") {
            interp.throw_type_error("The provided value '" + *text + "' is not a valid enum value of type CredentialMediationRequirement.");
            return std::nullopt;
        }
        out.mediation = std::move(*text);
    }
    std::optional<js::Value> const public_key = dictionary->get("publicKey");
    if (!public_key)
        return std::nullopt;
    out.public_key = *public_key;
    std::optional<js::Value> const signal = dictionary->get("signal");
    if (!signal)
        return std::nullopt;
    if (!signal->is_undefined()) {
        if (!object_of<AbortSignalObject>(*signal)) {
            interp.throw_type_error("Failed to read the 'signal' property: The provided value is not of type 'AbortSignal'.");
            return std::nullopt;
        }
        out.signal = *signal;
    }
    return out;
}

// A requirement's DOMString (§5.4.6, §5.8.6): an unknown value is ignored.
std::optional<auth::Requirement> requirement(std::optional<std::string> const& text)
{
    if (!text)
        return std::nullopt;
    if (*text == "required")
        return auth::Requirement::Required;
    if (*text == "preferred")
        return auth::Requirement::Preferred;
    if (*text == "discouraged")
        return auth::Requirement::Discouraged;
    return std::nullopt;
}

// The checks and steps of §5.1.3 (create) or §5.1.4 (get) that read the
// public-key options, filling `request` and `call`. Nothing with an
// exception pending (a TypeError of the conversion); a Refusal for a
// DOMException; and whether no authenticator here can serve it (an
// attachment of "platform").
struct Prepared {
    bool no_authenticator = false;
    bool rp_id_given = false; // otherwise the caller's effective domain is the RP ID
};

std::optional<Prepared> prepare(Realm::Internals& in, js::Value const& value, bool create, auth::Request& request,
    Realm::Internals::CredentialCall& call, std::optional<Refusal>& refusal)
{
    js::Interpreter& interp = in.interpreter;
    Prepared prepared;
    std::optional<Dictionary> const options = Dictionary::of(interp, value,
        create ? "PublicKeyCredentialCreationOptions" : "PublicKeyCredentialRequestOptions");
    if (!options)
        return std::nullopt;
    request.operation = create ? auth::Operation::Create : auth::Operation::Get;
    call.operation = request.operation;

    // The members, in the dictionary's (lexicographic) order.
    std::optional<std::string> attachment;
    if (create) {
        std::optional<js::Value> const attestation_value = options->get("attestation");
        if (!attestation_value)
            return std::nullopt;
        request.attestation = auth::AttestationConveyance::None;
        if (!attestation_value->is_undefined()) {
            std::optional<std::string> text = dom_string(in, *attestation_value);
            if (!text)
                return std::nullopt;
            if (*text == "indirect")
                request.attestation = auth::AttestationConveyance::Indirect;
            else if (*text == "direct")
                request.attestation = auth::AttestationConveyance::Direct;
            else if (*text == "enterprise")
                request.attestation = auth::AttestationConveyance::Enterprise;
        }
        std::optional<js::Value> const formats = options->get("attestationFormats");
        if (!formats)
            return std::nullopt;
        if (!formats->is_undefined() && !string_sequence(in, *formats, "attestationFormats"))
            return std::nullopt;
        std::optional<js::Value> const selection_value = options->get("authenticatorSelection");
        if (!selection_value)
            return std::nullopt;
        std::optional<Dictionary> const selection = Dictionary::of(interp, *selection_value, "AuthenticatorSelectionCriteria");
        if (!selection)
            return std::nullopt;
        std::optional<std::optional<std::string>> attach = string_member(in, *selection, "authenticatorAttachment");
        if (!attach)
            return std::nullopt;
        attachment = std::move(*attach);
        std::optional<bool> const require_rk = boolean_member(*selection, "requireResidentKey", false);
        if (!require_rk)
            return std::nullopt;
        std::optional<std::optional<std::string>> resident = string_member(in, *selection, "residentKey");
        if (!resident)
            return std::nullopt;
        std::optional<std::optional<std::string>> uv = string_member(in, *selection, "userVerification");
        if (!uv)
            return std::nullopt;
        // §5.1.3 step 20.2: residentKey when it names a value, else
        // requireResidentKey.
        std::optional<auth::Requirement> const rk = requirement(*resident);
        request.resident_key = rk ? *rk : *require_rk ? auth::Requirement::Required : auth::Requirement::Discouraged;
        request.user_verification = requirement(*uv).value_or(auth::Requirement::Preferred);
    }
    if (!create) {
        // allowCredentials first: the prf extension reads it.
        std::optional<std::vector<auth::CredentialDescriptor>> allow = descriptor_list(in, *options, "allowCredentials");
        if (!allow)
            return std::nullopt;
        request.credentials = std::move(*allow);
    }
    std::optional<js::Value> const challenge_value = options->required("challenge");
    if (!challenge_value)
        return std::nullopt;
    std::optional<Bytes> challenge = buffer_source(interp, *challenge_value, "challenge");
    if (!challenge)
        return std::nullopt;
    if (create) {
        std::optional<std::vector<auth::CredentialDescriptor>> exclude = descriptor_list(in, *options, "excludeCredentials");
        if (!exclude)
            return std::nullopt;
        request.credentials = std::move(*exclude);
    }
    std::optional<js::Value> const extensions = options->get("extensions");
    if (!extensions)
        return std::nullopt;
    if (!read_extensions(in, *extensions, request.operation, request.credentials, call, refusal))
        return std::nullopt;
    if (refusal)
        return prepared;
    std::optional<js::Value> const hints = options->get("hints");
    if (!hints)
        return std::nullopt;
    if (!hints->is_undefined() && !string_sequence(in, *hints, "hints"))
        return std::nullopt;
    if (create) {
        std::optional<js::Value> const params = options->required("pubKeyCredParams");
        if (!params)
            return std::nullopt;
        std::optional<std::vector<js::Value>> const list = sequence(interp, *params, "pubKeyCredParams");
        if (!list)
            return std::nullopt;
        std::vector<std::int64_t> algorithms;
        for (js::Value const& one : *list) {
            std::optional<Dictionary> const param = Dictionary::of(interp, one, "PublicKeyCredentialParameters");
            if (!param)
                return std::nullopt;
            std::optional<js::Value> const alg = param->required("alg");
            if (!alg)
                return std::nullopt;
            std::optional<double> const number = interp.to_number(*alg);
            if (!number)
                return std::nullopt;
            std::optional<js::Value> const type = param->required("type");
            if (!type)
                return std::nullopt;
            std::optional<std::string> const type_text = dom_string(in, *type);
            if (!type_text)
                return std::nullopt;
            // §5.1.3 step 10: a type this client does not know is skipped.
            if (*type_text != "public-key")
                continue;
            algorithms.push_back(static_cast<std::int32_t>(to_unsigned_long(*number)));
        }
        if (list->empty())
            algorithms = { auth::cose::alg_es256, auth::cose::alg_rs256 };
        if (algorithms.empty()) {
            refusal = Refusal { "NotSupportedError", "None of pubKeyCredParams names a credential type this browser supports." };
            return prepared;
        }
        request.algorithms = std::move(algorithms);
        std::optional<js::Value> const rp_value = options->required("rp");
        if (!rp_value)
            return std::nullopt;
        std::optional<Dictionary> const rp = Dictionary::of(interp, *rp_value, "PublicKeyCredentialRpEntity");
        if (!rp)
            return std::nullopt;
        std::optional<std::optional<std::string>> rp_id = string_member(in, *rp, "id");
        if (!rp_id)
            return std::nullopt;
        std::optional<js::Value> const rp_name = rp->required("name");
        if (!rp_name)
            return std::nullopt;
        std::optional<std::string> rp_name_text = dom_string(in, *rp_name);
        if (!rp_name_text)
            return std::nullopt;
        request.rp_name = std::move(*rp_name_text);
        // An rp.id given as null converts to "null", which no origin
        // allows: kept as given, and refused by the caller's check.
        prepared.rp_id_given = rp_id->has_value();
        if (*rp_id)
            request.rp_id = std::move(**rp_id);
        prepared.no_authenticator = attachment && *attachment == "platform";
    } else {
        std::optional<std::optional<std::string>> rp_id = string_member(in, *options, "rpId");
        if (!rp_id)
            return std::nullopt;
        prepared.rp_id_given = rp_id->has_value();
        if (*rp_id)
            request.rp_id = std::move(**rp_id);
    }
    std::optional<js::Value> const timeout = options->get("timeout");
    if (!timeout)
        return std::nullopt;
    std::optional<std::uint32_t> timeout_ms;
    if (!timeout->is_undefined()) {
        std::optional<double> const number = interp.to_number(*timeout);
        if (!number)
            return std::nullopt;
        timeout_ms = to_unsigned_long(*number);
    }
    request.timeout = auth::ceremony_timeout(timeout_ms);
    if (create) {
        std::optional<js::Value> const user_value = options->required("user");
        if (!user_value)
            return std::nullopt;
        std::optional<Dictionary> const user = Dictionary::of(interp, *user_value, "PublicKeyCredentialUserEntity");
        if (!user)
            return std::nullopt;
        std::optional<js::Value> const display_name = user->required("displayName");
        if (!display_name)
            return std::nullopt;
        std::optional<std::string> display_text = dom_string(in, *display_name);
        if (!display_text)
            return std::nullopt;
        std::optional<js::Value> const id_value = user->required("id");
        if (!id_value)
            return std::nullopt;
        std::optional<Bytes> user_id = buffer_source(interp, *id_value, "id");
        if (!user_id)
            return std::nullopt;
        std::optional<js::Value> const name = user->required("name");
        if (!name)
            return std::nullopt;
        std::optional<std::string> name_text = dom_string(in, *name);
        if (!name_text)
            return std::nullopt;
        // §5.1.3 step 5: a user handle of 1 to 64 bytes.
        if (user_id->empty() || user_id->size() > 64) {
            interp.throw_type_error("user.id must be between 1 and 64 bytes long.");
            return std::nullopt;
        }
        request.user.id = std::move(*user_id);
        request.user.name = std::move(*name_text);
        request.user.display_name = std::move(*display_text);
        request.user.has_name = true;
        request.user.has_display_name = true;
    } else {
        std::optional<std::optional<std::string>> uv = string_member(in, *options, "userVerification");
        if (!uv)
            return std::nullopt;
        request.user_verification = requirement(*uv).value_or(auth::Requirement::Preferred);
    }
    call.client_data_json = auth::base64url(*challenge); // the challenge until the client data is made
    return prepared;
}

Native credential_operation(js::Interpreter& interp, Args args, bool create)
{
    Realm::Internals& in = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    std::string_view const operation = create ? "create" : "get";
    std::optional<CredentialOptions> const options = credential_options(in, js::argument(args, 0), create);
    if (!options)
        return std::nullopt;
    interp.root(options->signal);
    interp.root(options->public_key);
    // §2.5.1 / §2.5.4: the document, the signal, the credential type.
    auto* const signal = object_of<AbortSignalObject>(options->signal);
    if (signal && signal->aborted)
        return rejected_promise(interp, signal->reason);
    if (options->public_key.is_undefined() || options->other_type)
        return reject_dom(in, "NotSupportedError", "Only public-key credentials are supported.");
    if (options->mediation == "conditional")
        return interp.throw_type_error("Conditional mediation is not available.");
    Realm::Internals& top = top_of(in);
    bool const same_origin = same_origin_with_ancestors(in);
    // §5.1.3 steps 2 and 3, §5.1.4 step 2 (§5.9, §5.10): the feature, and
    // for a frame of another origin creating, the reader's act.
    if (!feature_enabled(in, create ? "publickey-credentials-create" : "publickey-credentials-get"))
        return reject_dom(in, "NotAllowedError", "The 'publickey-credentials-" + std::string(operation) + "' feature is not enabled in this document.");
    if (create && !same_origin && top.hooks.user_activation && !top.hooks.user_activation())
        return reject_dom(in, "NotAllowedError", "A frame of another origin may create a credential only while the reader is acting on it.");
    if (top.hooks.document_focused && !top.hooks.document_focused())
        return reject_dom(in, "NotAllowedError", "The document does not have focus.");
    if (!in.credential_calls.empty())
        return reject_dom(in, "NotAllowedError", "A request is already pending.");

    auto call = std::make_shared<Realm::Internals::CredentialCall>();
    auth::Request request;
    std::optional<Refusal> refusal;
    std::optional<Prepared> const prepared = prepare(in, options->public_key, create, request, *call, refusal);
    if (!prepared)
        return std::nullopt;
    if (refusal)
        return reject_dom(in, refusal->name, refusal->message);
    // §5.1.3 steps 6 to 8: the caller's origin and effective domain, and
    // the RP ID's check against it.
    net::Url const& caller = in.origin_url;
    if (origin_of(in) == "null")
        return reject_dom(in, "NotAllowedError", "An opaque origin cannot use Web Authentication.");
    if (!prepared->rp_id_given)
        request.rp_id = caller.host;
    if (!auth::rp_id_allowed(request.rp_id, caller.scheme, caller.host))
        return reject_dom(in, "SecurityError", "The relying party ID is not a registrable domain suffix of, nor equal to the current domain.");
    if (request.extensions.appid) {
        if (!appid_allowed(*request.extensions.appid, caller))
            return reject_dom(in, "SecurityError", "The AppID is not one this origin may use.");
    }
    if (request.extensions.appid_exclude) {
        if (!appid_allowed(*request.extensions.appid_exclude, caller))
            return reject_dom(in, "SecurityError", "The AppID is not one this origin may use.");
    }
    // §5.1.3 steps 12 to 14: the client data, and its hash.
    auth::CollectedClientData data;
    data.type = create ? "webauthn.create" : "webauthn.get";
    data.challenge = call->client_data_json;
    data.origin = origin_of(in);
    data.cross_origin = !same_origin;
    if (!same_origin)
        data.top_origin = origin_of(top);
    call->client_data_json = auth::serialize_client_data(data);
    std::span<std::uint8_t const> const json(reinterpret_cast<std::uint8_t const*>(call->client_data_json.data()), call->client_data_json.size());
    request.client_data_hash = crypto::Sha256::hash(json);
    request.extensions = call->extensions;

    std::optional<js::PromiseCapability> const capability = js::new_promise_capability(interp, js::Value::object(interp.intrinsics().promise_constructor));
    if (!capability)
        return std::nullopt;
    call->resolve = capability->resolve;
    call->reject = capability->reject;
    call->signal = options->signal;
    call->host = &top;
    std::uint64_t const id = in.next_credential_call++;
    in.credential_calls[id] = call;

    // The ceremony, through the page's host; the outcome comes back on the
    // page's thread as a remote task.
    if (!prepared->no_authenticator && top.hooks.security_key_ceremony) {
        HostHooks::SecurityKeyAsk host_ask;
        host_ask.site = request.rp_id;
        host_ask.origin = data.origin;
        host_ask.cross_origin = data.cross_origin;
        std::shared_ptr<Agent::RemoteTasks> const remote = in.agent.remote_tasks;
        std::weak_ptr<bool> const alive = in.alive;
        Realm::Internals* const target = &in;
        call->ceremony = top.hooks.security_key_ceremony(host_ask, request, [remote, alive, target, id](auth::Outcome outcome) {
            auto held = std::make_shared<auth::Outcome>(std::move(outcome));
            std::function<void()> wake;
            {
                std::lock_guard<std::mutex> const lock(remote->mutex);
                remote->tasks.push_back([alive, target, id, held] {
                    if (!alive.expired())
                        outcome_arrived(*target, id, *held);
                });
                wake = remote->wake;
            }
            if (wake)
                wake();
        });
    }
    // The lifetime timer, on the page's clock.
    js::ClosureFunction* const timer = interp.new_closure("credential timeout", 0, { js::Value::number(static_cast<double>(id)) },
        [](js::Interpreter& inner, js::ClosureFunction& self, js::Value const&, Args) -> Native {
            lifetime_ended(internals_of(inner), static_cast<std::uint64_t>(self.slot(0).as_number()));
            return js::Value::undefined();
        });
    interp.root(js::Value::object(timer));
    schedule_native(in, static_cast<double>(request.timeout.count()), js::Value::object(timer));
    // The page's signal aborts it (§5.1.3 step 22.1).
    if (signal) {
        std::weak_ptr<bool> const alive = in.alive;
        Realm::Internals* const target = &in;
        signal->abort_algorithms.push_back([alive, target, id] {
            if (alive.expired())
                return;
            std::shared_ptr<Realm::Internals::CredentialCall> const aborted = take_call(*target, id);
            if (!aborted)
                return;
            cancel_ceremony(*aborted, true);
            auto* const source = object_of<AbortSignalObject>(aborted->signal);
            settle(*target, *aborted, false, source ? source->reason : abort_error(*target, "The operation was aborted."));
        });
    }
    return capability->promise;
}

// ---------- toJSON and the JSON parsers (§5.1.8, §5.1.9, §5.1.10) ----------------------------

js::Value response_json(Realm::Internals& in, AuthenticatorResponseObject const& response)
{
    js::Interpreter& interp = in.interpreter;
    js::Heap::NoCollect const guard(interp.heap());
    js::Object* const out = interp.new_object();
    auto const put = [&](std::string_view name, js::Value const& value) {
        static_cast<void>(interp.create_data_property(*out, interp.key(name), value, false));
    };
    put("clientDataJSON", in.string(auth::base64url(response.client_data_json)));
    put("authenticatorData", in.string(auth::base64url(response.authenticator_data)));
    if (response.attestation) {
        std::vector<js::Value> transports;
        for (std::string const& transport : response.transports)
            transports.push_back(in.string(transport));
        put("transports", js::Value::object(interp.new_array(transports)));
        if (response.public_key)
            put("publicKey", in.string(auth::base64url(*response.public_key)));
        put("publicKeyAlgorithm", js::Value::number(static_cast<double>(response.public_key_algorithm)));
        put("attestationObject", in.string(auth::base64url(response.attestation_object)));
    } else {
        put("signature", in.string(auth::base64url(response.signature)));
        if (response.user_handle)
            put("userHandle", in.string(auth::base64url(*response.user_handle)));
    }
    return js::Value::object(out);
}

// Copies the members of a *JSON dictionary into a new object, a base64url
// string member decoded to an ArrayBuffer where `decode` names it: an
// EncodingError for one that is not base64url.
struct JsonCopy {
    Realm::Internals& in;

    bool copy(js::Value const& from, js::Object& to, std::string_view name, bool required, bool decode)
    {
        js::Interpreter& interp = in.interpreter;
        std::optional<js::Value> const value = from.is_object() ? interp.get(from, name) : std::optional<js::Value>(js::Value::undefined());
        if (!value)
            return false;
        interp.root(*value);
        if (value->is_undefined()) {
            if (required) {
                interp.throw_type_error("Failed to read the '" + std::string(name) + "' property: Required member is undefined.");
                return false;
            }
            return true;
        }
        js::Value out = *value;
        if (decode) {
            std::optional<std::string> const text = in.to_utf8(*value);
            if (!text)
                return false;
            std::optional<Bytes> const bytes = media::base64url_decode(*text);
            if (!bytes) {
                in.throw_dom_exception("EncodingError", "'" + std::string(name) + "' is not base64url.");
                return false;
            }
            Native const buffer = array_buffer(interp, *bytes);
            if (!buffer)
                return false;
            out = interp.root(*buffer);
        }
        return interp.create_data_property(to, interp.key(name), out, false).has_value();
    }

    // A member that is itself a dictionary, copied by `inner`.
    template <class F>
    bool nested(js::Value const& from, js::Object& to, std::string_view name, bool required, F const& inner)
    {
        js::Interpreter& interp = in.interpreter;
        std::optional<js::Value> const value = from.is_object() ? interp.get(from, name) : std::optional<js::Value>(js::Value::undefined());
        if (!value)
            return false;
        interp.root(*value);
        if (value->is_undefined()) {
            if (required) {
                interp.throw_type_error("Failed to read the '" + std::string(name) + "' property: Required member is undefined.");
                return false;
            }
            return true;
        }
        if (!value->is_object()) {
            interp.throw_type_error("The '" + std::string(name) + "' member is not a dictionary.");
            return false;
        }
        js::Object* const made = interp.new_object();
        interp.root(js::Value::object(made));
        if (!inner(*value, *made))
            return false;
        return interp.create_data_property(to, interp.key(name), js::Value::object(made), false).has_value();
    }

    // A sequence member whose entries are dictionaries copied by `inner`.
    template <class F>
    bool list(js::Value const& from, js::Object& to, std::string_view name, F const& inner)
    {
        js::Interpreter& interp = in.interpreter;
        std::optional<js::Value> const value = from.is_object() ? interp.get(from, name) : std::optional<js::Value>(js::Value::undefined());
        if (!value)
            return false;
        interp.root(*value);
        if (value->is_undefined())
            return true;
        std::optional<std::vector<js::Value>> const entries = sequence(interp, *value, name);
        if (!entries)
            return false;
        std::vector<js::Value> made;
        for (js::Value const& entry : *entries) {
            if (!entry.is_object()) {
                interp.throw_type_error("An entry of '" + std::string(name) + "' is not a dictionary.");
                return false;
            }
            js::Object* const object = interp.new_object();
            interp.root(js::Value::object(object));
            if (!inner(entry, *object))
                return false;
            made.push_back(js::Value::object(object));
        }
        js::ArrayObject* const array = interp.new_array(made);
        return interp.create_data_property(to, interp.key(name), js::Value::object(array), false).has_value();
    }

    bool descriptor(js::Value const& from, js::Object& to)
    {
        return copy(from, to, "id", true, true) && copy(from, to, "type", true, false) && copy(from, to, "transports", false, false);
    }

    bool prf_values(js::Value const& from, js::Object& to)
    {
        return copy(from, to, "first", true, true) && copy(from, to, "second", false, true);
    }

    bool extensions(js::Value const& from, js::Object& to)
    {
        js::Interpreter& interp = in.interpreter;
        for (std::string_view const plain : { "appid", "appidExclude", "credProps", "credentialProtectionPolicy", "enforceCredentialProtectionPolicy",
                 "getCredBlob", "hmacCreateSecret", "minPinLength" }) {
            if (!copy(from, to, plain, false, false))
                return false;
        }
        if (!copy(from, to, "credBlob", false, true))
            return false;
        if (!nested(from, to, "largeBlob", false, [&](js::Value const& blob, js::Object& out) {
                return copy(blob, out, "support", false, false) && copy(blob, out, "read", false, false) && copy(blob, out, "write", false, true);
            }))
            return false;
        return nested(from, to, "prf", false, [&](js::Value const& prf, js::Object& out) {
            if (!nested(prf, out, "eval", false, [&](js::Value const& v, js::Object& o) { return prf_values(v, o); }))
                return false;
            std::optional<js::Value> const by = interp.get(prf, "evalByCredential");
            if (!by)
                return false;
            interp.root(*by);
            if (by->is_undefined())
                return true;
            if (!by->is_object()) {
                interp.throw_type_error("evalByCredential is not a record.");
                return false;
            }
            js::Object* const record = interp.new_object();
            interp.root(js::Value::object(record));
            for (js::PropertyKey const& key : by->as_object()->own_keys()) {
                if (!key.is_atom())
                    continue;
                std::string const name = key.as_atom()->to_utf8();
                std::optional<js::Value> const entry = interp.get(*by, name);
                if (!entry)
                    return false;
                interp.root(*entry);
                js::Object* const values = interp.new_object();
                interp.root(js::Value::object(values));
                if (!prf_values(*entry, *values))
                    return false;
                static_cast<void>(interp.create_data_property(*record, interp.key(name), js::Value::object(values), false));
            }
            return interp.create_data_property(out, interp.key("evalByCredential"), js::Value::object(record), false).has_value();
        });
    }
};

// ---------- Installing ------------------------------------------------------------------------

void define_static_resolving(Realm::Internals& in, js::Object& constructor, std::string_view name, bool answer)
{
    if (answer) {
        define_promise_operation(in.interpreter, constructor, name, 0,
            [](js::Interpreter& interp, js::Value const&, Args) -> Native { return resolved_promise(interp, js::Value::boolean(true)); });
    } else {
        define_promise_operation(in.interpreter, constructor, name, 0,
            [](js::Interpreter& interp, js::Value const&, Args) -> Native { return resolved_promise(interp, js::Value::boolean(false)); });
    }
}

// The signal methods (§5.1.10–§5.1.12): the options are checked as the
// specification checks them, and then there is nothing to update, since
// this browser keeps no credentials of its own.
Native signal_method(js::Interpreter& interp, Args args, std::string_view name, std::initializer_list<std::string_view> base64_members,
    std::initializer_list<std::string_view> string_members)
{
    Realm::Internals& in = internals_of(interp);
    js::Interpreter::Roots const roots(interp);
    js::Value const options = js::argument(args, 0);
    std::optional<Dictionary> const dictionary = Dictionary::of(interp, options, std::string(name));
    if (!dictionary)
        return std::nullopt;
    std::optional<js::Value> const rp_id_value = dictionary->required("rpId");
    if (!rp_id_value)
        return std::nullopt;
    std::optional<std::string> const rp_id = dom_string(in, *rp_id_value);
    if (!rp_id)
        return std::nullopt;
    for (std::string_view const member : string_members) {
        std::optional<js::Value> const value = dictionary->required(member);
        if (!value || !dom_string(in, *value))
            return std::nullopt;
    }
    for (std::string_view const member : base64_members) {
        std::optional<js::Value> const value = dictionary->required(member);
        if (!value)
            return std::nullopt;
        std::vector<std::string> texts;
        if (member == "allAcceptedCredentialIds") {
            std::optional<std::vector<std::string>> list = string_sequence(in, *value, member);
            if (!list)
                return std::nullopt;
            texts = std::move(*list);
        } else {
            std::optional<std::string> text = dom_string(in, *value);
            if (!text)
                return std::nullopt;
            texts.push_back(std::move(*text));
        }
        for (std::string const& text : texts) {
            if (!media::base64url_decode(text))
                return interp.throw_type_error("'" + std::string(member) + "' is not base64url.");
        }
    }
    net::Url const& caller = in.origin_url;
    if (!auth::rp_id_allowed(*rp_id, caller.scheme, caller.host))
        return reject_dom(in, "SecurityError", "The relying party ID is not a registrable domain suffix of, nor equal to the current domain.");
    return resolved_promise(interp, js::Value::undefined());
}

}

void install_web_authn(Realm::Internals& in, js::Object& navigator)
{
    js::Interpreter& interpreter = in.interpreter;
    // Every member here is [SecureContext].
    if (!secure_context(in))
        return;

    // Credential (Credential Management §2.2).
    js::Object* const credential_proto = define_interface(in, "Credential", nullptr);
    define_getter(in, *credential_proto, "id", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        return internals_of(interp).string((*found)->id);
    });
    define_getter(in, *credential_proto, "type", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        if (!this_credential(interp, this_value))
            return std::nullopt;
        return internals_of(interp).string("public-key");
    });
    js::Object& credential_constructor = *interpreter.get(*interpreter.global(), interpreter.key("Credential"))->as_object();
    define_static_resolving(in, credential_constructor, "isConditionalMediationAvailable", false);

    // PublicKeyCredential (§5.1).
    js::Object* const public_key_proto = define_interface(in, "PublicKeyCredential", credential_proto);
    define_getter(in, *public_key_proto, "rawId", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        return same_buffer(interp, (*found)->raw_id_buffer, (*found)->raw_id);
    });
    define_getter(in, *public_key_proto, "response", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->response;
    });
    define_getter(in, *public_key_proto, "authenticatorAttachment", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        return (*found)->attachment.empty() ? js::Value::null() : internals_of(interp).string((*found)->attachment);
    });
    define_operation(interpreter, *public_key_proto, "getClientExtensionResults", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        return extension_results(internals_of(interp), **found, false);
    });
    define_operation(interpreter, *public_key_proto, "toJSON", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<PublicKeyCredentialObject*> const found = this_credential(interp, this_value);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        PublicKeyCredentialObject& credential = **found;
        auto* const response = object_of<AuthenticatorResponseObject>(credential.response);
        js::Value const response_value = interp.root(response ? response_json(internals, *response) : js::Value::null());
        js::Value const extensions = interp.root(extension_results(internals, credential, true));
        js::Heap::NoCollect const guard(interp.heap());
        js::Object* const out = interp.new_object();
        auto const put = [&](std::string_view name, js::Value const& value) {
            static_cast<void>(interp.create_data_property(*out, interp.key(name), value, false));
        };
        put("id", internals.string(credential.id));
        put("rawId", internals.string(credential.id));
        put("response", response_value);
        if (!credential.attachment.empty())
            put("authenticatorAttachment", internals.string(credential.attachment));
        put("clientExtensionResults", extensions);
        put("type", internals.string("public-key"));
        return js::Value::object(out);
    });
    js::Object& public_key_constructor = *interpreter.get(*interpreter.global(), interpreter.key("PublicKeyCredential"))->as_object();
    // No platform authenticator on this system (Linux has none a browser
    // reaches: Firefox answers the same there), no autofill UI yet.
    define_static_resolving(in, public_key_constructor, "isUserVerifyingPlatformAuthenticatorAvailable", false);
    define_static_resolving(in, public_key_constructor, "isConditionalMediationAvailable", false);
    define_promise_operation(interpreter, public_key_constructor, "getClientCapabilities", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        // §5.1.7: the record's keys in ascending order.
        static constexpr std::pair<std::string_view, bool> capabilities[] = {
            { "conditionalCreate", false },
            { "conditionalGet", false },
            { "extension:appid", true },
            { "extension:appidExclude", true },
            { "extension:credBlob", true },
            { "extension:credProps", true },
            { "extension:credProtect", true },
            { "extension:hmacCreateSecret", true },
            { "extension:largeBlob", true },
            { "extension:minPinLength", true },
            { "extension:prf", true },
            { "hybridTransport", false },
            { "passkeyPlatformAuthenticator", false },
            { "relatedOrigins", false },
            { "signalAllAcceptedCredentials", true },
            { "signalCurrentUserDetails", true },
            { "signalUnknownCredential", true },
            { "userVerifyingPlatformAuthenticator", false },
        };
        js::Heap::NoCollect const guard(interp.heap());
        js::Object* const out = interp.new_object();
        for (auto const& [name, value] : capabilities)
            static_cast<void>(interp.create_data_property(*out, interp.key(name), js::Value::boolean(value), false));
        return resolved_promise(interp, js::Value::object(out));
    });
    define_operation(interpreter, public_key_constructor, "parseCreationOptionsFromJSON", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Value const options = js::argument(args, 0);
        if (!options.is_object())
            return interp.throw_type_error("The provided value is not of type 'PublicKeyCredentialCreationOptionsJSON'.");
        JsonCopy json { internals };
        js::Object* const out = interp.new_object();
        interp.root(js::Value::object(out));
        bool const ok = json.nested(options, *out, "rp", true, [&](js::Value const& from, js::Object& to) {
            return json.copy(from, to, "id", false, false) && json.copy(from, to, "name", true, false);
        }) && json.nested(options, *out, "user", true, [&](js::Value const& from, js::Object& to) {
            return json.copy(from, to, "id", true, true) && json.copy(from, to, "name", true, false) && json.copy(from, to, "displayName", true, false);
        }) && json.copy(options, *out, "challenge", true, true)
            && json.list(options, *out, "pubKeyCredParams", [&](js::Value const& from, js::Object& to) {
                   return json.copy(from, to, "type", true, false) && json.copy(from, to, "alg", true, false);
               })
            && json.copy(options, *out, "timeout", false, false)
            && json.list(options, *out, "excludeCredentials", [&](js::Value const& from, js::Object& to) { return json.descriptor(from, to); })
            && json.copy(options, *out, "authenticatorSelection", false, false) && json.copy(options, *out, "hints", false, false)
            && json.copy(options, *out, "attestation", false, false) && json.copy(options, *out, "attestationFormats", false, false)
            && json.nested(options, *out, "extensions", false, [&](js::Value const& from, js::Object& to) { return json.extensions(from, to); });
        if (!ok)
            return std::nullopt;
        // Absent from the JSON, a member with a default has it (§5.4).
        if (!interp.get(*out, interp.key("pubKeyCredParams"))->is_object())
            return interp.throw_type_error("Failed to read the 'pubKeyCredParams' property: Required member is undefined.");
        return js::Value::object(out);
    });
    define_operation(interpreter, public_key_constructor, "parseRequestOptionsFromJSON", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        js::Interpreter::Roots const roots(interp);
        js::Value const options = js::argument(args, 0);
        if (!options.is_object())
            return interp.throw_type_error("The provided value is not of type 'PublicKeyCredentialRequestOptionsJSON'.");
        JsonCopy json { internals };
        js::Object* const out = interp.new_object();
        interp.root(js::Value::object(out));
        bool const ok = json.copy(options, *out, "challenge", true, true) && json.copy(options, *out, "timeout", false, false)
            && json.copy(options, *out, "rpId", false, false)
            && json.list(options, *out, "allowCredentials", [&](js::Value const& from, js::Object& to) { return json.descriptor(from, to); })
            && json.copy(options, *out, "userVerification", false, false) && json.copy(options, *out, "hints", false, false)
            && json.nested(options, *out, "extensions", false, [&](js::Value const& from, js::Object& to) { return json.extensions(from, to); });
        if (!ok)
            return std::nullopt;
        return js::Value::object(out);
    });
    define_promise_operation(interpreter, public_key_constructor, "signalUnknownCredential", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return rejecting(interp, signal_method(interp, args, "UnknownCredentialOptions", { "credentialId" }, {}));
    });
    define_promise_operation(interpreter, public_key_constructor, "signalAllAcceptedCredentials", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return rejecting(interp, signal_method(interp, args, "AllAcceptedCredentialsOptions", { "userId", "allAcceptedCredentialIds" }, {}));
    });
    define_promise_operation(interpreter, public_key_constructor, "signalCurrentUserDetails", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return rejecting(interp, signal_method(interp, args, "CurrentUserDetailsOptions", { "userId" }, { "name", "displayName" }));
    });

    // AuthenticatorResponse and its two kinds (§5.2).
    js::Object* const response_proto = define_interface(in, "AuthenticatorResponse", nullptr);
    define_getter(in, *response_proto, "clientDataJSON", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, std::nullopt);
        if (!found)
            return std::nullopt;
        return same_buffer(interp, (*found)->client_data_buffer, (*found)->client_data_json);
    });
    js::Object* const attestation_proto = define_interface(in, "AuthenticatorAttestationResponse", response_proto);
    define_getter(in, *attestation_proto, "attestationObject", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, true);
        if (!found)
            return std::nullopt;
        return same_buffer(interp, (*found)->attestation_buffer, (*found)->attestation_object);
    });
    define_operation(interpreter, *attestation_proto, "getTransports", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, true);
        if (!found)
            return std::nullopt;
        Realm::Internals& internals = internals_of(interp);
        js::Heap::NoCollect const guard(interp.heap());
        std::vector<js::Value> transports;
        for (std::string const& transport : (*found)->transports)
            transports.push_back(internals.string(transport));
        return js::Value::object(interp.new_array(transports));
    });
    define_operation(interpreter, *attestation_proto, "getAuthenticatorData", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, true);
        if (!found)
            return std::nullopt;
        return array_buffer(interp, (*found)->authenticator_data);
    });
    define_operation(interpreter, *attestation_proto, "getPublicKey", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, true);
        if (!found)
            return std::nullopt;
        if (!(*found)->public_key)
            return js::Value::null();
        return array_buffer(interp, *(*found)->public_key);
    });
    define_operation(interpreter, *attestation_proto, "getPublicKeyAlgorithm", 0, [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, true);
        if (!found)
            return std::nullopt;
        return js::Value::number(static_cast<double>((*found)->public_key_algorithm));
    });
    js::Object* const assertion_proto = define_interface(in, "AuthenticatorAssertionResponse", response_proto);
    define_getter(in, *assertion_proto, "authenticatorData", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, false);
        if (!found)
            return std::nullopt;
        return same_buffer(interp, (*found)->authenticator_data_buffer, (*found)->authenticator_data);
    });
    define_getter(in, *assertion_proto, "signature", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, false);
        if (!found)
            return std::nullopt;
        return same_buffer(interp, (*found)->signature_buffer, (*found)->signature);
    });
    define_getter(in, *assertion_proto, "userHandle", [](js::Interpreter& interp, js::Value const& this_value, Args) -> Native {
        std::optional<AuthenticatorResponseObject*> const found = this_response(interp, this_value, false);
        if (!found)
            return std::nullopt;
        if (!(*found)->user_handle)
            return js::Value::null();
        return same_buffer(interp, (*found)->user_handle_buffer, *(*found)->user_handle);
    });

    // CredentialsContainer (Credential Management §2.3).
    js::Object* const container = define_interface(in, "CredentialsContainer", nullptr);
    define_promise_operation(interpreter, *container, "create", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return rejecting(interp, credential_operation(interp, args, true));
    });
    define_promise_operation(interpreter, *container, "get", 0, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        return rejecting(interp, credential_operation(interp, args, false));
    });
    define_promise_operation(interpreter, *container, "store", 1, [](js::Interpreter& interp, js::Value const&, Args args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        if (!object_of<PublicKeyCredentialObject>(js::argument(args, 0))) {
            interp.throw_type_error("Failed to execute 'store' on 'CredentialsContainer': parameter 1 is not of type 'Credential'.");
            return rejected_promise(interp, interp.take_exception());
        }
        // PublicKeyCredential's [[Store]] (§5.1.5).
        return reject_dom(internals, "NotSupportedError", "A public-key credential cannot be stored.");
    });
    define_promise_operation(interpreter, *container, "preventSilentAccess", 0, [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        return resolved_promise(interp, js::Value::undefined());
    });
    define_getter(in, navigator, "credentials", [](js::Interpreter& interp, js::Value const&, Args) -> Native {
        Realm::Internals& internals = internals_of(interp);
        return js::Value::object(same_object(internals, "navigator.credentials", [&] {
            return internals.interpreter.heap().allocate<PlainPlatformObject>(internals.prototype("CredentialsContainer"), internals.realm_record);
        }));
    });
}

void trace_web_authn(Realm::Internals& in, js::Tracer& tracer)
{
    for (auto const& [id, call] : in.credential_calls) {
        tracer.visit(call->resolve);
        tracer.visit(call->reject);
        tracer.visit(call->signal);
    }
}

void end_credential_calls(Realm::Internals& in)
{
    // The document unloads: each ceremony is cancelled and its prompt
    // closed; the promises are let go with the document.
    std::map<std::uint64_t, std::shared_ptr<Realm::Internals::CredentialCall>> const calls = std::exchange(in.credential_calls, {});
    for (auto const& [id, call] : calls)
        cancel_ceremony(*call, false);
}

}
