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
    if p[1] ~= "2" or not state.pending or p[2] ~= tostring(state.pending.id) then return end
    -- Every frame, including BEGIN/END, belongs to one specific server request.
    table.remove(p, 2)
    local kind = p[2]
    if kind == "BEGIN" then
        state.staging, state.stagingOrder, state.stagingId = {}, {}, state.pending.id
    elseif kind == "RAID" and state.staging and state.stagingId == state.pending.id then
        local map, tanks, healers, damage = tonumber(p[5]), tonumber(p[6]), tonumber(p[7]), tonumber(p[8])
        if not p[3] or not string.match(p[3], "^[a-z]+$") or not map or not tanks or not healers or not damage then return end
        if not state.staging[p[3]] then state.stagingOrder[#state.stagingOrder + 1] = p[3] end
        state.staging[p[3]] = { alias = p[3], name = p[4], map = map,
            tanks = tanks, healers = healers, damage = damage, enabled = p[9] == "1" }
    elseif kind == "END" and state.staging and state.stagingId == state.pending.id then
        local request = state.pending
        if not request or request.action ~= "catalog" then
            state.staging, state.stagingOrder, state.stagingId = nil, nil, nil
            return
        end
        state.pending = nil
        if (request.generation or 0) ~= state.catalogGeneration then
            state.staging, state.stagingOrder, state.stagingId = nil, nil, nil
            return "stale_catalog"
        end
        state.raids, state.order = state.staging, state.stagingOrder
        state.staging, state.stagingOrder, state.stagingId = nil, nil, nil
        state.catalogDirty = false
        if not state.raids[state.selected] then
            state.selected = state.order[1]
            state.members, state.role, state.save = nil, nil, nil
        end
        state.notice = state.assemblyResult and state.assemblyResult.message
            or (#state.order == 0 and "No raids are available for your progression, level and group."
            or "Choose a raid. Human healers and damage dealers are supported; a bot tank leads.")
        return "catalog"
    elseif kind == "STATUS" and p[3] == state.selected then
        state.members, state.role, state.save = tonumber(p[4]), tonumber(p[5]), p[6]
    elseif kind == "RESULT" and state.pending and p[3] == state.pending.action and
        p[4] == state.pending.raid and p[4] == state.selected then
        state.pending = nil
        if p[3] == "assemble" then
            -- Assembly itself changes the roster. Preserve its exact reply, then
            -- revalidate the resulting group before offering outdoor travel.
            state.assemblyResult = { raid = p[4], success = p[5] == "1", message = p[6] or "No details returned." }
            state.notice = state.assemblyResult.message
            M.Invalidate(state)
            return "assemble"
        end
        if state.catalogDirty then return "stale_action" end
        state.notice = p[6] or "No details returned."
        if p[3] == "status" and state.assemblyResult then
            state.notice = state.assemblyResult.message .. (p[5] == "1" and "" or ("\n" .. state.notice))
        end
        if p[5] == "1" then return p[3] end
        return "error"
    end
end
