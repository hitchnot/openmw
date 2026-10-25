local ui = require('openmw.ui')
local I = require('openmw.interfaces')

return {
    interfaceName = 'Dialogue',
    ---
    -- Access to the player's dialogue window. Mutations are queued; a true return
    -- means the request was accepted, not that it has already appeared on screen.
    -- Requests are discarded if the conversation ends or changes before execution.
    -- A dialogue covered by another mode (such as barter) is not open for this API.
    --
    -- Player event `DialogueTopicSelected` contains `actor`, `topic` (lower-case id),
    -- and `pseudo = false`. It follows the vanilla response and `DialogueResponse`.
    -- Player event `DialogueChoiceSelected` contains `actor`, `offerId`, `choiceId`,
    -- and `lua`. Lua selections clear all Lua offers before the event is emitted.
    -- Vanilla selections use an empty offerId and the decimal choice number as
    -- choiceId, with lua = false; their normal response runs before the event.
    -- @module Dialogue
    -- @context player
    -- @usage require('openmw.interfaces').Dialogue
    interface = {
        --- Interface version.
        -- @field [parent=#Dialogue] #number version
        version = 1,

        --- Append a response paragraph to the open window.
        -- @function [parent=#Dialogue] addResponse
        -- @param #string text Response text.
        -- @param #table opts (optional) speaker is the title (default empty);
        -- teachTopics runs the normal topic discovery and hyperlink pass (default false).
        -- @return #boolean False when no dialogue is open; otherwise the request is queued.
        addResponse = function(text, opts) return ui.dialogue.addResponse(text, opts) end,

        --- Add choices using the normal Morrowind choice links.
        -- Replaces any pending offer with the same id. Other offers and vanilla
        -- choices remain. An empty list clears that offer. A click clears all Lua
        -- offers and leaves vanilla choices pending, without re-running the last topic.
        -- Offers are transient and cleared when the conversation ends or a game loads.
        -- @function [parent=#Dialogue] addChoices
        -- @param #string offerId The offer's identifier.
        -- @param #table choices Array of tables with string id and text fields.
        -- @param #table opts (optional) exclusive defaults to true: topics and Goodbye
        -- are disabled while the offer is pending. False leaves them available unless
        -- another exclusive offer or a vanilla choice is pending.
        -- @return #boolean False when dialogue is closed or ending with Goodbye; otherwise queued.
        addChoices = function(offerId, choices, opts) return ui.dialogue.addChoices(offerId, choices, opts) end,

        --- Clear Lua-owned choices, preserving vanilla choices.
        -- @function [parent=#Dialogue] clearChoices
        -- @param #string offerId (optional) Clear this offer; nil clears all Lua offers.
        -- @return #boolean False when no dialogue is open; otherwise queued, even for an unknown offer.
        clearChoices = function(offerId) return ui.dialogue.clearChoices(offerId) end,

        --- Start a conversation with the actor and its normal greeting through I.UI.addMode.
        -- The mode change is asynchronous: the window opens on a later frame.
        -- isOpen() is false in the same handler when starting from a closed window.
        -- Wait for isOpen() or a mode event before adding responses or choices.
        -- Actors without a greeting may not open a dialogue window.
        -- @function [parent=#Dialogue] open
        -- @param openmw.core#GameObject actor Actor to talk to.
        open = function(actor) I.UI.addMode('Dialogue', {target = actor}) end,

        --- End the conversation, including when choices are pending.
        -- @function [parent=#Dialogue] close
        -- @return #boolean False when no dialogue is open; otherwise queued.
        close = function() return ui.dialogue.close() end,

        --- Whether a dialogue window is currently open with an actor.
        -- @function [parent=#Dialogue] isOpen
        -- @return #boolean
        isOpen = function() return ui.dialogue.isOpen() end,

        --- Return the actor in the open conversation.
        -- @function [parent=#Dialogue] actor
        -- @return openmw.core#GameObject The actor, or nil when no dialogue is open.
        actor = function() return ui.dialogue.actor() end,

        --- Select a listed topic through the same response path as a topic click.
        -- Topic ids are matched without case sensitivity; service labels are not topics.
        -- Availability is checked again when the queued request executes.
        -- @function [parent=#Dialogue] selectTopic
        -- @param #string topicId Topic id.
        -- @return #boolean False if closed, not listed, or blocked by choices or Goodbye; otherwise queued.
        selectTopic = function(topicId) return ui.dialogue.selectTopic(topicId) end,
    },
}
