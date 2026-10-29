#ifndef MWLUA_DIALOGUEBINDINGS_EXT_H
#define MWLUA_DIALOGUEBINDINGS_EXT_H

#include <sol/sol.hpp>

#include <functional>
#include <string>
#include <string_view>

#include "../mwworld/ptr.hpp"

namespace MWBase
{
    class DialogueManager;
}

namespace MWLua
{
    struct Context;
    // GUI boundary for topic bindings; the actor is empty when dialogue is closed or covered.
    struct DialogueTopicWindow
    {
        std::function<MWWorld::Ptr()> mActor;
        std::function<bool(std::string_view)> mCanSelect;
        std::function<void(const std::string&)> mSelect;
        std::function<void()> mRefresh;
    };
    void addDialogueTopicBindings(sol::table api, MWBase::DialogueManager& dialogue,
        std::function<void(std::function<void()>)> queue, DialogueTopicWindow window);
    sol::table initDialogueUiPackage(const Context& context);
}

#endif
