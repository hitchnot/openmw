#ifndef MWLUA_RECORDSBINDINGS_EXT_H
#define MWLUA_RECORDSBINDINGS_EXT_H

#include <functional>

#include <sol/sol.hpp>

namespace ESM
{
    struct Dialogue;
}

namespace MWWorld
{
    template <class T>
    class Store;
}

namespace MWLua
{
    /// openmw.world.records: DMO's creation of dialogues and their infos. Each mutation is queued; the call
    /// returns the id at once and refuses, at once, what the queued mutation would refuse.
    sol::table initDialogueRecordsBindings(sol::state_view lua, MWWorld::Store<ESM::Dialogue>& store,
        std::function<void(std::function<void()>)> queueAction);
}

#endif
