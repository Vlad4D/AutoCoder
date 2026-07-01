#pragma once

#include <QString>

// OS-backed secret storage for API keys. On Windows this uses the Windows
// Credential Manager (generic credentials, encrypted per-user via DPAPI), so
// keys are not kept in plaintext QSettings/registry. On other platforms the
// functions are no-ops returning empty/false (the app is Windows-targeted).
namespace secretstore {

// Store `secret` for `provider` (e.g. "deepseek", "claude"). Empty secret
// clears the entry. Returns true on success.
bool store(const QString& provider, const QString& secret);

// Load the secret for `provider`, or an empty string if none is stored.
QString load(const QString& provider);

// Remove any stored secret for `provider`. Returns true if removed or absent.
bool clear(const QString& provider);

}  // namespace secretstore
