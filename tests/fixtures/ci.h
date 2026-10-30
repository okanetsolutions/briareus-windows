#ifndef BRIAREUS_CI_FIXTURE_H
#define BRIAREUS_CI_FIXTURE_H
#define CI_STATS_FIXTURE "{\"totals\":{\"total\":12,\"pending\":2,\"succeeded\":8,\"failed\":2,\"successRate\":0.8,\"avgDurationMs\":12400}," \
    "\"daily\":[{\"day\":\"2026-10-30\",\"total\":12,\"succeeded\":8,\"failed\":2,\"successRate\":0.8,\"avgDurationMs\":12400}]," \
    "\"repositories\":[{\"repo\":\"example/synthetic\",\"total\":12,\"succeeded\":8,\"failed\":2,\"successRate\":0.8,\"avgDurationMs\":12400}]," \
    "\"checks\":[{\"name\":\"Build and regression tests\",\"app\":\"GitHub Actions\",\"total\":12,\"succeeded\":8,\"failed\":2,\"successRate\":0.8,\"avgDurationMs\":12400}]}"
#define CI_RUNS_FIXTURE "{\"runs\":[{\"id\":\"123\",\"name\":\"Build and regression tests\",\"repo\":\"example/synthetic\",\"branch\":\"feature/synthetic\",\"status\":\"completed\",\"conclusion\":\"failure\",\"durationMs\":12400,\"headSha\":\"abcdef123456\",\"runAt\":1793188800000,\"url\":\"https://github.com/example/synthetic/actions/runs/123\"}],\"nextOffset\":50,\"truncated\":false}"
#endif
