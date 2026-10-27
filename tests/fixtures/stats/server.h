#ifndef BRIAREUS_SERVER_STATS_FIXTURE_H
#define BRIAREUS_SERVER_STATS_FIXTURE_H
// Swift d86db7d StatsLogicTests.testServerStatsTilesDisksAndCharts;
// endpoint shape checked against nadinyamaui/briareus #160 and current sampler.
#define SERVER_STATS_FIXTURE \
    "{\"host\":{\"cpus\":4,\"loadAverage\":[3.8,2,1],\"memoryBytes\":1000,\"freeMemoryBytes\":50,\"intervalMs\":10000,\"cpuPercent\":80,\"uptimeSeconds\":90000}," \
    "\"process\":{\"cpuPercent\":40,\"memoryBytes\":300,\"heapUsedBytes\":10,\"heapTotalBytes\":20,\"uptimeSeconds\":3700,\"nodeVersion\":\"22.1.0\"}," \
    "\"disks\":[{\"label\":\"workspaces\",\"totalBytes\":100,\"freeBytes\":20,\"usedBytes\":80},{\"label\":\"files\",\"totalBytes\":100,\"freeBytes\":90}]," \
    "\"sessions\":{\"open\":2,\"cpuPercent\":120,\"memoryBytes\":500,\"processes\":7,\"diskBytes\":4096}," \
    "\"history\":[{\"at\":1000,\"cpuPercent\":40,\"memoryBytes\":300,\"hostCpuPercent\":null,\"freeMemoryBytes\":100,\"loadAverage\":1}," \
    "{\"at\":2000,\"cpuPercent\":80,\"memoryBytes\":310,\"hostCpuPercent\":60,\"freeMemoryBytes\":50,\"loadAverage\":2}]}"
#define OPEN_STATS_FIXTURE \
    "{\"host\":{\"cpus\":4,\"memoryBytes\":1000,\"intervalMs\":10000},\"sessions\":[" \
    "{\"sessionId\":\"a\",\"at\":1000,\"cpuPercent\":10,\"memoryBytes\":100,\"processes\":1}," \
    "{\"sessionId\":\"b\",\"at\":2000,\"cpuPercent\":null,\"memoryBytes\":900,\"processes\":1}," \
    "{\"sessionId\":\"c\",\"cpuPercent\":10,\"memoryBytes\":300,\"processes\":2,\"disk\":{\"bytes\":1,\"measuredAt\":1}},{\"cpuPercent\":99}]}"
#endif
