---
-- Loopback TCP transport with newline-delimited messages.
-- Available only when `[Lua] dmo ipc enabled = true` in settings.cfg (default false).
-- Requiring this package in other contexts, or with the setting off, fails.
-- Nothing is saved: loading or starting a game closes all connections and drops callbacks.
-- @context global|player
-- @module ipc
-- @usage local ipc = require('openmw.ipc')

---
-- Connect to a literal loopback address. The connection starts asynchronously.
-- A connecting or open connection with the same name is returned unchanged, even
-- if the requested endpoint differs. A closed name creates a new connection;
-- the old handle stays closed. Reconnection is the script's responsibility.
-- @function [parent=#ipc] connect
-- @param #table opts Fields: `host` (`"127.0.0.1"` or `"::1"`), `port` (integer 1..65535), `name` (string).
-- @return #Connection

---
-- Return `"tcp"`. When disabled, the package cannot be required, so this function is unavailable.
-- @function [parent=#ipc] backend
-- @return #string

---
-- A named connection shared by global and player scripts.
-- @type Connection

---
-- Read-only connection state: `"connecting"`, `"open"`, or `"closed"`.
-- A failed connect or a disconnected peer leaves the connection closed.
-- @field [parent=#Connection] #string state

---
-- A stable read-only table, refreshed on access and during delivery.
-- `sent` counts complete lines written to the socket; `received` counts complete
-- lines read, including dropped lines; `dropped` counts inbound and poll-buffer
-- overflow; `reconnects` counts replacements of this name since the last load/clear.
-- Other counters start at zero for each replacement.
-- @field [parent=#Connection] #table stats

---
-- Queue a line and append LF on the wire. CR and LF in the string are rejected;
-- JSON syntax is the caller's responsibility. Connecting connections accept lines.
-- The outbound queue holds 1024 lines. Closing discards queued lines.
-- @function [parent=#Connection] send
-- @param self
-- @param #string line
-- @return #boolean False if the connection is closed or the outbound queue is full.

---
-- Remove up to max lines from the Lua poll buffer, oldest first.
-- Delivery runs in the Lua update, including paused dialogue, before onUpdate.
-- At most 256 lines per frame across all connections enter the poll buffers or
-- callbacks. Both each inbound queue and each poll buffer hold at most
-- `dmo ipc max queue` lines (default 4096); overflow discards the oldest line.
-- LF terminates received lines; an immediately preceding CR is removed.
-- Empty lines are delivered; an incomplete final line is discarded at disconnect.
-- @function [parent=#Connection] poll
-- @param self
-- @param #number max Nonnegative integer; zero returns an empty array.
-- @return #list<string>

---
-- Set one async callback receiving each line, or pass nil to resume polling.
-- Buffered poll lines reach a newly installed callback first, within the drain cap.
-- Callbacks run only in the Lua update and may close or reconnect safely.
-- A removed script's callback is dropped and delivery resumes in the poll buffer.
-- @function [parent=#Connection] onMessage
-- @param self
-- @param callback An `openmw.async` callback taking one string, or nil.

---
-- Close, discard queued and buffered lines, and drop the callback. Idempotent.
-- @function [parent=#Connection] close
-- @param self

return nil
