# SelfBot action queue (server API)

The character can prepare a route separately from automatic quest selection,
then explicitly start it. Each action has a stable ID, a type and type-specific
data. The initial action type is `quest` with a quest ID. A quest action means
accept the quest if necessary, perform its objectives, then turn it in through
the existing native quest handlers. Dispatching a packet or completing objectives
alone does not complete an action: the server must observe the rewarded quest.
Already rewarded quests are marked done when execution reaches them.

The queue is owned by `SelfBotController` in the logged-in character's
`PlayerbotMgr`. It survives AI resets and SelfBot Stop/Start. **This version is
session-local: logout or server restart discards the queue.** No database schema
change is required. The client's HUD exposes separate Queue and Quests windows.

## Execution

- Preparing or editing a draft does not interrupt automatic quest selection.
- Starting selects queue mode and activates SelfBot with the usual permission,
  settings and server configuration checks. Only the current action supplies
  quest candidates. Other accepted quests and nearby quest givers cannot change
  the route. Native quest-chain auto-accept behavior can still add quests to the
  journal; the executor only works on the current queued quest.
- After a rewarded quest, execution advances in the configured queue order. It never inserts
  prerequisites automatically. A client can enqueue a prerequisite and its later
  chain steps before those later steps become available.
- A blocked step stays current and exposes its reason. Existing bounded retries
  check it again; later steps never bypass it. Unsupported mechanisms, failed
  quests, unmet prerequisites, difficulty, journal capacity and missing safe paths
  can block execution. Existing solo difficulty, navigation, combat, recovery,
  vendor, trainer and disconnect protections remain in force.
- Explicit quests use the existing maximum travel budget regardless of their XP
  reward. Other automatic XP and quest batching preferences do not reorder them.
- Complete SelfBot quest routes use a private bounded navigation query with up to
  16384 search nodes and 1024 polygons. Ordinary movement keeps its existing shared
  query and limits. A long route must still reach its source, fit the existing
  2400-yard travel budget, and pass the terrain and creature-corridor safety checks
  before the executor follows its next local step. Navigation failures expose the
  failing check in the queue reason and log, rather than only a generic no-source message.
- Pause and completion retain queue mode, preventing automatic quest selection.
  Combat, recovery and enabled maintenance may continue. `queue_auto` explicitly
  restores automatic quest selection and preserves the route for later resumption.
- SelfBot `stop` pauses the queue. Ordinary SelfBot `start` leaves that pause in
  place; `queue_start` explicitly resumes it.
- Editing requires a non-running queue. A blocked queue must be paused first.
  There are at most 64 actions, including completed history. Duplicate quest IDs,
  zero IDs, unknown action types and quests outside the server catalog are rejected.
  Unsupported catalog quests can be queued and report a block when reached.
  Removing completed history adjusts the cursor without changing the current
  action. Removing a pending action does not abandon it in the actual quest log.
- Pending actions can be inserted or moved before another pending action by its
  stable action ID. Zero as the anchor means the end of the queue. Moves preserve
  all other relative ordering, action IDs and completed history. Completed actions
  cannot be moved or used as insertion anchors. Moving an action to its current
  place succeeds without advancing the queue revision. Editing never starts or
  resumes execution; after a paused edit, `queue_start` follows the new order.

## Requests

Use the authenticated, self-addressed `IDLEBOT` addon whisper endpoint documented
in [selfbot-client-protocol.md](selfbot-client-protocol.md). Perform the normal
`status` handshake first. Request IDs, throttling and exact duplicate reply replay
are shared with the existing API. The server advertises these capabilities:

```text
queue,queue_add,queue_remove,queue_move,queue_clear,queue_start,queue_pause,queue_auto
```

```text
2|request|session|requestId|queue
2|request|session|requestId|queue_add|expectedQueueRevision|quest|questId
2|request|session|requestId|queue_add|expectedQueueRevision|quest|questId|beforeActionId
2|request|session|requestId|queue_remove|expectedQueueRevision|actionId
2|request|session|requestId|queue_move|expectedQueueRevision|actionId|beforeActionId
2|request|session|requestId|queue_clear|expectedQueueRevision
2|request|session|requestId|queue_start|expectedQueueRevision
2|request|session|requestId|queue_pause|expectedQueueRevision
2|request|session|requestId|queue_auto|expectedQueueRevision
```

