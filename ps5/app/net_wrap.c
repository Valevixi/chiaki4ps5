/* The PS5 title runtime's getaddrinfo fails even for numeric addresses.
   chiaki only ever passes IP literals here (the config holds the PS5's IP),
   so numeric IPv4 is resolved locally; anything else goes to the real one. */
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define LOCAL_AI_MARK 0x40000000


/* inet_pton is broken in the title runtime too, so parse dotted IPv4 here. */
static int parse_ipv4(const char *s, struct in_addr *out)
{
	unsigned long parts[4];
	int n = 0;
	while(n < 4)
	{
		if(*s < '0' || *s > '9') return 0;
		unsigned long v = 0;
		int digits = 0;
		while(*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long)(*s - '0'); s++; if(++digits > 3) return 0; }
		if(v > 255) return 0;
		parts[n++] = v;
		if(n < 4) { if(*s != '.') return 0; s++; }
	}
	if(*s != '\0') return 0;
	unsigned char *b = (unsigned char *)&out->s_addr;
	for(int i = 0; i < 4; i++) b[i] = (unsigned char)parts[i];
	return 1;
}

int __wrap_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	struct in_addr a;
	if(node && parse_ipv4(node, &a) && (!hints || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET))
	{
		struct { struct addrinfo ai; struct sockaddr_in sin; } *blk = calloc(1, sizeof(*blk));
		if(!blk) return EAI_MEMORY;
		blk->sin.sin_len = sizeof(blk->sin);
		blk->sin.sin_family = AF_INET;
		blk->sin.sin_addr = a;
		blk->sin.sin_port = service ? htons((unsigned short)atoi(service)) : 0;
		blk->ai.ai_flags = LOCAL_AI_MARK;
		blk->ai.ai_family = AF_INET;
		blk->ai.ai_socktype = hints ? hints->ai_socktype : 0;
		blk->ai.ai_protocol = hints ? hints->ai_protocol : 0;
		blk->ai.ai_addrlen = sizeof(blk->sin);
		blk->ai.ai_addr = (struct sockaddr *)&blk->sin;
		*res = &blk->ai;
		return 0;
	}
	(void)service; (void)hints;
	if(res) *res = NULL;
	return EAI_FAIL; /* no DNS in the title runtime; chiaki only needs IP literals */
}

void __wrap_freeaddrinfo(struct addrinfo *ai)
{
	if(ai && (ai->ai_flags & LOCAL_AI_MARK)) { free(ai); return; }
	(void)ai;
}

/* getnameinfo is a null import in a PS5 title (calling it jumps to 0).
   chiaki only asks for NI_NUMERICHOST text, which we format ourselves. */
__attribute__((visibility("hidden"))) int __ps5app_getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host, size_t hostlen,
	char *serv, size_t servlen, int flags)
{
	(void)salen; (void)flags;
	if(!sa) return EAI_FAIL;
	if(host && hostlen)
	{
		host[0] = '\0';
		if(sa->sa_family == AF_INET)
		{
			const unsigned char *b = (const unsigned char *)&((const struct sockaddr_in *)sa)->sin_addr;
			char tmp[16]; int n = 0;
			for(int i = 0; i < 4; i++)
			{
				unsigned v = b[i];
				if(v >= 100) tmp[n++] = (char)('0' + v / 100);
				if(v >= 10) tmp[n++] = (char)('0' + (v / 10) % 10);
				tmp[n++] = (char)('0' + v % 10);
				if(i < 3) tmp[n++] = '.';
			}
			tmp[n] = '\0';
			if((size_t)n + 1 > hostlen) return EAI_OVERFLOW;
			memcpy(host, tmp, (size_t)n + 1);
		}
		else return EAI_FAMILY;
	}
	if(serv && servlen)
	{
		unsigned port = 0;
		if(sa->sa_family == AF_INET) port = ntohs(((const struct sockaddr_in *)sa)->sin_port);
		char tmp[8]; int n = 0; char rev[8]; int r = 0;
		do { rev[r++] = (char)('0' + port % 10); port /= 10; } while(port && r < 7);
		while(r) tmp[n++] = rev[--r];
		tmp[n] = '\0';
		if((size_t)n + 1 > servlen) return EAI_OVERFLOW;
		memcpy(serv, tmp, (size_t)n + 1);
	}
	return 0;
}
