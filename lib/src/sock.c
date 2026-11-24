// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/sock.h>
#ifdef CHIAKI_PS5
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/socket.h>
#endif
#include <fcntl.h>

CHIAKI_EXPORT ChiakiErrorCode chiaki_socket_set_nonblock(chiaki_socket_t sock, bool nonblock)
{
#ifdef _WIN32
	u_long nbio = nonblock ? 1 : 0;
	if(ioctlsocket(sock, FIONBIO, &nbio) != NO_ERROR)
		return CHIAKI_ERR_UNKNOWN;
#else
#ifdef CHIAKI_PS5
	/* fcntl(F_SETFL) fails on sockets in a PS5 title; FIONBIO / SO_NBIO work. */
	int on = nonblock ? 1 : 0;
	if(ioctl(sock, FIONBIO, &on) == 0)
		return CHIAKI_ERR_SUCCESS;
	if(setsockopt(sock, SOL_SOCKET, 0x1200 /* SO_NBIO */, &on, sizeof(on)) == 0)
		return CHIAKI_ERR_SUCCESS;
#endif
	int flags = fcntl(sock, F_GETFL, 0);
	if(flags == -1)
		return CHIAKI_ERR_UNKNOWN;
	flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if(fcntl(sock, F_SETFL, flags) == -1)
		return CHIAKI_ERR_UNKNOWN;
#endif
	return CHIAKI_ERR_SUCCESS;
}