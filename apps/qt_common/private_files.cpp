#include "private_files.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>

#ifdef _WIN32
// Kept to this file: <windows.h> defines min and max macros and global functions named Polygon and
// Rectangle, which would clash with the viewer's own code.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <aclapi.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

struct LocalFreeDeleter {
    void operator()(void* memory) const { LocalFree(memory); }
};

// The current user's SID, copied out of the process token; empty when it cannot be read.
std::vector<BYTE> currentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::vector<BYTE> sid;
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        const PSID user = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
        const auto* bytes = static_cast<const BYTE*>(user);
        sid.assign(bytes, bytes + GetLengthSid(user));
    }
    CloseHandle(token);
    return sid;
}

std::vector<BYTE> wellKnownSid(WELL_KNOWN_SID_TYPE type) {
    std::vector<BYTE> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(type, nullptr, sid.data(), &size)) {
        return {};
    }
    sid.resize(size);
    return sid;
}

// Full control for the user, SYSTEM and Administrators, passed on to everything inside, and nothing from the
// folder above: PROTECTED stops it inheriting. Setting it also reaches what the folder already holds.
bool setOwnerOnlyAccessList(const QString& path) {
    const std::array<std::vector<BYTE>, 3> sids{currentUserSid(), wellKnownSid(WinLocalSystemSid),
                                                wellKnownSid(WinBuiltinAdministratorsSid)};
    std::array<EXPLICIT_ACCESS_W, 3> entries{};
    for (std::size_t i = 0; i < sids.size(); ++i) {
        if (sids[i].empty()) {
            return false;
        }
        entries[i].grfAccessPermissions = FILE_ALL_ACCESS;
        entries[i].grfAccessMode = SET_ACCESS;
        entries[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        entries[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        entries[i].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        entries[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(const_cast<BYTE*>(sids[i].data()));
    }
    PACL rawList = nullptr;
    if (SetEntriesInAclW(static_cast<ULONG>(entries.size()), entries.data(), nullptr, &rawList) !=
        ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, LocalFreeDeleter> list(rawList);
    std::wstring native = QDir::toNativeSeparators(path).toStdWString(); // the call takes a non-const path
    return SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                 nullptr, rawList, nullptr) == ERROR_SUCCESS;
}

} // namespace
#endif

bool makePrivateFolder(const QString& path) {
    if (!QDir().mkpath(path)) {
        return false;
    }
#ifdef _WIN32
    return setOwnerOnlyAccessList(path);
#else
    return QFile::setPermissions(path, kOwnerOnlyFolder);
#endif
}

bool writeFileAtomically(const QString& path, const std::function<bool(QIODevice&)>& write, QString* error) {
    QSaveFile file(path);
    const auto fail = [error](const QString& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(file.errorString());
    }
    if (!write(file)) {
        file.cancelWriting();
        return fail(file.errorString());
    }
    // QSaveFile notices a failed write and refuses to commit it.
    if (!file.commit()) {
        return fail(file.errorString());
    }
    if (!QFile::setPermissions(path, kOwnerOnlyFile)) {
        return fail(QCoreApplication::translate("PrivateFiles", "cannot make [%1] private").arg(path));
    }
    return true;
}
