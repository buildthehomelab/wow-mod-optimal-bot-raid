// Called by the group's leader-change hook before the old leader is forgotten.
// Existing cleanup continues to prune departed bots and disbanded groups.
static void TransferDraftOwnership(ObjectGuid oldLeader, ObjectGuid newLeader)
{
    if (oldLeader == newLeader)
        return;
    std::lock_guard<std::mutex> lock(s_draftedBotsLock);
    auto it = s_draftedBots.find(oldLeader);
    if (it == s_draftedBots.end())
        return;
    auto bots = std::move(it->second);
    s_draftedBots.erase(it);
    auto& destination = s_draftedBots[newLeader];
    destination.insert(bots.begin(), bots.end());
}
