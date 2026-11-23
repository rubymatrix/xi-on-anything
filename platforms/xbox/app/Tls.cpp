// The LandSandBoat sign-in's TLS (uwp_bridge.h): SChannel is not available to UWP apps, so the one
// request and its reply go over Windows.Networking.Sockets. Called from the sign-in's worker thread.
#include "pch.h"

#include "uwp_bridge.h"

using namespace winrt;
using namespace Windows::Networking;
using namespace Windows::Networking::Sockets;
using namespace Windows::Security::Cryptography::Certificates;
using namespace Windows::Storage::Streams;

extern "C" int uwp_tls_exchange(uint32_t server, uint16_t port, const char* request, char* reply, size_t replyn, char* err,
    size_t errn)
{
    char host[16];
    snprintf(host, sizeof host, "%u.%u.%u.%u", server >> 24, (server >> 16) & 255, (server >> 8) & 255, server & 255);
    try
    {
        StreamSocket s;
        // private servers present self-signed certificates, as xiloader accepts
        for (auto e : { ChainValidationResult::Untrusted, ChainValidationResult::InvalidName, ChainValidationResult::Expired,
                        ChainValidationResult::IncompleteChain, ChainValidationResult::WrongUsage,
                        ChainValidationResult::RevocationInformationMissing, ChainValidationResult::RevocationFailure })
            s.Control().IgnorableServerCertificateErrors().Append(e);
        s.ConnectAsync(HostName(to_hstring(host)), to_hstring(port), SocketProtectionLevel::Tls12).get();
        DataWriter w(s.OutputStream());
        w.WriteBytes(array_view<uint8_t const>((uint8_t const*)request, (uint32_t)strlen(request)));
        w.StoreAsync().get();
        w.DetachStream();
        DataReader r(s.InputStream());
        r.InputStreamOptions(InputStreamOptions::Partial);
        uint32_t n = r.LoadAsync((uint32_t)(replyn - 1)).get(); // the first record with data in it
        if (!n)
        {
            snprintf(err, errn, "the login server did not reply");
            return 0;
        }
        std::vector<uint8_t> buf(n);
        r.ReadBytes(buf);
        memcpy(reply, buf.data(), n);
        reply[n] = 0;
        return 1;
    }
    catch (hresult_error const& e)
    {
        snprintf(err, errn, "TLS to %s:%u: %s", host, port, to_string(e.message()).c_str());
        return 0;
    }
}
