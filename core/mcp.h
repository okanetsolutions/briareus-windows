// Operator MCP registry forms; secrets are write-only and sign-in lifetime belongs to core.
#ifndef BRIAREUS_MCP_H
#define BRIAREUS_MCP_H
#include "json.h"

typedef enum { MCP_KEEP, MCP_REPLACE, MCP_CLEAR } McpSecretMode;
/// Copies only writable fields, validates them, and includes secrets only with explicit replacement/clearing.
/// fields holds JSON args/repos and string-to-string headers/env; error is a new, value-free diagnostic.
/// An omitted HTTP url preserves the endpoint on updates; creates and transport changes must supply it.
Json *mcp_form_body(const Json *fields, McpSecretMode headers, McpSecretMode env, McpSecretMode client_secret, char **error);
bool mcp_secure_url(const char *url);
bool mcp_callback_url(const char *url);
/// Millisecond ids are larger than int; malformed responses must never select another server.
double mcp_server_id(const Json *row);

typedef struct { char *url; bool paste, blocked; } McpSignIn;
/// A fresh read is authoritative: null URL means expired, completed, or invalidated, regardless of status.
/// After a failed finish, the same URL stays blocked until the operator starts a fresh sign-in.
void mcp_sign_in_update(McpSignIn *state, const Json *row);
void mcp_sign_in_failed(McpSignIn *state);
void mcp_sign_in_clear(McpSignIn *state);
bool mcp_sign_in_can_finish(const McpSignIn *state);
#endif
