#ifndef BRIAREUS_ACTION_RUNTIME_H
#define BRIAREUS_ACTION_RUNTIME_H
#include "store.h"
// Fresh runtime selection for a CLI-restricted action; free before its UI owner dies.
typedef struct ActionRuntime {
    ApiClient *client;
    char *device, *repo, *binary, *error;
    RuntimeCatalog catalog;
    RuntimeChoice choice;
    Json *raw;
    Request *read;
    bool ready, closed, allow_project_default, server_default;
    void (*changed)(void *ctx); void *ctx;
} ActionRuntime;
void action_runtime_init(ActionRuntime *s, const char *repo, const char *binary, bool allow_project_default, void (*changed)(void *), void *ctx);
void action_runtime_load(ActionRuntime *s);
bool action_runtime_current(const ActionRuntime *s);
bool action_runtime_select(ActionRuntime *s, int provider, const char *model, const char *effort);
/// A confirmed server default returns a zero choice: omit provider, model and effort together.
bool action_runtime_take(ActionRuntime *s, RuntimeChoice *out);
void action_runtime_free(ActionRuntime *s);
#endif
