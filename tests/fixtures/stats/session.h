#ifndef BRIAREUS_STATS_FIXTURE_H
#define BRIAREUS_STATS_FIXTURE_H
// Swift #133 StatsLogicTests session fixture, with the server's epoch-ms and nested-role contract.
#define SESSION_STATS_FIXTURE "{\"host\":{\"cpus\":4,\"memoryBytes\":8000,\"freeMemoryBytes\":2000,\"loadAverage\":[1.5,1,0.5],\"intervalMs\":5000}," \
    "\"stats\":{\"sessionId\":\"s1\",\"at\":1000,\"cpuPercent\":200,\"memoryBytes\":4000,\"processes\":5," \
    "\"roles\":{\"browser\":{\"cpuPercent\":null,\"memoryBytes\":1000,\"processes\":2},\"agent\":{\"cpuPercent\":150,\"memoryBytes\":3000,\"processes\":3}}," \
    "\"disk\":{\"bytes\":2048,\"measuredAt\":900}}," \
    "\"history\":[{\"at\":1000,\"cpuPercent\":null,\"memoryBytes\":10,\"processes\":1},{\"at\":2000,\"cpuPercent\":50,\"memoryBytes\":20,\"processes\":1},{\"at\":0}]}"
#endif
