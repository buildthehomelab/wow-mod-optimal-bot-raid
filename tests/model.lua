dofile('addon/EvermoreRaidFinder/Model.lua')
local M = EvermoreRaidFinderModel
local s = M.New()
s.pending = { action = 'catalog' }
M.Receive(s, '1\tBEGIN')
M.Receive(s, '1\tRAID\tmc\tMolten Core\t409\t4\t10\t26\t1')
assert(not s.raids.mc, 'catalogue must commit atomically')
assert(M.Receive(s, '1\tEND') == 'catalog')
assert(s.selected == 'mc' and s.raids.mc.damage == 26 and not s.pending)
s.pending = { action = 'ready', raid = 'mc' }
assert(not M.Receive(s, '2\tRESULT\tready\tmc\t1\tReady'), 'ignore incompatible protocol')
assert(not M.Receive(s, '1\tRESULT\tready\tbwl\t1\tReady'), 'ignore another raid')
assert(not M.Receive(s, '1\tRESULT\tassemble\tmc\t1\tReady'), 'ignore another action')
assert(M.Receive(s, '1\tRESULT\tready\tmc\t0\tNot ready') == 'error')
assert(not s.pending and s.notice == 'Not ready')
assert(not M.Receive(s, '1\tRESULT\tready\tmc\t1\tReady'), 'unsolicited reply cannot start a run')
s.pending = { action = 'ready', raid = 'mc' }
assert(M.Receive(s, '1\tRESULT\tready\tmc\t1\tReady') == 'ready')
assert(not M.Receive(s, '1\tRESULT\tready\tmc\t1\tReady'), 'replayed success cannot start again')
M.Receive(s, '1\tSTATUS\tbwl\t40\t1\tSaved')
assert(not s.members, 'other raids cannot overwrite status')
M.Receive(s, '1\tSTATUS\tmc\t40\t1\tSaved')
assert(s.members == 40 and s.role == 1 and s.save == 'Saved')
s.pending = { action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '1\tBEGIN')
M.Receive(s, '1\tRAID\tbad;command\tBad\t409\t4\t10\t26\t1')
M.Receive(s, '1\tEND')
assert(#s.order == 0 and not s.selected, 'bad catalogue cannot inject commands')
print('PASS: catalogue, protocol version, selection, failure, unsolicited/replayed results, status, malformed entries')

-- Empty/restricted snapshots remove previously selected content and its saved metadata.
s.pending = { action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '1\tBEGIN')
M.Receive(s, '1\tRAID\tbwl\tBlackwing Lair\t469\t2\t3\t35\t1')
M.Receive(s, '1\tEND')
s.members, s.role, s.save = 40, 1, 'old save'
s.pending = { action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '1\tBEGIN')
M.Receive(s, '1\tEND')
assert(not s.selected and not s.save and not s.members and #s.order == 0)
assert(s.notice:find('No raids are available'))
-- Invalidation during a response must not reinstall stale availability or start a run.
s.pending = { action = 'catalog', generation = s.catalogGeneration }
M.Receive(s, '1\tBEGIN')
M.Receive(s, '1\tRAID\tbwl\tBlackwing Lair\t469\t2\t3\t35\t1')
M.Invalidate(s)
assert(M.Receive(s, '1\tEND') == 'stale_catalog')
assert(not s.selected and s.catalogDirty)
s.selected = 'bwl'
s.pending = { action = 'ready', raid = 'bwl' }
assert(M.Receive(s, '1\tRESULT\tready\tbwl\t1\tReady') == 'stale_action')
print('PASS: restricted/empty catalogue removes selection; stale catalogue/readiness rejected after progression or group change')
