#pragma once
/* PS5 (FreeBSD-based) compatibility shims for chiaki-ng lib */
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define _alloca __builtin_alloca
/* TODO: honour the timeout (pthread_timedjoin_np) once verified on console */
static inline int pthread_clockjoin_np(pthread_t t, void **ret, clockid_t c, const struct timespec *ts)
{ (void)c; (void)ts; return pthread_join(t, ret); }
