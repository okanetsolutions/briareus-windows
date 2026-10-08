#ifndef BRIAREUS_MAIL_INBOX_H
#define BRIAREUS_MAIL_INBOX_H
#include "store.h"
typedef struct Screen Screen;
/// Includes screen/filter generation and account permission checks before consuming private responses.
bool mail_inbox_result_current(bool shown, int generation, const Request *request);
/// Sets the exact thread id without the filter dialog. Regressions use it; app_tests does not link that dialog.
void mail_inbox_set_thread_filter(Screen *screen, const char *thread);
#endif
