// Synthetic API responses following core 8079fcd; provenance and limits are in docs/core-october-compatibility.md.
// These are client-boundary examples, not recorded provider/deployment observations.
static const char CLAUDE_EVENTS[] =
    "[{\"seq\":1,\"kind\":\"result\",\"costUsd\":0.04,\"durationMs\":9000,\"isError\":false},"
    "{\"seq\":2,\"kind\":\"result\",\"costUsd\":0.25,\"isError\":false},"
    "{\"seq\":3,\"kind\":\"result\",\"costUsd\":0.25,\"isError\":false},"
    "{\"seq\":4,\"kind\":\"btw_answer\",\"id\":\"fork\",\"text\":\"ok\",\"costUsd\":0.02},"
    "{\"seq\":5,\"kind\":\"result\",\"costUsd\":0,\"isError\":true},"
    "{\"seq\":6,\"kind\":\"result\",\"costUsd\":null,\"isError\":true},"
    "{\"seq\":7,\"kind\":\"future_event\",\"text\":\"CLI extension\",\"future\":{\"kept\":true}}]";

static const char CODEX_SESSIONS[] =
    "[{\"id\":\"parent\",\"status\":\"idle\",\"provider\":\"codex\",\"costUsd\":null,"
    "\"usage\":{\"sessions\":1,\"costUsd\":1.25,\"estimatedCostUsd\":0.25,\"estimatedTurns\":1,\"unpricedTurns\":2},"
    "\"absorbedCostUsd\":1,\"absorbedSessions\":1,\"absorbedEstimatedCostUsd\":0.25,\"absorbedEstimatedTurns\":1,\"absorbedUnpricedTurns\":2},"
    "{\"id\":\"unpriced\",\"status\":\"idle\",\"provider\":\"codex\",\"costUsd\":null,"
    "\"usage\":{\"sessions\":0,\"costUsd\":null,\"estimatedCostUsd\":null,\"unpricedTurns\":1}},"
    "{\"id\":\"old-server\",\"status\":\"idle\",\"provider\":\"claude\",\"costUsd\":0.04}]";

static const char CODEX_EVENT[] =
    "{\"seq\":8,\"kind\":\"result\",\"costUsd\":1.25,\"costEstimated\":true,\"isError\":false}";

static const char RUNTIMES[] =
    "{\"default\":{\"providerId\":2,\"model\":\"gpt-5.6-sol\",\"effort\":\"high\"},\"providers\":["
    "{\"id\":1,\"label\":\"Claude\",\"available\":true,\"defaultModel\":\"claude-opus-5\",\"models\":["
    "{\"id\":\"claude-opus-5\",\"label\":\"Opus\",\"efforts\":[\"high\"],\"defaultEffort\":\"high\"}]},"
    "{\"id\":2,\"label\":\"Codex\",\"available\":true,\"defaultModel\":\"gpt-5.6-sol\",\"models\":["
    "{\"id\":\"gpt-5.6-sol\",\"label\":\"Codex\",\"efforts\":[\"low\",\"high\"],\"defaultEffort\":\"high\"}]}]}";

static const char REVIEW[] =
    "{\"body\":\"### Not fixed\\n\\nRejected: [original comment](https://github.com/o/r/pull/7#discussion_r1) "
    "does not apply; verified the current call path.\\n\\nConfirmed optional: "
    "[existing note](https://github.com/o/r/pull/7#issuecomment-2) records the cost of changing it.\","
    "\"findings\":[{\"key\":\"optional\",\"title\":\"Confirmed optional\",\"assessment\":{\"verified\":true,\"evidence\":\"Confirmed current call path\",\"worthFixing\":false,\"reason\":\"Low value\"},\"parked\":\"value\",\"parkedWhy\":\"Low value\"}],"
    "\"session\":{\"id\":\"review\",\"status\":\"idle\",\"reviewLoop\":{\"on\":true,\"triage\":{"
    "\"prNumber\":7,\"findings\":[{\"key\":\"optional\",\"assessment\":{\"verified\":true,\"evidence\":\"Confirmed current call path\",\"worthFixing\":false,\"reason\":\"Low value\"},\"parked\":\"value\",\"parkedWhy\":\"Low value\"}]}}}}";
