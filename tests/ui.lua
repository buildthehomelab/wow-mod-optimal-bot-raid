-- Exercise the real addon under a minimal 3.3.5-style frame/event API.
local frames, buttons, sent = {}, {}, {}
local methods = {}
function methods:SetScript(name, fn) self.scripts[name] = fn end
function methods:SetText(text) self.text = text; if self.kind == 'Button' then buttons[text] = self end end
function methods:Show() self.shown = true; if self.scripts.OnShow then self.scripts.OnShow(self) end end
function methods:Hide() self.shown = false end
function methods:IsShown() return self.shown end
function methods:Enable() self.enabled = true end
function methods:Disable() self.enabled = false end
function methods:CreateFontString() return setmetatable({ scripts = {} }, { __index = methods }) end
setmetatable(methods, { __index = function() return function() end end })
function CreateFrame(kind, name)
    local f = setmetatable({ kind = kind, name = name, scripts = {}, enabled = true }, { __index = methods })
    frames[#frames + 1] = f
    if name then _G[name] = f end
    return f
end
UIParent = {}; UISpecialFrames = {}; StaticPopupDialogs = {}; SlashCmdList = {}; CANCEL = 'Cancel'
local now = 0
local lastNonce
function GetTime() return now end
function GetNumRaidMembers() return 40 end
function UnitName() return 'Tester' end
function SendChatMessage(text, channel)
    local raw = text
    if string.find(text, '^%.botraid finder ') then
        lastNonce = string.match(text, ' (%d+)$')
        assert(lastNonce, 'finder commands must carry request IDs')
        text = string.gsub(text, ' %d+$', '')
        text = string.gsub(text, ' catalog %-$', ' catalog')
    end
    sent[#sent + 1] = { text, channel, raw }
end
function SendAddonMessage(prefix, text, channel) sent[#sent + 1] = { prefix .. ':' .. text, channel } end
function date(_, n) return tostring(n) end
local popup
function StaticPopup_Show(which) popup = { which = which }; return popup end
function StaticPopup_Hide() popup = nil end
dofile('addon/EvermoreRaidFinder/Model.lua')
dofile('addon/EvermoreRaidFinder/RaidFinder.lua')
local f = EvermoreRaidFinderFrame
local events = frames[#frames]
local function message(text, sender, nonce)
    text = "2\t" .. (nonce or lastNonce) .. "\t" .. string.sub(text, 3)
    events.scripts.OnEvent(events, 'CHAT_MSG_ADDON', 'EMLFR', text, 'WHISPER', sender or 'Tester')
end
SlashCmdList.EVERMORERAIDFINDER()
assert(sent[#sent][1] == '.botraid finder catalog')
message('1\tBEGIN')
message('1\tRAID\tmc\tMolten Core\t409\t2\t3\t35\t1')
message('1\tEND')
assert(sent[#sent][1] == '.botraid finder status mc')
message('1\tSTATUS\tmc\t1\t3\tNo personal raid save.')
message('1\tRESULT\tstatus\tmc\t1\tReady')
buttons['Find Raid'].scripts.OnClick()
assert(sent[#sent][1] == '.botraid finder assemble mc')
message('1\tRESULT\tassemble\tmc\t1\tAssembled', 'OtherPlayer')
assert(not popup, 'other players cannot offer entry')
message('1\tRESULT\tassemble\tmc\t0\tShort of healers')
assert(not popup, 'failed assembly must not offer entry')
buttons['Find Raid'].scripts.OnClick()
message('1\tRESULT\tassemble\tmc\t1\tAssembled')
assert(popup and popup.which == 'EVERMORE_LFR_ENTER')
StaticPopupDialogs.EVERMORE_LFR_ENTER.OnAccept(popup)
assert(sent[#sent][1] == '.botraid finder enter mc')
events.scripts.OnEvent(events, 'PLAYER_ENTERING_WORLD')
assert(not buttons['Bring Bots'].enabled)
now = now + 0.5; f.scripts.OnUpdate(f, 0.5)
message('1\tBEGIN')
message('1\tRAID\tmc\tMolten Core\t409\t2\t3\t35\t1')
message('1\tEND')
message('1\tRESULT\tstatus\tmc\t1\tReady')
buttons['Bring Bots'].scripts.OnClick()
message('1\tRESULT\trally\tmc\t1\tArriving')
buttons['Start Run'].scripts.OnClick()
message('1\tRESULT\tready\tmc\t0\tNot arrived')
assert(sent[#sent][1] == '.botraid finder ready mc', 'failed readiness must not start DC')
buttons['Start Run'].scripts.OnClick()
message('1\tRESULT\tready\tmc\t1\tReady')
assert(sent[#sent-1][1] == '.dc on')
local count = #sent
message('1\tRESULT\tready\tmc\t1\tReady')
assert(#sent == count, 'replay must not issue another DC command')
buttons.Refresh.scripts.OnClick()
now = 11; f.scripts.OnUpdate(f, 11)
assert(buttons.Refresh.enabled, 'timeout must unlock the UI')
print('PASS: real addon catalogue, status, assembly failure/success, sender authentication, entry confirmation, rally, readiness, replay, timeout')

-- Refresh remains usable when no content is eligible; no launch button is offered.
buttons.Refresh.scripts.OnClick()
message('1\tBEGIN')
message('1\tEND')
assert(buttons.Refresh.enabled and not buttons['Find Raid'].enabled and not buttons['Enter Raid'].enabled)
events.scripts.OnEvent(events, 'QUEST_LOG_UPDATE')
assert(not buttons['Start Run'].enabled)
now = now + 0.5; f.scripts.OnUpdate(f, 0.5)
assert(sent[#sent][1] == '.botraid finder catalog')
print('PASS: arrival/progression events invalidate offers and refresh the catalogue; empty catalogue cannot launch')

-- An old END must not complete a newer catalogue after the first request times out.
local oldNonce = lastNonce
now = now + 11; f.scripts.OnUpdate(f, 11)
buttons.Refresh.scripts.OnClick()
assert(lastNonce ~= oldNonce)
message('1\tBEGIN', nil, oldNonce)
message('1\tRAID\tbwl\tOld raid\t469\t2\t3\t35\t1', nil, oldNonce)
message('1\tEND', nil, oldNonce)
assert(not buttons.Refresh.enabled and not buttons['Find Raid'].enabled)
message('1\tBEGIN')
message('1\tEND')
assert(buttons.Refresh.enabled and not buttons['Find Raid'].enabled)
print('PASS: real UI rejects timed-out catalogue packets during retry')
