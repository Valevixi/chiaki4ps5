/* Symbols the PacBrew-built libraries expect from the PayloadSDK libc that the
   title runtime does not provide. None of these paths run in chiaki-ps5. */
#include <errno.h>
#include <stdlib.h>
struct mmsghdr;
int ___mb_cur_max(void) { return 1; }
long sendmmsg(int s, struct mmsghdr *m, unsigned int n, int f) { (void)s; (void)m; (void)n; (void)f; errno = ENOSYS; return -1; }
long recvmmsg(int s, struct mmsghdr *m, unsigned int n, int f, void *t) { (void)s; (void)m; (void)n; (void)f; (void)t; errno = ENOSYS; return -1; }
/* OpenSSL async fibres only (never used). */
int _setjmp(void *env) { (void)env; return 0; }
void _longjmp(void *env, int v) { (void)env; (void)v; abort(); }
