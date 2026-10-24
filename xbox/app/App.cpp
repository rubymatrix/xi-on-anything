// XI on Xbox: the UWP app. A bare CoreWindow, no XAML: the game host draws everything on it with
// Direct3D 12, from its own sign-in screen (host/signin.c, in the game's UI art) into the game. The
// app gives it the window and its input, sound, the on-screen keyboard, the sign-in's TLS and the
// credential locker (uwp_bridge.h, keychain.h).
#include "pch.h"

#include "Game.h"
#include "Input.h"
#include "Options.h"
#include "TextInput.h"

#include "uwp_bridge.h"

using namespace winrt;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;

namespace
{
    CoreWindow g_window{ nullptr };

    // What went wrong before the game's log is open: LocalState\app.log (on the console, through
    // Device Portal's file explorer).
    void log(std::wstring const& text)
    {
        try
        {
            if (FILE* f = _wfopen((LocalFolder() + L"\\app.log").c_str(), L"a, ccs=UTF-8"))
            {
                fwprintf(f, L"%s\n", text.c_str());
                fclose(f);
            }
        }
        catch (...)
        {
        }
        OutputDebugStringW((text + L"\n").c_str());
    }
}

// uwp_bridge.h: the window the swap chain goes on
extern "C" void* uwp_core_window(void) { return g_window ? get_unknown(g_window) : nullptr; }

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const& view)
    {
        view.Activated([](CoreApplicationView const&, IActivatedEventArgs const&) { CoreWindow::GetForCurrentThread().Activate(); });
        Input::WatchControllers();
    }

    void SetWindow(CoreWindow const& window)
    {
        g_window = window;
        // Xbox: the whole TV, not the title-safe area inside it
        ApplicationView::GetForCurrentView().SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
        Input::Attach(window);
        TextInput::Attach(window);
    }

    void Load(hstring const&) {}

    void Run()
    {
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        try
        {
            Game::Start();
        }
        catch (hresult_error const& e)
        {
            log(L"starting the game: " + std::wstring(e.message().c_str()) + L" (" + std::to_wstring(e.code()) + L")");
            throw;
        }
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessUntilQuit);
    }

    void Uninitialize() {}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    init_apartment();
    CoreApplication::Run(make<App>());
    return 0;
}
