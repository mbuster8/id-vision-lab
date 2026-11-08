#pragma once

#include <QString>

namespace test_support {

// Whether no account but the owner can read or change the file or folder at `path`. On macOS and Linux: it
// has no group or other permission bits. On Windows, where Qt's permission bits cannot say this, its access
// list lets only the current user, SYSTEM and Administrators (the inherited list of the user's own app data
// folders; Administrators can take any file anyway) do more than read its attributes and access list. False
// when it cannot be read.
bool isOwnerOnly(const QString& path);

// Lets every account read the file at `path`, so a test can check that isOwnerOnly notices.
bool letEveryoneRead(const QString& path);

} // namespace test_support
