#ifndef BRIAREUS_ATTACH_LIST_INTERNAL_H
#define BRIAREUS_ATTACH_LIST_INTERNAL_H
#include "attach_list.h"
#include "attach.h"
#include "store.h"

typedef struct Attachment {
    char *name; size_t size; char *id; RECT remove_rc;
    AttachFile source;
    volatile LONG cancelled;
    char *error;
    Request *req;
} Attachment;

struct AttachList {
    Screen *screen;
    char *call, *scope;
    Attachment **items; size_t count;
    Attacher *attacher;
    Attachment *active;
    ApiClient *client;
    Json *device;
    size_t refs;
    bool retired;
    struct AttachList *next;
};
void attach_list_changed(AttachList *l);
Attacher *attach_list_attacher(AttachList *l);
void attach_list_sync(AttachList *l);
#endif
