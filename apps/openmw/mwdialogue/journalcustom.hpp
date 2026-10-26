#ifndef GAME_MWDIALOGUE_JOURNALCUSTOM_H
#define GAME_MWDIALOGUE_JOURNALCUSTOM_H

#include <algorithm>
#include <cstddef>
#include <optional>

#include <components/esm/refid.hpp>

namespace MWDialogue
{
    struct CustomJournalOptions
    {
        std::optional<int> mStage;
        std::optional<int> mDay;
        std::optional<int> mMonth; // zero-based, like StampedJournalEntry
        std::optional<int> mDayOfMonth;
        ESM::RefId mBefore;
        bool mSilent = false;
    };

    bool isCustomJournalEntry(const ESM::RefId& infoId);
    void validateCustomJournalQuest(const ESM::RefId& id);

    template <class Container>
    auto journalEntryPosition(Container& entries, const ESM::RefId& before)
    {
        if (before.empty())
            return entries.end();
        return std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.mInfoId == before; });
    }

    template <class Container>
    std::size_t countPersistentJournalEntries(const Container& entries)
    {
        return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(),
            [](const auto& entry) { return !isCustomJournalEntry(entry.mInfoId); }));
    }
}

#endif
