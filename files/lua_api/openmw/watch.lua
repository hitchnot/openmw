---
-- Native predicates evaluated once per frame while a game is running, including paused frames.
-- Watches are cleared on loading a game or starting a new game and are never saved.
-- Global scripts receive `WatchFired` and `WatchExpired` through their `eventHandlers`.
-- @context global
-- @module watch
-- @usage local watch = require('openmw.watch')

---
-- Register a watch and return its id. Registering an existing id replaces it and resets its edge state.
-- The specification is copied; subsequent changes to the supplied table have no effect.
-- Unknown predicate kinds, missing required fields, wrong types, and invalid combat states raise field-named errors.
-- A watch is first evaluated on the next update. An initially true predicate fires then.
-- A once watch is removed after firing; a repeating watch fires on each false-to-true transition.
-- Expiry takes precedence over firing on the same frame.
-- @function [parent=#watch] register
-- @param #WatchSpec spec
-- @return #string id

---
-- Remove a watch silently. An unknown id has no effect.
-- @function [parent=#watch] cancel
-- @param #string id

---
-- Return independent copies of the current watches, ordered by id.
-- @function [parent=#watch] list
-- @return #list<#WatchEntry>

---
-- @type WatchSpec
-- @field #string id Watch id, shared by all global scripts.
-- @field #Predicate predicate
-- @field #boolean once Optional; defaults to false.
-- @field #number expiryGameHours Optional absolute deadline in game hours, in the same time base as `core.getGameTime() / 3600`. An elapsed deadline expires on the next update.

---
-- @type WatchEntry
-- @field #string id
-- @field #Predicate predicate
-- @field #boolean once
-- @field #number expiresAt Optional absolute deadline in game hours; absent without expiry.

---
-- A table with exactly one of the fields below. Composite children are arrays of predicates.
-- Cells use `cell.id` / `RefId::serializeText`: lowercased interior ids or `Esm3ExteriorCell:<x>:<y>`.
-- An unavailable or deleted reference, or an object outside a cell, makes its predicate false.
-- A non-actor makes `combatState` and `actorInCell` false.
-- @type Predicate
-- @field #Near near
-- @field #Los los
-- @field #InCellBetween inCellBetween
-- @field #CombatState combatState
-- @field #ActorInCell actorInCell
-- @field #list<#Predicate> all All children must match. An empty array is true.
-- @field #list<#Predicate> any At least one child must match. An empty array is false.

---
-- Exactly one of `ref` and `cell` is required.
-- @type Near
-- @field openmw.core#GameObject ref The player must be within radius in three dimensions, including the boundary. Objects must share a worldspace (the same interior, or exteriors in the same worldspace).
-- @field #string cell The player must be inside this cell; radius is ignored for cell targets.
-- @field #number radius Required distance in world units.

---
-- Uses the engine's line-of-sight check. Objects without active physics actors cannot have LOS to each other.
-- @type Los
-- @field openmw.core#GameObject from
-- @field openmw.core#GameObject to

---
-- The player must be in the cell and the game hour within `[hourFrom, hourTo)`.
-- The interval wraps past midnight when hourFrom is greater than hourTo. Equal endpoints never match.
-- @type InCellBetween
-- @field #string cell
-- @field #number hourFrom Inclusive start hour.
-- @field #number hourTo Exclusive end hour.

---
-- The player is engaged when mechanics reports actors fighting them, otherwise calm; the player never flees.
-- @type CombatState
-- @field openmw.core#GameObject ref
-- @field #string state `engaged`, `fleeing`, or `calm`. Fleeing takes precedence over engaged; calm means neither.

---
-- @type ActorInCell
-- @field openmw.core#GameObject ref Actor whose cell is checked, independently of the player.
-- @field #string cell

---
-- Data for the `WatchFired` global event. Bindings are a flat array in predicate traversal order.
-- Each successful leaf contributes one binding. An `all` contributes all its children's bindings;
-- an `any` contributes every matching child's bindings. Failed branches contribute nothing.
-- Repeated matches are retained; an empty `all` fires with an empty bindings array.
-- @type WatchFired
-- @field #string id
-- @field #list<#Binding> bindings

---
-- A matched leaf's references and cell.
-- @type Binding
-- @field openmw.core#GameObject ref Present for reference-based near, combatState, and actorInCell.
-- @field openmw.core#GameObject from Present for los.
-- @field openmw.core#GameObject to Present for los.
-- @field #string cell Matched object's or player's cell id; absent for los.

---
-- Data for the `WatchExpired` global event. The watch is already removed when this event is delivered.
-- @type WatchExpired
-- @field #string id

return nil
