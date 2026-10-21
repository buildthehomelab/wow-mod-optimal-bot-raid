local M = EvermoreRaidFinderModel
local state = M.New()
local frame = CreateFrame("Frame", "EvermoreRaidFinderFrame", UIParent)
frame:SetSize(760, 510)
frame:SetPoint("CENTER")
frame:SetFrameStrata("DIALOG")
frame:SetMovable(true)
frame:EnableMouse(true)
frame:RegisterForDrag("LeftButton")
frame:SetScript("OnDragStart", frame.StartMoving)
frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
frame:SetBackdrop({ bgFile = "Interface\\DialogFrame\\UI-DialogBox-Background",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border", tile = true, tileSize = 32,
    edgeSize = 32, insets = { left = 8, right = 8, top = 8, bottom = 8 } })
frame:SetBackdropColor(0.05, 0.06, 0.08, 1)
frame:Hide()
table.insert(UISpecialFrames, "EvermoreRaidFinderFrame")
local function Label(parent, text, x, y, width, font)
    local label = parent:CreateFontString(nil, "OVERLAY", font or "GameFontNormal")
    label:SetPoint("TOPLEFT", x, y)
    label:SetWidth(width)
    label:SetJustifyH("LEFT")
    label:SetText(text)
    return label
end
Label(frame, "EVERMORE  |  RAID FINDER", 26, -24, 660, "GameFontNormalLarge")
Label(frame, "Dungeons & Raids  /  Raid Finder", 26, -52, 660, "GameFontHighlightSmall")
local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
close:SetPoint("TOPRIGHT", -6, -6)
close:SetScript("OnClick", function() frame:Hide() end)
Label(frame, "CLASSIC", 28, -94, 180)
Label(frame, "More raids will appear as complete runs are validated.", 28, -202, 176, "GameFontDisableSmall")
local title = Label(frame, "Select a raid", 230, -92, 495, "GameFontNormalLarge")
local composition = Label(frame, "", 230, -124, 495, "GameFontHighlight")
local description = Label(frame, "", 230, -159, 485, "GameFontHighlight")
local members = Label(frame, "", 230, -239, 485, "GameFontHighlight")
local save = Label(frame, "", 230, -270, 485, "GameFontHighlightSmall")
local notice = Label(frame, "", 230, -308, 485, "GameFontHighlight")
notice:SetHeight(72)
notice:SetJustifyV("TOP")
local run = Label(frame, "", 28, -430, 690, "GameFontHighlightSmall")
local buttons, rows = {}, {}
local Refresh, Request
local catalogDue = 0
local requestId = math.floor(GetTime() * 1000) % 2147483646
local function Button(text, x, y, width, callback)
    local b = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
    b:SetSize(width, 26)
    b:SetPoint("TOPLEFT", x, y)
    b:SetText(text)
    b:SetScript("OnClick", callback)
    buttons[#buttons + 1] = b
    return b
end
Request = function(action)
    if state.pending then return end
    if action ~= "catalog" and (not state.selected or state.catalogDirty) then return end
    if action == "catalog" then state.catalogDirty = true end
    requestId = requestId % 2147483646 + 1
    state.pending = { action = action, raid = state.selected, at = GetTime(), generation = state.catalogGeneration, id = requestId }
    state.notice = action == "assemble" and "Recruiting the raid..." or "Checking with the realm..."
    -- Native server commands also work while solo. Commands are consumed by the server.
    SendChatMessage(".botraid finder " .. action .. " " .. (action == "catalog" and "-" or state.selected) .. " " .. requestId, "SAY")
    Refresh()
end
StaticPopupDialogs.EVERMORE_LFR_ENTER = {
    text = "Enter %s? Existing raid saves and normal loot rules apply. Each human must confirm entry separately.",
    button1 = "Enter Raid", button2 = CANCEL, timeout = 0, whileDead = false, hideOnEscape = true,
    OnAccept = function(self)
        if self.data == state.selected then Request("enter") end
    end,
}
local find = Button("Find Raid", 230, -389, 145, function() Request("assemble") end)
local enter = Button("Enter Raid", 385, -389, 145, function()
    local raid = state.raids[state.selected]
    if raid then
        local popup = StaticPopup_Show("EVERMORE_LFR_ENTER", raid.name)
        if popup then popup.data = state.selected end
    end
end)
local rally = Button("Bring Bots", 540, -389, 145, function() Request("rally") end)
local start = Button("Start Run", 230, -466, 115, function() Request("ready") end)
local pause = Button("Pause / Resume", 355, -466, 145, function() SendChatMessage(".dc pause", "SAY") end)
local stop = Button("Stop Run", 510, -466, 115, function() SendChatMessage(".dc off", "SAY") end)
local check = Button("Refresh", 28, -282, 174, function() Request("catalog") end)
local journal = Button("Bosses & Loot", 28, -320, 174, function()
    if Journal and Journal.OpenSection then Journal.OpenSection("raids")
    else state.notice = "Install the Evermore Journal addon to browse bosses and loot."; Refresh() end
end)
StaticPopupDialogs.EVERMORE_LFR_DISMISS = {
    text = "Dismiss the bots recruited for you? Your friends and manually invited bots remain in the group.",
    button1 = "Dismiss Bots", button2 = CANCEL, timeout = 0, whileDead = false, hideOnEscape = true,
    OnAccept = function() SendChatMessage(".dc off", "SAY"); SendChatMessage(".botraid dismiss", "SAY") end,
}
local dismiss = Button("Dismiss Bots", 28, -358, 174, function() StaticPopup_Show("EVERMORE_LFR_DISMISS") end)
local names = { mc = "Descend into the Firelord's domain.\n\nFull raid • Level 60 bots • Normal difficulty",
    bwl = "Assault Nefarian's stronghold above Blackrock Spire.\n\nFull raid • Level 60 bots • Normal difficulty" }
Refresh = function()
    local raid = not state.catalogDirty and state.raids[state.selected]
    title:SetText(raid and raid.name or (state.catalogDirty and "Checking eligibility..." or "No eligible raids"))
    composition:SetText(raid and string.format("40 players   |   %d tanks   %d healers   %d damage", raid.tanks, raid.healers, raid.damage) or "")
    description:SetText(raid and (names[raid.alias] or "Full raid") or "Only raids unlocked for your character and group are listed.")
    local roles = { [0] = "Tank (unsupported for autonomous runs)", [1] = "Healer", [2] = "Melee damage", [3] = "Ranged damage" }
    members:SetText(string.format("Roster: %d / 40   |   Your active spec: %s", state.members or 1, roles[state.role] or "Checking"))
    save:SetText((state.save and string.gsub(state.save, "resets (%d+)", function(epoch) return "resets " .. date("%b %d, %H:%M", tonumber(epoch)) end)) or "Refresh to check eligibility and your raid save.")
    notice:SetText(state.notice)
    for i, alias in ipairs(state.order) do
        if not rows[i] then
            local row = CreateFrame("Button", nil, frame, "UIPanelButtonTemplate")
            row:SetSize(174, 36); row:SetPoint("TOPLEFT", 28, -119 - (i - 1) * 42)
            row:SetScript("OnClick", function(self)
                if state.pending then return end
                StaticPopup_Hide("EVERMORE_LFR_ENTER")
                state.selected = self.alias
                state.members, state.role, state.save = nil, nil, nil
                run:SetText("")
                Request("status")
            end)
            rows[i] = row
        end
        local r = state.raids[alias]
        rows[i].alias = alias
        rows[i]:SetText((alias == state.selected and "|cffffd100" or "") .. r.name .. (r.enabled and "" or "  [Locked]"))
        if state.catalogDirty then rows[i]:Hide() else rows[i]:Show() end
    end
    for i = #state.order + 1, #rows do rows[i]:Hide() end
    local available = raid and raid.enabled and not state.pending
    for _, b in ipairs({find, enter, rally, start}) do if available then b:Enable() else b:Disable() end end
    if state.pending then check:Disable() else check:Enable() end
    if not state.pending then journal:Enable() else journal:Disable() end
end
local elapsed = 0
frame:SetScript("OnUpdate", function(_, dt)
    elapsed = elapsed + dt
    if state.pending and GetTime() - state.pending.at > 10 then
        state.pending = nil
        catalogDue = math.huge
        state.notice = "No reply from Raid Finder. Check the module version and connection, then Refresh."
        Refresh()
    end
    if state.catalogDirty and not state.pending and GetTime() >= catalogDue then Request("catalog") end
    if elapsed > 5 and not state.pending and GetNumRaidMembers() > 0 then
        elapsed = 0
        SendAddonMessage("DC", "CMD\tstatus", "RAID")
    end
end)
frame:SetScript("OnShow", function() M.Invalidate(state); Request("catalog") end)
local events = CreateFrame("Frame")
events:RegisterEvent("CHAT_MSG_ADDON")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("PARTY_MEMBERS_CHANGED")
events:RegisterEvent("RAID_ROSTER_UPDATE")
events:RegisterEvent("QUEST_LOG_UPDATE")
events:RegisterEvent("PLAYER_LEVEL_UP")
events:SetScript("OnEvent", function(_, event, prefix, message, channel, sender)
    if event ~= "CHAT_MSG_ADDON" then
        if event == "PLAYER_ENTERING_WORLD" then
            run:SetText("")
            state.pending = nil
        end
        M.Invalidate(state)
        StaticPopup_Hide("EVERMORE_LFR_ENTER")
        state.notice = "Checking progression and group eligibility..."
        catalogDue = GetTime() + 0.5
        Refresh()
        return
    end
    -- Finder packets are built by the server as whispers from ourselves.
    -- Never accept another player's packet as authority to issue a start command.
    if prefix == "EMLFR" and channel == "WHISPER" and sender == UnitName("player") then
        local result = M.Receive(state, message)
        Refresh()
        if result == "catalog" and state.selected then Request("status") end
        if result == "assemble" then
            local popup = StaticPopup_Show("EVERMORE_LFR_ENTER", state.raids[state.selected].name)
            if popup then popup.data = state.selected end
        elseif result == "ready" then
            SendChatMessage(".dc on", "SAY")
            SendAddonMessage("DC", "CMD\tstatus", "RAID")
            state.notice = "Start requested. Dungeon Clear will report the run state below."
            Refresh()
        end
    elseif prefix == "DC" and frame:IsShown() then
        local p = M.Split(message)
        if p[1] == "STATUS" then
            run:SetText("Run: " .. (p[7] or "unknown") .. "   |   " .. (p[4] or "") .. "   " .. (p[8] or ""))
        elseif p[1] == "ERROR" then state.notice = p[2] or "Dungeon Clear error."; Refresh() end
    end
end)
SLASH_EVERMORERAIDFINDER1 = "/lfr"
SLASH_EVERMORERAIDFINDER2 = "/raidfinder"
SlashCmdList.EVERMORERAIDFINDER = function() if frame:IsShown() then frame:Hide() else frame:Show() end end
Refresh()
