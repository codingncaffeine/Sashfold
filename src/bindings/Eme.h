#pragma once

// Encrypted Media Extensions between its two halves: the key system's
// objects (Eme.cpp — MediaKeySystemAccess, MediaKeys, MediaKeySession and
// their events, with the Clear Key key system of media/ClearKey.h) and the
// media element that plays with them (Media.cpp — mediaKeys,
// setMediaKeys, the encrypted and waitingforkey events and the waiting
// for a key while it plays).

#include "bindings/Internal.h"

#include <memory>

namespace sashfold::media {
class KeyStore;
}

namespace sashfold::bindings {

// --- Eme.cpp ------------------------------------------------------------------------------

// MediaKeySystemAccess, MediaKeys, MediaKeySession, MediaKeyStatusMap,
// MediaKeyMessageEvent and MediaEncryptedEvent.
void install_eme(Realm::Internals&);
// navigator.requestMediaKeySystemAccess (EME §3.1.1.1).
Native request_media_key_system_access(js::Interpreter&, Args);
// What an operation of Promise type answers: its result, or, where its body
// threw, a promise rejected with the exception (WebIDL §3.7.7).
Native as_promise(js::Interpreter&, Native result);
// Whether a value is a MediaKeys object, and the keys its sessions hold.
std::shared_ptr<media::KeyStore const> media_keys_store(js::Value const&);
// A MediaKeys is used by one media element at a time (EME §7.2, step
// 4.1): the element whose state object this is claims it, false when
// another one has; and lets it go.
bool claim_media_keys(js::Value const& media_keys, js::Object* element_state);
void release_media_keys(js::Value const& media_keys, js::Object* element_state);

// --- Media.cpp ----------------------------------------------------------------------------

// "Attempt to Resume Playback If Necessary" (EME §7.3.5) on the element
// whose state object this is: keys its MediaKeys holds have changed.
void media_keys_changed(Realm::Internals&, js::Object* element_state);

}
