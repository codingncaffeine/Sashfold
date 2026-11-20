#pragma once
// Security keys on Linux, without any library: the kernel's hidraw
// interface. A key is a /sys/class/hidraw/hidrawN whose
// device/report_descriptor declares the FIDO usage (auth/HidDescriptor.h);
// it is spoken to through /dev/hidrawN, one report per read() or write(),
// the report ID first when the device numbers its reports. Keys that come
// and go are noticed with inotify on /dev (the node is made, then udev
// grants the seat's user access to it, both events).
//
// Under the browser's confinement (platform/Confine.h) the nodes of the
// keys present at startup are kept writable; a key plugged in later cannot
// be opened for writing, so it is opened for reading and its output
// reports are sent with the HIDIOCSOUTPUT request instead of write(),
// which reaches the same kernel path. (The confinement restricts what is
// written to files; it leaves device requests alone.)
//
// The two roots are parameters so that tests can point the transport at a
// tree of their own; the shipped browser uses /sys/class/hidraw and /dev.
#include "auth/Transport.h"

#include <memory>
#include <string>
#include <vector>

namespace sashfold::platform {

std::unique_ptr<auth::Transport> hidraw_transport(std::string sysfs_root = "/sys/class/hidraw", std::string device_root = "/dev");

// The device nodes of the FIDO keys present now, for the confinement's
// list of devices kept writable.
std::vector<std::string> security_key_nodes(std::string const& sysfs_root = "/sys/class/hidraw", std::string const& device_root = "/dev");

}
