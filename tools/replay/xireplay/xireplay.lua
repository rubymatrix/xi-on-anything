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
Scenes: /xireplay run <name> [more...] (or all) plays one of the scenes below on your LandSandBoat
server with GM commands (the character must be a GM) and records the zone visits it makes, as
<name>-<n>.jsonl; tools/replay.py record does it unattended. /xireplay list shows them, /xireplay
stop ends one. !perftime and !perfcrowd are tools/replay/lsb's commands.
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

-------------------------------------------------------------------------------- scenes
-- Vantages: zone-line arrival points and plazas from LandSandBoat's data, and for weather an open
-- spot in a zone whose own weather includes it (LandSandBoat's zone_weather): the client draws a
-- weather's effects only in a zone that has it. Spots picked from mob spawn points by screenshots.
local MARKETS = '!exec player:setPos(-201.904,1.928,-194.828,192,235)';
local MINES_PLAZA = '!exec player:setPos(39,0,-49,128,234)';
local CROWD_SPOT = '!exec player:setPos(0.840,-5.027,76.838,192,106)';  -- North Gustaberg, facing the open field
-- North Gustaberg again, for the lighting scene: Lua to append to the !exec that sets the clock, so
-- the move's zone change brings the new time with it
local LIGHTING_SPOT = 'player:setPos(0.020,-4.409,-75.405,192,106)';
local WEATHER_SPOTS = {
    '!exec player:setPos(-211.575,-4.418,-53.407,84,125)',  -- Western Altepa Desert: hot spell, heat wave, sand storm
    '!exec player:setPos(59.789,-12.359,202.22,137,115)',   -- West Sarutabaruta: sunshine
    '!exec player:setPos(-367.359,-0.358,-80.909,132,108)', -- Konschtat Highlands: dust storm
    '!exec player:setPos(-544.848,-7.25,246.057,100,102)',  -- La Theine Plateau: wind
    '!exec player:setPos(-66.0,0.5,58.0,53,113)',           -- Cape Teriggan: gales
    '!exec player:setPos(456.86,25.503,422.368,152,109)',   -- Pashhow Marshlands: rain
    '!exec player:setPos(365.458,21.543,247.04,22,123)',    -- Yuhtunga Jungle: squall
    '!exec player:setPos(238.092,-15.383,-191.151,203,112)', -- Xarcabard: snow
    '!exec player:setPos(-2.931,-58.725,-110.482,37,111)',  -- Beaucedine Glacier: blizzards
    '!exec player:setPos(9.348,1.411,-22.694,164,121)',     -- The Sanctuary of Zi'Tah: thunder, thunderstorms
};
-- North Gustaberg's mobs (ids 0x106A000 + slot), which !perfcrowd brings to the character: held
-- (mob mods 65 and 67, NO_MOVE and NO_AGGRO: numbers keep the command under the chat line's limit)
local CROWD = 'for i=1,200 do local m=GetMobByID(0x106A000+i) if m then ';
local PIN_CROWD = '!exec ' .. CROWD .. 'm:setMobMod(65,1) m:setMobMod(67,1) end end';

-- A step is { seconds to wait before it, action }: '!...' and '/...' are sent as typed (GM commands
-- at least BANG_GAP apart: the client drops lines sent faster), 'zone' waits for the next zone-in,
-- 'mark <label>' writes a marker, 'end' finishes. Every scene but home pins the clock (noon, unless it
-- is about the time of day) and clears the weather at its zone-in.
local function hold(enter, seconds, extra)
    local s = { { 1, '!perftime 12' }, { 2, enter }, { 0, 'zone' }, { 3, '!setweather 0' } };
    for _, e in ipairs(extra or { { 5, 'mark start' }, { seconds, 'mark end' } }) do s[#s + 1] = e; end
    s[#s + 1] = { 1, 'end' };
    return s;
end

local scenes = {
    -- GM Home, where a replay session waits for !replay
    home = { { 1, '!gmhome' }, { 0, 'zone' }, { 5, 'mark start' }, { 10, 'mark end' }, { 1, 'end' } },
    -- a city at noon from a fixed vantage
    markets = hold(MARKETS, 45),
    -- the Mines plaza: the auction counters' NPCs
    mines = hold(MINES_PLAZA, 45),
    -- one vantage at four times of day: a zone visit each (the clock needs a rezone)
    lighting = (function()
        local s = { { 1, "!exec xi.commands.perftime = dofile('scripts/commands/perftime.lua')" } };
        for _, h in ipairs({ { 6, 'dawn' }, { 12, 'noon' }, { 18, 'dusk' }, { 0, 'midnight' } }) do
            for _, step in ipairs({ { 2, ('!exec xi.commands.perftime.onTrigger(player,%d);'):format(h[1]) .. LIGHTING_SPOT },
                { 0, 'zone' }, { 3, '!setweather 0' }, { 5, 'mark ' .. h[2] }, { 15, 'mark end' } }) do
                s[#s + 1] = step;
            end
        end
        s[#s + 1] = { 1, 'end' };
        return s;
    end)(),
    -- a short stay at each weather spot: the replay gives each zone-in its weather (--weather)
    weather = (function()
        local s = { { 1, '!perftime 12' } };
        for _, spot in ipairs(WEATHER_SPOTS) do
            for _, step in ipairs({ { 3, spot }, { 0, 'zone' }, { 3, '!setweather 0' }, { 3, 'mark start' }, { 10, 'mark end' } }) do
                s[#s + 1] = step;
            end
        end
        s[#s + 1] = { 1, 'end' };
        return s;
    end)(),
    -- forty mobs around the character in the open (the replay holds them in place)
    crowd = hold(CROWD_SPOT, 0, { { 3, '!perfcrowd 40' }, { 1, PIN_CROWD }, { 8, 'mark start' }, { 40, 'mark end' } }),
    -- the character casting: Chainspell then spikes and barriers, three rounds
    effects = (function()
        local s = { { 1, '!perftime 12' }, { 2, '!changejob RDM 99' }, { 3, '!addallspells' },
            { 3, MINES_PLAZA }, { 0, 'zone' }, { 3, '!setweather 0' }, { 5, 'mark start' } };
        for round = 1, 3 do
            for _, c in ipairs({
                { 3, '!reset' }, { 2, '!exec player:setMP(player:getMaxMP())' },
                { 2, '!exec for _,e in ipairs({36,37,39,48}) do player:delStatusEffect(e) end' },
                { 1, 'mark round ' .. round }, { 1, '/ja "Chainspell" <me>' },
                { 3, '/ma "Blaze Spikes" <me>' }, { 3, '/ma "Ice Spikes" <me>' }, { 3, '/ma "Shock Spikes" <me>' },
                { 3, '/ma "Stoneskin" <me>' }, { 3, '/ma "Blink" <me>' }, { 3, '/ma "Aquaveil" <me>' },
            }) do s[#s + 1] = c; end
        end
        s[#s + 1] = { 8, 'mark end' };
        s[#s + 1] = { 1, 'end' };
        return s;
    end)(),
};
local ALL = { 'home', 'markets', 'mines', 'lighting', 'weather', 'crowd', 'effects' };

local run = nil;        -- { name, steps, index, due (ms), waiting_zone, armed, part, queue }
local BANG_GAP = 2000;  -- ms
local last_bang = 0;
local in_world = false; -- a zone-in seen: chat commands work
local pending = nil;    -- what was asked for before that, as a function to call then

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
    if run ~= nil and not run.armed then return; end -- a run records the zone visits it makes
    local name = ('%scapture-%s-zone%d.jsonl'):format(dir, os.date('%Y%m%d-%H%M%S'), zone);
    local scene = '';
    if run ~= nil then
        run.part = run.part + 1;
        name = ('%s%s-%d.jsonl'):format(dir, run.name, run.part);
        scene = run.name;
    end
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

-------------------------------------------------------------------------------- the director
local function start(name, queue)
    if not in_world then
        pending = function () start(name, queue); end;
        return;
    end
    run = { name = name, steps = scenes[name], index = 1, part = 0, waiting_zone = false, queue = queue or {} };
    run.due = now_ms() + 5000 + run.steps[1][1] * 1000;
    print('[xireplay] scene ' .. name .. ': running');
end

local function tick()
    if run == nil or run.waiting_zone or now_ms() < run.due then return; end
    local s = run.steps[run.index];
    if s == nil then return; end
    local action = s[2];
    if action:sub(1, 1) == '!' and now_ms() < last_bang + BANG_GAP then
        run.due = last_bang + BANG_GAP;
        return;
    end
    run.index = run.index + 1;
    local next_step = run.steps[run.index];
    run.due = now_ms() + (next_step and next_step[1] or 0) * 1000;
    if action == 'zone' then
        run.waiting_zone, run.armed = true, true;
    elseif action == 'end' then
        print(('[xireplay] scene %s done: %d zone visits recorded'):format(run.name, run.part));
        local queue = run.queue;
        run = nil;
        close();
        if #queue > 0 then
            start(table.remove(queue, 1), queue);
        else
            -- for an unattended recording (tools/replay.py record): the run is over
            local f = io.open(dir .. 'last-run.txt', 'w');
            if f ~= nil then f:write('done ', os.date('!%Y-%m-%dT%H:%M:%SZ'), '\n'); f:close(); end
        end
    elseif action:sub(1, 5) == 'mark ' then
        write(('{"t":%d,"mark":"%s"}\n'):format(now_ms(), action:sub(6)));
        print('[xireplay] ' .. action:sub(6));
    else
        if action:sub(1, 1) == '!' then last_bang = now_ms(); end
        AshitaCore:GetChatManager():QueueCommand(1, action);
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
        if not in_world then
            in_world = true;
            if pending ~= nil then
                local p = pending;
                pending = nil;
                ashita.tasks.once(3, p);
            end
        end
        open(u32(e.data, 0x30));
        if run ~= nil and run.waiting_zone then
            run.waiting_zone = false;
            local s = run.steps[run.index];
            run.due = now_ms() + (s and s[1] or 0) * 1000;
        end
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
    tick();
end);

-- in quiet mode, only this addon's lines reach the chat log
ashita.events.register('text_in', 'xireplay_text', function (e)
    if quiet and not e.message:find('^%[%d%d:%d%d:%d%d%] ') and not e.message:find('%[xireplay%]') then
        e.blocked = true;
    end
end);

local USAGE = '/xireplay record on|off, frames on|off, quiet on|off, run <scene...|all>, stop, list, play <!replay words>';

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
    if verb == 'play' and #args >= 3 then
        -- says "!replay <words>" to the replay server once in the world (for a startup script)
        local words = table.concat(args, ' ', 3);
        local function go() AshitaCore:GetChatManager():QueueCommand(1, '!replay ' .. words); end
        if in_world then go(); else pending = go; end
        return;
    elseif verb == 'run' and #args >= 3 then
        local queue = {};
        for i = 3, #args do
            if args[i] == 'all' then
                for _, n in ipairs(ALL) do queue[#queue + 1] = n; end
            elseif scenes[args[i]] then
                queue[#queue + 1] = args[i];
            else
                print('[xireplay] no scene ' .. args[i]);
                return;
            end
        end
        close();
        start(table.remove(queue, 1), queue);
        return;
    elseif verb == 'stop' then
        run = nil;
        close();
        print('[xireplay] stopped');
        return;
    elseif verb == 'list' then
        print('[xireplay] scenes: ' .. table.concat(ALL, ', '));
        return;
    end
    print('[xireplay] ' .. USAGE);
end);

ashita.events.register('unload', 'xireplay_unload', function ()
    close();
    frames_flush();
    if frames.file ~= nil then frames.file:close(); end
end);
