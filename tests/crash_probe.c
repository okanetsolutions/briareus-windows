#include "crash.h"
#include <windows.h>

int main(void) {
    crash_init();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    RaiseException(0xE0424242, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 1;
}
