# SelfBot client API

The client controls its own SelfBot through the `IDLEBOT` addon channel. The
server owns AI state and command execution. The client publishes a snapshot only
after receiving the entire correlated response.

The server also supports an explicitly started FIFO action queue. Its commands,
execution states and response records are documented in
[selfbot-action-queue.md](selfbot-action-queue.md).

## Ownership

- `PlayerbotMgr` owns `SelfBotController` and `BotProtocol` for the logged-in
  character. Both survive Start/Stop; Stop deletes only the existing AI.
- `SelfBotController` executes Start/Stop and captures `BotStatusSnapshot` values.
  It also validates, loads, saves and applies typed `SelfBotSettings`. It holds no
  `Player*` or `PlayerbotAI*` between calls.
- `BotStatusSnapshot::Capture` reads the existing quest, trainer and vendor
  states. It does not advance the planner or send packets. `LastAction` is a
  diagnostic value from the AI, not a second planner.
- `BotWireProtocol` parses requests, tracks ordering and encodes snapshots without
  game, network or database dependencies. `BotProtocol` owns the per-player request
  tracker, cached reply and packet transmission.
- The client's native `BotClientService` installs a separate Lua service. It owns
  transport, timeouts, lifecycle and the snapshot cache. `BotStatusUI` reads copies
  through `IdleWoWBotClient` and sends Start/Stop through its methods.
  `BotSettingsUI` keeps an editable draft separate from the confirmed snapshot.

The server entry point accepts only addon whispers where sender and receiver are
the same player. `CMSG_MESSAGECHAT` is processed on the world thread, between map
updates. These controller calls and snapshot reads stay at that boundary. Code
originating on a map thread must not call these mutation methods directly.
Existing Start behavior loads saved strategies on the world thread, then applies
the character's settings. Settings reads and save completion run on that same
thread. Strategy resets apply cached settings without database access.

## Version 2 requests

Addon prefix: `IDLEBOT`. Channel: `WHISPER`. Recipient: the player's own name.
The WoW addon prefix separator is a tab; the payload contains no tabs.

```text
2|request|session|requestId|operation
```

- `session`: opaque decimal correlation token, 1 to 20 digits, refreshed by the
  client on entering the world or rebuilding the UI.
- `requestId`: canonical decimal integer from 1 to 2147483647, increasing within
  the client lifecycle; leading zeroes are rejected.
- `operation`: `status`, `start` or `stop`.
- Payload limit: 246 bytes, excluding the addon prefix and tab.

Settings use an extended, strictly typed request:

```text
2|request|session|requestId|settings_set|expectedSettingsRevision|sellQualities|vendorFreeSlots|autoTrain|trainingReserveCopper
```

`sellQualities` is a mask from 0 to 15: bits 0, 1, 2 and 3 select poor, common,
uncommon and rare quality. Zero disables automatic selling. `vendorFreeSlots`
ranges from 0 to 16, `autoTrain` is exactly `0` or `1`, and
`trainingReserveCopper` ranges from 0 to 2147483647. Numbers are canonical decimal
integers; the expected revision is a positive uint32. Unknown options, extra
fields and malformed values are rejected. The server accepts writes only against
the current settings revision.

A new session token must first send `status`. For the current token, a repeated
last request with the same ID, operation and arguments replays its cached reply without
executing the command again. An older ID or reuse with another operation is
rejected. A new token's status establishes a new lifecycle on the same
authenticated, ordered WoW connection. Tokens correlate requests; they are not
credentials or a replay ledger across all previous lifecycles.

New accepted operations are spaced at least 200 ms apart. A faster request gets a
`throttled` result without executing Start/Stop. The client spaces transmissions
by at least 250 ms, including controls immediately following a status response.

## Atomic response

Each response contains eight base addon payloads. All carry the same session token,
request ID and snapshot revision:

```text
2|begin|session|requestId|revision|result|status,start,stop
2|state|session|requestId|revision|enabled|activity|questId|objective|done|total|distance
2|title|session|requestId|revision|text
2|destination|session|requestId|revision|text
2|detail|session|requestId|revision|text
2|action|session|requestId|revision|text
2|note|session|requestId|revision|text
2|end|session|requestId|revision
```

`revision` is a positive uint32 capture counter owned by the controller. Zero is
skipped on wrap. The client uses exact response correlation, not global revision
ordering across logins or controller recreation.

The last begin field lists supported operations. A missing manager reports
`unavailable` with only the `status` capability. The client disables unavailable
controls. An empty text value is valid; all five text messages must still arrive.
Text is UTF-8, at most 120 bytes without cutting a code point. Pipes and control
characters are replaced with spaces. UI formatting escapes are therefore not
accepted from server text.

A server that provides settings advertises `settings,settings_set` in addition
to the existing capabilities, and sends a ninth payload before `end`:

```text
2|settings|session|requestId|revision|settingsRevision|sellQualities|vendorFreeSlots|autoTrain|trainingReserveCopper
```

This field is required for publication when `settings` is advertised. Old v2
clients ignore it, and new clients still accept servers without that capability.
If storage cannot be read, settings capabilities are withheld rather than
publishing guessed defaults. The settings revision is independent of the status
capture counter and persists across logins.

