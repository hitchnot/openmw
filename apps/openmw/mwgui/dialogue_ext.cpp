#include "dialogue.hpp"
#include "windowmanagerimp.hpp"

#include <algorithm>

#include <components/misc/strings/algorithm.hpp>

#include "../mwbase/environment.hpp"

namespace MWGui
{
    bool DialogueWindow::canSelectTopic(std::string_view topicId) const
    {
        if (mPtr.isEmpty() || mGoodbye || MWBase::Environment::get().getDialogueManager()->isInChoice())
            return false;
        return std::any_of(mKeywords.begin(), mKeywords.end(),
            [&](const std::string& keyword) { return Misc::StringUtils::ciEqual(keyword, topicId); });
    }

    void DialogueWindow::selectTopic(const std::string& topicId)
    {
        if (canSelectTopic(topicId))
            onTopicActivated(topicId);
    }

    void DialogueWindow::closeDialogue()
    {
        onGoodbyeActivated();
    }

    MWWorld::Ptr WindowManager::getDialogueActor() const
    {
        if (!mDialogueWindow || !containsMode(GM_Dialogue) || getMode() != GM_Dialogue)
            return {};
        return mDialogueWindow->getActor();
    }

    void WindowManager::addDialogueResponse(const std::string& title, const std::string& text, bool teachTopics)
    {
        if (getDialogueActor().isEmpty())
            return;
        if (teachTopics)
        {
            MWBase::Environment::get().getDialogueManager()->addTopicsFromText(text);
            mDialogueWindow->updateTopics();
        }
        mDialogueWindow->addResponse(title, text);
    }

    bool WindowManager::canSelectDialogueTopic(std::string_view topicId) const
    {
        return !getDialogueActor().isEmpty() && mDialogueWindow->canSelectTopic(topicId);
    }

    void WindowManager::selectDialogueTopic(const std::string& topicId)
    {
        if (!getDialogueActor().isEmpty())
            mDialogueWindow->selectTopic(topicId);
    }

    void WindowManager::closeDialogue()
    {
        if (!getDialogueActor().isEmpty())
            mDialogueWindow->closeDialogue();
    }
}
