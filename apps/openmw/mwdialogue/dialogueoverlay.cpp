#include "dialogueoverlay.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <components/debug/debuglog.hpp>
#include <components/esm/defs.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/esmwriter.hpp>

namespace MWDialogue
{
    namespace
    {
        constexpr std::string_view sOwnedPrefix = "dmo:";
        constexpr std::string_view sJournalPrefix = "dmo:q:";
        constexpr std::string_view sGreetingPrefix = "dmo:g:";

        ESM::Dialogue::InfoContainer::iterator findInfo(ESM::Dialogue& dialogue, const ESM::RefId& info)
        {
            return std::find_if(dialogue.mInfo.begin(), dialogue.mInfo.end(),
                [&](const ESM::DialInfo& candidate) { return candidate.mId == info; });
        }

        bool holdsInfo(const ESM::Dialogue& dialogue, const ESM::RefId& info)
        {
            return std::any_of(dialogue.mInfo.begin(), dialogue.mInfo.end(),
                [&](const ESM::DialInfo& candidate) { return candidate.mId == info; });
        }
    }

    std::string InfoAnchor::toString() const
    {
        switch (mKind)
        {
            case Kind::After:
                return "after:" + mInfo.serializeText();
            case Kind::Before:
                return "before:" + mInfo.serializeText();
            case Kind::Front:
                return "front";
            case Kind::End:
                break;
        }
        return "end";
    }

    std::optional<InfoAnchor> InfoAnchor::parse(std::string_view value)
    {
        if (value == "front")
            return InfoAnchor{ Kind::Front, {} };
        if (value == "end")
            return InfoAnchor{ Kind::End, {} };
        constexpr std::pair<std::string_view, Kind> prefixes[]{ { "after:", Kind::After }, { "before:", Kind::Before } };
        for (const auto& [prefix, kind] : prefixes)
        {
            if (value.starts_with(prefix) && value.size() > prefix.size())
                return InfoAnchor{ kind, ESM::RefId::stringRefId(value.substr(prefix.size())) };
        }
        return std::nullopt;
    }

    bool DialogueOverlay::isOwned(const ESM::RefId& info)
    {
        return info.startsWith(sOwnedPrefix);
    }

    std::string DialogueOverlay::checkDialogue(const ESM::RefId& id, std::string_view name, ESM::Dialogue::Type type)
    {
        switch (type)
        {
            case ESM::Dialogue::Topic:
                if (id.startsWith(sOwnedPrefix) || id != ESM::RefId::stringRefId(name))
                    return "a created topic's id is its lower-case name, never prefixed";
                return {};
            case ESM::Dialogue::Journal:
                if (!id.startsWith(sJournalPrefix))
                    return "a created journal's id begins dmo:q:";
                return {};
            case ESM::Dialogue::Greeting:
                if (!id.startsWith(sGreetingPrefix))
                    return "a created greeting's id begins dmo:g:";
                return {};
            default:
                return "a created dialogue is a topic, a journal or a greeting";
        }
    }

    std::string DialogueOverlay::checkInfo(
        const ESM::Dialogue& dialogue, const ESM::RefId& info, const InfoAnchor& anchor) const
    {
        if (!isOwned(info))
            return "a created info's id begins dmo:";
        if (!takesInfos(dialogue.mId))
            return "infos are inserted into a dialogue DMO created";
        if (holdsInfo(dialogue, info))
            return "the dialogue already holds info " + info.serializeText();
        if ((anchor.mKind == InfoAnchor::Kind::After || anchor.mKind == InfoAnchor::Kind::Before)
            && !holdsInfo(dialogue, anchor.mInfo))
            return "the anchor info " + anchor.mInfo.serializeText() + " is not in the dialogue";
        return {};
    }

    ESM::Dialogue& DialogueOverlay::createDialogue(
        Dialogues& dialogues, const ESM::RefId& id, std::string_view name, ESM::Dialogue::Type type)
    {
        if (const std::string rule = checkDialogue(id, name, type); !rule.empty())
            throw std::invalid_argument(rule);
        const auto found = dialogues.find(id);
        if (found != dialogues.end())
        {
            ESM::Dialogue& existing = found->second;
            if (existing.mType != type)
                throw std::invalid_argument("dialogue " + id.serializeText() + " exists with another type");
            if (!takesInfos(id))
            {
                if (type != ESM::Dialogue::Topic)
                    throw std::invalid_argument("a content file holds dialogue " + id.serializeText());
                // A content file's topic of the same name: DMO's infos append to it.
                mCreated.push_back(id);
                mConflicts.emplace(id, "content-file topic");
            }
            return existing;
        }
        ESM::Dialogue dialogue;
        dialogue.blank();
        dialogue.mId = id;
        dialogue.mStringId = std::string(name);
        dialogue.mType = type;
        mCreated.push_back(id);
        return dialogues.emplace(id, std::move(dialogue)).first->second;
    }

