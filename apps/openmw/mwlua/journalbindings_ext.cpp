#include "journalbindings_ext.hpp"

#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "../mwbase/environment.hpp"
#include "../mwbase/journal.hpp"
#include "../mwdialogue/journalcustom.hpp"
#include "../mwworld/esmstore.hpp"
#include "object.hpp"

namespace MWLua
{
    Quests::Quests(const Object& object)
        : mMutable(object.isGObject() || object.isSelfObject())
        , mGlobalObject(object.isGObject())
    {
    }

    void addQuestListBindings(sol::state_view lua, const MWBase::Journal* journal)
    {
        sol::usertype<Quests> quests = lua.new_usertype<Quests>("Quests");
        quests[sol::meta_function::to_string] = [](const Quests&) { return "Quests"; };
        quests[sol::meta_function::index] = [](const Quests& self, std::string_view questId) -> sol::optional<Quest> {
            ESM::RefId quest = ESM::RefId::deserializeText(questId);
            const ESM::Dialogue* dial = MWBase::Environment::get().getESMStore()->get<ESM::Dialogue>().search(quest);
            if (dial == nullptr || dial->mType != ESM::Dialogue::Journal)
                return sol::nullopt;
            return Quest{ quest, self.mMutable, self.mGlobalObject };
        };
        quests[sol::meta_function::pairs] = [journal](const Quests& self) {
            std::vector<ESM::RefId> ids;
            for (const auto& [id, _] : journal->getQuests())
                ids.push_back(id);
            return [ids = std::move(ids), i = std::size_t{ 0 }, self]() mutable
                       -> sol::optional<std::tuple<std::string, Quest>> {
                if (i >= ids.size())
                    return sol::nullopt;
                const ESM::RefId& id = ids[i++];
                return std::make_tuple(id.serializeText(), Quest{ id, self.mMutable, self.mGlobalObject });
            };
        };
    }

    void addCustomJournalEntryBinding(
        sol::usertype<Quest>& quest, std::function<void(std::function<void()>)> queueAction)
    {
        quest["addCustomJournalEntry"]
            = [queueAction = std::move(queueAction)](const Quest& q, std::string text, sol::optional<sol::table> opts) {
                  queueAction(makeCustomJournalEntryAction(q, std::move(text), std::move(opts)));
              };
    }

    std::function<void()> makeCustomJournalEntryAction(
        const Quest& quest, std::string text, sol::optional<sol::table> opts)
    {
        if (!quest.mGlobalObject)
            throw std::runtime_error("addCustomJournalEntry can only be used in global scripts");
        MWDialogue::validateCustomJournalQuest(quest.mQuestId);
        MWDialogue::CustomJournalOptions options;
        sol::optional<GObject> actor;
        if (opts)
        {
            if (auto stage = opts->get<sol::optional<int>>("stage"))
                options.mStage = *stage;
            if (auto day = opts->get<sol::optional<int>>("day"))
                options.mDay = *day;
            if (auto month = opts->get<sol::optional<int>>("month"))
            {
                if (*month < 1 || *month > 12)
                    throw std::runtime_error("Journal month must be between 1 and 12");
                options.mMonth = *month - 1;
            }
            if (auto dayOfMonth = opts->get<sol::optional<int>>("dayOfMonth"))
                options.mDayOfMonth = *dayOfMonth;
            if (auto before = opts->get<sol::optional<std::string>>("before"))
                options.mBefore = ESM::RefId::deserializeText(*before);
            options.mSilent = opts->get_or("silent", false);
            actor = opts->get<sol::optional<GObject>>("actor");
        }
        return [id = quest.mQuestId, text = std::move(text), options, actor = std::move(actor)] {
            MWWorld::Ptr actorPtr;
            if (actor)
                actorPtr = actor->ptr();
            MWBase::Environment::get().getJournal()->addCustomEntry(id, text, options, actorPtr);
        };
    }
}
