# Known issues

Structured ledger of the open problems. The short user-facing list lives in
[README.md](../README.md) → *Known limitations / open issues*; this file keeps the evidence,
hypotheses and next steps so work can resume without rediscovering them.

Use `tools/diagnose.ps1` to produce the evidence mentioned below.

---

## ES-02 - Superseded live searches can leave stale results - OPEN, FIX ATTEMPTED, NOT VERIFIED

**Repro.** Type a short query such as `p` and let live search start. Before it
finishes, change the text to `pdf` without pressing Enter. The request file and
Everything can contain `pdf` while Explorer continues enumerating the old
`p` result set.

**Cause.** Explorer can enumerate through a separate `CFolder` instance from
the one that receives view callbacks. Generation and IPC cancellation were
owned by each instance, so updating the new instance did not invalidate the old
enumerator or cancel its in-flight Everything page. `Refresh()` may also return
`S_FALSE`; the mod now sends the update notification for any result other than
`S_OK`.

**Implementation.** NSE folders for the same Explorer tab share a request
generation, so an enumerator created from an older folder instance stops when
the request sequence changes. In-flight page IPC jobs carry the tab identity
and sequence, allowing a newer request to cancel the superseded Everything
query. The mod also retries a coalesced refresh until the results view is
active, and uses an update notification when `Refresh()` does not return `S_OK`.

**Status.** A fix has been attempted (shared generation, IPC cancellation,
coalesced refresh and update notification), but the result has never been
verified in Explorer. Treat this issue as open, not resolved.

**Still open.** The `p` → `pdf` case needs manual verification in Explorer,
including confirming that old rows stop loading and the new rows appear without
pressing Enter.

---

## ES-03 - Legacy "Organize" bar reappears - PARKED (accepted)

**Symptom.** After a search, normal folders in the same window can show the
legacy "Organize" command bar again.

**Evidence/Hypothesis.** Related to `IExplorerPaneVisibility`; looks like an
Explorer behaviour that outlives our visibility override. Not reproduced
deterministically.

**Next step.** None planned; treated as an unresolved Explorer quirk.

---

## ES-04 - Window/tab title stays `Everything Search` - PARKED

**Symptom.** Title remains `Everything Search` instead of
`<query> - Search Results in ...`.

**Hypothesis.** Explorer does not call `GetDisplayNameOf` on the NSE root, so
there is no hook to supply a dynamic title.

---

## ES-05 - Desktop `Everything Search` icon reappears - PARKED

**Symptom.** An `Everything Search` icon on the desktop can reappear
intermittently.

**Hypothesis.** Shell/icon-cache artefact, not a real namespace folder or a
registration problem. No functional impact.

---

## ES-06 - First options-menu selection sometimes looks ineffective - UNDER OBSERVATION

**Symptom.** The user reported that selecting a *Type* filter did not seem to
change results on first use, then worked on a later search. Not confirmed.

**Evidence.** The log shows the correct modifier written to the per-tab request
file every time (`filter='doc:'`, `filter='image:'`, `filter='date-modified:today size:10kb..100kb'`, ...). The filter reaches the NSE.

**Hypothesis.** In-place refresh keeps showing the old list until
re-enumeration completes (same mechanism as ES-02), rather than a wrong filter.

**Next step.** Capture one reproducible case with `tools/diagnose.ps1` before
changing code.

---

## ES-07 - Inclusion toggles (system / ZIP / content) - OPEN, FIX ATTEMPTED, NOT VERIFIED

**Symptom.** The three checkmarked options in *Search options* do not all behave
as intended. With a query such as `.zip`, results were empty when a previous
`filter=doc:` was still active (AND narrowing), and terms added as bare OR
(`zip:` / `attributes:s`) returned every ZIP or system file regardless of the
query.

**Root cause (system files, found).** The query builder added `!attributes:s` to
every search by default. Everything does not index attributes (`index_attributes`
defaults to 0), so `attributes:` falls back to a per-file lookup: on a ~550k
result index the query took 11-22 s, exceeded the NSE's 5 s reply timeout, and
the view showed **0 results** for every plain search. Reproduced with
`tests/ipc_probe.cpp` (same query, reply arrives after ~11-22 s) and
`tests/run-mingw.ps1 -Ipc` / `nse_probe` (query `p`, total 0).

