#pragma once

// A census of what scripts looked for on the platform's objects and did not
// find: a property read that ran off the end of the prototype chain, or an
// `in` that answered no, on the global object or on an object that says
// what it is (a @@toStringTag somewhere up its chain — every interface's
// prototype has one). With SASHFOLD_MISS_CENSUS=<file> in the environment
// the counts are written there as the process ends: how often, on what, by
// what name, and whether it was read or asked for with `in`.
//
// It is the engine's own account of a page: a name a page asks for a
// hundred times and never finds is an interface member nobody wrote yet,
// and this is the list of them, in the order a page cares.

#include "js/Value.h"

namespace sashfold::js {

class Object;

// Whether the census is being taken (read once from the environment).
bool miss_census_on();
// A lookup of `key` that began at `object` found nothing.
void note_miss(Object const& object, PropertyKey const& key, bool asked_in);

}
