// host/keychain.h for the UWP build: the sign-in screen's remembered passwords and the server's trust
// tokens, in the Windows credential locker (PasswordVault), one entry per key under one resource.
// Called from the sign-in's worker threads, never the UI thread.
#include "pch.h"

extern "C" {
#include "keychain.h"
}

using namespace winrt;
using namespace Windows::Security::Credentials;

namespace
{
    constexpr wchar_t RESOURCE[] = L"XI on Xbox";
}

extern "C" int keychain_get(const char* key, char* out, size_t n)
{
    try
    {
        PasswordCredential c = PasswordVault().Retrieve(RESOURCE, to_hstring(key));
        c.RetrievePassword();
        std::string v = to_string(c.Password());
        if (v.size() >= n)
            return 0;
        memcpy(out, v.c_str(), v.size() + 1);
        return 1;
    }
    catch (hresult_error const&) // none saved
    {
        return 0;
    }
}

extern "C" int keychain_set(const char* key, const char* password)
{
    keychain_delete(key);
    try
    {
        PasswordVault().Add(PasswordCredential(RESOURCE, to_hstring(key), to_hstring(password)));
        return 1;
    }
    catch (hresult_error const& e)
    {
        fprintf(stderr, "[app] credential locker: %s\n", to_string(e.message()).c_str());
        return 0;
    }
}

extern "C" void keychain_delete(const char* key)
{
    try
    {
        PasswordVault vault;
        vault.Remove(vault.Retrieve(RESOURCE, to_hstring(key)));
    }
    catch (hresult_error const&) // none saved
    {
    }
}
