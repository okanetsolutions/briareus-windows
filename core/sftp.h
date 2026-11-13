// What the SFTP sessions tab says to OpenSSH's sftp client and reads back: paths written as sftp's command line wants
// them, the long listing `ls -la` prints, and remote path arithmetic. Also the arguments every OpenSSH program here
// (terminal, SFTP and SQL sessions) runs with, so they share one host key policy.
#ifndef BRIAREUS_SFTP_H
#define BRIAREUS_SFTP_H
#include <stdbool.h>

/// One row of `ls -la`: "drwxr-xr-x    2 root     root         4096 Jan  1 12:00 name".
typedef struct {
    char *name;
    char perms[11];
    bool dir, link;
    long long size;
    char *when;   // "Jan 1 12:00" or "Jan 1 2024"
} SftpEntry;

/// Reads one line of a long listing; false for anything else (an error, a blank line) and for "." and "..".
bool sftp_parse_entry(const char *line, SftpEntry *out);
void sftp_entry_free(SftpEntry *e);

/// A path as one word of sftp's command line: every character outside letters, digits and `/._-+,=@%:` escaped with a
/// backslash (which also keeps glob characters literal), and "./" before a leading '-'. NULL when it holds a line break
/// or another control character, which the command line cannot carry. The caller frees it.
char *sftp_quote(const char *path);

/// `dir` + '/' + `name`, without doubling the slash.
char *sftp_join(const char *dir, const char *name);
/// The folder holding `path`: "/" for "/x", NULL for "/" itself.
char *sftp_parent(const char *path);
/// The last component of `path` ("/" for "/").
const char *sftp_basename(const char *path);
/// Whether `path` is `folder` or inside it.
bool sftp_path_within(const char *path, const char *folder);

/// A size as Explorer shows it: "512 bytes", "1.4 KB", "3.2 MB".
char *sftp_format_size(long long bytes);

/// The arguments after the program, for a user and host term_target_problem accepted. Every one is a plain word, so
/// Git for Windows' (msys) and Windows' own OpenSSH split them alike. Each adds StrictHostKeyChecking=accept-new: a new
/// server's host key is saved without asking, and a key that differs from the saved one is still refused. Given on the
/// command line it overrides ~/.ssh/config, but a ProxyJump host runs in an ssh of its own that keeps its config's
/// policy, so an unknown jump host still asks to be trusted. The caller frees each.
/// Interactive ssh: "-p PORT -l USER -o StrictHostKeyChecking=accept-new -- HOST".
char *openssh_terminal_args(const char *user, const char *host, int port);
/// sftp, keeping idle connections alive: "-P PORT -o User=USER -o ServerAliveInterval=30 -o StrictHostKeyChecking=accept-new -- HOST".
char *openssh_sftp_args(const char *user, const char *host, int port);
/// SQL over ssh without a terminal, the remote command to follow: "-T -o ServerAliveInterval=30 -o LogLevel=ERROR " and
/// the interactive arguments.
char *openssh_query_args(const char *user, const char *host, int port);

#endif
