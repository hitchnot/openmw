#include "journalcustom.hpp"

#include <stdexcept>
#include <utility>

#include <components/interpreter/defines.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwscript/interpretercontext.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/globals.hpp"
#include "journalimp.hpp"

namespace MWDialogue
{
    bool isCustomJournalEntry(const ESM::RefId& infoId)
    {
        return infoId.startsWith("dmo:j:");
    }

    void validateCustomJournalQuest(const ESM::RefId& id)
    {
        const ESM::Dialogue* dialogue = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>().search(id);
        if (!dialogue || dialogue->mType != ESM::Dialogue::Journal)
            throw std::runtime_error("Quest " + id.serializeText() + " has no Journal dialogue record");
    }

    void Topic::insertEntry(const Entry& entry, const ESM::RefId& before)
    {
        mEntries.insert(journalEntryPosition(mEntries, before), entry);
    }

    int Journal::getCustomJournalGlobal(MWWorld::GlobalVariableName name) const
    {
        return MWBase::Environment::get().getWorld()->getGlobalInt(name);
    }

    void Journal::customJournalQuestUpdated(const ESM::RefId& id, int index)
    {
        MWBase::Environment::get().getLuaManager()->questUpdated(id, index);
    }

    void Journal::showCustomJournalMessage()
    {
        MWBase::Environment::get().getWindowManager()->messageBox("#{sJournalEntry}");
    }

    void Journal::addCustomEntry(const ESM::RefId& id, std::string text, const CustomJournalOptions& options,
        const MWWorld::Ptr& actor)
    {
        validateCustomJournalQuest(id);
        const auto date = [this](const std::optional<int>& value, MWWorld::GlobalVariableName global) {
            return value ? *value : getCustomJournalGlobal(global);
        };
        const int day = date(options.mDay, MWWorld::Globals::sDaysPassed);
        const int month = date(options.mMonth, MWWorld::Globals::sMonth);
        const int dayOfMonth = date(options.mDayOfMonth, MWWorld::Globals::sDay);
        std::string actorName;
        if (!actor.isEmpty())
        {
            MWScript::InterpreterContext interpreterContext(&actor.getRefData().getLocals(), actor);
            text = Interpreter::fixDefinesDialog(text, interpreterContext);
            actorName = actor.getClass().getName(actor);
        }
        const ESM::RefId infoId = ESM::RefId::stringRefId("dmo:j:" + std::to_string(++mCustomEntryCounter));
        StampedJournalEntry entry(id, infoId, std::move(text), day, month, dayOfMonth, std::move(actorName));
        Quest& quest = getOrStartQuest(id);
        const auto position = journalEntryPosition(mJournal, options.mBefore);
        const auto next = std::find_if(position, mJournal.end(), [&](const auto& item) { return item.mTopic == id; });
        quest.insertEntry(entry, next == mJournal.end() ? ESM::RefId() : next->mInfoId);
        mJournal.insert(position, std::move(entry));
        if (!options.mSilent)
        {
            if (options.mStage)
                quest.setIndex(*options.mStage, [&] { customJournalQuestUpdated(id, *options.mStage); });
            showCustomJournalMessage();
        }
    }
}
