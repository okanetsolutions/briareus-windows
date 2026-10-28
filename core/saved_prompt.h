#ifndef BRIAREUS_SAVED_PROMPT_H
#define BRIAREUS_SAVED_PROMPT_H
#include "json.h"
bool saved_prompt_scope_allowed(const char *permission, char *const *repos, size_t count, const char *repo);
// All returned JSON and strings are owned. Lists retain server identities and scope.
// NULL list means malformed response; a valid empty response returns an empty array.
Json *saved_prompt_list(const Json *answer, const char *repo);
const Json *saved_prompt_find(const Json *rows, int id);
char *saved_prompt_insert(const char *draft, const char *body);
// Create includes chosen scope; edit emits only changed title/body, never scope/order.
// NULL means invalid UTF-8, empty required fields or server UTF-16 length limits.
Json *saved_prompt_create(const char *title, const char *body, const char *repo);
Json *saved_prompt_edit(const Json *original, const char *title, const char *body);
// Defense at the wire boundary: whitelist and validate fields, including partial PUT.
Json *saved_prompt_wire(const Json *args, bool create);
#endif
