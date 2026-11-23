# Known issues

Structured ledger of the open problems. The short user-facing list lives in
[README.md](../README.md) → *Known limitations / open issues*; this file keeps the evidence,
hypotheses and next steps so work can resume without rediscovering them.

Use `tools/diagnose.ps1` to produce the evidence mentioned below.

---

## ES-01 - `Current folder` scope lost on a second search - OPEN, high

**Symptom.** After searching from a real folder, a second search submitted from
inside the results view loses the origin folder. Selecting *Current folder*
then produces `parent:""` and returns nothing.

**Repro.**

1. Open a real folder and search (e.g. `C:\`, query `Onedrive`): request has
   `flags=7`, `folder='C:\'`.
2. Press Enter again while the results view is active: request is rewritten with
   `flags=2`, `folder=''`.
3. Choose *Search options → Current folder*: `flags=3`, `folder=''` → broken.

**Evidence** (from an actual log):

```
20:31:30 writing search request frame=... flags=7 folder='C:\' query='Onedrive'
20:32:12 writing search request frame=... flags=2 folder='' query='Onedrive'
20:32:15 options applied ... flags=0x3
request-2295964.txt: flags=3, folder=''
```

**Hypothesis.** `IsResultsFolder(frame)` intermittently returns false for a
search submitted from the results view (caller thread / focus / timing). The
`KeyDown` handler then takes the "not in results" branch, calls
`CurrentFolderPath()`, which returns an empty string because the NSE folder is
a virtual namespace with no filesystem path, and fails to reuse `state->origin`.
The `originFallback` captured by the option-menu click handlers is a snapshot
taken when the flyout is built, so it can also be empty.

**Next step.** Never overwrite a non-empty folder with an empty path; read
`state->origin` at click time rather than capturing it at flyout construction;
if the folder is still empty, keep *Current folder* disabled and do not write a
`parent:""` modifier. Affects `mod/everything-search-explorer.wh.cpp`
(`ApplySearchOptions`, `KeyDown` handler, option flyout).

---

## ES-02 - Second search with many results renders late - PARKED

**Symptom.** After a second consecutive search with a large result set, the new
list becomes visible only after the whole re-enumeration finishes.

**Hypothesis.** In-place navigation to the same PIDL plus
`SHChangeNotify(SHCNE_UPDATEDIR)` and DefView's own caching; the NSE cannot change
item identities cheaply enough. A dynamic/token PIDL approach for the second
render was considered and parked.

**Next step.** Revisit only if it becomes a blocking complaint; measure against
`tests/run-mingw.ps1` first.

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

## ES-07 - Inclusion toggles (system / ZIP / content) unfinished - OPEN

**Symptom.** The three checkmarked options in *Search options* do not yet behave
as intended. With a query such as `.zip`, results were empty when a previous
`filter=doc:` was still active (AND narrowing), and terms added as bare OR
(`zip:` / `attributes:s`) returned every ZIP or system file regardless of the
query.

**State.** Work in progress. An `include=` field was added to the request
protocol and `BuildSearchStringInternal()` was changed several times (AND, OR,
`!` exclusions, `< >` grouping) without end-to-end verification. None of these
variants is confirmed correct.

**Intended behaviour.** Toggles widen the search: with every option active the
user expects files whose name matches the query, plus system files, plus ZIPs,
plus files whose content matches the query (content search only if Everything
1.5+ supports it). With all options off, only normal matches should appear.

**Next step.** Verify the exact query string sent to Everything for each
toggle combination with `tools/diagnose.ps1` and a real results view, then settle
on one query construction and remove the unused variants. Do not treat the
current `include=` logic as finished.

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
