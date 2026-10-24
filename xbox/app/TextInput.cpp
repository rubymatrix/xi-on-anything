#include "pch.h"

#include "TextInput.h"

#include "uwp_bridge.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Text::Core;
using namespace Windows::UI::ViewManagement::Core;

namespace
{
    CoreDispatcher g_ui{ nullptr };
    CoreWindow g_window{ nullptr };
    CoreTextEditContext g_context{ nullptr };
    std::wstring g_text;      // what the keyboard is editing: the field's text
    CoreTextRange g_selection{ 0, 0 };
    std::atomic<bool> g_up;

    CoreTextRange clamped(CoreTextRange r)
    {
        int32_t n = (int32_t)g_text.size();
        r.StartCaretPosition = std::clamp(r.StartCaretPosition, 0, n);
        r.EndCaretPosition = std::clamp(r.EndCaretPosition, r.StartCaretPosition, n);
        return r;
    }

    bool g_away; // the window lost focus while the keyboard was up (the console's keyboard takes it)
    bool g_seen; // ... or the keyboard said it covers part of the window: either way it is there
    unsigned g_shown; // counts the keyboards opened, so a late check knows which one it is for

    void hidden(char const* why)
    {
        if (!g_up.exchange(false))
            return;
        g_away = g_seen = false;
        g_context.NotifyFocusLeave();
        fprintf(stderr, "[app] on-screen keyboard closed (%s)\n", why);
    }

    // On the UI thread: the keyboard over a field holding text.
    void show(int kind, std::wstring const& text)
    {
        g_context.NotifyFocusLeave(); // a new field: the system asks for the text afresh
        g_text = text;
        g_selection = { (int32_t)g_text.size(), (int32_t)g_text.size() };
        g_context.InputScope(kind == UWP_TEXT_SECRET   ? CoreTextInputScope::Password
                             : kind == UWP_TEXT_NUMBER ? CoreTextInputScope::Number
                                                       : CoreTextInputScope::Default);
        g_context.NotifyFocusEnter();
        bool shown = CoreInputView::GetForCurrentView().TryShow(CoreInputViewKind::Keyboard);
        fprintf(stderr, "[app] on-screen keyboard %s\n", shown ? "opened" : "did not open");
        if (!shown)
        {
            hidden("not shown");
            return;
        }
        // If nothing says it is there within 2 s, it is not: the controller goes back to the screen.
        unsigned which = ++g_shown;
        std::thread([which] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            g_ui.RunAsync(CoreDispatcherPriority::Normal, [which] {
                if (which == g_shown && g_up && !g_seen && !g_away)
                    hidden("never seen");
            });
        }).detach();
    }

    // From the game's thread: the sign-in screen's request.
    void request(int on, int kind, const char* text)
    {
        if (on)
            g_up = true; // at once: the controller's next press is the keyboard's
        std::wstring w = to_hstring(text ? text : "").c_str();
        g_ui.RunAsync(CoreDispatcherPriority::High, [on, kind, w] {
            if (on)
                show(kind, w);
            else
            {
                CoreInputView::GetForCurrentView().TryHide();
                hidden("the screen asked");
            }
        });
    }
}

void TextInput::Attach(CoreWindow const& window)
{
    g_window = window;
    g_ui = window.Dispatcher();
    g_context = CoreTextServicesManager::GetForCurrentView().CreateEditContext();
    g_context.Name(L"FINAL FANTASY XI sign-in");
    g_context.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);

    g_context.TextRequested([](auto&&, CoreTextTextRequestedEventArgs const& e) {
        CoreTextTextRequest r = e.Request();
        CoreTextRange range = clamped(r.Range());
        r.Text(g_text.substr((size_t)range.StartCaretPosition, (size_t)(range.EndCaretPosition - range.StartCaretPosition)));
    });
    g_context.SelectionRequested([](auto&&, CoreTextSelectionRequestedEventArgs const& e) { e.Request().Selection(g_selection); });
    g_context.SelectionUpdating([](auto&&, CoreTextSelectionUpdatingEventArgs const& e) { g_selection = clamped(e.Selection()); });
    g_context.TextUpdating([](auto&&, CoreTextTextUpdatingEventArgs const& e) {
        CoreTextRange range = clamped(e.Range());
        g_text.replace((size_t)range.StartCaretPosition, (size_t)(range.EndCaretPosition - range.StartCaretPosition), e.Text());
        g_selection = e.NewSelection();
        uwp_replace_text(to_string(g_text).c_str());
    });
    // where the text is, for the keyboard to stay clear of: the whole window (the screen draws it)
    g_context.LayoutRequested([](auto&&, CoreTextLayoutRequestedEventArgs const& e) {
        Rect b = g_window.Bounds();
        e.Request().LayoutBounds().ControlBounds(b);
        e.Request().LayoutBounds().TextBounds(b);
    });
    // Which of these the console sends when its keyboard closes is not known, so any of them will do
    g_context.FocusRemoved([](auto&&, auto&&) { hidden("focus removed"); });
    CoreInputView view = CoreInputView::GetForCurrentView();
    view.PrimaryViewHiding([](auto&&, auto&&) { hidden("hiding"); });
    view.OcclusionsChanged([](auto&&, CoreInputViewOcclusionsChangedEventArgs const& e) {
        fprintf(stderr, "[app] on-screen keyboard occlusions: %u\n", e.Occlusions().Size());
        if (e.Occlusions().Size())
            g_seen = true;
        else
            hidden("no occlusions");
    });
    // the window back in focus after the keyboard had it
    window.Activated([](CoreWindow const&, WindowActivatedEventArgs const& e) {
        bool active = e.WindowActivationState() != CoreWindowActivationState::Deactivated;
        if (g_up && !active)
            g_away = true;
        else if (g_up && active && g_away)
            hidden("window back in focus");
    });

    uwp_set_text_input(request);
}

bool TextInput::IsUp() { return g_up; }
