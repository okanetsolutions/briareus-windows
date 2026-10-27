#include "provider_status.h"

bool provider_login_landed(const Json *from_auth, const Json *status) {
    const Json *auth = json_get(status, "auth");
    if (!json_bool_is(json_get(auth, "loggedIn"), true)) return false;
    if (!json_bool_is(json_get(from_auth, "loggedIn"), true)) return true;
    Json *was = json_clone(from_auth), *now = json_clone(auth);
    json_object_remove(was, "checkedAt"); json_object_remove(now, "checkedAt");
    bool changed = !json_equal(was, now);
    json_free(was); json_free(now);
    return changed;
}
