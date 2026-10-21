dofile('addon/EvermoreRaidFinder/Model.lua')
local M = EvermoreRaidFinderModel
local s = M.New()
s.pending = { id = 1, action = 'catalog' }
M.Receive(s, '2\t1\tBEGIN')
M.Receive(s, '2\t1\tRAID\tmc\tMolten Core\t409\t4\t10\t26\t1')
assert(not s.raids.mc, 'catalogue must commit atomically')
assert(M.Receive(s, '2\t1\tEND') == 'catalog')
assert(s.selected == 'mc' and s.raids.mc.damage == 26 and not s.pending)
s.pending = { id = 1, action = 'ready', raid = 'mc' }
assert(not M.Receive(s, '3\t1\tRESULT\tready\tmc\t1\tReady'), 'ignore incompatible protocol')
assert(not M.Receive(s, '2\t1\tRESULT\tready\tbwl\t1\tReady'), 'ignore another raid')
assert(not M.Receive(s, '2\t1\tRESULT\tassemble\tmc\t1\tReady'), 'ignore another action')
assert(M.Receive(s, '2\t1\tRESULT\tready\tmc\t0\tNot ready') == 'error')
assert(not s.pending and s.notice == 'Not ready')
assert(not M.Receive(s, '2\t1\tRESULT\tready\tmc\t1\tReady'), 'unsolicited reply cannot start a run')
s.pending = { id = 1, action = 'ready', raid = 'mc' }
assert(M.Receive(s, '2\t1\tRESULT\tready\tmc\t1\tReady') == 'ready')
assert(not M.Receive(s, '2\t1\tRESULT\tready\tmc\t1\tReady'), 'replayed success cannot start again')
s.pending = { id = 1, action = 'status', raid = 'mc' }
M.Receive(s, '2\t1\tSTATUS\tbwl\t40\t1\tSaved')
assert(not s.members, 'other raids cannot overwrite status')
M.Receive(s, '2\t1\tSTATUS\tmc\t40\t1\tSaved')
assert(s.members == 40 and s.role == 1 and s.save == 'Saved')
s.pending = { id = 1, action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '2\t1\tBEGIN')
M.Receive(s, '2\t1\tRAID\tbad;command\tBad\t409\t4\t10\t26\t1')
M.Receive(s, '2\t1\tEND')
assert(#s.order == 0 and not s.selected, 'bad catalogue cannot inject commands')
print('PASS: catalogue, protocol version, selection, failure, unsolicited/replayed results, status, malformed entries')

-- Empty/restricted snapshots remove previously selected content and its saved metadata.
s.pending = { id = 1, action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '2\t1\tBEGIN')
M.Receive(s, '2\t1\tRAID\tbwl\tBlackwing Lair\t469\t2\t3\t35\t1')
M.Receive(s, '2\t1\tEND')
s.members, s.role, s.save = 40, 1, 'old save'
s.pending = { id = 1, action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '2\t1\tBEGIN')
M.Receive(s, '2\t1\tEND')
assert(not s.selected and not s.save and not s.members and #s.order == 0)
assert(s.notice:find('No raids are available'))
-- Invalidation during a response must not reinstall stale availability or start a run.
s.pending = { id = 1, action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '2\t1\tBEGIN')
M.Receive(s, '2\t1\tRAID\tbwl\tBlackwing Lair\t469\t2\t3\t35\t1')
M.Invalidate(s)
assert(M.Receive(s, '2\t1\tEND') == 'stale_catalog')
assert(not s.selected and s.catalogDirty)
s.selected = 'bwl'
s.pending = { id = 1, action = 'ready', raid = 'bwl' }
assert(M.Receive(s, '2\t1\tRESULT\tready\tbwl\t1\tReady') == 'stale_action')
print('PASS: restricted/empty catalogue removes selection; stale catalogue/readiness rejected after progression or group change')

-- A timed-out request and its retry have different IDs, even for the same action/raid.
s.catalogDirty = false
s.pending = { id = 22, action = 'ready', raid = 'bwl' }
assert(not M.Receive(s, '2\t21\tRESULT\tready\tbwl\t1\tOld ready'))
assert(s.pending.id == 22, 'old reply must not consume the retry')
assert(M.Receive(s, '2\t22\tRESULT\tready\tbwl\t1\tNew ready') == 'ready')
s.pending = { id = 24, action = 'catalog', generation = s.catalogGeneration }
assert(not M.Receive(s, '2\t23\tBEGIN'))
assert(not s.staging)
M.Receive(s, '2\t24\tBEGIN')
assert(not M.Receive(s, '2\t23\tRAID\tbwl\tOld raid\t469\t2\t3\t35\t1'))
assert(not M.Receive(s, '2\t23\tEND'))
assert(s.pending.id == 24)
M.Receive(s, '2\t24\tEND')
assert(#s.order == 0 and not s.selected, 'old snapshot cannot restore an ineligible raid')
print('PASS: timed-out ready/catalogue replies cannot impersonate a retry')

-- Assembly results survive their own roster events and subsequent status polling.
s.raids = { mc = {} }; s.selected = 'mc'; s.catalogDirty = false
s.pending = { id = 30, action = 'assemble', raid = 'mc' }
M.Invalidate(s)
assert(M.Receive(s, '2\t30\tRESULT\tassemble\tmc\t0\tShort of healers') == 'assemble')
assert(s.assemblyResult.message == 'Short of healers' and not s.assemblyResult.success)
s.pending = { id = 31, action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '2\t31\tBEGIN'); M.Receive(s, '2\t31\tRAID\tmc\tMolten Core\t409\t2\t3\t35\t1'); M.Receive(s, '2\t31\tEND')
s.pending = { id = 32, action = 'status', raid = 'mc' }
M.Receive(s, '2\t32\tRESULT\tstatus\tmc\t1\tReady')
assert(s.notice == 'Short of healers', 'post-assembly refresh must preserve failure details')
print('PASS: recruitment roster updates preserve the correlated assembly result and failure notice')