Numeric arguments are canonical positive uint32 decimal integers, except
`beforeActionId`, which also accepts `0`. Omitting `beforeActionId` in `queue_add`
is equivalent to `0` (append). `queue_move` requires this field. Use the stable
`actionId` returned by the server, not the quest ID or displayed position.
Both the moved action and a nonzero anchor must be pending. Every
mutation requires the current queue revision. Progress and state changes can
also advance it. A stale revision returns `conflict` with the current snapshot;
refresh before retrying. `busy` means editing was attempted while running or
blocked, or an existing settings save is in flight. `invalid_argument` describes
an invalid queue operation; `disabled`, `forbidden`, `other_master` and
`unavailable` retain their existing meanings. Invalid wire syntax is ignored.

Read-only queue queries work before starting the AI. A different request's
session token must still establish its lifecycle with `status`, not `queue`.
Retries with the same request ID replay the original snapshot; use a newer ID
to read subsequent progress.

## Atomic response

Queue requests and quest-list queries include queue records. Existing status/settings requests
retain their response records, allowing current clients to keep using them.
Queue records appear inside the existing `begin`/`end` envelope and carry the
same correlation and snapshot revision as the other records:

```text
2|queue|session|requestId|snapshotRevision|queueRevision|mode|state|doneCount|totalCount|currentActionId|currentQuestId
2|queue_note|session|requestId|snapshotRevision|reason
2|queue_item|session|requestId|snapshotRevision|position|actionId|quest|questId|itemState|title
```

`mode` is `auto` or `queue`; `state` is `draft`, `running`, `paused`, `blocked` or
`completed`. `doneCount` is the cursor (zero-based completed count). Positions
are one-based; each row's state is `done` or `pending`. Current IDs are zero when
there is no pending action. `title` is the quest name from the server catalog,
so the client can display a quest before accepting it. A client with localized
quest data may use the quest ID to prefer a local title. Every response contains one queue summary, one note
(possibly empty), and exactly `totalCount` item records. Text uses the existing
sanitization and 120-byte UTF-8 limit. Every addon payload stays within 246 bytes.
Publish a response only after the matching end and all declared rows arrive.

Every accepted queue mutation returns this full authoritative snapshot as well
as its result. Render the returned order after acknowledgement. For drag and
drop, send `queue_move` with the dragged action ID and the action ID of the row
it should precede, or `0` for the end. A `conflict` response includes the latest
order; preserve the user's draft, show that the queue changed and let the user
reapply it against the new revision. Do not blindly repeat an old edit.

For example, after the handshake, query `queue`, append quest A with its returned
queue revision, append quest B with the next returned revision, then send
`queue_start` with the latest revision. Poll `queue` for progress. The running
queue finishes A including its reward before beginning B.

To insert C before B, use `queue_add|revision|quest|CQuestId|BActionId`.
To move B before A, use `queue_move|revision|BActionId|AActionId`.
To move A to the end, use `queue_move|revision|AActionId|0`.
Each edit uses the revision from the previous acknowledged snapshot. If the
queue is running or blocked, pause it first and use the pause response's revision.

## Client quest browser

The HUD's `Quests` button opens the quest browser. Each row shows the server's
quest title, level, state, starting NPC/object, zone and continent, with an `Add to queue` button that appends that quest
through `queue_add` using the latest confirmed queue revision. Adding a quest
does not start the bot. Open `Queue` to review the route and start it explicitly.
Running and blocked routes must be paused before adding; full queues and quests
already present in the route disable the row button, including completed history.

The optional `quests` capability supports this read-only request:

```text
2|request|session|requestId|quests|offset
```

