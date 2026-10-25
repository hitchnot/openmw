#ifndef MWDLG_DIALOGUECHOICES_H
#define MWDLG_DIALOGUECHOICES_H

#include <string>

namespace MWDialogue
{
    inline constexpr int sLuaChoiceBegin = 0x4000000;
    inline constexpr int sLuaChoiceEnd = 0x7fffffff;

    constexpr bool isLuaChoiceId(int id)
    {
        return id >= sLuaChoiceBegin && id < sLuaChoiceEnd;
    }

    struct LuaChoice
    {
        std::string mId;
        std::string mText;
    };

    struct ChoiceSelection
    {
        std::string mOfferId;
        std::string mChoiceId;
        bool mLua;
    };
}

#endif
