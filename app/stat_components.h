#ifndef BRIAREUS_STAT_COMPONENTS_H
#define BRIAREUS_STAT_COMPONENTS_H
#include "screens.h"
#include "stats.h"
// Shared Resources drawing, suitable for session and server figures. Inputs are borrowed.
void doc_stat_tiles(Doc *doc, int x, int w, const StatTile *tiles, size_t count);
void doc_stat_gauge(Doc *doc, int x, int w, StatNumber fraction);
// Takes ownership of series.points, including on the initial/empty-history path.
void doc_stats_chart(Doc *doc, int x, int w, StatsSeries series);
#endif
