#include "text/ShaperInternal.h"

namespace sashfold::text::shaping {

ScriptShaper const arabic_shaper {};

void arabic_fallback_shape(Plan const&, ShapeFont const&, Buffer&) { }

}