State fields:

| Field | Meaning |
| --- | --- |
| enabled | `0` or `1`: whether the player's own AI is enabled |
| activity | `manual`, `idle`, `travel`, `interact`, `combat`, `dead`, `recover`, `train`, `vendor` |
| questId | Current planner quest ID, or zero |
| objective | Raw zero-based planner objective slot; not the Lua quest leaderboard index |
| done / total | Counters paired with the server's `detail`; total zero means no numeric objective |
| distance | Direct destination distance in yards, or `-1` when unavailable; not ETA or path length |

Results: `ok`, `disabled`, `forbidden`, `other_master`, `unavailable`,
`invalid_request`, `throttled`, `conflict`, `save_failed`, `busy`. A valid rejected command also includes a full
snapshot. Start/Stop are idempotent; only the controller performs the mutation.
The configured account and server restrictions still apply.

## Client behavior

`IdleWoWBotClient` provides `GetStatus`, `IsFresh`, `IsInWorld`, `IsBusy`,
`GetPendingCommand`, `GetNotice`, `Supports`, `Subscribe`, `Start`, `Stop`,
`GetSettings`, `SaveSettings(draft, expectedRevision)` and `GetLastSettingsSave`.
`GetStatus` returns a copy, so presentation cannot mutate cached authority.

Only one request can be outstanding. Parts with the wrong sender, channel,
version, session, request ID or revision are ignored. The previous complete
snapshot remains until a matching begin, all six data fields and end are present.
An incomplete response does not refresh its timestamp.

Normal polling is every two seconds. Before service discovery or when the
service is unavailable, polling uses five seconds. Status requests time out after
three seconds; commands after six. Commands are never automatically retried.
Status older than eight seconds disables controls. Leaving the world clears
pending requests and cached status; hiding the panel does not stop the service.

The settings panel opens from the HUD's Settings button. Edits survive status
polls and closing the panel; world exit clears them. Save publishes no optimistic
settings value. Only the correlated completed response confirms success.
Conflicts preserve the draft and require Reload saved before another save;
timeouts preserve the draft and leave the save outcome uncertain until refreshed.

## Settings persistence

`SelfBotSettingsStore` uses the module-owned `playerbots_selfbot_settings` table,
keyed by character GUID and account ID. The dated module SQL update creates it at
server startup through the existing updater. No core PlayerSettings switch or
configuration change is required.

The initial selling/training enablement follows the configured noncombat
strategies. Selling initially selects only poor quality, the vendor threshold is
four free general-purpose bag slots, and the training reserve is zero. Quests
remain automatic and have no setting in this panel. Server feature restrictions
remain authoritative. Settings apply after saving, after Start and after strategy
resets, including when the bot was stopped while edited.

A vendor trip begins when general-purpose free slots are at or below the chosen
threshold and at least one eligible selected-quality item is in the bags. No
quality selected means no automatic vendor trip. Equipped items, active quest
requirements/source items, quest starters, quest-bound items, wrapped gifts,
refundable/tradeable items and nonempty bags are excluded. The candidate is checked
again immediately before selling through the normal sell handler. Specialized
bag space does not postpone a trip when general bags are full.

Class-trainer selection and every actual purchase use the same affordability
check: current money must cover both the spell cost and the saved reserve. The
calculation subtracts the reserve only after checking the balance, avoiding
unsigned underflow. The editor accepts gold with up to four decimals and converts
exactly to copper. The server stores copper. Disabling learning or changing its
budget resets the current trainer trip; vendor policy changes similarly reset
the vendor trip and its cached need calculation.

Each save uses a prepared transaction with a revision-checked update. The endpoint
waits for commit completion and verification before sending `ok`; failures return
`save_failed`, and stale revisions return `conflict` with the current snapshot.
While the write is pending, duplicate requests do not run again and other
requests receive `busy`. Completion resolves the character by GUID and validates
the original controller/protocol lifetime, so a late reply cannot reach another
character or a replacement session. A controller-owned completion is drained by
the world-only `OnSessionUpdate` hook, not the session's general query callback
processor, which can run on map threads. No raw game-object pointers are retained.
The controller rechecks storage at most once every five seconds on world-thread
client requests, retaining the last verified policy if a read fails. Unchanged
reads do not reset the AI. This bounded refresh also discovers a write that
completed after the previous session logged out, even when the new session first
loaded before that commit. Map-thread consumers use only the cached value.

## Compatibility and extension

The server retains v1 `STATUS` snapshots and the legacy chat `self` commands as
adapters to the same controller. The v2 panel uses only addon protocol requests.
The client does not fall back to chat commands on an older server.

Further settings should add typed controller operations and explicit protocol
capabilities. Keep persistence and validation on the server; do not expose the
internal `AiObjectContext` or send arbitrary strategy strings from the UI.

Offline tests in workspace `tests/bot-status-offline` and
`tests/bot-protocol-offline` cover the service, panel, wire codec and actual
C++-encoded replies consumed by the embedded Lua implementation.
