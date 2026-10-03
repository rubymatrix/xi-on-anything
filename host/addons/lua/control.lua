--[==[
The control port: a built-in addon (xi kind) that lets another program drive the game. host64 loads
it on the first frame when FFXI_CONTROL is set (core.c): FFXI_CONTROL=1 listens on 127.0.0.1:54300,
FFXI_CONTROL=<port> on that port. tools/xi_mcp.py speaks it for MCP clients.

The protocol is JSON, one object per line each way:

  -> {"id": 1, "cmd": "state", "args": {...}}
  <- {"id": 1, "ok": true, "result": ...}    or    {"id": 1, "ok": false, "error": "..."}

Requests run as tasks on the game thread (one frame at a time, so a request may wait on frames:
key holds, frame captures, waits). Commands:

  state                      sign-in state, the player, zone, position, target, menu, chat input
  chat_send {line}           a line as if typed: /commands, //commands; other text is said (/say)
  chat_log {since, limit}    chat lines after sequence number `since`: the server's chat (0x017) and
                             the host's lines (UTF-8, codes removed)
  chat_input {text, open}    the chat input line: read it, or set its text
  keys {keys, hold_ms, gap_ms}  key presses in order: "enter", "down", "numpad5", "ctrl+t",
                             "wait:500" (ms); down for hold_ms, then up, gap_ms between
  key {key, down}            one key down (true) or up (false), left as it is
  release_keys               every key this port pressed comes up
  capture {path}             the next frame to a file (see d3d8_capture); -> path, w, h, format
  entities {radius, limit}   entities near the player: index, id, name, type, distance, hp%
  target {index}             target an entity as the game does
  wait {ms | frames}         returns after that long
  lua {code}                 Lua run in this addon (xi.* available): what it returns, as JSON
--]==]

local socket = require('socket.core')

local PORT = tonumber(os.getenv('FFXI_CONTROL') or '') or 1
if PORT <= 1 then PORT = 54300 end

local game, res, memory = xi.game, xi.res, xi.memory

-------------------------------------------------------------------------------- JSON

local json = {}

