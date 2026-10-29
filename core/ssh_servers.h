#ifndef BRIAREUS_SSH_SERVERS_H
#define BRIAREUS_SSH_SERVERS_H
#include "models.h"

/// Owned folders {repo,title,indices}, with indices into borrowed rows; sidebar order, unlisted repos sorted, unassigned last.
Json *ssh_server_folders(const Json *rows, const Project *projects, size_t count, const char *query);
/// Owned stable registry identity, shared by project and global session views.
char *ssh_server_key(const Json *row);
/// Owned display name, falling back to user@host.
char *ssh_server_name(const Json *row);
/// Search overrides folding; toggles during a search leave the saved folds untouched.
bool ssh_server_folder_open(const Json *folded, const char *repo, const char *query);
void ssh_server_folder_toggle(Json *folded, const char *repo, const char *query);
#endif
