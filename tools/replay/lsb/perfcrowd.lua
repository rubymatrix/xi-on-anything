-----------------------------------
-- func: perfcrowd
-- desc: Brings the first N mobs of the current zone to the player, in a ring around them, for
--       tools/replay's recorded scenes (copy to LandSandBoat's scripts/commands/).
-- usage: !perfcrowd [count] (default 40)
-----------------------------------
---@type TCommand
local commandObj = {}

commandObj.cmdprops =
{
    permission = 1,
    parameters = 'i'
}

commandObj.onTrigger = function(player, count)
    local zone = player:getZone()
    if not zone then
        return
    end
    count = count or 40
    local base = 0x1000000 + zone:getID() * 0x1000
    local x, y, z, rot = player:getXPos(), player:getYPos(), player:getZPos(), player:getRotPos()
    local brought, spawned = 0, 0
    for slot = 1, 1023 do
        if brought == count then
            break
        end

        local id = base + slot
        local mob = GetMobByID(id)
        if mob then
            if not mob:isSpawned() then
                SpawnMob(id)
                spawned = spawned + 1
            end

            -- fan the crowd out in a ring, 6 yalms out, so they do not stack on one point
            local a = (brought / count) * 2 * math.pi
            mob:setPos(x + 6 * math.cos(a), y, z + 6 * math.sin(a), rot)
            brought = brought + 1
        end
    end

    player:printToPlayer(string.format('perfcrowd: %d brought (%d spawned for it)', brought, spawned))
end

return commandObj
