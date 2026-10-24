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

    void hidden()
    {
        if (!g_up.exchange(false))
            return;
        g_context.NotifyFocusLeave();
        fprintf(stderr, "[app] on-screen keyboard closed\n");
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
            hidden();
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
                hidden();
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
    g_context.FocusRemoved([](auto&&, auto&&) { hidden(); });
    CoreInputView::GetForCurrentView().PrimaryViewHiding([](auto&&, auto&&) { hidden(); });

    uwp_set_text_input(request);
}

bool TextInput::IsUp() { return g_up; }
