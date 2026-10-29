#include "project_ssh_internal.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>

enum { SSH_ACTION = 700 };
typedef struct { Screen base; ProjectSsh *ssh; bool shown; } SshScreen;

static void destroy(Screen *base) {
    SshScreen *s = (SshScreen *)base;
    project_ssh_free(s->ssh); free(base->id); free(s);
}
static void layout(Screen *base, Doc *doc) {
    SshScreen *s = (SshScreen *)base;
    servers_ssh_scope_changed();
    project_ssh_layout(s->ssh, doc, doc->width);
}
static void header(Screen *base, HeaderInfo *info) {
    SshScreen *s = (SshScreen *)base;
    snprintf(info->title, sizeof info->title, "SSH servers");
    project_ssh_header(s->ssh, info);
    if (s->ssh->loading) snprintf(info->subtitle, sizeof info->subtitle, "Loading servers…");
}
static void action(Screen *base, int act, intptr_t arg, POINT point) {
    project_ssh_action(((SshScreen *)base)->ssh, act, arg, point);
}
static void place(Screen *base, const RECT *content, int scroll) {
    SshScreen *s = (SshScreen *)base;
    project_ssh_place(s->ssh, content, scroll, s->shown && project_ssh_offered());
}
static void visible(Screen *base, bool shown) {
    SshScreen *s = (SshScreen *)base; s->shown = shown;
    if (shown) project_ssh_refresh(s->ssh);
    else place(base, NULL, 0);
}
static void refresh(Screen *base) { project_ssh_refresh(((SshScreen *)base)->ssh); }
static const ScreenVTable vt = {
    .destroy = destroy, .layout = layout, .header = header, .action = action,
    .place = place, .visible = visible, .refresh = refresh,
};
Screen *ssh_servers_screen_new(void) {
    SshScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &vt; s->base.id = xstrdup("ssh-servers");
    s->ssh = project_ssh_new(NULL, &s->base, SSH_ACTION);
    return &s->base;
}
