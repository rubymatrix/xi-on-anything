--[[
ThornyFFXI's gdifonts library (tHotBar, tCrossBar...) on the host's renderer (host/addons/gdifont_ffi.c)
and Direct3D (d3d_ffi.c). tests/addons/d3d8_ffi.py copies the library (not in this repository)
into gdifonts/ next to this file; with fancychat's or balloon's version of it, their colours,
clipping and fonts are checked too. Prints PASS/FAIL lines and "DONE <passed>/<total>".
--]]

package.path = xi.paths.ashita .. 'addons/libs/?.lua;' .. package.path
require('common')
local ffi = require('ffi')
local C = ffi.C

-- what an Ashita addon has, as much as gdifonts uses
AshitaCore = AshitaCore or { GetDirect3DDevice = function() return xi.d3d8_device() end }
ashita = ashita or {
    events = {
        register = function(name, key, fn) if name == 'd3d_present' then xi.events.on('present', fn, key) end end,
        unregister = function(name, key) if name == 'd3d_present' then xi.events.off('present', key) end end,
    },
    fs = { exists = function() return false end },
}

local passed, total = 0, 0
local function check(name, ok, why)
    total = total + 1
    if ok then
        passed = passed + 1
        print('PASS ' .. name)
    else
        print('FAIL ' .. name .. ': ' .. tostring(why))
    end
end
local function try(name, fn)
    local ok, err = pcall(fn)
    if not ok then check(name, false, err) end
end

local function pixel(tex, x, y)
    local _, lr = tex:LockRect(0, nil, 0)
    local v = ffi.cast('uint32_t*', lr.pBits)[y * (lr.Pitch / 4) + x]
    tex:UnlockRect(0)
    return v
end

local gdi
try('load', function()
    gdi = require('gdifonts.include')
    check('require gdifonts', gdi ~= nil)
end)

local font, rect
try('font', function()
    check('GetFontAvailable Arial', gdi:get_font_available('Arial') == true)
    check('GetFontAvailable missing', gdi:get_font_available('No Such Font Anywhere') == false)
    font = gdi:create_object({ font_height = 24, text = 'Hello\nWorld', outline_width = 2, font_flags = 1,
        gradient_style = 3, gradient_color = 0xFFFF0000, z_order = 1 }, false)
    local tex, r = font:get_texture()
    check('CreateTexture', tex ~= nil and r.right > 40 and r.bottom > 50, r and r.right .. 'x' .. r.bottom)
    local lit, clear = 0, 0
    local _, lr = tex:LockRect(0, nil, 0)
    local p = ffi.cast('uint32_t*', lr.pBits)
    for i = 0, (lr.Pitch / 4) * r.bottom - 1 do
        if bit.rshift(p[i], 24) > 200 then lit = lit + 1 elseif p[i] == 0 then clear = clear + 1 end
    end
    tex:UnlockRect(0)
    check('text pixels', lit > 100 and clear > 100, lit .. ' lit, ' .. clear .. ' clear')
end)

-- The library's own version: fancychat's colours runs of its text with markers, balloon's (the
-- regions fork) colours character ranges and clips to one.
local include = io.open(xi.info.path .. 'gdifonts/include.lua'):read('*a')

