#pragma once

#include <Arduino.h>

// Web search and page fetch through the DeepSeek Search / Fetch API (same key
// as the agent model).  Responses are written to storage and scanned there:
// a fetched page can be far larger than the heap.
namespace vat::web {

// Numbered results: title, site, date, summary, url.  `query` is 1-128
// characters and may use site:, -site:, "phrase", lang:, after:, before:.
String search(const String& query, int count);

// Page content as Markdown.  The whole page is offered to the context store
// (it becomes out#N with a card); the returned text is its beginning.
String fetch(const String& url);

}  // namespace vat::web
