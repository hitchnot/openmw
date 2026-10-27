#include "recordsbindings_ext.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm3/dialoguecondition.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loadinfo.hpp>
#include <components/lua/luastate.hpp>
#include <components/misc/strings/lower.hpp>

#include "../mwdialogue/dialogueoverlay.hpp"
#include "../mwworld/store.hpp"

namespace MWLua
{
    namespace
    {
        using Condition = ESM::DialogueCondition;

        // What this frame's queued calls will create, so a later call in the same frame may name it.
        struct Pending
        {
            std::uint64_t mEpoch = 0;
            std::unordered_map<ESM::RefId, ESM::Dialogue::Type> mDialogues;
            std::unordered_map<ESM::RefId, std::unordered_set<ESM::RefId>> mInfos;

            // A load or a new game drops what was queued before it, so nothing it named is pending any more.
            void follow(std::uint64_t epoch)
            {
                if (mEpoch == epoch)
                    return;
                mEpoch = epoch;
                mDialogues.clear();
                mInfos.clear();
            }
        };

        constexpr std::array<std::pair<std::string_view, Condition::Function>, 12> sConditionTypes{ {
            { "global", Condition::Function_Global },
            { "local", Condition::Function_Local },
            { "journal", Condition::Function_Journal },
            { "item", Condition::Function_Item },
            { "dead", Condition::Function_Dead },
            { "not_id", Condition::Function_NotId },
            { "not_faction", Condition::Function_NotFaction },
            { "not_class", Condition::Function_NotClass },
            { "not_race", Condition::Function_NotRace },
            { "not_cell", Condition::Function_NotCell },
            { "not_local", Condition::Function_NotLocal },
            { "choice", Condition::Function_Choice },
        } };

        constexpr std::array<std::pair<std::string_view, Condition::Comparison>, 6> sOperators{ {
            { "==", Condition::Comp_Eq },
            { "!=", Condition::Comp_Ne },
            { ">", Condition::Comp_Gt },
            { ">=", Condition::Comp_Ge },
            { "<", Condition::Comp_Ls },
            { "<=", Condition::Comp_Le },
        } };

        // The fields an info draft may carry; a condition carries type, variable, operator and value.
        constexpr std::array<std::string_view, 16> sInfoFields{ "id", "text", "resultScript", "actor", "race",
            "class", "faction", "pcFaction", "cell", "disposition", "journalIndex", "rank", "pcRank", "gender",
            "questStatus", "conditions" };

        template <class Table>
        std::string required(const Table& table, std::string_view key, std::string_view where)
        {
            const auto value = table.template get<sol::optional<std::string>>(key);
            if (!value || value->empty())
                throw std::runtime_error(std::string(where) + "." + std::string(key) + " is required");
            return *value;
        }

        template <class Table>
        ESM::RefId refId(const Table& table, std::string_view key)
        {
            const auto value = table.template get<sol::optional<std::string>>(key);
            return value ? ESM::RefId::stringRefId(*value) : ESM::RefId();
        }

        ESM::Dialogue::Type dialogueType(std::string_view type)
        {
            if (type == "topic")
                return ESM::Dialogue::Topic;
            if (type == "journal")
                return ESM::Dialogue::Journal;
            if (type == "greeting")
                return ESM::Dialogue::Greeting;
            throw std::runtime_error("draft.type is topic, journal or greeting");
        }

        MWDialogue::InfoAnchor anchorFrom(const sol::table& anchor)
        {
            using Kind = MWDialogue::InfoAnchor::Kind;
            if (const auto after = anchor.get<sol::optional<std::string>>("after"))
                return { Kind::After, ESM::RefId::stringRefId(*after) };
            if (const auto before = anchor.get<sol::optional<std::string>>("before"))
                return { Kind::Before, ESM::RefId::stringRefId(*before) };
            const std::string at = anchor.get<sol::optional<std::string>>("at").value_or("");
            if (at == "front")
                return { Kind::Front, {} };
            if (at == "end")
                return { Kind::End, {} };
            throw std::runtime_error("anchor is {after = id}, {before = id} or {at = 'front' | 'end'}");
        }

