#include "dialoguebindings_ext.hpp"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <components/lua/util.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwdialogue/dialoguechoices.hpp"
#include "../mwdialogue/dialoguepseudotopics.hpp"
#include "../mwworld/class.hpp"
#include "context.hpp"
#include "luamanagerimp.hpp"
#include "object.hpp"

namespace MWLua
{
    namespace
    {
        bool queueDialogueAction(LuaManager* luaManager, std::function<void()> action)
        {
            MWBase::WindowManager* windows = MWBase::Environment::get().getWindowManager();
            MWBase::DialogueManager* dialogue = MWBase::Environment::get().getDialogueManager();
            if (windows->getDialogueActor().isEmpty())
                return false;
            const auto session = dialogue->dialogueSession();
            luaManager->addAction(
                [windows, dialogue, session, action = std::move(action)] {
                    if (!windows->getDialogueActor().isEmpty() && dialogue->dialogueSession() == session)
                        action();
                },
                "Update dialogue");
            return true;
        }
    }

    void addDialogueTopicBindings(sol::table api, MWBase::DialogueManager& dialogue,
        std::function<void(std::function<void()>)> queue, DialogueTopicWindow window)
    {
        const auto queueTopicAction = [&dialogue, queue, window](std::function<void()> action) {
            if (window.mActor().isEmpty())
                return false;
            const auto session = dialogue.dialogueSession();
            queue([&dialogue, window, session, action = std::move(action)] {
                if (!window.mActor().isEmpty() && dialogue.dialogueSession() == session)
                    action();
            });
            return true;
        };
        api["selectTopic"] = [queueTopicAction, window](std::string topicId) {
            if (!window.mCanSelect(topicId))
                return false;
            return queueTopicAction([window, topicId = std::move(topicId)] { window.mSelect(topicId); });
        };
        api["refresh"] = [queueTopicAction, window] { return queueTopicAction(window.mRefresh); };
        api["addPseudoTopic"] = [&dialogue, queue, window](const LObject& actor, std::string topicId, std::string label,
                                    sol::optional<sol::table> options) {
            if (!actor.ptr().getClass().isActor())
                throw std::runtime_error("Pseudo-topics require an actor");
            if (topicId.empty() || label.empty())
                throw std::runtime_error("Pseudo-topic id and label must not be empty");
            MWDialogue::PseudoTopic topic{ std::move(topicId), std::move(label),
                options ? options->get_or<std::string>("skin", "") : "" };
            const auto epoch = dialogue.pseudoTopicEpoch();
            queue([&dialogue, window, actor, epoch, topic = std::move(topic)] {
                if (dialogue.pseudoTopicEpoch() != epoch)
                    return;
                const auto& ptr = actor.ptrOrEmpty();
                if (ptr.isEmpty())
                    return;
                dialogue.addPseudoTopic(ptr, topic);
                if (window.mActor() == ptr)
                    window.mRefresh();
            });
        };
        api["removePseudoTopic"] = [&dialogue, queue, window](const LObject& actor, std::string topicId) {
            if (!actor.ptr().getClass().isActor())
                throw std::runtime_error("Pseudo-topics require an actor");
            const auto epoch = dialogue.pseudoTopicEpoch();
            queue([&dialogue, window, actor, epoch, topicId = std::move(topicId)] {
                if (dialogue.pseudoTopicEpoch() != epoch)
                    return;
                const auto& ptr = actor.ptrOrEmpty();
                if (ptr.isEmpty())
                    return;
                dialogue.removePseudoTopic(ptr, topicId);
                if (window.mActor() == ptr)
                    window.mRefresh();
            });
        };
    }

    sol::table initDialogueUiPackage(const Context& context)
    {
        sol::table api(context.sol(), sol::create);
        auto* luaManager = context.mLuaManager;
        MWBase::WindowManager* windows = MWBase::Environment::get().getWindowManager();
        MWBase::DialogueManager* dialogue = MWBase::Environment::get().getDialogueManager();
        api["isOpen"] = [windows] { return !windows->getDialogueActor().isEmpty(); };
        api["actor"] = [windows]() -> sol::optional<LObject> {
            const auto actor = windows->getDialogueActor();
            if (actor.isEmpty())
                return sol::nullopt;
            return LObject(actor);
        };
        api["addResponse"] = [luaManager, windows](std::string text, sol::optional<sol::table> options) {
            std::string speaker;
            bool teachTopics = false;
            if (options)
            {
                speaker = options->get_or<std::string>("speaker", "");
                teachTopics = options->get_or("teachTopics", false);
            }
            return queueDialogueAction(luaManager,
                [windows, text = std::move(text), speaker = std::move(speaker), teachTopics] {
                    windows->addDialogueResponse(speaker, text, teachTopics);
                });
        };
        api["addChoices"] = [luaManager, dialogue, windows](std::string offerId, sol::table choices,
                                sol::optional<sol::table> options) {
            if (windows->getDialogueActor().isEmpty() || dialogue->isGoodbye())
                return false;
            std::vector<MWDialogue::LuaChoice> parsed;
            for (std::size_t i = 1; i <= choices.size(); ++i)
            {
                sol::table choice = choices.get<sol::table>(i);
                parsed.push_back({ choice.get<std::string>("id"), choice.get<std::string>("text") });
            }
            const bool exclusive = options ? options->get_or("exclusive", true) : true;
            return queueDialogueAction(luaManager,
                [dialogue, offerId = std::move(offerId), parsed = std::move(parsed), exclusive] {
                    if (!dialogue->isGoodbye())
                        dialogue->addLuaChoices(offerId, parsed, exclusive);
                });
        };
        api["clearChoices"] = [luaManager, dialogue](sol::optional<std::string> offerId) {
            std::optional<std::string> id;
            if (offerId)
                id = *offerId;
            return queueDialogueAction(luaManager, [dialogue, id = std::move(id)] { dialogue->clearLuaChoices(id); });
        };
        addDialogueTopicBindings(api, *dialogue,
            [luaManager](
                std::function<void()> action) { luaManager->addAction(std::move(action), "Update dialogue topics"); },
            { [windows] { return windows->getDialogueActor(); },
                [windows](std::string_view topic) { return windows->canSelectDialogueTopic(topic); },
                [windows](const std::string& topic) { windows->selectDialogueTopic(topic); },
                [windows] { windows->refreshDialogue(); } });
        api["close"] = [luaManager, windows] {
            return queueDialogueAction(luaManager, [windows] { windows->closeDialogue(); });
        };
        return LuaUtil::makeReadOnly(api);
    }

    void LuaManager::onDialogueTopicSelected(const MWWorld::Ptr& actor, const std::string& topicId, bool pseudo)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["actor"] = LObject(actor);
            data["topic"] = Misc::StringUtils::lowerCase(topicId);
            data["pseudo"] = pseudo;
            sendLocalEvent(mPlayer, "DialogueTopicSelected", data);
        });
    }

    void LuaManager::onDialogueChoiceSelected(
        const MWWorld::Ptr& actor, const std::string& offerId, const std::string& choiceId, bool lua)
    {
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table data = view.newTable();
            data["actor"] = LObject(actor);
            data["offerId"] = offerId;
            data["choiceId"] = choiceId;
            data["lua"] = lua;
            sendLocalEvent(mPlayer, "DialogueChoiceSelected", data);
        });
    }
}
