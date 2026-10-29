#include "dialoguepseudotopics.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwworld/class.hpp"
#include "dialoguemanagerimp.hpp"

namespace MWDialogue
{
    void DialogueManager::addPseudoTopic(const MWWorld::Ptr& actor, const PseudoTopic& topic)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
            throw std::runtime_error("Pseudo-topics require an actor");
        if (topic.mId.empty() || topic.mLabel.empty())
            throw std::runtime_error("Pseudo-topic id and label must not be empty");
        auto& topics = mPseudoTopics[actor.getCellRef().getRefNum()];
        PseudoTopic normalized{ Misc::StringUtils::lowerCase(topic.mId), topic.mLabel, topic.mSkin };
        const auto it = std::find_if(
            topics.begin(), topics.end(), [&](const PseudoTopic& entry) { return entry.mId == normalized.mId; });
        if (it == topics.end())
            topics.push_back(std::move(normalized));
        else
            *it = std::move(normalized);
    }

    void DialogueManager::removePseudoTopic(const MWWorld::Ptr& actor, std::string_view topicId)
    {
        if (actor.isEmpty())
            return;
        const auto it = mPseudoTopics.find(actor.getCellRef().getRefNum());
        if (it == mPseudoTopics.end())
            return;
        std::erase_if(
            it->second, [&](const PseudoTopic& topic) { return Misc::StringUtils::ciEqual(topic.mId, topicId); });
        if (it->second.empty())
            mPseudoTopics.erase(it);
    }

    const std::vector<PseudoTopic>& DialogueManager::getPseudoTopics(const MWWorld::Ptr& actor) const
    {
        static const std::vector<PseudoTopic> empty;
        if (actor.isEmpty())
            return empty;
        const auto it = mPseudoTopics.find(actor.getCellRef().getRefNum());
        return it == mPseudoTopics.end() ? empty : it->second;
    }

    bool DialogueManager::selectPseudoTopic(const MWWorld::Ptr& actor, std::string_view topicId)
    {
        if (isInChoice() || isGoodbye())
            return false;
        const auto& topics = getPseudoTopics(actor);
        const auto it = std::find_if(topics.begin(), topics.end(),
            [&](const PseudoTopic& topic) { return Misc::StringUtils::ciEqual(topic.mId, topicId); });
        if (it == topics.end())
            return false;
        emitPseudoTopicSelected(actor, it->mId);
        return true;
    }

    void DialogueManager::emitPseudoTopicSelected(const MWWorld::Ptr& actor, const std::string& topicId)
    {
        MWBase::Environment::get().getLuaManager()->onDialogueTopicSelected(actor, topicId, true);
    }
}
