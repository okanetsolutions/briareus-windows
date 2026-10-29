#ifndef BRIAREUS_PROVIDER_SETTINGS_H
#define BRIAREUS_PROVIDER_SETTINGS_H
#include "json.h"

// Owned supported binary IDs from OpenAPI's Provider.binary enum, or NULL when absent/malformed.
// This lists supported binaries independently of configured provider rows.
Json *provider_settings_binary_ids(const Json *openapi);
#endif
