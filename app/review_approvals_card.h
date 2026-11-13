// The human approvals a pull request's risk asks for, as a card under its review outcome (the Mac's ReviewApprovalsCard):
// how many count of how many, the server's explanation and reasons, what set the risk tier (folded), every approval on
// record with whether it counts, and Approve this revision and Withdraw for a token that may record them, each with an
// optional note typed in a box laid over the page. The Code reviews tab and ◉ Status draw it; the words and decisions are
// review_approvals.c's.
#ifndef BRIAREUS_REVIEW_APPROVALS_CARD_H
#define BRIAREUS_REVIEW_APPROVALS_CARD_H
#include "review_approvals.h"
#include "screens.h"

// The card's actions, offset from the owner's `action_base`; Withdraw's argument is the approval's id.
enum { APPROVAL_ACT_APPROVE, APPROVAL_ACT_WITHDRAW, APPROVAL_ACT_SEND, APPROVAL_ACT_CANCEL, APPROVAL_ACT_RISK, APPROVAL_ACTIONS };
// What `open` and `saving` hold for Approve; an approval's id stands for its withdrawal, 0 for nothing.
enum { APPROVAL_FORM_APPROVE = -1 };

typedef struct {
    // The approvals the card shows now; NULL when there are none.
    const ReviewApprovals *(*approvals)(void *owner);
    // Another write of the owner's is under way, which holds this one back.
    bool (*busy)(void *owner);
    // Called just before the write goes: a read under way may predate it and gives way.
    void (*sending)(void *owner);
    // The write's completion, on the UI thread, with `owner`; it hands the request to approval_form_finish.
    RequestDone done;
} ApprovalFormHost;

// One pull request's card state. The owner keeps it alive past every request it starts (approval_form_clear cancels it)
// and clears `box_laid` before each layout (approval_form_layout_begin).
typedef struct {
    const ApprovalFormHost *host; void *owner; Screen *screen;   // borrowed
    char *repo; int number; int action_base;
    int64_t open, saving;   // the note form shown and the write under way
    bool risk_open;
    char *note, *error;     // the note as typed; why the last write failed
    Request *req;
    // The note box, a child of the screen's pane over `box_rc` (content coordinates) while `box_laid`; `box_for` is the
    // form its text belongs to, and `focus_box` focuses it once shown.
    HWND box; RECT box_rc; bool box_laid, focus_box; int64_t box_for;
} ApprovalForm;

// `risk_open` starts with what set the risk shown, as ◉ Status does for the approval it asks for.
void approval_form_init(ApprovalForm *form, const ApprovalFormHost *host, void *owner, Screen *screen, const char *repo, int number,
    int action_base, bool risk_open);
// Cancels the write, destroys the note box and frees what the form holds; the form may be initialised again.
void approval_form_clear(ApprovalForm *form);
bool approval_form_writing(const ApprovalForm *form);
void approval_form_layout_begin(ApprovalForm *form);
// The card, boxed across `w`, with space under it; nothing when `approvals` is not shown (review_approvals_shown).
void approval_form_layout(ApprovalForm *form, Doc *doc, int x, int w, const ReviewApprovals *approvals);
// Handles one of the card's actions; false for any other.
bool approval_form_action(ApprovalForm *form, int action, intptr_t arg);
// Shows the note box where the card laid it, clipped to the view under a pinned header, or hides it.
void approval_form_place(ApprovalForm *form, const RECT *content, int scroll_y, bool shown);
// Follows the note box's text, so the form keeps it as typed.
void approval_form_command(ApprovalForm *form, HWND control, int code);
// What the write's answer amounts to: it landed (the form closes; fold the answer, review_outcome_source_after), it
// failed (its error shows on the card), or a refusal said the kept outcome is out of date (read it again; the error stays).
typedef enum { APPROVAL_LANDED, APPROVAL_FAILED, APPROVAL_STALE } ApprovalSent;
ApprovalSent approval_form_finish(ApprovalForm *form, const Request *req);
#endif
