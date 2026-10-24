#include "pch.h"

#include "Display.h"

using namespace winrt;
using namespace Windows::Graphics::Display;
using namespace Windows::Graphics::Display::Core;

Resolution ScreenResolution()
{
    try // Xbox: the TV's mode. Not available on a PC, where it is null or throws.
    {
        if (HdmiDisplayInformation hdmi = HdmiDisplayInformation::GetForCurrentView())
            if (HdmiDisplayMode mode = hdmi.GetCurrentDisplayMode())
                if (mode.ResolutionWidthInRawPixels() && mode.ResolutionHeightInRawPixels())
                    return { (int)mode.ResolutionWidthInRawPixels(), (int)mode.ResolutionHeightInRawPixels() };
    }
    catch (hresult_error const&)
    {
    }
    auto b = Windows::UI::Core::CoreWindow::GetForCurrentThread().Bounds();
    double scale = DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
    return { (int)(b.Width * scale + 0.5), (int)(b.Height * scale + 0.5) };
}

Resolution ParseResolution(std::string const& text)
{
    int w = 0, h = 0;
    if (sscanf(text.c_str(), "%dx%d", &w, &h) != 2 || w < 640 || h < 480 || w > 7680 || h > 4320)
        return { 0, 0 };
    return { w, h };
}

static int BackgroundResolution(Resolution r)
{
    int bg = 1024;
    while (bg < r.first && bg < 2048)
        bg *= 2;
    return bg;
}

bool SetGameResolution(std::wstring const& reg_path, Resolution r)
{
    auto [w, h] = r;
    int ui = h >= 1080 ? 2 : 1;
    int bg = BackgroundResolution(r);
    char const* keys[] = { "\"0001\"", "\"0002\"", "\"0037\"", "\"0038\"", "\"0003\"", "\"0004\"" };
    int values[] = { w, h, w / ui, h / ui, bg, bg };
    constexpr int nkeys = (int)std::size(keys);
    auto line_for = [](char const* key, int v) {
        char s[64];
        snprintf(s, sizeof s, "%s=dword:%08x", key, (unsigned)v);
        return std::string(s);
    };

    std::vector<std::string> lines;
    if (FILE* f = _wfopen(reg_path.c_str(), L"rb"))
    {
        char buf[4096];
        while (fgets(buf, sizeof buf, f))
        {
            std::string l = buf;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    if (lines.empty())
        lines.push_back("REGEDIT4");

    // the game's key, with or without WOW6432Node (the runtime folds it away)
    auto is_game_key = [](std::string const& l) {
        return l.size() > 2 && l.front() == '[' && l.find("\\SquareEnix\\FinalFantasyXI]") != std::string::npos;
    };
    bool done[nkeys] = {};
    bool in_game = false;
    size_t section_end = std::string::npos; // where missing values go
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (!lines[i].empty() && lines[i].front() == '[')
        {
            in_game = is_game_key(lines[i]);
            if (in_game)
                section_end = i + 1;
            continue;
        }
        if (!in_game)
            continue;
        section_end = i + 1;
        for (int k = 0; k < nkeys; ++k)
            if (lines[i].rfind(keys[k], 0) == 0 && lines[i].size() > strlen(keys[k]) && lines[i][strlen(keys[k])] == '=')
                lines[i] = line_for(keys[k], values[k]), done[k] = true;
    }
    if (section_end == std::string::npos)
    {
        lines.push_back("");
        lines.push_back("[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]");
        section_end = lines.size();
    }
    for (int k = nkeys - 1; k >= 0; --k)
        if (!done[k])
            lines.insert(lines.begin() + (ptrdiff_t)section_end, line_for(keys[k], values[k]));

    FILE* f = _wfopen(reg_path.c_str(), L"wb");
    if (!f)
        return false;
    for (auto const& l : lines)
        fprintf(f, "%s\r\n", l.c_str());
    fclose(f);
    return true;
}
