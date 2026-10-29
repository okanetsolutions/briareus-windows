#include "provider_settings.h"
#include "str.h"

Json *provider_settings_binary_ids(const Json *openapi) {
    const Json *schemas = json_get(json_get(openapi, "components"), "schemas");
    const Json *properties = json_get(json_get(schemas, "Provider"), "properties");
    const Json *ids = json_get(json_get(properties, "binary"), "enum");
    if (!json_is_array(ids) || !json_count(ids) || json_count(ids) > 128) return NULL;
    for (size_t i = 0; i < json_count(ids); i++) {
        const char *id = json_str_nonempty(json_at(ids, i));
        if (!id) return NULL;
        for (size_t k = 0; k < i; k++) if (str_eq(id, json_str(json_at(ids, k)))) return NULL;
    }
    return json_clone(ids);
}
