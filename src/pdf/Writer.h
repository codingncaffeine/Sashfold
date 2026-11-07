#pragma once

// Writing a document back (ISO 32000-2 §7.5.6): an incremental update
// appended to the file's own bytes — the objects that changed, written
// afresh with the numbers they had, new ones after them, then a
// cross-reference section (a table or a stream, as the file's newest was)
// whose trailer names the one before. Nothing the file had is touched, so
// the saved copy opens in every reader as the original did, with the
// changes on top. Strings and streams of an encrypted file are encrypted
// with its key as its own are.
//
// What is written today: form field values (Form.h), each with the
// appearance a viewer draws for it, so other readers show the same.

#include "pdf/Document.h"
#include "pdf/Form.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sashfold::pdf {

// One object serialized as a file carries it (no "N G obj" wrapper).
std::string serialize_object(Object const& object);

// The document's bytes with the values applied as an update; the bytes
// unchanged when there are no values to write.
std::vector<std::uint8_t> save_with_form_values(Document const& doc, FormValues const& values);

}
