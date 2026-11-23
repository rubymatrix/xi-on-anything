#include "pch.h"

#include "GameFolder.h"

#include <fstream>

#include "Options.h"

using namespace winrt;

namespace
{
    bool holds_game(std::wstring const& folder)
    {
        return !folder.empty() && GetFileAttributesW((folder + L"\\FFXiMain.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::wstring game_txt()
    {
        std::ifstream f(LocalFolder() + L"\\game.txt");
        std::string line;
        if (!f || !std::getline(f, line))
            return L"";
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3); // a UTF-8 byte order mark
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\\' || line.back() == '/'))
            line.pop_back();
        while (!line.empty() && line.front() == ' ')
            line.erase(0, 1);
        return to_hstring(line).c_str();
    }

    std::wstring beside_app()
    {
        try
        {
            std::wstring root = Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str();
            return root + L"\\SquareEnix\\FINAL FANTASY XI";
        }
        catch (hresult_error const&)
        {
            return L"";
        }
    }
}

std::wstring FindGameFolder()
{
    std::wstring named = game_txt();
    if (!named.empty())
        return named; // named on purpose: used even if it cannot be read, so the error says which
    for (std::wstring const& f : { beside_app(), LocalFolder() + L"\\SquareEnix\\FINAL FANTASY XI" })
        if (holds_game(f))
            return f;
    return L"";
}
