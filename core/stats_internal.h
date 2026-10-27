#ifndef BRIAREUS_STATS_INTERNAL_H
#define BRIAREUS_STATS_INTERNAL_H
#include "stats.h"
StatNumber stats_number(const Json *j, bool integer);
StatNumber stats_timestamp(const Json *j);
ProcessStats stats_figures(const Json *j);
StatsHost stats_host(const Json *j);
StatNumber stats_used(StatNumber total, StatNumber free_bytes);
#endif
