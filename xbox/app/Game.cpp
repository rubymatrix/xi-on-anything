#include "pch.h"

#include "Game.h"

#include "Audio.h"
#include "Display.h"
#include "GameFolder.h"
#include "Options.h"

extern "C" int host_main(int argc, char** argv); // host/host64.c, built with /Dmain=host_main

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using namespace Windows::UI::Popups;

namespace
{
    std::string utf8(std::wstring const& w) { return to_string(w); }

    // Copies the files under from that are missing under to, folders included; never overwrites. The
    // install's USER folder (per character: cnf.dat - the Config menu, camera included - key
    // bindings, macros) seeds the app's, so a player keeps their settings. 0: nothing to copy.
    unsigned copy_missing(std::wstring const& from, std::wstring const& to)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((from + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE)
            return 0;
        unsigned copied = 0;
        CreateDirectoryW(to.c_str(), nullptr);
        do
        {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..")
                continue;
            std::wstring src = from + L"\\" + name, dst = to + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                copied += copy_missing(src, dst);
            else if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES && SUCCEEDED(CopyFile2(src.c_str(), dst.c_str(), nullptr)))
                ++copied;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return copied;
    }

    // The game's display settings (host64 --data-dir's settings.reg, which the sign-in screen leaves
    // alone once it is there): windowed (the view is the window) and the retail controller layout.
    // Written once; the resolutions are set on every start (SetGameResolution).
    void write_default_settings(std::wstring const& path)
    {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return;
        FILE* f = _wfopen(path.c_str(), L"w");
        if (!f)
            return;
        fprintf(f,
            "REGEDIT4\n\n"
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI]\n"
            "\"0000\"=dword:00000006\n"          // mip mapping
            "\"0007\"=dword:00000001\n"          // sound
            "\"0011\"=dword:00000001\n"          // environment animation
            "\"0017\"=dword:00000001\n"          // bump mapping
            "\"0018\"=dword:00000001\n"          // texture compression
            "\"0019\"=dword:00000001\n"          // map compression
            "\"0022\"=dword:00000001\n"          // hardware mouse
            "\"0034\"=dword:00000001\n"          // windowed
            "\"0035\"=dword:00000001\n"          // always on top
            "\"padmode000\"=\"1,1,0,0,0,1\"\n"
            "\"padsin000\"=\"8,9,13,12,10,0,1,3,2,15,-1,-1,14,-33,-33,32,32,-36,-36,35,35,6,7,5,4,11,-1\"\n");
        fclose(f);
    }

    // The last lines of the log, for a message.
    std::wstring log_tail(std::wstring const& path, int lines)
    {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f)
            return L"";
        fseek(f, 0, SEEK_END);
        long size = ftell(f), from = size > 4000 ? size - 4000 : 0;
        fseek(f, from, SEEK_SET);
        std::string tail((size_t)(size - from), '\0');
        tail.resize(fread(tail.data(), 1, tail.size(), f));
        fclose(f);
        size_t cut = 0;
        int n = 0;
        for (size_t i = tail.size(); i-- > 0;)
            if (tail[i] == '\n' && ++n > lines)
            {
                cut = i + 1;
                break;
            }
        return to_hstring(tail.substr(cut)).c_str();
    }

    // A message, then the app closes. On the UI thread.
    fire_and_forget fail(std::wstring text)
    {
        fprintf(stderr, "[app] %s\n", utf8(text).c_str());
        MessageDialog d(text, L"XI on Xbox");
        co_await d.ShowAsync();
        CoreApplication::Exit();
    }
}

void Game::Start()
{
    std::wstring local = LocalFolder();
    std::wstring log = local + L"\\host64.log";
    // stdout and stderr (every [recomp], [gfx], [signin] and [app] line) to LocalState
    _wfreopen(log.c_str(), L"w", stderr);
    _wfreopen((local + L"\\host64.out.log").c_str(), L"w", stdout);
    setvbuf(stderr, nullptr, _IONBF, 0);

    std::wstring game = FindGameFolder();
    if (game.empty())
    {
        fail(L"FINAL FANTASY XI was not found.\n\nPut the install beside the app (xbox/tools/xbox.py setup), or name its folder in "
             L"LocalState\\game.txt.");
        return;
    }
    DWORD attrs = GetFileAttributesW(game.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        fail(L"The app cannot open the FINAL FANTASY XI folder\n" + game + L"\n(error " + std::to_wstring(GetLastError()) +
             L"). It has to exist, and be readable by apps.");
        return;
    }

    unsigned seeded = copy_missing(game + L"\\USER", local + L"\\USER");
    CreateDirectoryW((local + L"\\USER").c_str(), nullptr);
    std::wstring settings = local + L"\\settings.reg";
    write_default_settings(settings);
    Resolution chosen = ParseResolution(Option("Resolution"));
    Resolution res = chosen.first ? chosen : ScreenResolution();
    SetGameResolution(settings, res);

    // the host's command line: argv[0] in LocalState, where it writes patch.<version>.ver; no
    // --user, so the sign-in screen comes first, keeping signin.cfg beside settings.reg
    std::vector<std::string> args = { utf8(local + L"\\host64.exe"), "--game", utf8(game), "--user-dir", utf8(local + L"\\USER"),
                                      "--data-dir", utf8(local) };
    bool profile = Option("Profile") == "1";
    _putenv_s("FFXI_PROFILE", profile ? "1" : "0");
    _putenv_s("FFXI_CACHE_DIR", utf8(local).c_str()); // the D3D12 pipeline cache
    if (profile) // where the game thread's time goes (tools/sample_report.py)
        _putenv_s("FFXI_SAMPLE", utf8(local + L"\\samples.bin").c_str());
    fprintf(stderr, "[app] starting the game: %s at %dx%d (%s)\n", utf8(game).c_str(), res.first, res.second,
        chosen.first ? "xbox.cfg" : "the screen's");
    if (seeded)
        fprintf(stderr, "[app] USER: %u file(s) copied from the install's USER folder (settings, key bindings, macros)\n", seeded);

    StartAudio(); // before the game opens its DirectSound device

    CoreDispatcher ui = CoreWindow::GetForCurrentThread().Dispatcher();
    std::thread([args, ui, log]() mutable {
        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        int code = host_main((int)args.size(), argv.data());
        fprintf(stderr, "[app] the game host returned %d\n", code);
        fflush(stderr);
        ui.RunAsync(CoreDispatcherPriority::Normal, [code, log] {
            if (code)
                fail(L"The game ended with an error (" + std::to_wstring(code) + L").\n\n" + log_tail(log, 12));
            else
                CoreApplication::Exit();
        });
    }).detach();
}