        Condition conditionFrom(const sol::table& table, std::uint8_t index)
        {
            Condition condition;
            condition.mIndex = index;
            const std::string type = required(table, "type", "condition");
            const auto function = std::find_if(sConditionTypes.begin(), sConditionTypes.end(),
                [&](const auto& entry) { return entry.first == type; });
            if (function == sConditionTypes.end())
                throw std::runtime_error("condition.type " + type + " is not one this build creates");
            condition.mFunction = function->second;
            if (condition.mFunction != Condition::Function_Choice)
                condition.mVariable = required(table, "variable", "condition");
            const std::string op = required(table, "operator", "condition");
            const auto comparison = std::find_if(
                sOperators.begin(), sOperators.end(), [&](const auto& entry) { return entry.first == op; });
            if (comparison == sOperators.end())
                throw std::runtime_error("condition.operator is ==, !=, >, >=, < or <=");
            condition.mComparison = comparison->second;
            const auto value = table.get<sol::optional<double>>("value");
            if (!value)
                throw std::runtime_error("condition.value is a number");
            if (std::trunc(*value) == *value)
                condition.mValue = static_cast<std::int32_t>(*value);
            else
                condition.mValue = static_cast<float>(*value);
            return condition;
        }

        ESM::DialInfo infoFrom(const sol::table& draft, ESM::Dialogue::Type type)
        {
            for (const auto& [key, _] : draft)
            {
                const std::string name = key.is<std::string>() ? key.as<std::string>() : std::string("?");
                if (std::find(sInfoFields.begin(), sInfoFields.end(), name) == sInfoFields.end())
                    throw std::runtime_error("info has no field " + name);
            }
            ESM::DialInfo info;
            info.blank();
            info.mId = ESM::RefId::stringRefId(required(draft, "id", "info"));
            info.mResponse = draft.get<sol::optional<std::string>>("text").value_or("");
            info.mResultScript = draft.get<sol::optional<std::string>>("resultScript").value_or("");
            info.mActor = refId(draft, "actor");
            info.mRace = refId(draft, "race");
            info.mClass = refId(draft, "class");
            info.mFaction = refId(draft, "faction");
            info.mFactionLess = info.mFaction == "FFFF";
            info.mPcFaction = refId(draft, "pcFaction");
            info.mCell = refId(draft, "cell");
            info.mData.mType = type;
            if (type == ESM::Dialogue::Journal)
                info.mData.mJournalIndex = draft.get<sol::optional<int>>("journalIndex").value_or(0);
            else
                info.mData.mDisposition = draft.get<sol::optional<int>>("disposition").value_or(0);
            info.mData.mRank = static_cast<signed char>(draft.get<sol::optional<int>>("rank").value_or(-1));
            info.mData.mPCrank = static_cast<signed char>(draft.get<sol::optional<int>>("pcRank").value_or(-1));
            const std::string gender = draft.get<sol::optional<std::string>>("gender").value_or("");
            if (gender == "male")
                info.mData.mGender = ESM::DialInfo::Male;
            else if (gender == "female")
                info.mData.mGender = ESM::DialInfo::Female;
            else if (!gender.empty())
                throw std::runtime_error("info.gender is male or female");
            const std::string status = draft.get<sol::optional<std::string>>("questStatus").value_or("none");
            if (status == "none")
                info.mQuestStatus = ESM::DialInfo::QS_None;
            else if (status == "name")
                info.mQuestStatus = ESM::DialInfo::QS_Name;
            else if (status == "finished")
                info.mQuestStatus = ESM::DialInfo::QS_Finished;
            else if (status == "restart")
                info.mQuestStatus = ESM::DialInfo::QS_Restart;
            else
                throw std::runtime_error("info.questStatus is none, name, finished or restart");
            if (const auto conditions = draft.get<sol::optional<sol::table>>("conditions"))
            {
                const std::size_t count = conditions->size();
                if (count > 6)
                    throw std::runtime_error("an info carries at most six conditions");
                for (std::size_t i = 1; i <= count; ++i)
                    info.mSelects.push_back(
                        conditionFrom(conditions->get<sol::table>(i), static_cast<std::uint8_t>(i - 1)));
            }
            return info;
        }
    }

