#ifndef MWGUI_DIALOGUE_EXT_H
#define MWGUI_DIALOGUE_EXT_H

#include <functional>
#include <string_view>
#include <vector>

namespace MWDialogue
{
    struct PseudoTopic;
}

namespace MWGui
{
    void dispatchDialogueTopic(std::string_view topicId, const std::vector<MWDialogue::PseudoTopic>& pseudoTopics,
        const std::function<void()>& pseudo, const std::function<void()>& ordinary);
}

#endif