**Fix.** `BuildSearchStringInternal()` no longer sends `!attributes:s`; system
files are dropped in `ParseReply()` using the attributes Everything already
returns with each item (`includeSystemFiles`, driven by the `attributes:s`
include token). The ZIP toggle still uses `!zip:` (an indexed filter macro, fast).
The per-token OR alternatives were removed: when a toggle is on, simply omitting
its exclusion already widens the search, and the old OR clause returned every
archive/system file regardless of the query.

**Expected behavior.** Each enabled toggle admits matches for the typed query
from its category; disabling it removes that category's matches. System and
ZIP results still have to match the query. Content matches are an alternative
to filename matches, while the selected scope and type/date/size filters apply
to both alternatives.

**Current implementation.** ZIP matches are admitted by omitting `!zip:`;
system matches are retained or discarded locally using the attributes returned
by Everything. Content matches use a grouped `content:<query>` alternative,
with the same scope, filters, and ZIP exclusion as filename matches. The menu
refreshes its checked state from the active tab when opened. Superseded IPC
queries are cancelled when the NSE folder's request changes.

**Status.** A fix has been attempted (local system-file filtering, ZIP
exclusion only when disabled, grouped content alternative, menu state resync),
but no Explorer verification has confirmed it. Treat this issue as open, not
resolved.

**Still open.** No manual Explorer interaction has verified every toggle
combination after these changes. The ZIP default (`!zip:`) is a product choice,
not an Everything default.

**Next step.** Verify filename, content, ZIP and system matches with one
representative query and with type/date/size filters active, then update this
entry if any combination differs from the expected behavior.

---

## ES-08 - Search box loses focus on live search - OPEN, FIX ATTEMPTED, NOT FIXED

**Symptom.** Type a query without pressing Enter; when live search switches the
view to the results folder, the search box loses focus.

**Evidence.** `%TEMP%\everything-search-explorer.log` shows the same sequence on
every occurrence: `BrowseObject hr=0x00000000`, `DIRECT focus gained` after
~14 ms, then `DIRECT focus lost` after ~445 ms, with no recovery until the user
interacts again.

**Attempted fix.** `focusRestoreTimer` was changed from a one-shot 180 ms timer
to a repeating 100 ms timer (12 attempts) that checks the real
`box.FocusState()` instead of the cached `textHasFocus` flag. That build was
installed and the log still showed the focus loss, so the fix does not work.
The retry code is still present in the mod.

**Status.** OPEN, confirmed not fixed. The cause is not established: the
foreground-window guard and `TextChanged` stopping `focusRestoreTimer` were
considered but not proven. An event-driven restore on `LostFocus` is the next
candidate, to be implemented and verified only when requested.

---

## Resolved (kept for context)

- Origin folder loss on repeated search (ES-01, PR #1): per-tab origin tracking
  (`SaveTabOrigin` / `LoadTabOrigin`), `lastFolder` sync, and in-place
  `IShellView::Refresh()` on searches submitted from the results view.
- Static window and tab title (ES-04, PR #1): dynamic CLSID display name
  registry update (`UpdateNseDisplayName`) using `@shell32.dll,-34132` and
  dynamic title formatting in `DisplayNameLocked()`.
- Native Shell MUI localization: replaced hardcoded Italian/English UI text
  with `@shell32.dll`, `@propsys.dll` and `@ExplorerFrame.dll` string resources
  for the search placeholder, result columns, command bar detection, and the
  entire search options flyout.
- Cross-window / cross-tab result bleed: request files are keyed by the active
  `ShellTabWindowClass` HWND (`request-<HWND>.txt`); a global `request.txt` is
  kept as fallback.
- Per-tab search text restore and caret-at-end on tab switch.
- Keyboard hook installed before the XAML diagnostics observer, so `Ctrl+F` and
  `F3` work on tabs after the first.
- IPC page fetch moved outside the folder mutex (`LoadMore()` / `m_fetchMutex`)
  and serialized globally in `bridge.cpp` (`g_queryMutex`).
- Thread shutdown no longer closes events still used by unjoined threads.
