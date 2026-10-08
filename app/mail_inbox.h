#ifndef BRIAREUS_MAIL_INBOX_H
#define BRIAREUS_MAIL_INBOX_H
#include "store.h"
/// Includes screen/filter generation and account permission checks before consuming private responses.
bool mail_inbox_result_current(bool shown, int generation, const Request *request);
#endif
