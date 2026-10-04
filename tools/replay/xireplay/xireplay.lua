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
With /xireplay frames on it logs every frame's time and every event to frames-<date>.csv (the
input of tools/replayreport.py), and with /xireplay quiet on the chat log shows nothing but these
lines. The latest event is in state.json, for a script deciding when to take a screenshot (READY:
the scene is loaded and settled).
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

-------------------------------------------------------------------------------- frame times
-- Each Present's time in microseconds ("f,<us>"), each zone-in's ("z,<us>") and each event
-- ("m,<us>,<event>"), written in batches.
local frames = { file = nil, buf = {} };

local function frames_flush()
    if frames.file ~= nil and #frames.buf > 0 then
        frames.file:write(table.concat(frames.buf, '\n'), '\n');
        frames.file:flush();
        frames.buf = {};
    end
end

local function frames_note(line)
    if frames.file == nil then return; end
    frames.buf[#frames.buf + 1] = line;
    if #frames.buf >= 240 then frames_flush(); end
end

local function micro() return ('%.0f'):format(ashita.time.clock().micro); end -- not 1.79e+15

-- a scene's measured window (its start marker to its end), for the END line's numbers: since (us)
-- while it is open, its frame times, and final, the numbers once it has closed
local function window(since) return { since = since, last = nil, ms = {}, final = nil }; end
local measuring = window(nil);

local function measure_frame()
    if measuring.since == nil then return; end
    local now = ashita.time.clock().micro;
    if measuring.last ~= nil then measuring.ms[#measuring.ms + 1] = (now - measuring.last) / 1000; end
    measuring.last = now;
end

local function stats()
    local ms = measuring.ms;
    if #ms == 0 then return 'no frames'; end
    local sum, sorted = 0, {};
    for i, v in ipairs(ms) do sum = sum + v; sorted[i] = v; end
    table.sort(sorted);
    return ('%d frames, %.1f fps, p99 %.1f ms, max %.1f ms'):format(#ms, 1000 * #ms / sum,
        sorted[math.min(#sorted, math.floor(#sorted * 0.99) + 1)], sorted[#sorted]);
end

local quiet = false; -- the chat log shows only this addon's lines

-------------------------------------------------------------------------------- events
local function say(text)
    print(('[%s] %s'):format(os.date('%H:%M:%S'), text));
end

local function event(kind, detail, state)
    say(('xireplay %-5s %s'):format(kind, detail));
    local f = io.open(dir .. 'state.json', 'w');
    if f ~= nil then
        local fields = { ('"event":"%s"'):format(kind), ('"time":"%s"'):format(os.date('!%Y-%m-%dT%H:%M:%SZ')) };
        for k, v in pairs(state or {}) do
            fields[#fields + 1] = type(v) == 'number' and ('"%s":%s'):format(k, v)
                or ('"%s":"%s"'):format(k, (tostring(v):gsub('"', "'")));
        end
        f:write('{', table.concat(fields, ','), '}\n');
        f:close();
    end
end

local function on_event(text)
    frames_note('m,' .. micro() .. ',' .. text);
    frames_flush();
    local f = {};
    for part in (text .. '|'):gmatch('([^|]*)|') do f[#f + 1] = part; end
    local kind, n, name = f[1], f[2], f[3];
    local scene = { scene = tonumber(n), name = name };
    -- READY at the start marker, or, in a scene without one, at its first marker that is not the end
    local first = measuring.since == nil and measuring.final == nil;
    if kind == 'begin' then
        measuring = window(nil);
        scene.group, scene.zone = f[4], tonumber(f[5]);
        event('BEGIN', ('#%s %s (%s) zone %s'):format(n, name, f[4] ~= '' and f[4] or '-', f[5] or '?'), scene);
    elseif kind == 'mark' then
        local label = f[4];
        scene.phase = label;
        if label == 'start' or (first and label ~= 'end') then
            measuring = window(ashita.time.clock().micro);
            event('READY', ('#%s %s'):format(n, name), scene);
        end
        if label ~= 'start' and label ~= 'end' then
            event('PHASE', ('#%s %s: %s'):format(n, name, label), scene);
        elseif label == 'end' then
            measuring.since, measuring.final = nil, stats(); -- the measured window is over
        end
    elseif kind == 'end' then
        local result = measuring.final or stats();
        measuring = window(nil);
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
        frames_note('z,' .. micro());
        open(u32(e.data, 0x30));
    end
    record('in', e);
    if e.id == 0x00B then close(); end
end);

ashita.events.register('packet_out', 'xireplay_out', function (e)
    if not e.injected then record('out', e); end
end);

ashita.events.register('d3d_present', 'xireplay_present', function ()
    frames_note('f,' .. micro());
    measure_frame();
end);

-- in quiet mode, only this addon's lines reach the chat log
ashita.events.register('text_in', 'xireplay_text', function (e)
    if quiet and not e.message:find('^%[%d%d:%d%d:%d%d%] ') and not e.message:find('%[xireplay%]') then
        e.blocked = true;
    end
end);

local USAGE = '/xireplay record on|off, frames on|off, quiet on|off';

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
    if verb == 'quiet' then
        quiet = (args[3] or 'on') == 'on';
        print('[xireplay] quiet ' .. (quiet and 'on' or 'off'));
        return;
    elseif verb == 'frames' then
        frames_flush();
        if frames.file ~= nil then frames.file:close(); frames.file = nil; end
        if (args[3] or 'on') == 'on' then
            ashita.fs.create_directory(dir);
            local name = ('%sframes-%s.csv'):format(dir, os.date('%Y%m%d-%H%M%S'));
            frames.file = io.open(name, 'w');
            print('[xireplay] frame times to ' .. name);
        else
            print('[xireplay] frames off');
        end
        return;
    end
    print('[xireplay] ' .. USAGE);
end);

ashita.events.register('unload', 'xireplay_unload', function ()
    close();
    frames_flush();
    if frames.file ~= nil then frames.file:close(); end
end);
