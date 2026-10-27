#include "project_settings.h"
#include "str.h"

ProjectBabysit project_babysit_read(const Json *row) {
    const Json *value = json_get(row, "babysitPrs");
    bool on = json_bool_is(value, true);
    bool supported = false;
    for (size_t i = 0; i < json_count(row); i++) if (str_eq(json_key(row, i), "babysitPrs")) supported = true;
    return (ProjectBabysit){ supported, on, on };
}

void project_babysit_toggle(ProjectBabysit *s) { if (s->supported) s->on = !s->on; }
bool project_babysit_changed(const ProjectBabysit *s) { return s->supported && s->on != s->saved; }

void project_babysit_write(const ProjectBabysit *s, Json *body) {
    if (s->supported) json_set_bool(body, "babysitPrs", s->on);
    else json_object_remove(body, "babysitPrs");
}

const char *project_babysit_note(const ProjectBabysit *s) {
    if (!s->supported || !s->on) return NULL;
    return "Every 15 minutes the server starts the next needed errand for each eligible open pull request: solve conflicts, fix failing checks, then a code review when recommended, using the code review runtime. Each errand runs at most once per head commit. Drafts, forks, blocked or do-not-merge pull requests, and those with an open session are skipped. A PR author limits conflict and check fixes to that author's pull requests. Repeated unsuccessful fixes stop after three attempts; reviews can still run.";
}
