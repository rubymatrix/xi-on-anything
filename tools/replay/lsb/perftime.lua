-----------------------------------
-- func: perftime
-- desc: Sets the Vana'diel hour for tools/replay's recorded scenes (copy to LandSandBoat's
--       scripts/commands/). It only ever moves the clock back: sessions expire by this clock.
-- usage: !perftime [hour] (0-23, default 12)
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'i'
}

local function error(player, msg)
    player:printToPlayer(msg)
    player:printToPlayer('!perftime [hour]')
end

commandObj.onTrigger = function(player, hour)
    hour = hour or 12
    if hour < 0 or hour > 23 then
        error(player, 'The hour must be 0 to 23.')
        return
    end

    -- earth seconds back to that hour (25 Vana'diel seconds to one), never forward
    local offset = GetSystemTime() - os.time()
    local current = (VanadielHour() * 60 + VanadielMinute()) * 60
    local backwards = math.ceil(((current - hour * 3600) % 86400) / 25)
    SetTimeOffset(offset - backwards)
    player:printToPlayer(string.format('perftime: moved back %d earth seconds', backwards))
    xi.commands.time.onTrigger(player)
end

return commandObj
