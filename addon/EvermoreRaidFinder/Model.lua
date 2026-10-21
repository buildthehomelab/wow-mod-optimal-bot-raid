-- No saved queue: every action rechecks the live server roster and instance.
EvermoreRaidFinderModel = {}
local M = EvermoreRaidFinderModel
function M.Split(text)
    local parts = {}
    for part in string.gmatch((text or "") .. "\t", "([^\t]*)\t") do
        parts[#parts + 1] = part
    end
    return parts
end
function M.New()
    return { raids = {}, order = {}, catalogDirty = true, catalogGeneration = 0, notice = "Connecting to the realm..." }
end
function M.Invalidate(state)
    state.catalogGeneration = state.catalogGeneration + 1
    state.catalogDirty = true
end
function M.Receive(state, message)
    local p = M.Split(message)
    if p[1] ~= "1" then return end
    local kind = p[2]
    if kind == "BEGIN" then
        state.staging, state.stagingOrder = {}, {}
    elseif kind == "RAID" and state.staging then
        local map, tanks, healers, damage = tonumber(p[5]), tonumber(p[6]), tonumber(p[7]), tonumber(p[8])
        if not p[3] or not string.match(p[3], "^[a-z]+$") or not map or not tanks or not healers or not damage then return end
        if not state.staging[p[3]] then state.stagingOrder[#state.stagingOrder + 1] = p[3] end
        state.staging[p[3]] = { alias = p[3], name = p[4], map = map,
            tanks = tanks, healers = healers, damage = damage, enabled = p[9] == "1" }
    elseif kind == "END" and state.staging then
        local request = state.pending
        if not request or request.action ~= "catalog" then
            state.staging, state.stagingOrder = nil, nil
            return
        end
        state.pending = nil
        if (request.generation or 0) ~= state.catalogGeneration then
            state.staging, state.stagingOrder = nil, nil
            return "stale_catalog"
        end
        state.raids, state.order = state.staging, state.stagingOrder
        state.staging, state.stagingOrder = nil, nil
        state.catalogDirty = false
        if not state.raids[state.selected] then
            state.selected = state.order[1]
            state.members, state.role, state.save = nil, nil, nil
        end
        state.notice = #state.order == 0 and "No raids are available for your progression, level and group."
            or "Choose a raid. Human healers and damage dealers are supported; a bot tank leads."
        return "catalog"
    elseif kind == "STATUS" and p[3] == state.selected then
        state.members, state.role, state.save = tonumber(p[4]), tonumber(p[5]), p[6]
    elseif kind == "RESULT" and state.pending and p[3] == state.pending.action and
        p[4] == state.pending.raid and p[4] == state.selected then
        state.pending = nil
        if state.catalogDirty then return "stale_action" end
        state.notice = p[6] or "No details returned."
        if p[5] == "1" then return p[3] end
        return "error"
    end
end
