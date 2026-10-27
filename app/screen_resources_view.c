#include "screen_resources_internal.h"
#include "stat_components.h"

static void process_rows(ResourcesScreen *s, Doc *doc, int w) {
    bool any = false;
    for (int r = 0; r < STATS_ROLES; r++) {
        if (!s->reading.roles[r]) continue;
        if (!any) doc_section(doc, 0, w, "Processes");
        any = true;
        ProcessStats *p = &s->reading.role[r];
        char *cpu = stats_percent(p->cpu), *memory = stats_bytes(p->memory), *count = stats_processes(p->processes, true);
        char *line = xstrfmt("%s · %s · CPU %s · %s", stats_role_label(r), count, cpu, memory);
        int box = doc_box_begin(doc, 0, w, px(10), theme.raise, theme.line, px(8));
        doc_text(doc, px(12), w - px(24), line, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
        doc_space(doc, px(5)); doc_stat_gauge(doc, px(12), w - px(24), stats_cpu_share(p, &s->reading.host));
        doc_box_end(doc, box, px(10)); doc_space(doc, px(6));
        free(cpu); free(memory); free(count); free(line);
    }
}
void resources_layout(Screen *base, Doc *doc) {
    ResourcesScreen *s = (ResourcesScreen *)base; int w = doc->width;
    doc_space(doc, px(12));
    if (!resources_current(s)) {
        doc_empty_state(doc, 0, w, 0xE9D9, "Resources unavailable", "This connection or token no longer supports this session's resources."); return;
    }
    resources_discard_revoked(s);
    if (s->error) doc_notice(doc, 0, w, s->error);
    double until = resources_retry_until(s);
    bool paused = until > (double)time(NULL);
    if (paused) {
        char *at = stats_time((StatNumber){until * 1000, true});
        char *text = xstrfmt("Reads paused until %s", at);
        doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(at); free(text);
    }
    if (!s->loaded) {
        if (!s->error && !paused) doc_loading(doc, 0, w, "Loading resources…");
        return;
    }
    char *read = stats_time((StatNumber){s->received_at * 1000, true});
    char *at = stats_time(s->reading.at_ms);
    char *freshness = xstrfmt("%s · last read %s · sampled %s", s->fresh ? "Last successful figures" : "Figures may be stale", read, at);
    doc_text(doc, 0, w, freshness, FONT_CAPTION, theme.muted, DT_WORDBREAK);
    free(read); free(at); free(freshness);
    if (!s->reading.open) {
        doc_empty_state(doc, 0, w, 0xE9D9, "The session is not open.", "Resource figures appear once the session is open and the server samples it."); return;
    }
    doc_space(doc, px(8));
    StatTile tiles[4]; session_stats_tiles(&s->reading, tiles);
    doc_stat_tiles(doc, 0, w, tiles, 4); stats_tiles_free(tiles, 4);
    if (s->reading.disk_bytes.present) {
        char *measured = stats_time(s->reading.disk_at_ms), *text = xstrfmt("Workspace measured %s", measured);
        doc_text(doc, 0, w, text, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(measured); free(text);
    }
    process_rows(s, doc, w);
    doc_section(doc, 0, w, "Last hour");
    doc_stats_chart(doc, 0, w, session_stats_series(&s->reading, STATS_PERCENT));
    doc_stats_chart(doc, 0, w, session_stats_series(&s->reading, STATS_BYTES));
}