-- opaque pixels: all of them, red ones (a run's or region's colour), white ones (the font's)
local function opaque(tex, r)
    local all, red, white = 0, 0, 0
    local _, lr = tex:LockRect(0, nil, 0)
    local p = ffi.cast('uint32_t*', lr.pBits)
    for y = 0, r.bottom - 1 do
        for x = 0, r.right - 1 do
            local v = p[y * (lr.Pitch / 4) + x]
            if bit.rshift(v, 24) > 200 then
                all = all + 1
                local R, G, B = bit.band(bit.rshift(v, 16), 255), bit.band(bit.rshift(v, 8), 255), bit.band(v, 255)
                if R > 200 and G < 60 and B < 60 then
                    red = red + 1
                elseif R > 200 and G > 200 and B > 200 then
                    white = white + 1
                end
            end
        end
    end
    tex:UnlockRect(0)
    return all, red, white
end

local function plain(text)
    local o = gdi:create_object({ font_height = 24, text = text, outline_width = 0 }, false)
    return o, o:get_texture()
end

try('wrap', function()
    local _, _, one = plain('ABCD EFGH IJKL')
    local o = gdi:create_object({ font_height = 24, text = 'ABCD EFGH IJKL', outline_width = 0, box_width = 90 }, false)
    local tex, r = o:get_texture()
    check('wrap in box_width', tex ~= nil and r.right <= 90 and r.bottom >= one.bottom * 2.5,
        r and (r.right .. 'x' .. r.bottom .. ', one line ' .. one.bottom))
end)

if include:find('CreateTextureColor', 1, true) then
    try('colour markers', function()
        local _, pt, pr = plain('ABCD')
        local _, mt, mr = plain('AB\\§FFFF0000ç\\CD\\§--------ç\\')
        check('CreateTextureColor size', mt ~= nil and mr.right == pr.right and mr.bottom == pr.bottom,
            mr and (mr.right .. 'x' .. mr.bottom .. ' for ' .. pr.right .. 'x' .. pr.bottom))
        local _, red, white = opaque(mt, mr)
        check('CreateTextureColor runs', red > 20 and white > 20, red .. ' red, ' .. white .. ' white')
    end)
end

-- fancychat's ships its fonts beside include.lua: its game icons (U+EE1B, its gil) are drawn, not
-- the missing glyph a character no font has gets (U+10FFFD)
if include:find('SetFontFolder', 1, true) then
    try('font folder', function()
        local _, it, ir = plain('\238\184\155')
        local _, mt, mr = plain('\244\143\191\189')
        local icon, missing = opaque(it, ir), opaque(mt, mr)
        check('SetFontFolder icons', icon > 20 and icon ~= missing, icon .. ' for ' .. missing .. ' missing')
    end)
end

if include:find('GdiRegion_t', 1, true) then
    try('regions', function()
        local _, ft, fr = plain('ABCD')
        local all = opaque(ft, fr)
        local o = plain('ABCD')
        o:set_regions({ ['255,0,0'] = { { start_u = 3, length_u = 2 } } })
        local rt, rr = o:get_texture()
        local _, red, white = opaque(rt, rr)
        check('region colour', red > 20 and white > 20, red .. ' red, ' .. white .. ' white')
        o = plain('ABCD')
        o:set_clip_range(1, 2)
        local ct, cr = o:get_texture()
        local shown = opaque(ct, cr)
        check('clip keeps the size', cr.right == fr.right and cr.bottom == fr.bottom, cr.right .. ' for ' .. fr.right)
        check('clip shows the range', shown > 20 and shown < all * 0.75, shown .. ' of ' .. all)
        -- balloon passes no family when it found no default font: still its layout, a font found
        local d = ffi.new('GdiFontData_t')
        local text = ffi.new('char[5]', 'ABCD')
        d.FontHeight, d.FontColor, d.FontText = 24, 0xFFFFFFFF, text
        local mgr = C.CreateFontManager(nil, 0)
        local nr = C.CreateTexture(mgr, d)
        check('no family', nr.Texture ~= nil and nr.Width > fr.right * 0.6 and nr.Width < fr.right * 1.6,
            nr.Width .. ' for ' .. fr.right)
        if nr.Texture ~= nil then nr.Texture:Release() end
        C.DestroyFontManager(mgr)
    end)
end

try('rect', function()
    rect = gdi:create_rect({ width = 50, height = 20, corner_rounding = 10, fill_color = 0xFF00FF00,
        outline_color = 0xFF000000, outline_width = 2 }, false)
    local tex, r = rect:get_texture()
    check('CreateRectTexture', tex ~= nil and r.right == 50 and r.bottom == 20, r and r.right)
    check('rect corner', pixel(tex, 0, 0) == 0, string.format('%08x', pixel(tex, 0, 0)))
    check('rect fill', pixel(tex, 25, 10) == 0xFF00FF00, string.format('%08x', pixel(tex, 25, 10)))
    check('rect outline', pixel(tex, 25, 0) == 0xFF000000, string.format('%08x', pixel(tex, 25, 0)))
end)

try('encoding', function()
    local enc = dofile(xi.info.path .. 'gdifonts/encoding.lua')
    local s = enc:UTF8_To_ShiftJIS('テスト abc')
    check('UTF8_To_ShiftJIS', s == '\131\101\131\88\131\103 abc', (s:gsub('.', function(c) return ('%02x'):format(c:byte()) end)))
    check('ShiftJIS_To_UTF8', enc:ShiftJIS_To_UTF8(s) == 'テスト abc')
end)

local frames = 0
xi.events.on('present', function()
    frames = frames + 1
    if frames == 2 then
        local _, v = xi.d3d8_stats()
        check('auto render', v >= 12, v) -- the text and the rect through gdifonts' own sprite
    end
end)

xi.tasks.oncef(4, function()
    try('destroy', function()
        gdi:destroy_interface()
        font, rect = nil, nil
        collectgarbage()
        check('destroy_interface', true)
    end)
    print(('DONE %d/%d'):format(passed, total))
end)