    const ESM::DialInfo& DialogueOverlay::insertInfo(
        Dialogues& dialogues, const ESM::RefId& dialogueId, ESM::DialInfo info, const InfoAnchor& anchor)
    {
        const auto found = dialogues.find(dialogueId);
        if (found == dialogues.end())
            throw std::invalid_argument("no dialogue " + dialogueId.serializeText());
        ESM::Dialogue& dialogue = found->second;
        if (const std::string rule = checkInfo(dialogue, info.mId, anchor); !rule.empty())
            throw std::invalid_argument(rule);
        auto position = dialogue.mInfo.end();
        switch (anchor.mKind)
        {
            case InfoAnchor::Kind::Front:
                position = dialogue.mInfo.begin();
                break;
            case InfoAnchor::Kind::After:
                position = std::next(findInfo(dialogue, anchor.mInfo));
                break;
            case InfoAnchor::Kind::Before:
                position = findInfo(dialogue, anchor.mInfo);
                break;
            case InfoAnchor::Kind::End:
                break;
        }
        info.mData.mType = dialogue.mType;
        info.mPrev = position == dialogue.mInfo.begin() ? ESM::RefId() : std::prev(position)->mId;
        info.mNext = position == dialogue.mInfo.end() ? ESM::RefId() : position->mId;
        const auto inserted = dialogue.mInfo.insert(position, std::move(info));
        mInserted.push_back(Inserted{ dialogueId, inserted->mId, anchor });
        return *inserted;
    }

    void DialogueOverlay::restore(Dialogues& dialogues)
    {
        for (const ESM::RefId& id : mCreated)
        {
            if (!mConflicts.contains(id))
            {
                // Lua may hold pointers into a created dialogue: its node is kept, not freed.
                if (auto node = dialogues.extract(id))
                    mDeadDialogues.push_back(std::move(node));
                continue;
            }
            const auto found = dialogues.find(id);
            if (found == dialogues.end())
                continue;
            ESM::Dialogue::InfoContainer& infos = found->second.mInfo;
            for (auto it = infos.begin(); it != infos.end();)
            {
                const auto next = std::next(it);
                if (isOwned(it->mId))
                    mGraveyard.splice(mGraveyard.end(), infos, it);
                it = next;
            }
        }
        mCreated.clear();
        mConflicts.clear();
        mInserted.clear();
    }

    bool DialogueOverlay::takesInfos(const ESM::RefId& dialogue) const
    {
        return std::find(mCreated.begin(), mCreated.end(), dialogue) != mCreated.end();
    }

    std::vector<DialogueDelta> DialogueOverlay::deltas() const
    {
        std::vector<DialogueDelta> result;
        result.reserve(mCreated.size() + mInserted.size());
        for (const ESM::RefId& id : mCreated)
        {
            const auto conflict = mConflicts.find(id);
            result.push_back(DialogueDelta{ id, {}, conflict == mConflicts.end() ? std::string() : conflict->second });
        }
        for (const Inserted& inserted : mInserted)
            result.push_back(DialogueDelta{ inserted.mDialogue, inserted.mInfo, {} });
        return result;
    }

    std::size_t DialogueOverlay::countSavedGameRecords() const
    {
        return mCreated.size() + mInserted.size();
    }

    void DialogueOverlay::write(const Dialogues& dialogues, ESM::ESMWriter& writer) const
    {
        for (const ESM::RefId& id : mCreated)
        {
            writer.startRecord(ESM::REC_DMDL);
            writer.writeHNT("DVER", sFormatVersion);
            writer.writeHNRefId("DIDS", id);
            dialogues.at(id).save(writer);
            writer.endRecord(ESM::REC_DMDL);
        }
        for (const Inserted& inserted : mInserted)
        {
            const ESM::Dialogue& dialogue = dialogues.at(inserted.mDialogue);
            const auto info = std::find_if(dialogue.mInfo.begin(), dialogue.mInfo.end(),
                [&](const ESM::DialInfo& candidate) { return candidate.mId == inserted.mInfo; });
            writer.startRecord(ESM::REC_DMIF);
            writer.writeHNT("DVER", sFormatVersion);
            writer.writeHNRefId("DTOP", inserted.mDialogue);
            writer.writeHNRefId("DIID", inserted.mInfo);
            writer.writeHNT("DOPS", sOpCreated);
            writer.writeHNString("DANC", inserted.mAnchor.toString());
            writer.writeHNT("DHSH", std::uint64_t{ 0 });
            info->save(writer);
            writer.endRecord(ESM::REC_DMIF);
        }
    }

    void DialogueOverlay::readRecord(Dialogues& dialogues, ESM::ESMReader& reader, std::uint32_t type)
    {
        std::uint32_t version = 0;
        reader.getHNT(version, "DVER");
        if (version != sFormatVersion)
        {
            Log(Debug::Warning) << "Warning: skipping a DMO dialogue record of format " << version;
            reader.skipRecord();
            return;
        }
        try
        {
            if (type == ESM::REC_DMDL)
            {
                reader.getHNRefId("DIDS");
                ESM::Dialogue dialogue;
                bool deleted = false;
                dialogue.load(reader, deleted);
                createDialogue(dialogues, dialogue.mId, dialogue.mStringId, dialogue.mType);
                return;
            }
            const ESM::RefId topic = reader.getHNRefId("DTOP");
            reader.getHNRefId("DIID");
            std::uint8_t op = 0;
            reader.getHNT(op, "DOPS");
            const std::optional<InfoAnchor> anchor = InfoAnchor::parse(reader.getHNString("DANC"));
            std::uint64_t hash = 0;
            reader.getHNT(hash, "DHSH");
            if (op != sOpCreated || !anchor)
            {
                Log(Debug::Warning) << "Warning: skipping a DMO info record with operation " << int(op);
                reader.skipRecord();
                return;
            }
            ESM::DialInfo info;
            bool deleted = false;
            info.load(reader, deleted);
            insertInfo(dialogues, topic, std::move(info), *anchor);
        }
        catch (const std::invalid_argument& error)
        {
            Log(Debug::Warning) << "Warning: skipping a DMO dialogue record: " << error.what();
        }
    }
}
