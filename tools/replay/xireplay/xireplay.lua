--[[
xireplay: records zone visits for tools/replayserver.py and shows its events.

An Ashita v4 addon: Ashita on Windows, and xi-on-anything's addon host. Copy this folder to the
addons folder (ashita/addons/xireplay) and load it: /addon load xireplay.

Recording: every zone-in (incoming 0x00A) starts a file in Ashita's config\addons\xireplay\ folder,
and the zone-out (incoming 0x00B) ends it. Each line is
    {"t": <ms>, "dir": "in"|"out", "id": <packet type>, "hex": "<packet, header included>"}
after a first {"meta": {...}} line; {"t": <ms>, "mark": "<label>"} lines mark a scene's phases.
Record only on a server you run yourself. /xireplay record off stops it.

Events: tools/replayserver.py sends its events as chat lines from "xireplay"; this hides them and
shows each as one line of its own:
    [17:18:37] xireplay BEGIN #2 mines (city) zone 234
--]]

addon.name    = 'xireplay';
addon.author  = 'xi-on-anything';
addon.version = '1.0';
addon.desc    = 'Records zone visits and shows the events of tools/replayserver.py.';

require('common');

local dir = ('%s\\config\\addons\\xireplay\\'):format(AshitaCore:GetInstallPath());
ashita.fs.create_directory(dir);
local file = nil;
local lines = 0;

local function now_ms() return ashita.time.clock().ms; end

local function u32(s, o)
    local a, b, c, d = s:byte(o + 1, o + 4);
    return a + b * 256 + c * 65536 + d * 16777216;
end

local function hex(s)
    return (s:gsub('.', function(c) return ('%02x'):format(c:byte()); end));
end


-------------------------------------------------------------------------------- recording
local recording = true;

local function close()
    if file ~= nil then
        file:close();
        print(('[xireplay] saved %d packets'):format(lines));
        file = nil;
    end
end

local function open(zone)
    close();
    if not recording then return; end
    local name = ('%scapture-%s-zone%d.jsonl'):format(dir, os.date('%Y%m%d-%H%M%S'), zone);
    local scene = '';
    file = io.open(name, 'w');
    lines = 0;
    if file == nil then
        print('[xireplay] cannot write ' .. name);
        return;
    end
    file:write(('{"meta":{"zone":%d,"scene":"%s","captured":"%s"}}\n'):format(zone, scene, os.date('!%Y-%m-%dT%H:%M:%SZ')));
    file:flush();
    print('[xireplay] recording to ' .. name);
end

local function write(s)
    if file ~= nil then
        file:write(s);
        file:flush(); -- a client that is killed keeps what it recorded
    end
end

local function record(dirn, e)
    if file == nil then return; end
    write(('{"t":%d,"dir":"%s","id":%d,"hex":"%s"}\n'):format(now_ms(), dirn, e.id, hex(e.data)));
    lines = lines + 1;
end


-------------------------------------------------------------------------------- events
local function say(text)
    print(('[%s] %s'):format(os.date('%H:%M:%S'), text));
end

local function event(kind, detail, state)
    say(('xireplay %-5s %s'):format(kind, detail));
end

local function on_event(text)
    local f = {};
    for part in (text .. '|'):gmatch('([^|]*)|') do f[#f + 1] = part; end
    local kind, n, name = f[1], f[2], f[3];
    local scene = { scene = tonumber(n), name = name };
    -- READY at the start marker, or, in a scene without one, at its first marker that is not the end
    local first = true;
    if kind == 'begin' then
        scene.group, scene.zone = f[4], tonumber(f[5]);
        event('BEGIN', ('#%s %s (%s) zone %s'):format(n, name, f[4] ~= '' and f[4] or '-', f[5] or '?'), scene);
    elseif kind == 'mark' then
        local label = f[4];
        scene.phase = label;
        if label == 'start' or (first and label ~= 'end') then
            event('READY', ('#%s %s'):format(n, name), scene);
        end
        if label ~= 'start' and label ~= 'end' then
            event('PHASE', ('#%s %s: %s'):format(n, name, label), scene);
        end
    elseif kind == 'end' then
        local result = 'played';
        scene.result = result;
        event('END', ('#%s %s: %s'):format(n, name, result), scene);
    elseif kind == 'done' then
        event('DONE', ('%s scenes played'):format(n or '?'), { played = tonumber(n) });
    elseif kind == 'home' then
        event('HOME', 'waiting: !replay <#|name|group|all|list|stop>', {});
    elseif kind == 'queue' then
        event('QUEUE', ((n or ''):gsub(',', ', ')), { queue = n });
    elseif kind == 'list' then
        scene.group = f[4];
        event('SCENE', ('#%s %s (%s)'):format(n, name, f[4] ~= '' and f[4] or '-'), scene);
    elseif kind == 'info' or kind == 'error' then
        event(kind:upper(), n or '', {});
    end
end


-------------------------------------------------------------------------------- the game's events
ashita.events.register('packet_in', 'xireplay_in', function (e)
    if e.injected then return; end
    if e.id == 0x017 and #e.data > 23 and e.data:sub(9, 16) == 'xireplay' then
        e.blocked = true;
        on_event((e.data:sub(24):match('^[^%z]*')));
        return;
    end
    if e.id == 0x00A and #e.data >= 0x34 then
        open(u32(e.data, 0x30));
    end
    record('in', e);
    if e.id == 0x00B then close(); end
end);

ashita.events.register('packet_out', 'xireplay_out', function (e)
    if not e.injected then record('out', e); end
end);


local USAGE = '/xireplay record on|off';

ashita.events.register('command', 'xireplay_command', function (e)
    local args = e.command:args();
    if #args == 0 or args[1]:lower() ~= '/xireplay' then return; end
    e.blocked = true;
    local verb = (args[2] or ''):lower();
    if verb == 'record' then
        recording = (args[3] or 'on') == 'on';
        if not recording then close(); end
        print('[xireplay] recording ' .. (recording and 'on' or 'off'));
        return;
    end
    print('[xireplay] ' .. USAGE);
end);

ashita.events.register('unload', 'xireplay_unload', function ()
    close();
end);
