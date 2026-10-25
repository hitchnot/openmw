#ifndef MWLUA_DIALOGUEBINDINGS_EXT_H
#define MWLUA_DIALOGUEBINDINGS_EXT_H

#include <sol/sol.hpp>

namespace MWLua
{
    struct Context;
    sol::table initDialogueUiPackage(const Context& context);
}

#endif
