#pragma once

// A release that is fetched and put in place of the build that is running:
// the second half of updates (ui/UpdateCheck.h is the first). What is here
// has no network and no window in it — what a release's files are called,
// the check a downloaded one must pass, and its unpacking.
//
// A release is published with SHA256SUMS, the SHA-256 of each of its
// downloads as `sha256sum` writes them, and SHA256SUMS.sig, an Ed25519
// signature over that file's bytes by the release key, whose public half is
// built in below. A download is put in place only when the signature
// verifies under that key, the archive's own hash is the one listed under
// the name this build expects for that version, and the version is later
// than the one running — so a server that is not the project's, a download
// cut short, and an old release replayed are all refused. The check is made
// again from the bytes on disk by the program that starts next, before it
// trusts a byte of them: the folder they wait in is the profile's, which a
// page that took the browser over could have written.

#include "crypto/Ed25519.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sashfold::ui {

// The key releases are signed with. Its secret half is the maintainer's
// and is in no repository.
inline constexpr crypto::Ed25519PublicKey release_key = { 0x6c, 0xb1, 0xaa, 0x91, 0xe4, 0xf8, 0x90, 0x1e, 0x7e, 0xa7, 0xf5,
    0x87, 0xf7, 0x0c, 0x3b, 0x15, 0xff, 0xdd, 0xbb, 0x26, 0xaa, 0xf5, 0xd6, 0x6d, 0xda, 0x10, 0xf3, 0x52, 0x0f, 0x8c, 0x90, 0x77 };

// The download a build of this kind is replaced from, for a version:
// "sashfold-0.2.1-linux-x64.tar.gz". Empty where this build has no
// download that replaces it (another machine or system than the releases
// are made for).
std::string release_archive_name(std::string_view version);
// The folder at the top of that archive: "sashfold-0.2.1-linux-x64".
std::string release_folder_name(std::string_view version);
// Where a file of a release is fetched from.
std::string release_file_address(std::string_view version, std::string_view name);

// SHA256SUMS: a line a file, 64 hexadecimal digits, two spaces (or a space
// and `*`), the name. Nothing when a line is anything else.
struct ReleaseFile {
    std::string name;
    std::array<std::uint8_t, 32> sha256 {};
};
std::optional<std::vector<ReleaseFile>> parse_checksums(std::string_view text);

// The check: empty when the archive is the release `version` signed by
// `key` and later than `running`, else why it is not.
std::string release_refusal(std::string_view version, std::string_view running, std::span<std::uint8_t const> checksums,
    std::span<std::uint8_t const> signature, std::span<std::uint8_t const> archive,
    crypto::Ed25519PublicKey const& key = release_key);

// A file of a release, by its path under the archive's top folder.
struct ReleaseEntry {
    std::string path; // "sashfold", "themes/default.json"
    std::vector<std::uint8_t> bytes;
    bool executable = false;
};
// The files of a release archive — a .tar.gz (Linux's download) or a .zip
// (Windows'), told apart by their first bytes — whose every entry lies
// under `folder`. Nothing, with why, when it is not such an archive: not
// gzip and tar or zip, a file whose CRC-32 is not its own, an entry outside
// the folder or reaching out of it (`..`, an absolute path), or anything
// that is not a plain file or a folder (a link, a device, an encrypted or
// ZIP64 entry).
std::optional<std::vector<ReleaseEntry>> unpack_release(std::span<std::uint8_t const> archive, std::string_view folder,
    std::string* why_not = nullptr);

// Puts the files in `directory`, over what is there: each written whole
// beside its place and renamed into it, so that a file is the old one or
// the new one and never part of each. What the release does not hold is
// left alone. False, with why, at the first file that cannot be written;
// the files before it are in place.
bool install_release(std::string const& directory, std::vector<ReleaseEntry> const& entries, std::string* why_not = nullptr);

// --- From one run to the next -----------------------------------------------
//
// A release that passed the check waits in a folder of the profile — its
// archive, its list, the signature and a line naming its version — until
// the browser next starts. That start checks it again, from those bytes
// and before anything else is read, and only then puts it in place: the
// folder is one a page that took the browser over could write, so nothing
// in it is believed without the release key's word.

// Where a release waits: `update` in the profile.
std::string staging_folder(std::string const& profile);
// Keeps a release there for the next start, in place of whatever waited.
bool stage_release(std::string const& folder, std::string_view version, std::span<std::uint8_t const> checksums,
    std::span<std::uint8_t const> signature, std::span<std::uint8_t const> archive, std::string* why_not = nullptr);
// The version that waits in the folder, if one does (as the folder says
// it: not yet checked).
std::optional<std::string> staged_version(std::string const& folder);

// What a start found waiting and did with it.
struct StagedOutcome {
    enum class What {
        Nothing, // nothing waited
        Installed, // a release was checked and put in `install`
        Refused, // what waited did not pass the check, or could not be put in place
    };
    What what = What::Nothing;
    std::string version;
    std::string why; // of a refusal
};
// Checks what waits in `folder` against `running` and `key`, puts it in
// the folder `install` when it passes — the program that was there kept
// beside the new one as `<name>.previous` — and empties `folder` whatever
// came of it, so that a release that failed is not tried at every start.
StagedOutcome apply_staged_release(std::string const& folder, std::string const& install, std::string_view running,
    crypto::Ed25519PublicKey const& key = release_key);

// Whether the program in `install` may be replaced by a release: only a
// build that was made to be (the downloads; a package manager's copy and a
// build tree are left to whoever made them), in a folder this user can
// write.
bool can_replace_itself(std::string const& install);

// --- A new build's first start ----------------------------------------------
//
// A release put in place is on trial until it has shown it starts: the
// program before it starts it and waits, and if it dies before it has had
// its window up for a while, puts itself back, remembers the version as
// one that failed here so that it is not fetched again, and goes on. The
// new program says it has started by ending the trial; a start that finds
// a trial no one is watching (the machine went down in the middle of it)
// takes it as failed. Chrome and Firefox keep no such watch: a build that
// cannot start there leaves the reader with no browser.

// The file a trial is kept in: `update-trial` in the profile.
std::string trial_file(std::string const& profile);
struct Trial {
    std::string version; // on trial
    std::string previous; // the version it replaced
    std::uint64_t watcher = 0; // the process that watches it; 0 none
};
bool begin_trial(std::string const& file, Trial const& trial);
std::optional<Trial> read_trial(std::string const& file);
void end_trial(std::string const& file);
// How long the new program must have had its window up to have started.
inline constexpr int trial_seconds = 10;

// The versions that failed their first start here, from the profile; one
// that has is not fetched again (a later one is).
std::vector<std::string> refused_releases(std::string const& profile);
void refuse_release(std::string const& profile, std::string_view version);

// Puts `<program>.previous` back in the program's place: false, with why,
// when there is none or it cannot be put there.
bool restore_previous(std::string const& program, std::string* why_not = nullptr);

}
