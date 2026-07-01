#include "persistence/SecretStore.h"

#if defined(_WIN32)
#include <windows.h>
#include <wincred.h>
#endif

namespace secretstore {

#if defined(_WIN32)

namespace {
std::wstring targetName(const QString& provider) {
    return (QStringLiteral("AutoCoder/") + provider).toStdWString();
}
}  // namespace

bool store(const QString& provider, const QString& secret) {
    if (secret.isEmpty()) return clear(provider);

    std::wstring target = targetName(provider);
    const QByteArray blob = secret.toUtf8();

    CREDENTIALW cred = {};
    cred.Type           = CRED_TYPE_GENERIC;
    cred.TargetName     = const_cast<LPWSTR>(target.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(blob.constData()));
    cred.Persist        = CRED_PERSIST_LOCAL_MACHINE;
    static wchar_t userName[] = L"AutoCoder";
    cred.UserName       = userName;

    return CredWriteW(&cred, 0) != FALSE;
}

QString load(const QString& provider) {
    std::wstring target = targetName(provider);
    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &pcred) || !pcred) {
        return {};
    }
    QString result = QString::fromUtf8(
        reinterpret_cast<const char*>(pcred->CredentialBlob),
        static_cast<int>(pcred->CredentialBlobSize));
    CredFree(pcred);
    return result;
}

bool clear(const QString& provider) {
    std::wstring target = targetName(provider);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    return GetLastError() == ERROR_NOT_FOUND;
}

#else  // non-Windows: no secure store available.

bool store(const QString&, const QString&) { return false; }
QString load(const QString&) { return {}; }
bool clear(const QString&) { return true; }

#endif

}  // namespace secretstore
