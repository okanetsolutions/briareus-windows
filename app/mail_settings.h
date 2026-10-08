// Windowless parts of the account screen's request lifecycle.
#ifndef BRIAREUS_MAIL_SETTINGS_H
#define BRIAREUS_MAIL_SETTINGS_H
#include "store.h"
bool mail_settings_result_current(bool shown, const Request *request);
int mail_settings_retry_ms(int failures, double retry_after);
#endif
