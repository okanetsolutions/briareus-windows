#include "screens.h"
#include "dialogs.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_REFRESH = 1200, ACT_FILTER, ACT_ENTITY, ACT_SERVICE, ACT_DATA, ACT_SEND, ACT_RESPONSE, ACT_EVENT, ACT_TAB };
enum { READ_STATUS, READ_ENTITIES, READ_SERVICES, READ_ENTITY, WRITE_ACTION, WRITE_EVENT, TIMER_POLL = 21 };
typedef struct {
    Screen base;
    ApiClient *account;
    Request *reads[4], *write;
    Poller poll;
    Json *status, *entities, *services, *entity, *service;
    char *filter, *entity_id, *domain, *service_name, *event_type, *action_data, *event_data, *error, *receipt;
    char *read_errors[4];
    bool shown, connected, response, modal, retired;
    int tab;
} HomeScreen;

bool home_assistant_offered(void) {
    return g_store.has_device && str_eq(g_store.device.permission, "admin") && store_supports("home_assistant");
}
static bool current(HomeScreen *s) { return s->account && s->account == g_store.client && home_assistant_offered(); }
static void repaint(HomeScreen *s) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
static void load(HomeScreen *s);
static void done(void *owner, Request *req);
static void destroy(Screen *base);
static void read_call(HomeScreen *s, int tag, const char *operation, Json *args) {
    if (!s->shown || !current(s) || s->reads[tag] || (double)GetTickCount64() < g_store.home_retry_until[tag] || !store_supports(operation)) { json_free(args); return; }
    store_call(operation, args, 0, s, done, tag, &s->reads[tag]);
}
static void entities_load(HomeScreen *s) {
    Json *args = json_object();
    // Reconcile commands against all devices, regardless of the browsing filter.
    if (g_store.home_uncertain) set_string(&s->filter, NULL);
    if (!str_empty(s->filter)) json_set_str(args, "domain", s->filter);
    read_call(s, READ_ENTITIES, "home_assistant_entities", args);
    if (!str_empty(s->entity_id)) {
        args = json_object(); json_set_str(args, "id", s->entity_id);
        read_call(s, READ_ENTITY, "home_assistant_entity", args);
    }
}
static void done(void *owner, Request *req) {
    HomeScreen *s = owner;
    if (!current(s) || req->client != s->account) return;
    if (req->tag >= WRITE_ACTION) {
        bool valid = req->ok && (req->tag == WRITE_EVENT ? json_bool_is(json_get(req->result, "ok"), true) : json_is_array(json_get(req->result, "entities")));
        g_store.home_uncertain = !valid;
        free(s->receipt);
        if (valid) {
            char *payload = json_pretty(req->result);
            s->receipt = xstrfmt("%s\n%s", req->tag == WRITE_EVENT ? "Event accepted. This does not confirm automation completion." : "Action accepted. Verify the device state below; reported changes do not confirm physical completion.", payload);
            free(payload);
        } else {
            char *why = req->ok ? xstrdup("Invalid action receipt.") : api_error_description(&req->error);
            s->receipt = xstrfmt("%s\nThe outcome may be unknown. Read the device state before sending another action. Nothing was retried.", why);
            free(why);
        }
        // Discard reads begun before the action so they cannot overwrite its follow-up state.
        request_cancel(&s->reads[READ_ENTITIES]); request_cancel(&s->reads[READ_ENTITY]);
        entities_load(s);
    } else if (!req->ok) {
        if (req->error.retry_after > 0) g_store.home_retry_until[req->tag] = (double)GetTickCount64() + req->error.retry_after * 1000;
        free(s->read_errors[req->tag]); s->read_errors[req->tag] = api_error_description(&req->error);
        if (req->tag == READ_STATUS) s->connected = false;
        if (req->tag == READ_STATUS || req->tag == READ_ENTITIES) poller_finished(&s->poll, true, req->error.retry_after);
    } else if (req->tag == READ_STATUS) {
        json_free(s->status); s->status = json_clone(req->result);
        s->connected = json_bool_is(json_get(s->status, "configured"), true) && json_bool_is(json_get(s->status, "connected"), true);
        set_string(&s->read_errors[READ_STATUS], NULL);
        if (s->connected) {
            entities_load(s);
            if (!s->services) read_call(s, READ_SERVICES, "home_assistant_services", json_object());
        }
        if (!s->reads[READ_ENTITIES]) poller_finished(&s->poll, false, -1);
    } else if (req->tag == READ_ENTITIES || req->tag == READ_SERVICES) {
        const char *key = req->tag == READ_ENTITIES ? "entities" : "services";
        if (!json_is_array(json_get(req->result, key))) set_string(&s->read_errors[req->tag], "The server returned an invalid smart home list.");
        else {
            set_string(&s->read_errors[req->tag], NULL);
            Json **target = req->tag == READ_ENTITIES ? &s->entities : &s->services;
            json_free(*target); *target = json_clone(json_get(req->result, key));
            if (req->tag == READ_ENTITIES && !json_str_nonempty(json_get(req->args, "domain"))) g_store.home_uncertain = false;
        }
        if (req->tag == READ_ENTITIES) poller_finished(&s->poll, s->read_errors[READ_ENTITIES] != NULL, -1);
    } else if (req->tag == READ_ENTITY) {
        if (!json_is_object(json_get(req->result, "entity"))) set_string(&s->read_errors[READ_ENTITY], "The server returned an invalid entity.");
        else { set_string(&s->read_errors[READ_ENTITY], NULL); json_free(s->entity); s->entity = json_clone(json_get(req->result, "entity")); }
    }
    repaint(s);
}
static void load(HomeScreen *s) { read_call(s, READ_STATUS, "home_assistant", json_object()); }
static void note(Doc *doc, const char *text) {
    doc_text(doc, 0, doc->width, text, FONT_BODY, theme.ink, DT_WORDBREAK); doc_space(doc, px(12));
}
static void button(Doc *doc, const char *text, int action, intptr_t arg, bool enabled) {
    doc_button(doc, 0, doc->width, text, BUTTON_BORDERED, action, arg, enabled); doc_space(doc, px(8));
}
static void layout(Screen *base, Doc *doc) {
    HomeScreen *s = (HomeScreen *)base;
    if (!current(s)) { note(doc, "Smart home requires an Admin token and a server offering Home Assistant. Reopen this screen after reconnecting."); return; }
    for (int i = 0; i < 4; i++) if (s->read_errors[i]) doc_notice_box(doc, 0, doc->width, s->read_errors[i]);
    if (s->error) doc_notice_box(doc, 0, doc->width, s->error);
    if (s->receipt) note(doc, s->receipt);
    if (!s->status) { note(doc, "Checking Home Assistant connection..."); return; }
    if (json_bool_is(json_get(s->status, "configured"), false)) { note(doc, "Home Assistant is not configured. Set HOME_ASSISTANT_URL and HOME_ASSISTANT_TOKEN on the server."); return; }
    if (!s->connected) { note(doc, "Home Assistant is disconnected. Check the server connection and credentials, then refresh."); return; }
    note(doc, "Connected to Home Assistant");
    int x = 0, y = doc->y;
    const char *tabs[] = { "Entities", "Actions", "Events" };
    for (int i = 0; i < 3; i++) doc_tab(doc, &x, &y, 0, doc->width, px(42), 0, tabs[i], NULL, s->tab == i, ACT_TAB, i);
    doc->y = y + px(50);
    if (!s->tab) {
        char *label = xstrfmt("Domain: %s", str_empty(s->filter) ? "all" : s->filter);
        button(doc, label, ACT_FILTER, 0, true); free(label);
        if (s->entity) { char *text = json_pretty(s->entity); note(doc, text); free(text); }
        if (!s->entities) note(doc, "Loading entities...");
        else if (!json_count(s->entities)) note(doc, "No entities match this domain.");
        for (size_t i = 0; i < json_count(s->entities); i++) {
            const Json *e = json_at(s->entities, i);
            const char *id = json_str_nonempty(json_get(e, "id")); if (!id) continue;
            label = xstrfmt("%s (%s): %s", json_str_or(json_get(e, "name"), id), id, json_str_or(json_get(e, "state"), "unknown"));
            button(doc, label, ACT_ENTITY, (intptr_t)i, store_supports("home_assistant_entity")); free(label);
        }
    } else if (s->tab == 1) {
        if (!store_supports("home_assistant_services")) { note(doc, "This server does not offer action discovery."); return; }
        if (s->service) {
            char *label = xstrfmt("%s.%s", s->domain, s->service_name); note(doc, label); free(label);
            char *definition = json_pretty(s->service); note(doc, definition); free(definition);
            button(doc, s->response ? "Response data: requested" : "Response data: not requested", ACT_RESPONSE, 0, !s->write);
            button(doc, "Edit action data", ACT_DATA, 0, !s->write); note(doc, s->action_data);
            button(doc, s->write ? "Sending..." : "Run action", ACT_SEND, 0, !s->write && !g_store.home_uncertain && store_supports("home_assistant_action"));
        }
        if (!s->services) note(doc, "Loading available actions...");
        for (size_t i = 0; i < json_count(s->services); i++) {
            const Json *domain = json_at(s->services, i), *services = json_get(domain, "services");
            const char *name = json_str_nonempty(json_get(domain, "domain")); if (!name) continue;
            note(doc, name);
            for (size_t k = 0; k < json_count(services) && k < 65536; k++) {
                const char *key = json_key(services, k); if (!key) continue;
                char *label = xstrfmt("%s.%s", name, key);
                button(doc, label, ACT_SERVICE, (intptr_t)(i * 65536 + k), !s->write); free(label);
            }
        }
    } else {
        note(doc, "Send event data to Home Assistant automations.");
        button(doc, str_empty(s->event_type) ? "Choose event type" : s->event_type, ACT_EVENT, 0, !s->write);
        button(doc, "Edit event data", ACT_DATA, 0, !s->write); note(doc, s->event_data);
        button(doc, s->write ? "Sending..." : "Fire event", ACT_SEND, 0, !s->write && !g_store.home_uncertain && !str_empty(s->event_type) && store_supports("home_assistant_event"));
    }
    if (g_store.home_uncertain) note(doc, "Refresh entity states before sending another action.");
}
static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    HomeScreen *s = (HomeScreen *)base; (void)pt;
    if (!current(s)) return;
    if (act == ACT_REFRESH) { json_free(s->services); s->services = NULL; load(s); }
    else if (act == ACT_TAB && arg >= 0 && arg < 3) s->tab = (int)arg;
    else if (act == ACT_ENTITY) {
        const char *id = json_str_nonempty(json_get(json_at(s->entities, (size_t)arg), "id"));
        if (id) { set_string(&s->entity_id, id); json_free(s->entity); s->entity = NULL; request_cancel(&s->reads[READ_ENTITY]); entities_load(s); }
    } else if (act == ACT_FILTER || act == ACT_EVENT || act == ACT_DATA) {
        if (s->write) return;
        char **draft = s->tab == 2 ? &s->event_data : &s->action_data;
        s->modal = true;
        char *value = dialog_text(pane_hwnd(base->pane), "Smart home", act == ACT_FILTER ? "Domain (empty for all)" : act == ACT_EVENT ? "Event type" : "Data as a JSON object", "Apply", act == ACT_FILTER ? s->filter : act == ACT_EVENT ? s->event_type : *draft);
        s->modal = false;
        if (s->retired) { free(value); destroy(base); return; }
        if (!current(s)) { free(value); return; }
        if (value) {
            if (act == ACT_DATA) {
                Json *data = json_parsez(value);
                if (json_is_object(data)) { set_string(draft, value); set_string(&s->error, NULL); }
                else set_string(&s->error, "Enter a valid JSON object, such as {\"entity_id\":\"light.kitchen\"}.");
                json_free(data);
            } else if (act == ACT_FILTER) {
                set_string(&s->filter, value); request_cancel(&s->reads[READ_ENTITIES]); json_free(s->entities); s->entities = NULL; entities_load(s);
            } else set_string(&s->event_type, value);
            free(value);
        }
    } else if (act == ACT_SERVICE && !s->write && arg >= 0) {
        const Json *domain = json_at(s->services, (size_t)arg / 65536), *services = json_get(domain, "services");
        const char *key = json_key(services, (size_t)arg % 65536), *name = json_str_nonempty(json_get(domain, "domain"));
        if (key && name) {
            set_string(&s->domain, name); set_string(&s->service_name, key);
            json_free(s->service); s->service = json_clone(json_get(services, key));
            const Json *response = json_get(s->service, "response");
            s->response = json_is_object(response) && !json_bool_is(json_get(response, "optional"), true);
            Json *data = json_object(); if (!str_empty(s->entity_id)) json_set_str(data, "entity_id", s->entity_id);
            free(s->action_data); s->action_data = json_serialize(data, false); json_free(data);
        }
    } else if (act == ACT_RESPONSE && !s->write) s->response = !s->response;
    else if (act == ACT_SEND && !s->write && !g_store.home_uncertain && s->connected) {
        bool event = s->tab == 2;
        const char *op = event ? "home_assistant_event" : "home_assistant_action";
        if (!store_supports(op) || (event ? str_empty(s->event_type) : !s->service)) return;
        Json *data = json_parsez(event ? s->event_data : s->action_data);
        if (!json_is_object(data)) { json_free(data); return; }
        Json *args = json_object(); json_object_set(args, "data", data);
        if (event) json_set_str(args, "type", s->event_type);
        else { json_set_str(args, "domain", s->domain); json_set_str(args, "service", s->service_name); json_set_num(args, "returnResponse", s->response ? 1 : 0); }
        set_string(&s->receipt, NULL);
        store_call(op, args, 60000, s, done, event ? WRITE_EVENT : WRITE_ACTION, &s->write);
    }
    repaint(s);
}
static void header(Screen *base, HeaderInfo *info) {
    HomeScreen *s = (HomeScreen *)base;
    snprintf(info->title, sizeof info->title, "Smart home");
    info->buttons[info->button_count++] = (HeaderButton){ .glyph = 0xE72C, .action = ACT_REFRESH, .enabled = current(s) && !s->reads[READ_STATUS], .tip = "Refresh" };
}
static void timer(Screen *base, UINT id) {
    HomeScreen *s = (HomeScreen *)base;
    if (poller_fired(&s->poll, id)) {
        SetTimer(pane_hwnd(base->pane), id, (UINT)poll_delay_ms(s->poll.base_ms, s->poll.failures, s->poll.retry_after), NULL);
        if (!s->reads[READ_STATUS] && !s->reads[READ_ENTITIES] && !s->write) load(s);
    }
}
static void visible(Screen *base, bool shown) {
    HomeScreen *s = (HomeScreen *)base; s->shown = shown;
    if (shown) { poller_start(&s->poll, base->pane, TIMER_POLL, 10000); load(s); }
    else { poller_stop(&s->poll); for (int i = 0; i < 4; i++) request_cancel(&s->reads[i]); }
}
static void refresh(Screen *base) { action(base, ACT_REFRESH, 0, (POINT){0}); }
static bool can_leave(Screen *base) {
    HomeScreen *s = (HomeScreen *)base;
    if (!s->write) return true;
    set_string(&s->receipt, "Wait for the pending smart home command to finish before leaving this screen.");
    repaint(s);
    return false;
}
static void destroy(Screen *base) {
    HomeScreen *s = (HomeScreen *)base; poller_stop(&s->poll);
    for (int i = 0; i < 4; i++) request_cancel(&s->reads[i]);
    request_cancel(&s->write);
    s->shown = false; s->retired = true;
    if (s->modal) return;
    for (int i = 0; i < 4; i++) free(s->read_errors[i]);
    api_client_release(s->account);
    json_free(s->status); json_free(s->entities); json_free(s->services); json_free(s->entity); json_free(s->service);
    free(s->filter); free(s->entity_id); free(s->domain); free(s->service_name); free(s->event_type); free(s->action_data); free(s->event_data); free(s->error); free(s->receipt);
    screen_release(base);
}
static const ScreenVTable vt = { .destroy = destroy, .layout = layout, .header = header, .action = action, .timer = timer, .visible = visible, .refresh = refresh, .can_leave = can_leave, .searchable = true };
Screen *home_assistant_screen_new(void) {
    HomeScreen *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("settings-home");
    s->account = api_client_retain(g_store.client); s->action_data = xstrdup("{}"); s->event_data = xstrdup("{}"); return &s->base;
}
