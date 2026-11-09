#include "owner_only.h"

#include <QFile>
#include <QFileDevice>
#include <QFileInfo>

#ifdef _WIN32
// Kept to this file: <windows.h> defines min and max macros and global functions named Polygon and
// Rectangle, which would clash with code in the tests that include it.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <aclapi.h>

#include <QDir>

#include <memory>
#include <string>
#include <vector>
#endif

namespace test_support {

#ifdef _WIN32
namespace {

struct LocalFreeDeleter {
    void operator()(void* memory) const { LocalFree(memory); }
};

// What anyone may have without seeing or changing the contents, as anyone may stat() a POSIX 0600 file. Qt
// gives the owner's primary group exactly these on the folders QTemporaryDir creates.
constexpr ACCESS_MASK kHarmlessRights = FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;

std::wstring nativePath(const QString& path) { return QDir::toNativeSeparators(path).toStdWString(); }

// The current user's SID, copied out of the process token.
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

} // namespace
#endif

bool isOwnerOnly(const QString& path) {
    if (!QFileInfo::exists(path)) {
        return false;
    }
#ifdef _WIN32
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    if (GetNamedSecurityInfoW(nativePath(path).c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &acl, nullptr, &rawDescriptor) != ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, LocalFreeDeleter> descriptor(rawDescriptor);
    if (acl == nullptr) {
        return false; // no access list at all lets everyone in
    }
    auto user = currentUserSid();
    auto system = wellKnownSid(WinLocalSystemSid);
    auto administrators = wellKnownSid(WinBuiltinAdministratorsSid);
    if (user.empty() || system.empty() || administrators.empty()) {
        return false;
    }
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void* entry = nullptr;
        if (!GetAce(acl, i, &entry)) {
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(entry);
        if ((header->AceFlags & INHERIT_ONLY_ACE) != 0 || header->AceType == ACCESS_DENIED_ACE_TYPE) {
            continue; // it applies only to what the folder will hold, or it takes access away
        }
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            return false; // a kind of entry this check does not understand
        }
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        PSID sid = const_cast<DWORD*>(&allowed->SidStart);
        const bool trusted = EqualSid(sid, user.data()) || EqualSid(sid, system.data()) ||
                             EqualSid(sid, administrators.data());
        if (!trusted && (allowed->Mask & ~kHarmlessRights) != 0) {
            return false;
        }
    }
    return true;
#else
    constexpr QFileDevice::Permissions kGroupOrOthers = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                                        QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                        QFileDevice::WriteOther | QFileDevice::ExeOther;
    return !(QFileInfo(path).permissions() & kGroupOrOthers);
#endif
}

#ifdef _WIN32
namespace {

// Adds an entry that lets everyone read `path`, passed on to what it holds as `inheritance` says.
bool grantEveryoneRead(const QString& path, DWORD inheritance) {
    const std::wstring native = nativePath(path);
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    if (GetNamedSecurityInfoW(native.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                              &acl, nullptr, &rawDescriptor) != ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, LocalFreeDeleter> descriptor(rawDescriptor);
    auto everyone = wellKnownSid(WinWorldSid);
    if (everyone.empty()) {
        return false;
    }
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = GENERIC_READ;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = inheritance;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(everyone.data());
    PACL rawWidened = nullptr;
    if (SetEntriesInAclW(1, &access, acl, &rawWidened) != ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<void, LocalFreeDeleter> widened(rawWidened);
    std::wstring writable = native; // SetNamedSecurityInfoW takes a non-const path
    return SetNamedSecurityInfoW(writable.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                 rawWidened, nullptr) == ERROR_SUCCESS;
}

} // namespace
#endif

bool letEveryoneRead(const QString& path) {
#ifdef _WIN32
    return grantEveryoneRead(path, NO_INHERITANCE);
#else
    return QFile::setPermissions(path,
                                 QFile::permissions(path) | QFileDevice::ReadGroup | QFileDevice::ReadOther);
#endif
}

bool letEveryoneReadWhatItHolds(const QString& path) {
#ifdef _WIN32
    return grantEveryoneRead(path, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
#else
    return QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ReadGroup |
                                           QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                           QFileDevice::ExeOther);
#endif
}

} // namespace test_support
