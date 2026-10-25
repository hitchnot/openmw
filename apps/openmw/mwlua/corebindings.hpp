#ifndef MWLUA_COREBINDINGS_H
#define MWLUA_COREBINDINGS_H

#include <sol/forward.hpp>

namespace MWLua
{
    struct Context;

    void addCoreTimeBindings(sol::table& api, const Context& context);

    sol::table initCorePackage(const Context& context);
    sol::table initCoreApiExtensions(sol::state_view lua);
}

#endif // MWLUA_COREBINDINGS_H
