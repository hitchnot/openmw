#include "dialoguechoices.hpp"

#include <algorithm>
#include <stdexcept>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "dialoguemanagerimp.hpp"

namespace MWDialogue
{
    void DialogueManager::addLuaChoices(
        const std::string& offerId, const std::vector<LuaChoice>& choices, bool exclusive)
    {
        if (choices.size() > static_cast<std::size_t>(sLuaChoiceEnd - mNextLuaChoice))
            throw std::runtime_error("Lua dialogue choice ids exhausted");
        clearLuaChoices(offerId);
        for (const auto& choice : choices)
        {
            const int id = mNextLuaChoice++;
            mLuaChoices.emplace(id, LuaChoiceEntry{ { offerId, choice.mId, true }, exclusive });
            addChoice(choice.mText, id);
        }
    }

    void DialogueManager::clearLuaChoices(const std::optional<std::string>& offerId)
    {
        std::erase_if(mChoices, [&](const auto& choice) {
            const auto it = mLuaChoices.find(choice.second);
            return it != mLuaChoices.end() && (!offerId || it->second.mSelection.mOfferId == *offerId);
        });
        std::erase_if(mLuaChoices, [&](const auto& entry) {
            return !offerId || entry.second.mSelection.mOfferId == *offerId;
        });
    }

    bool DialogueManager::isInChoice() const
    {
        return mIsInChoice || std::any_of(mLuaChoices.begin(), mLuaChoices.end(),
                                 [](const auto& entry) { return entry.second.mExclusive; });
    }

    void DialogueManager::questionAnswered(int answer, ResponseCallback* callback)
    {
        if (isLuaChoiceId(answer))
        {
            const auto it = mLuaChoices.find(answer);
            if (it != mLuaChoices.end())
            {
                const ChoiceSelection selection = it->second.mSelection;
                clearLuaChoices();
                emitChoiceSelected(selection);
            }
            return;
        }
        clearLuaChoices();
        answerVanilla(answer, callback);
        emitChoiceSelected({ {}, std::to_string(answer), false });
    }

    void DialogueManager::emitChoiceSelected(const ChoiceSelection& selection)
    {
        MWBase::Environment::get().getLuaManager()->onDialogueChoiceSelected(
            mActor, selection.mOfferId, selection.mChoiceId, selection.mLua);
    }
}
