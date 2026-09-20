#include "platform/Audio.h"

// Sound where this build has no way out to the speakers yet: the seam
// opens nothing and says so, and the engine plays silence rather than
// pretending. Windows (the session interface) and macOS (the OS audio
// units) follow the Linux client.

namespace sashfold::platform {

std::unique_ptr<AudioDevice> AudioDevice::open(AudioFormat const&, std::string const&, std::string& error)
{
    error = "this build has no way to reach the machine's speakers";
    return nullptr;
}

}
