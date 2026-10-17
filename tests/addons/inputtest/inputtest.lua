-- Two ImGui windows for the overlay's input capture (tests/overlay_input_test.sh): a HUD-like window
-- at (10, 10) that hides while hud is false, and a form at (400, 400) whose text field is at its top;
-- pop = true opens a popup from the form (menu_open: whether it was drawn in the last frame).
local imgui = xi.ui.imgui()
hud = true
local name = { '' }
xi.events.on('present', function()
    if hud then
        imgui.SetNextWindowPos({ 10, 10 })
        imgui.SetNextWindowSize({ 200, 100 })
        imgui.Begin('hud')
        imgui.Text('hp')
        imgui.End()
    end
    imgui.SetNextWindowPos({ 400, 400 })
    imgui.SetNextWindowSize({ 300, 100 })
    imgui.Begin('form', nil, 1) -- 1: ImGuiWindowFlags_NoTitleBar
    imgui.InputText('name', name, 32)
    if pop then
        imgui.OpenPopup('menu')
        pop = false
    end
    menu_open = imgui.BeginPopup('menu')
    if menu_open then
        imgui.Text('an item')
        imgui.EndPopup()
    end
    imgui.End()
end)
