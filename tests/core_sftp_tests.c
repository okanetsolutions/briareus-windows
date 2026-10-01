// sftp.c: the long listing, quoting paths for sftp's command line, and remote path arithmetic.
#include "sftp.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void test_parses_a_long_listing(void) {
    SftpEntry e;
    CHECK(sftp_parse_entry("drwxr-xr-x    2 root     root         4096 Jan  1 12:00 www", &e));
    CHECK_STR(e.name, "www"); CHECK(e.dir); CHECK(!e.link); CHECK_INT(e.size, 4096);
    CHECK_STR(e.when, "Jan 1 12:00"); CHECK_STR(e.perms, "drwxr-xr-x");
    sftp_entry_free(&e);
    // Names keep their spaces; OpenSSH's own server writes "?" for the link count on Windows; a carriage return goes.
    CHECK(sftp_parse_entry("-rw-r--r--    ? nadin    197609          3 Oct  1 19:34 a  b.txt\r", &e));
    CHECK_STR(e.name, "a  b.txt"); CHECK(!e.dir); CHECK_INT(e.size, 3);
    sftp_entry_free(&e);
    CHECK(sftp_parse_entry("-rw-------    1 1000     1000     123456789012 Mar 14  2023 big.iso", &e));
    CHECK_INT(e.size, 123456789012LL); CHECK_STR(e.when, "Mar 14 2023");
    sftp_entry_free(&e);
}

static void test_reads_links_and_skips_the_dot_entries(void) {
    SftpEntry e;
    CHECK(sftp_parse_entry("lrwxrwxrwx    1 root     root            7 Jan  1 12:00 bin -> usr/bin", &e));
    CHECK_STR(e.name, "bin"); CHECK(e.link); CHECK(!e.dir);
    sftp_entry_free(&e);
    CHECK(!sftp_parse_entry("drwxr-xr-x    2 root     root         4096 Jan  1 12:00 .", &e));
    CHECK(!sftp_parse_entry("drwxr-xr-x    2 root     root         4096 Jan  1 12:00 ..", &e));
    // `ls -la /dir/` prints the entries under their full paths.
    CHECK(!sftp_parse_entry("drwxr-xr-x    ? nadin    197609          0 Aug 23 14:27 /c/Users/.", &e));
    CHECK(sftp_parse_entry("lrwxrwxrwx    ? nadin    197609         14 Apr  1  2024 /c/Users/All Users", &e));
    CHECK_STR(e.name, "All Users"); CHECK(e.link);
    sftp_entry_free(&e);
    CHECK(sftp_parse_entry("-rw-r--r--    1 root     root            0 Jan  1 12:00 .bashrc", &e));
    CHECK_STR(e.name, ".bashrc");
    sftp_entry_free(&e);
}

static void test_refuses_lines_that_are_not_entries(void) {
    SftpEntry e;
    CHECK(!sftp_parse_entry("Can't ls: \"/nope\" not found", &e));
    CHECK(!sftp_parse_entry("sftp> ls -la /", &e));
    CHECK(!sftp_parse_entry("", &e));
    CHECK(!sftp_parse_entry(NULL, &e));
    CHECK(!sftp_parse_entry("drwxr-xr-x    2 root     root         4096 Jan  1 12:00", &e));
    CHECK(!sftp_parse_entry("drwxr-xr-x    2 root     root         40x6 Jan  1 12:00 a", &e));
    CHECK(!sftp_parse_entry("Fetching /a to C:/b", &e));
}

static void test_quotes_paths_for_the_command_line(void) {
    CHECK_OWNED_STR(sftp_quote("/home/me/file.txt"), "/home/me/file.txt");
    CHECK_OWNED_STR(sftp_quote("/srv/a b/up[1].txt"), "/srv/a\\ b/up\\[1\\].txt");
    CHECK_OWNED_STR(sftp_quote("q\"u'o*te?"), "q\\\"u\\'o\\*te\\?");
    CHECK_OWNED_STR(sftp_quote("C:/Users/me/x.zip"), "C:/Users/me/x.zip");
    CHECK_OWNED_STR(sftp_quote("~/#notes"), "\\~/\\#notes");
    CHECK_OWNED_STR(sftp_quote("back\\slash"), "back\\\\slash");
    CHECK_OWNED_STR(sftp_quote("-rf"), "./-rf");
    CHECK_OWNED_STR(sftp_quote("/caf\xC3\xA9"), "/caf\xC3\xA9");
    CHECK(!sftp_quote("two\nlines"));
    CHECK(!sftp_quote(""));
    CHECK(!sftp_quote(NULL));
}

static void test_does_path_arithmetic(void) {
    CHECK_OWNED_STR(sftp_join("/", "etc"), "/etc");
    CHECK_OWNED_STR(sftp_join("/home/me", "a b"), "/home/me/a b");
    CHECK_OWNED_STR(sftp_join("/home/me/", "x"), "/home/me/x");
    CHECK_OWNED_STR(sftp_parent("/home/me"), "/home");
    CHECK_OWNED_STR(sftp_parent("/home"), "/");
    CHECK_OWNED_STR(sftp_parent("/home/me/"), "/home");
    CHECK(!sftp_parent("/"));
    CHECK_STR(sftp_basename("/home/me"), "me");
    CHECK_STR(sftp_basename("/"), "/");
    CHECK(sftp_path_within("/home/me/x", "/home/me"));
    CHECK(sftp_path_within("/home/me", "/home/me"));
    CHECK(!sftp_path_within("/home/meow", "/home/me"));
    CHECK(sftp_path_within("/etc", "/"));
}

static void test_formats_sizes(void) {
    CHECK_OWNED_STR(sftp_format_size(1), "1 byte");
    CHECK_OWNED_STR(sftp_format_size(512), "512 bytes");
    CHECK_OWNED_STR(sftp_format_size(1536), "1.5 KB");
    CHECK_OWNED_STR(sftp_format_size(52428800), "50 MB");
}

void sftp_tests(void) {
    test_run("sftp parses a long listing", test_parses_a_long_listing);
    test_run("sftp reads links and skips the dot entries", test_reads_links_and_skips_the_dot_entries);
    test_run("sftp refuses lines that are not entries", test_refuses_lines_that_are_not_entries);
    test_run("sftp quotes paths for the command line", test_quotes_paths_for_the_command_line);
    test_run("sftp does path arithmetic", test_does_path_arithmetic);
    test_run("sftp formats sizes", test_formats_sizes);
}
