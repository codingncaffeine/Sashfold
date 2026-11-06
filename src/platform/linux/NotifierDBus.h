#pragma once

// The notifier over a bus named by its socket: what open_notifier() makes
// for the session's bus, and what a test makes for a bus of its own.

#include "platform/Notifier.h"
#include "platform/linux/DBus.h"

namespace sashfold::platform {

std::unique_ptr<Notifier> open_notifier_on(dbus::SocketAddress address, std::string application, std::function<void()> wake);

}
