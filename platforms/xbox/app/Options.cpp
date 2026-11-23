#include "pch.h"

#include "Options.h"

#include <fstream>

extern "C" {
#include "appdefaults.h"
}

using namespace winrt;
using namespace Windows::Storage;

namespace
{
    // host64's defaults where xbox.cfg gives none
    struct
    {
        char const* key;
        char const* value;
    } const BUILT_IN[] = {
        { "FFXIServer", "game.phoenix-xi.com" },
    };
}

std::wstring LocalFolder() { return std::wstring(ApplicationData::Current().LocalFolder().Path().c_str()); }

std::string Option(char const* key, std::string const& fallback)
{
    std::ifstream f(LocalFolder() + L"\\xbox.cfg");
    std::string line, found = fallback;
    while (f && std::getline(f, line))
    {
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3); // a UTF-8 byte order mark
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || line.compare(0, eq, key) != 0 || eq != strlen(key))
            continue;
        found = line.substr(eq + 1);
    }
    return found;
}

// host/appdefaults.h, for the UWP build: xbox.cfg's FFXI* keys, then BUILT_IN.
extern "C" int app_default(const char* key, char* out, size_t n)
{
    std::string v = Option(key);
    if (v.empty())
        for (auto const& b : BUILT_IN)
            if (!strcmp(b.key, key))
                v = b.value;
    if (v.empty() || v.size() >= n)
        return 0;
    memcpy(out, v.c_str(), v.size() + 1);
    return 1;
}

// No bundled resources (FFXIBackground's picture): the app's package has none.
extern "C" int app_resource(const char* name, char* out, size_t n)
{
    (void)name, (void)out, (void)n;
    return 0;
}
