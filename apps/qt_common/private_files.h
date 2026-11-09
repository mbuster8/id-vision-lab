#pragma once

#include <QFileDevice>
#include <QIODevice>
#include <QString>

#include <functional>

// Images may be private, and the log names them: everything the viewer keeps is readable by its owner only.
inline constexpr QFileDevice::Permissions kOwnerOnlyFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
inline constexpr QFileDevice::Permissions kOwnerOnlyFolder = kOwnerOnlyFile | QFileDevice::ExeOwner;

// Makes the folder at `path`, with any parents it lacks, and lets only its owner in, whatever the folder
// above passes on. On macOS and Linux through its permission bits. On Windows, where Qt's bits do not reach
// access lists, through a protected access list of its own: the current user, SYSTEM and Administrators (as
// in the user's own app data folder), which everything made inside it inherits, and which also replaces what
// files already there inherited. False when it cannot be made, or made private.
bool makePrivateFolder(const QString& path);

// Replaces the file at `path` all at once, through QSaveFile, with what `write` puts into the device, and
// makes it owner-only. A crash or a failed write leaves the previous file as it was. `write` returns false to
// give up. False on failure, with the reason in `error` when it is given.
bool writeFileAtomically(const QString& path, const std::function<bool(QIODevice&)>& write,
                         QString* error = nullptr);
