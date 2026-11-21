#pragma once

#include <string>
#include <vector>

#include "../shared/protocol.h"

namespace bridge {

// One round trip to Everything. Same batch size used by Query and by the
// lazy enumerator; raising it makes Everything slower per result.
constexpr DWORD kBatchSize = 2048;
constexpr DWORD kMaxBatches = 192;
constexpr DWORD kSafetyCeiling = kBatchSize * kMaxBatches;  // 393.216

// Makes sure the Everything notification window and our IPC worker exist.
bool EnsureReady();

// Builds the Everything search string for a request (scope -> path:"...").
std::wstring BuildSearchString(const esx::QueryRequest& req);

// Fetches one page. `offset` is the raw index in Everything's result list,
// `total` reports how many matches Everything found in total.
bool QueryPage(const std::wstring& search, DWORD offset, DWORD flags, DWORD wanted,
               std::vector<esx::QueryResultItem>& out, DWORD& rawCount,
               DWORD& totalCount, std::string& error);

// Runs one query against Everything, out of band.
//
// Everything is asked over its documented IPC (WM_COPYDATA to the Everything
// notification window) from a dedicated worker thread, so the shell thread
// never pumps foreign messages and never waits on a blocking DLL: if
// Everything is busy or missing, the caller gets an error after a bounded
// timeout instead of a frozen Explorer.
//
// Never throws; on failure returns false and fills `error`.
bool Query(const esx::QueryRequest& req, std::vector<esx::QueryResultItem>& out,
           std::string& error);

}  // namespace bridge
