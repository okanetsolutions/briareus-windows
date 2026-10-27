#ifndef BRIAREUS_SESSION_CACHE_H
#define BRIAREUS_SESSION_CACHE_H
#include "store.h"
// A snapshot contains only sessions belonging to the supplied accessible projects.
// Owned result; missing project lists are counted so partial snapshots stay explicit.
Session *session_cache_read(const Project *projects, size_t count, size_t *out_count, size_t *missing);
void session_cache_save(const Project *projects, size_t count, const Session *sessions, size_t session_count);
size_t status_waiting(const Project *projects, size_t count);
#endif
