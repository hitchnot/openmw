#ifndef MWLUA_JOURNALBINDINGS_EXT_H
#define MWLUA_JOURNALBINDINGS_EXT_H

#include <functional>
#include <string>

#include <sol/sol.hpp>

#include <components/esm/refid.hpp>

namespace MWBase
{
    class Journal;
}

namespace MWLua
{
    class Object;

    struct Quests
    {
        bool mMutable = false;
        bool mGlobalObject = false;

        explicit Quests(const Object& object);
    };

    struct Quest
    {
        ESM::RefId mQuestId;
        bool mMutable = false;
        bool mGlobalObject = false;
    };
}

namespace sol
{
    template <>
    struct is_automagical<MWBase::Journal> : std::false_type
    {
    };
    template <>
    struct is_automagical<MWLua::Quests> : std::false_type
    {
    };
    template <>
    struct is_automagical<MWLua::Quest> : std::false_type
    {
    };
}

namespace MWLua
{
    void addQuestListBindings(sol::state_view lua, const MWBase::Journal* journal);
    void addJournalEntryBindings(sol::table& playerBindings, sol::state_view lua, const MWBase::Journal* journal);
    void addCustomJournalEntryBinding(
        sol::usertype<Quest>& quest, std::function<void(std::function<void()>)> queueAction);
    std::function<void()> makeCustomJournalEntryAction(
        const Quest& quest, std::string text, sol::optional<sol::table> opts);
}

#endif
