// SQL run on a server's MySQL over SSH: this PC's OpenSSH client starts the server's own `mysql` client, which reads
// the password and then the statements from its standard input, so neither shows on a command line. Prompts for an SSH
// password or host key come up as the SFTP sessions' do. Each query is a process of its own; its answer is handed back
// on the UI thread.
#ifndef BRIAREUS_SSH_QUERY_H
#define BRIAREUS_SSH_QUERY_H
#include "terminal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// Where the database listens as seen from the SSH server, and the login it takes.
typedef struct { const char *host; int port; const char *user, *password; } SqlLogin;

/// A query's answer: `mysql --batch` output when `ok`, else what ssh or mysql said went wrong.
typedef void (*SshQueryDone)(void *ctx, intptr_t tag, bool ok, const char *out);

/// Starts `sql` on the server; `done(ctx, tag, …)` follows unless the query is cancelled first. False with `*error`
/// when ssh could not start.
bool ssh_query_run(const TermTarget *target, const SqlLogin *login, const char *sql, SshQueryDone done, void *ctx, intptr_t tag, char **error);
/// Stops every query `ctx` started; their answers are dropped.
void ssh_query_cancel(void *ctx);

/// `mysql --batch` output split into rows of fields, the first row the column names, with \t \n \\ \0 unescaped.
typedef struct { char **cells; size_t cols, rows; } SqlTable;
void sql_table_parse(const char *out, SqlTable *t);
void sql_table_free(SqlTable *t);
static inline const char *sql_cell(const SqlTable *t, size_t row, size_t col) { return t->cells[row * t->cols + col]; }
/// A name as a MySQL identifier: in backticks, any backtick doubled.
char *sql_ident(const char *name);
/// A string quoted for a POSIX shell: in single quotes, any single quote closed, escaped and reopened.
char *sh_quote(const char *s);
/// The remote command ssh is given for `login`.
char *ssh_query_remote_command(const SqlLogin *login);

#endif
