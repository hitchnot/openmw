#ifndef MWDLG_DIALOGUEPSEUDOTOPICS_H
#define MWDLG_DIALOGUEPSEUDOTOPICS_H

#include <string>

namespace MWDialogue
{
    struct PseudoTopic
    {
        std::string mId;
        std::string mLabel;
        std::string mSkin;

        bool operator==(const PseudoTopic&) const = default;
    };
}

#endif