local escapes = { ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t' }

local function encode(v, out, depth)
    local t = type(v)
    if depth > 32 then out[#out + 1] = 'null' return end
    if t == 'nil' then
        out[#out + 1] = 'null'
    elseif t == 'boolean' then
        out[#out + 1] = v and 'true' or 'false'
    elseif t == 'number' then
        if v ~= v or v == math.huge or v == -math.huge then out[#out + 1] = 'null'
        elseif v == math.floor(v) and math.abs(v) < 2^53 then out[#out + 1] = string.format('%d', v)
        else out[#out + 1] = string.format('%.6g', v) end
    elseif t == 'string' then
        out[#out + 1] = '"' .. v:gsub('[%c"\\]', function(c) return escapes[c] or string.format('\\u%04x', c:byte()) end) .. '"'
    elseif t == 'table' then
        local n = #v
        local is_array = n > 0 or next(v) == nil
        if is_array then
            for k in pairs(v) do
                if type(k) ~= 'number' or k < 1 or k > n or k ~= math.floor(k) then is_array = false break end
            end
        end
        if is_array then
            out[#out + 1] = '['
            for i = 1, n do
                if i > 1 then out[#out + 1] = ',' end
                encode(v[i], out, depth + 1)
            end
            out[#out + 1] = ']'
        else
            out[#out + 1] = '{'
            local first = true
            for k, x in pairs(v) do
                if not first then out[#out + 1] = ',' end
                first = false
                encode(tostring(k), out, depth + 1)
                out[#out + 1] = ':'
                encode(x, out, depth + 1)
            end
            out[#out + 1] = '}'
        end
    else
        out[#out + 1] = '"' .. tostring(v) .. '"'
    end
end

function json.encode(v)
    local out = {}
    encode(v, out, 0)
    return table.concat(out)
end

local function utf8_char(c)
    if c < 0x80 then return string.char(c) end
    if c < 0x800 then return string.char(0xC0 + math.floor(c / 64), 0x80 + c % 64) end
    return string.char(0xE0 + math.floor(c / 4096), 0x80 + math.floor(c / 64) % 64, 0x80 + c % 64)
end

function json.decode(s)
    local i = 1
    local function ws() i = s:find('[^ \t\r\n]', i) or #s + 1 end
    local value
    local function str()
        local out = {}
        i = i + 1
        while true do
            local c = s:sub(i, i)
            if c == '' then error('unterminated string') end
            if c == '"' then i = i + 1 break end
            if c == '\\' then
                local e = s:sub(i + 1, i + 1)
                if e == 'u' then
                    out[#out + 1] = utf8_char(tonumber(s:sub(i + 2, i + 5), 16) or 63)
                    i = i + 6
                else
                    out[#out + 1] = ({ b = '\b', f = '\f', n = '\n', r = '\r', t = '\t' })[e] or e
                    i = i + 2
                end
            else
                out[#out + 1] = c
                i = i + 1
            end
        end
        return table.concat(out)
    end
    value = function()
        ws()
        local c = s:sub(i, i)
        if c == '{' then
            local t = {}
            i = i + 1
            ws()
            if s:sub(i, i) == '}' then i = i + 1 return t end
            while true do
                ws()
                local k = str()
                ws()
                if s:sub(i, i) ~= ':' then error('expected :') end
                i = i + 1
                t[k] = value()
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == '}' then return t end
                if d ~= ',' then error('expected , or }') end
            end
        elseif c == '[' then
            local t = {}
            i = i + 1
            ws()
            if s:sub(i, i) == ']' then i = i + 1 return t end
            while true do
                t[#t + 1] = value()
                ws()
                local d = s:sub(i, i)
                i = i + 1
                if d == ']' then return t end
                if d ~= ',' then error('expected , or ]') end
            end
        elseif c == '"' then
            return str()
        elseif s:sub(i, i + 3) == 'true' then i = i + 4 return true
        elseif s:sub(i, i + 4) == 'false' then i = i + 5 return false
        elseif s:sub(i, i + 3) == 'null' then i = i + 4 return nil
        else
            local num = s:match('^-?%d+%.?%d*[eE]?[-+]?%d*', i)
            if not num or num == '' then error('unexpected ' .. c) end
            i = i + #num
            return tonumber(num)
        end
    end
    return value()
end

-------------------------------------------------------------------------------- the chat log

local log, log_seq, LOG_MAX = {}, 0, 500

local function plain(raw)
    local ok, s = pcall(res.to_utf8, raw)
    if not ok or not s then s = raw end
    return (s:gsub('[%z\1-\8\11-\31\127]', ''))
end

local function add_line(mode, text, sender)
    log_seq = log_seq + 1
    log[#log + 1] = { seq = log_seq, mode = mode, text = text, sender = sender }
    if #log > LOG_MAX then table.remove(log, 1) end
end

-- What the chat log shows comes two ways: lines the host writes (text_in), and the chat the server
-- sends (0x017: kind, attributes, zone, sender[15], message). The game's own lines (/echo, its
-- system messages) don't pass the host's write_line hook in every build.
xi.events.on('text_in', function(e)
    if not e.blocked then add_line(e.mode_modified or e.mode, plain(e.modified or e.data or '')) end
end)

xi.events.on('packet_in', function(e)
    if e.id ~= 0x017 or e.blocked or e.injected then return end
    local d = e.data or ''
    if #d < 24 then return end
    local sender = plain((d:sub(9, 23):gsub('%z.*', '')))
    add_line(d:byte(5), plain((d:sub(24):gsub('%z.*', ''))), sender ~= '' and sender or nil)
end)

-------------------------------------------------------------------------------- game state

local function zone_name(id)
    local ok, z = pcall(res.zone, id)
    if ok and type(z) == 'table' then
        local n = z.name_utf8 or z.name
        if type(n) == 'table' then n = n.en or n[2] or n[1] end
        if type(n) == 'string' then return n end
    end
    return nil
end

-- The menu on screen ("menu    charsel" .. "menu    inventor"), from the game's current menu object
local menu_ptr
local function menu_name()
    if menu_ptr == nil then
        local ok, p = pcall(memory.find, 'FFXiMain.dll', 0, '8B480C85C974??8B510885D274??3B05', 16, 0)
        menu_ptr = ok and p or 0
    end
    if not menu_ptr or menu_ptr == 0 then return nil end
    local ok, name = pcall(function()
        local p = memory.read_uint32(menu_ptr)
        if not p or p == 0 then return '' end
        local v = memory.read_uint32(p)
        if not v or v == 0 then return '' end
        local h = memory.read_uint32(v + 4)
        if not h or h == 0 then return '' end
        return memory.read_string(h + 0x46, 16)
    end)
    if not ok or not name then return nil end
    name = name:gsub('%z.*', ''):gsub('^menu%s+', ''):gsub('%s+$', '')
    return name
end

local function position(i)
    local x, z, y = game.entity(i, 'Movement.LocalPosition.X'), game.entity(i, 'Movement.LocalPosition.Z'),
        game.entity(i, 'Movement.LocalPosition.Y')
    if not x then return nil end
    return { x = x, y = y, z = z, heading = game.entity(i, 'Heading') }
end

local function state()
    local s = { login_status = game.login_status(), menu = menu_name() }
    local text = xi.chat.input()
    s.chat_input = text and plain(text) or nil
    local pi = game.player_index()
    if pi and pi >= 0 and game.entity(pi, 'Name') then
        local zone = game.party_member(0, 'Zone')
        s.player = {
            index = pi, id = game.entity(pi, 'ServerId'), name = game.entity(pi, 'Name'),
            hp = game.party_member(0, 'HP'), mp = game.party_member(0, 'MP'), tp = game.party_member(0, 'TP'),
            hpp = game.entity(pi, 'HPPercent'), status = game.entity(pi, 'Status'),
            job = game.party_member(0, 'MainJob'), level = game.party_member(0, 'MainJobLevel'),
            position = position(pi),
        }
        s.zone = { id = zone, name = zone and zone_name(zone) or nil }
    end
    local ti = game.target('Targets[0].Index')
    if ti and ti > 0 and game.target('Targets[0].IsActive') ~= 0 then
        s.target = { index = ti, id = game.entity(ti, 'ServerId'), name = game.entity(ti, 'Name'),
            hpp = game.entity(ti, 'HPPercent'), distance = game.entity(ti, 'Distance') and math.sqrt(game.entity(ti, 'Distance')) }
    end
    return s
end

local ENTITY_TYPES = { [0] = 'pc', [1] = 'npc', [2] = 'npc', [3] = 'npc', [4] = 'elevator', [5] = 'ship' }

local function entities(a)
    local radius = tonumber(a.radius) or 50
    local limit = tonumber(a.limit) or 50
    local out = {}
    local count = game.entity_count() or 0
    for i = 0, count - 1 do
        local name = game.entity(i, 'Name')
        if name and name ~= '' then
            local d2 = game.entity(i, 'Distance') or 0
            local flags = game.entity(i, 'SpawnFlags') or 0
            local d = math.sqrt(d2)
            if d <= radius and (game.entity(i, 'Render.Flags0') or 0) ~= 0 then
                out[#out + 1] = { index = i, id = game.entity(i, 'ServerId'), name = name, distance = d,
                    hpp = game.entity(i, 'HPPercent'), spawn_flags = flags, type = ENTITY_TYPES[game.entity(i, 'Type') or -1],
                    position = position(i) }
            end
        end
    end
    table.sort(out, function(p, q) return p.distance < q.distance end)
    while #out > limit do out[#out] = nil end
    return out
end

-------------------------------------------------------------------------------- keys

local pressed = {} -- DIK -> true: what this port holds down

local MODS = { ctrl = 'lctrl', control = 'lctrl', shift = 'lshift', alt = 'lalt', cmd = 'lwin', win = 'lwin' }

local function key_code(name)
    local d = xi.input.code(name)
    if not d then error('no key "' .. tostring(name) .. '"') end
    return d
end

local function key(d, down)
    xi.input.inject(d, down)
    pressed[d] = down or nil
end

local function sleep_ms(ms)
    if ms and ms > 0 then coroutine.sleep(ms / 1000) else coroutine.sleepf(1) end
end

local function keys(a)
    local list = a.keys
    if type(list) == 'string' then list = { list } end
    if type(list) ~= 'table' then error('keys: a list of key names') end
    local hold, gap = tonumber(a.hold_ms) or 80, tonumber(a.gap_ms) or 120
    for _, k in ipairs(list) do
        local ms = tostring(k):match('^wait:(%d+)$')
        if ms then
            sleep_ms(tonumber(ms))
        else
            local parts = {}
            for p in tostring(k):lower():gmatch('[^+]+') do parts[#parts + 1] = p end
            local codes = {}
            for i, p in ipairs(parts) do
                codes[i] = key_code(i < #parts and MODS[p] or p)
            end
            for _, d in ipairs(codes) do key(d, true) coroutine.sleepf(1) end
            sleep_ms(hold)
            for i = #codes, 1, -1 do key(codes[i], false) coroutine.sleepf(1) end
            sleep_ms(gap)
        end
    end
    return { sent = #list }
end

local function release_keys()
    local n = 0
    for d in pairs(pressed) do xi.input.inject(d, false) n = n + 1 end
    pressed = {}
    return { released = n }
end

-------------------------------------------------------------------------------- the rest of the commands

local function capture(a)
    local path = a.path or (xi.paths.data .. 'control_frame.xif')
    local before = xi.ui.capture_result()
    xi.ui.capture(path)
    for _ = 1, 120 do
        coroutine.sleepf(1)
        local serial, ok, w, h, f = xi.ui.capture_result()
        if serial ~= before then
            if not ok then error('the frame capture failed') end
            return { path = path, width = w, height = h, format = f }
        end
    end
    error('no frame was drawn')
end

local function run_lua(a)
    local fn, err = loadstring(a.code or '', '=control')
    if not fn then
        fn, err = loadstring('return ' .. (a.code or ''), '=control')
        if not fn then error(err) end
    end
    local r = { pcall(fn) }
    if not r[1] then error(r[2]) end
    if #r <= 2 then return r[2] end
    table.remove(r, 1)
    return r
end

local commands = {
    state = function() return state() end,
    chat_send = function(a)
        if type(a.line) ~= 'string' then error('chat_send: line') end
        local line = a.line
        if not line:match('^/') then line = '/say ' .. line end -- the game sends a bare line nowhere
        xi.chat.run(line, tonumber(a.mode) or 1)
        return true
    end,
    chat_log = function(a)
        local since, limit = tonumber(a.since) or 0, tonumber(a.limit) or 100
        local out = {}
        for _, l in ipairs(log) do
            if l.seq > since then out[#out + 1] = l end
        end
        while #out > limit do table.remove(out, 1) end
        return { lines = out, last = log_seq }
    end,
    chat_input = function(a)
        if type(a.text) == 'string' then xi.chat.set_input(a.text) end
        local text = xi.chat.input()
        return { text = text and plain(text) or '' }
    end,
    keys = keys,
    key = function(a)
        key(type(a.key) == 'number' and a.key or key_code(a.key), a.down ~= false)
        return true
    end,
    release_keys = release_keys,
    capture = capture,
    entities = entities,
    target = function(a)
        local i = tonumber(a.index)
        if not i then error('target: index') end
        game.set_target_index(i)
        coroutine.sleepf(2)
        return state().target
    end,
    wait = function(a)
        if a.frames then coroutine.sleepf(tonumber(a.frames)) else sleep_ms(tonumber(a.ms) or 100) end
        return true
    end,
    lua = run_lua,
}

-------------------------------------------------------------------------------- the server

local server, clients = nil, {}

local function send(c, t)
    local line = json.encode(t) .. '\n'
    local i = 1
    while i <= #line do
        local n, err, part = c.sock:send(line, i)
        if n then i = n + 1
        elseif err == 'timeout' and coroutine.running() then
            i = (part or i - 1) + 1
            coroutine.sleepf(1)
        else
            c.closed = true
            return
        end
    end
end

local function handle(c, line)
    local ok, req = pcall(json.decode, line)
    if not ok or type(req) ~= 'table' then
        send(c, { ok = false, error = 'bad JSON: ' .. tostring(req) })
        return
    end
    local fn = commands[req.cmd]
    if not fn then
        send(c, { id = req.id, ok = false, error = 'no command "' .. tostring(req.cmd) .. '"' })
        return
    end
    xi.tasks.spawn(function()
        local ok2, r = pcall(fn, type(req.args) == 'table' and req.args or {})
        if ok2 then
            send(c, { id = req.id, ok = true, result = r })
        else
            send(c, { id = req.id, ok = false, error = tostring(r) })
        end
    end)
end

local function poll()
    if server then
        while true do
            local s = server:accept()
            if not s then break end
            s:settimeout(0)
            pcall(s.setoption, s, 'tcp-nodelay', true)
            clients[#clients + 1] = { sock = s, buf = '' }
            xi.log('control: a client connected')
        end
    end
    for i = #clients, 1, -1 do
        local c = clients[i]
        while not c.closed do
            local data, err, part = c.sock:receive(8192)
            data = data or part
            if data and #data > 0 then c.buf = c.buf .. data end
            if err == 'closed' then c.closed = true end
            if not data or #data == 0 or err then break end
        end
        while true do
            local nl = c.buf:find('\n', 1, true)
            if not nl then break end
            local line = c.buf:sub(1, nl - 1)
            c.buf = c.buf:sub(nl + 1)
            if line:match('%S') then handle(c, line) end
        end
        if c.closed then
            c.sock:close()
            table.remove(clients, i)
            xi.log('control: a client left')
        end
    end
end

local function listen()
    local s = socket.tcp()
    s:setoption('reuseaddr', true)
    local ok, err = s:bind('127.0.0.1', PORT)
    if not ok then
        xi.log('control: cannot listen on 127.0.0.1:' .. PORT .. ': ' .. tostring(err))
        s:close()
        return nil
    end
    s:listen(4)
    s:settimeout(0)
    xi.log('control: listening on 127.0.0.1:' .. PORT)
    return s
end

server = listen()
xi.events.on('frame', poll)
xi.events.on('unload', function()
    release_keys()
    for _, c in ipairs(clients) do c.sock:close() end
    if server then server:close() end
end)
