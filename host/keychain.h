/* Saved sign-in passwords in the OS's credential store: the macOS keychain (generic passwords
 * under one service), the Windows Credential Manager (generic credentials), the freedesktop Secret
 * Service on Linux (GNOME Keyring, KWallet, KeePassXC: through libsecret, loaded when first used, so
 * it is no build dependency), or the credential locker in the UWP app (xbox/app/Keychain.cpp).
 * Without one (no libsecret, no Secret Service running) every call fails and the player types the
 * password each time. */
#pragma once

#include <stddef.h>

/* The password saved for key, into out. 0 when there is none (or no store). */
int keychain_get(const char* key, char* out, size_t n);
/* Saves (or replaces) key's password. 0 on failure. */
int keychain_set(const char* key, const char* password);
void keychain_delete(const char* key);
/* 1 if there may be a store to save into (on Linux: libsecret loads; whether a Secret Service
 * answers is only known when it is used). */
int keychain_available(void);