`offset` is a canonical uint32 integer divisible by 8, starting at zero. The
server evaluates its existing quest catalog for the character on the current
map, including its quest log, and sorts by quest level and ID. Unrewarded,
non-excluded quests ready for turn-in are included. Other rows must be supported
solo quests within the planner's level limit; new quests must also pass the
character's acceptance and journal requirements. This is an eligibility list;
the executor still checks safe paths and changing world conditions when running.
Future chain steps can still be entered by ID in the Queue window.

The response includes status, settings when available, the full queue, and one
page inside the same correlated atomic envelope:

```text
2|quests|session|requestId|snapshotRevision|offset|totalCount|pageCount
2|quest_item|session|requestId|snapshotRevision|index|questId|level|state|title
```

`index` is one-based within the page; `state` is `available`, `active` or
`complete`. Each page has up to eight rows, exactly `min(8, totalCount - offset)`.
An empty result uses zero offset and counts. When eligibility changes, an offset
beyond the result is clamped to the last page. Titles use the usual sanitized
120-byte UTF-8 limit, and every packet remains within 246 bytes.

Servers advertising `quest_browser` also accept the extended query:

```text
2|request|session|requestId|quests|offset|zoneId|search
```

`zoneId` is a canonical uint32; zero selects all zones. `search` is a literal
string of at most 64 bytes without control characters or pipe delimiters. It
may be empty. Matching ignores ASCII case and searches the title, quest ID,
starter name, zone and continent before slicing the result into eight-row pages.
The selected starter is the nearest catalog giver on the character's current
map when one exists. If its catalog AreaId is absent, the area is resolved from
the giver's map coordinates. Subareas are resolved to their parent zone. Names are English.
Extended queries include these additional records in the same atomic snapshot:

```text
2|quest_starter|session|requestId|snapshotRevision|index|starterName
2|quest_zone|session|requestId|snapshotRevision|index|zoneId|zoneName
2|quest_continent|session|requestId|snapshotRevision|index|continentName
2|quest_zones|session|requestId|snapshotRevision|zoneCount
2|quest_zone_option|session|requestId|snapshotRevision|index|zoneId|zoneName
```

Each quest requires all three detail records. Zone options cover the unfiltered
eligible result, so a search with no matches does not remove filter choices.
The client's search and zone selection are combined, reset scrolling to the
start, and remain attached to the request that produced the response. Reusing
a request ID with different filters is a conflict. Legacy queries keep their
original response format; older servers retain basic quest browsing.

The browser uses a resizable window, compact rows and smooth pixel scrolling
inside a clipped viewport. Transport pages are collected into a complete catalog;
mouse-wheel input only scrolls cached rows and never switches server pages.
There are no pagination controls. A search field at the top debounces typing for 300 ms, and a zone
dropdown provides an All zones option. Pending results from old filters never
replace the current view. Logout resets both filters.

The native service publishes the page and queue together only after all records
and the matching `end`. The browser refreshes on opening, quest-log/level/map
events, and every ten seconds while visible. Hidden browsers do not query quest
pages; normal queue polling continues independently. Timeouts and conflicts
never automatically resend an add. Logout clears the cached page and feedback.

## Verification

`tests/bot-protocol-offline/protocol-tests.cpp` covers queue lifecycle, FIFO,
blocked/pause/resume behavior, positioned insertion, moves in both directions,
completed-history protection, edit invariants, size/type limits, strict wire
parsing, mutation deduplication (including anchors), display titles, reordered
snapshot rows and full snapshot transport bounds.
`scripts/selfbot-quest-planner-tests.cpp` covers strict quest selection despite
automatic reward preferences, and preservation of safety and travel limits.
Both existing PowerShell runners compile standalone offline C++ tests; invoke
them when a build is authorized. No live server or database is required.

Workspace `tests/selfbot-navigation-offline/run-tests.ps1` reads the installed
mmap tiles and uses production Detour and slope costs to reproduce quest 332's
1024-node exhaustion and 148-polygon truncation. It verifies the extended search,
complete endpoint, travel budget, normal local-step query and restricted terrain
filter without connecting to the game. It requires the existing navigation build
libraries and the usual build authorization.
