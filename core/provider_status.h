#ifndef BRIAREUS_PROVIDER_STATUS_H
#define BRIAREUS_PROVIDER_STATUS_H
#include "json.h"

/// Whether status identifies a new login relative to the borrowed pre-login auth.
/// Read timestamps alone cannot confirm a login; same-account re-login may remain undetectable.
bool provider_login_landed(const Json *from_auth, const Json *status);
#endif
