--[=[
The `utf8` library Ashita's Lua has (global `utf8`, and require('utf8')): Lua 5.3's, which LuaJIT
lacks, written here in Lua. fancychat builds its icons with utf8.char, balloon counts a message's
characters with utf8.len.

    utf8.char(...) -> string                 utf8.charpattern
    utf8.codepoint(s[, i[, j]]) -> codes...  utf8.len(s[, i[, j]]) -> n, or nil and the bad byte's position
    utf8.offset(s, n[, i]) -> position       utf8.codes(s) -> for p, c in utf8.codes(s)

As Lua 5.3: code points up to 0x10FFFF, no overlong forms; surrogates pass.
--]=]

local bit = require('bit')
local band, bor, lshift, rshift = bit.band, bit.bor, bit.lshift, bit.rshift

-- (%z: a pattern can't hold a zero byte in Lua 5.1, which 5.3's '[\0-\x7F...' does)
local utf8 = { charpattern = '[%z\1-\x7F\xC2-\xF4][\x80-\xBF]*' }

local MAX = 0x10FFFF
local LIMITS = { [1] = 0x80, [2] = 0x800, [3] = 0x10000 } -- the least code point of 2, 3, 4 bytes

-- the code point at byte i of s, and the byte after it; nil if it isn't valid UTF-8
local function decode(s, i)
    local c = s:byte(i)
    if not c then return nil end
    if c < 0x80 then return c, i + 1 end
    if c < 0xC0 then return nil end
    local n = c >= 0xF0 and 3 or c >= 0xE0 and 2 or 1
    if c >= 0xF8 then return nil end
    local code = band(c, rshift(0x7F, n + 1))
    for k = 1, n do
        local cc = s:byte(i + k)
        if not cc or band(cc, 0xC0) ~= 0x80 then return nil end
        code = bor(lshift(code, 6), band(cc, 0x3F))
    end
    if code > MAX or code < LIMITS[n] then return nil end
    return code, i + n + 1
end

-- a position argument as Lua 5.3 takes it: negative from the end
local function at(i, len)
    if i >= 0 then return i end
    if -i > len then return 0 end
    return len + i + 1
end

local function encode(code)
    if code < 0x80 then return string.char(code) end
    if code < 0x800 then
        return string.char(bor(0xC0, rshift(code, 6)), bor(0x80, band(code, 0x3F)))
    end
    if code < 0x10000 then
        return string.char(bor(0xE0, rshift(code, 12)), bor(0x80, band(rshift(code, 6), 0x3F)), bor(0x80, band(code, 0x3F)))
    end
    return string.char(bor(0xF0, rshift(code, 18)), bor(0x80, band(rshift(code, 12), 0x3F)),
        bor(0x80, band(rshift(code, 6), 0x3F)), bor(0x80, band(code, 0x3F)))
end

function utf8.char(...)
    local out = {}
    for k = 1, select('#', ...) do
        local code = select(k, ...)
        if type(code) ~= 'number' then
            error(("bad argument #%d to 'char' (number expected, got %s)"):format(k, type(code)), 2)
        end
        if code < 0 or code > MAX or code % 1 ~= 0 then
            error(("bad argument #%d to 'char' (value out of range)"):format(k), 2)
        end
        out[k] = encode(code)
    end
    return table.concat(out)
end

function utf8.codepoint(s, i, j)
    s = tostring(s)
    i = at(i or 1, #s)
    j = at(j or i, #s)
    if i < 1 then error("bad argument #2 to 'codepoint' (out of range)", 2) end
    if j > #s then error("bad argument #3 to 'codepoint' (out of range)", 2) end
    local out, p = {}, i
    while p <= j do
        local code, nxt = decode(s, p)
        if not code then error('invalid UTF-8 code', 2) end
        out[#out + 1] = code
        p = nxt
    end
    return unpack(out)
end

function utf8.len(s, i, j)
    s = tostring(s)
    i = at(i or 1, #s)
    j = at(j or -1, #s)
    if i < 1 or i > #s + 1 then error("bad argument #2 to 'len' (initial position out of string)", 2) end
    if j > #s then error("bad argument #3 to 'len' (final position out of string)", 2) end
    local n, p = 0, i
    while p <= j do
        local code, nxt = decode(s, p)
        if not code then return nil, p end
        n, p = n + 1, nxt
    end
    return n
end

local function continuation(s, p)
    local c = s:byte(p)
    return c and band(c, 0xC0) == 0x80
end

function utf8.offset(s, n, i)
    s = tostring(s)
    local len = #s
    i = at(i or (n >= 0 and 1 or len + 1), len)
    if i < 1 or i > len + 1 then error("bad argument #3 to 'offset' (position out of range)", 2) end
    if n == 0 then
        while i > 1 and continuation(s, i) do i = i - 1 end
        return i
    end
    if continuation(s, i) then error('initial position is a continuation byte', 2) end
    if n < 0 then
        while n < 0 and i > 1 do
            repeat i = i - 1 until i <= 1 or not continuation(s, i)
            n = n + 1
        end
    else
        n = n - 1
        while n > 0 and i <= len do
            repeat i = i + 1 until not continuation(s, i)
            n = n - 1
        end
    end
    if n == 0 then return i end
    return nil
end

function utf8.codes(s)
    s = tostring(s)
    local p = 1
    return function()
        if p > #s then return nil end
        local code, nxt = decode(s, p)
        if not code then error('invalid UTF-8 code', 2) end
        local here = p
        p = nxt
        return here, code
    end
end

return utf8
