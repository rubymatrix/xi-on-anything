-- The addons' profile and cached pattern scans (tests/addon_prof_test.sh).

-- //busy <ms>: a command that takes that long, for the slow-call log and /addon prof
xi.events.on('command', function(e)
    local ms = e.data:match('^//busy (%d+)')
    if ms then
        local t = os.clock()
        while os.clock() - t < tonumber(ms) / 1000 do end
        e.blocked = true
    end
end)

-- the same scan twice: the second answered from the cache, the same address
local pat = '81 EC 04 04 00 00 8B 8C 24 08 04 00 00'
local a = xi.memory.find(0, 0, pat, 0, 0)
print('find again: ' .. tostring(a ~= 0 and xi.memory.find(0, 0, pat, 0, 0) == a))
-- the second match is not the first's entry
local second = xi.memory.find(0, 0, '8B 4C 24 04', 0, 1)
print('second match: ' .. tostring(second ~= xi.memory.find(0, 0, '8B 4C 24 04', 0, 0) and second == xi.memory.find(0, 0, '8B 4C 24 04', 0, 1)))
-- answered from the cache: ten of a scan that runs far into the code take less than the first
local function took(f, n)
    local t = os.clock()
    for _ = 1, n do f() end
    return os.clock() - t
end
local far
local first = took(function() far = xi.memory.find(0, 0, '8B 4C 24 04', 0, 1000) end, 1)
print('cached: ' .. tostring(far ~= 0 and took(function() xi.memory.find(0, 0, '8B 4C 24 04', 0, 1000) end, 10) < first))
-- a code patch over the match: answered as a fresh scan would, and again once it is undone
local old = xi.memory.read_uint8(a)
xi.memory.write_uint8(a, 0x90)
print('after a patch: ' .. tostring(xi.memory.find(0, 0, pat, 0, 0) ~= a))
xi.memory.write_uint8(a, old)
print('patch undone: ' .. tostring(xi.memory.find(0, 0, pat, 0, 0) == a))
