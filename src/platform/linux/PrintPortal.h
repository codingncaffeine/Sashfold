#pragma once
// The portal print request on a bus named by its address, for a test's
// bus of its own (print_document takes the session's).
#include "platform/Print.h"
#include "platform/linux/DBus.h"

namespace sashfold::platform {

std::unique_ptr<PrintRequest> print_document_on(dbus::SocketAddress address, PrintJob job, std::function<void()> wake);

}
