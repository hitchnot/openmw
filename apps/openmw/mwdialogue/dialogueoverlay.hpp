#ifndef GAME_MWDIALOGUE_DIALOGUEOVERLAY_H
#define GAME_MWDIALOGUE_DIALOGUEOVERLAY_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/esm3/loaddial.hpp>

namespace ESM
{
    class ESMReader;
    class ESMWriter;
}

namespace MWDialogue
{
    /// Where an inserted info goes in its dialogue's list.
    struct InfoAnchor
    {
        enum class Kind
        {
            After,
            Before,
            Front,
            End,
        };

        Kind mKind = Kind::End;
        ESM::RefId mInfo;

        /// "after:<id>", "before:<id>", "front" or "end", as the DMIF record stores it.
        std::string toString() const;
        static std::optional<InfoAnchor> parse(std::string_view value);
    };

    struct DialogueDelta
    {
        ESM::RefId mDialogue;
        /// Empty for a dialogue's own delta.
        ESM::RefId mInfo;
        std::string mConflict;
    };

    /// DMO's created dialogues and infos over the content files' dialogue store: what a load or a new game
    /// takes away again, a graveyard for records Lua may still point at, and the DMDL and DMIF save records.
    class DialogueOverlay
    {
    public:
        using Dialogues = std::unordered_map<ESM::RefId, ESM::Dialogue>;

        static constexpr std::uint32_t sFormatVersion = 1;
        static constexpr std::uint8_t sOpCreated = 1;

        /// Ownership is the id's prefix: an info DMO created begins "dmo:".
        static bool isOwned(const ESM::RefId& info);

        /// The rule a dialogue DMO would create breaks, or an empty string.
        static std::string checkDialogue(const ESM::RefId& id, std::string_view name, ESM::Dialogue::Type type);

        /// The rule an info inserted into this dialogue breaks, or an empty string.
        std::string checkInfo(const ESM::Dialogue& dialogue, const ESM::RefId& info, const InfoAnchor& anchor) const;

        /// Creates the dialogue, or adopts a content-file topic of the same id as a conflict.
        ESM::Dialogue& createDialogue(
            Dialogues& dialogues, const ESM::RefId& id, std::string_view name, ESM::Dialogue::Type type);
        const ESM::DialInfo& insertInfo(
            Dialogues& dialogues, const ESM::RefId& dialogue, ESM::DialInfo info, const InfoAnchor& anchor);

        /// Leaves every dialogue as the content files made it; what DMO created moves to the graveyard.
        void restore(Dialogues& dialogues);

        /// A dialogue DMO created or adopted, which takes inserted infos.
        bool takesInfos(const ESM::RefId& dialogue) const;
        std::vector<DialogueDelta> deltas() const;

        std::size_t countSavedGameRecords() const;
        void write(const Dialogues& dialogues, ESM::ESMWriter& writer) const;
        void readRecord(Dialogues& dialogues, ESM::ESMReader& reader, std::uint32_t type);

    private:
        struct Inserted
        {
            ESM::RefId mDialogue;
            ESM::RefId mInfo;
            InfoAnchor mAnchor;
        };

        std::vector<ESM::RefId> mCreated;
        std::map<ESM::RefId, std::string> mConflicts;
        std::vector<Inserted> mInserted;
        std::list<ESM::DialInfo> mGraveyard;
        std::vector<Dialogues::node_type> mDeadDialogues;
    };

    /// FillJournal's walk, over each dialogue's live info list: the order kept from loading is stale once DMO
    /// inserts infos.
    template <class Range, class JournalEntry, class TopicInfo, class Topic>
    void walkFillJournal(const Range& dialogues, JournalEntry&& journalEntry, TopicInfo&& topicInfo, Topic&& topic)
    {
        for (const ESM::Dialogue& dialogue : dialogues)
        {
            if (dialogue.mType == ESM::Dialogue::Type::Journal)
            {
                for (const ESM::DialInfo& info : dialogue.mInfo)
                    if (info.mQuestStatus != ESM::DialInfo::QS_Name)
                        journalEntry(dialogue, info);
            }
            else if (dialogue.mType == ESM::Dialogue::Type::Topic)
            {
                for (const ESM::DialInfo& info : dialogue.mInfo)
                    topicInfo(dialogue, info);
                topic(dialogue);
            }
        }
    }
}

#endif
