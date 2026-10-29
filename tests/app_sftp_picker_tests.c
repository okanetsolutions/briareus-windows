#include "project_sftp_internal.h"
#include "suites.h"
#include "test.h"
#include <commdlg.h>

static int depth, begins, ends, uploads;
static void begin(HWND owner) { depth++; begins++; app_modal_begin(owner); }
static void end(void) { CHECK_INT(depth, 1); depth--; ends++; app_modal_end(); }
static BOOL WINAPI cancel_picker(LPOPENFILENAMEW args) { (void)args; CHECK_INT(depth, 1); return FALSE; }
static DWORD WINAPI no_error(void) { return 0; }
static void no_upload(SftpSession *s, const wchar_t *path, const char *folder, bool dir) {
    (void)s; (void)path; (void)folder; (void)dir; uploads++;
}
#define app_modal_begin begin
#define app_modal_end end
#define GetOpenFileNameW cancel_picker
#define CommDlgExtendedError no_error
#define sftp_upload no_upload
#define project_sftp_target_folder picker_target_folder
#define project_sftp_upload_files picker_upload_files
#define project_sftp_pick_folder picker_pick_folder
#define project_sftp_upload_folder picker_upload_folder
#define project_sftp_set_dropping picker_set_dropping
char *picker_target_folder(SftpSession *s);
void picker_upload_files(ProjectSftp *p, SftpSession *s, const char *folder);
wchar_t *picker_pick_folder(HWND parent, const char *title);
void picker_upload_folder(ProjectSftp *p, SftpSession *s, const char *folder);
void picker_set_dropping(ProjectSftp *p, bool on);
#include "../app/project_sftp_upload.c"

static void cancelled_picker_balances_modal(void) {
    Screen host = {0}; ProjectSftp p = {.host = &host};
    begins = ends = uploads = depth = 0;
    picker_upload_files(&p, NULL, "/not-contacted");
    CHECK_INT(begins, 1); CHECK_INT(ends, 1); CHECK_INT(depth, 0); CHECK_INT(uploads, 0);
}
void app_sftp_picker_tests(void) { test_run("SFTP file picker cancellation balances modal scope without uploads", cancelled_picker_balances_modal); }