    sol::table initDialogueRecordsBindings(sol::state_view lua, MWWorld::Store<ESM::Dialogue>& store,
        std::function<void(std::function<void()>)> queueAction)
    {
        sol::table api(lua, sol::create);
        auto pending = std::make_shared<Pending>();

        api["createDialogue"] = [&store, queueAction, pending](const sol::table& draft) -> std::string {
            pending->follow(store.getEpoch());
            const ESM::Dialogue::Type type = dialogueType(required(draft, "type", "draft"));
            const std::string idText = required(draft, "id", "draft");
            const ESM::RefId id = ESM::RefId::stringRefId(idText);
            std::string name = required(draft, "name", "draft");
            // Ids compare without case in the engine; the text the caller sent must be the lower-case name.
            if (type == ESM::Dialogue::Topic && idText != Misc::StringUtils::lowerCase(name))
                throw std::runtime_error("a created topic's id is its lower-case name, never prefixed");
            if (const std::string rule = MWDialogue::DialogueOverlay::checkDialogue(id, name, type); !rule.empty())
                throw std::runtime_error(rule);
            if (const auto queued = pending->mDialogues.find(id); queued != pending->mDialogues.end())
            {
                if (queued->second != type)
                    throw std::runtime_error("dialogue " + id.serializeText() + " exists with another type");
            }
            else if (const ESM::Dialogue* existing = store.search(id))
            {
                if (existing->mType != type)
                    throw std::runtime_error("dialogue " + id.serializeText() + " exists with another type");
                if (type != ESM::Dialogue::Topic && !store.getOverlay().takesInfos(id))
                    throw std::runtime_error("a content file holds dialogue " + id.serializeText());
            }
            pending->mDialogues.emplace(id, type);
            queueAction([&store, pending, id, name = std::move(name), type, epoch = store.getEpoch()] {
                if (store.getEpoch() != epoch)
                    return;
                pending->mDialogues.erase(id);
                try
                {
                    store.createDialogue(id, name, type);
                }
                catch (const std::exception& error)
                {
                    Log(Debug::Error) << "world.records.createDialogue: " << error.what();
                }
            });
            return id.serializeText();
        };

        api["insertInfo"] = [&store, queueAction, pending](std::string_view dialogueId, const sol::table& draft,
                                const sol::table& anchorTable) -> std::string {
            pending->follow(store.getEpoch());
            const ESM::RefId dialogue = ESM::RefId::stringRefId(dialogueId);
            const ESM::Dialogue* record = store.search(dialogue);
            const auto queued = pending->mDialogues.find(dialogue);
            if (record == nullptr && queued == pending->mDialogues.end())
                throw std::runtime_error("no dialogue " + dialogue.serializeText());
            const ESM::Dialogue::Type type = queued != pending->mDialogues.end() ? queued->second : record->mType;
            ESM::DialInfo info = infoFrom(draft, type);
            const MWDialogue::InfoAnchor anchor = anchorFrom(anchorTable);
            if (!MWDialogue::DialogueOverlay::isOwned(info.mId) || info.mId.startsWith("dmo:j:"))
                throw std::runtime_error(
                    "a created info's id begins dmo: and not dmo:j:, the custom journal entries' prefix");
            if (queued == pending->mDialogues.end() && !store.getOverlay().takesInfos(dialogue))
                throw std::runtime_error("infos are inserted into a dialogue DMO created");
            std::unordered_set<ESM::RefId>& queuedInfos = pending->mInfos[dialogue];
            const auto holds = [&](const ESM::RefId& candidate) {
                return queuedInfos.contains(candidate)
                    || (record != nullptr
                        && std::any_of(record->mInfo.begin(), record->mInfo.end(),
                            [&](const ESM::DialInfo& existing) { return existing.mId == candidate; }));
            };
            if (holds(info.mId))
                throw std::runtime_error("the dialogue already holds info " + info.mId.serializeText());
            if ((anchor.mKind == MWDialogue::InfoAnchor::Kind::After
                    || anchor.mKind == MWDialogue::InfoAnchor::Kind::Before)
                && !holds(anchor.mInfo))
                throw std::runtime_error("the anchor info " + anchor.mInfo.serializeText() + " is not in the dialogue");
            queuedInfos.insert(info.mId);
            std::string result = info.mId.serializeText();
            queueAction([&store, pending, dialogue, info = std::move(info), anchor,
                            epoch = store.getEpoch()]() mutable {
                if (store.getEpoch() != epoch)
                    return;
                pending->mInfos[dialogue].erase(info.mId);
                try
                {
                    store.insertInfo(dialogue, std::move(info), anchor);
                }
                catch (const std::exception& error)
                {
                    Log(Debug::Error) << "world.records.insertInfo: " << error.what();
                }
            });
            return result;
        };

        api["listDeltas"] = [&store](sol::this_state state) {
            sol::table result(state, sol::create);
            std::size_t index = 0;
            for (const MWDialogue::DialogueDelta& delta : store.getOverlay().deltas())
            {
                sol::table entry(state, sol::create);
                entry["kind"] = delta.mInfo.empty() ? "dialogue" : "info";
                entry["dialogue"] = delta.mDialogue.serializeText();
                if (!delta.mInfo.empty())
                    entry["info"] = delta.mInfo.serializeText();
                entry["op"] = "created";
                if (!delta.mConflict.empty())
                    entry["conflict"] = delta.mConflict;
                result[++index] = entry;
            }
            return result;
        };

        return LuaUtil::makeReadOnly(api);
    }
}
