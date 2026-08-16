/*
 * Audi MHI2Q PPSSPP core: deliberately disconnected host-network backend.
 *
 * Keeping these ABI-compatible entry points lets PSP networking HLE modules
 * stay registered for games that import them, without linking QNX libsocket
 * or permitting an accidental host network connection. Every operation fails
 * immediately and predictably as "network down".
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;

static int qnx_offline_error(void) {
	errno = ENETDOWN;
	return -1;
}

int __cmsg_alignbytes(void) {
	return (int)sizeof(uint32_t) - 1;
}

int socket(int domain, int type, int protocol) {
	(void)domain; (void)type; (void)protocol;
	return qnx_offline_error();
}

int socketpair(int domain, int type, int protocol, int *sockets) {
	(void)domain; (void)type; (void)protocol; (void)sockets;
	return qnx_offline_error();
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
	(void)fd; (void)address; (void)length;
	return qnx_offline_error();
}

int bind(int fd, const struct sockaddr *address, socklen_t length) {
	(void)fd; (void)address; (void)length;
	return qnx_offline_error();
}

int listen(int fd, int backlog) {
	(void)fd; (void)backlog;
	return qnx_offline_error();
}

int accept(int fd, struct sockaddr *address, socklen_t *length) {
	(void)fd; (void)address; (void)length;
	return qnx_offline_error();
}

int shutdown(int fd, int how) {
	(void)fd; (void)how;
	return qnx_offline_error();
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
	(void)fd; (void)address; (void)length;
	return qnx_offline_error();
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
	(void)fd; (void)address; (void)length;
	return qnx_offline_error();
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
	(void)fd; (void)level; (void)option; (void)value; (void)length;
	return qnx_offline_error();
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
	(void)fd; (void)level; (void)option; (void)value; (void)length;
	return qnx_offline_error();
}

ssize_t recv(int fd, void *buffer, size_t length, int flags) {
	(void)fd; (void)buffer; (void)length; (void)flags;
	return (ssize_t)qnx_offline_error();
}

ssize_t recvfrom(int fd, void *buffer, size_t length, int flags,
		struct sockaddr *address, socklen_t *addressLength) {
	(void)fd; (void)buffer; (void)length; (void)flags;
	(void)address; (void)addressLength;
	return (ssize_t)qnx_offline_error();
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
	(void)fd; (void)message; (void)flags;
	return (ssize_t)qnx_offline_error();
}

ssize_t send(int fd, const void *buffer, size_t length, int flags) {
	(void)fd; (void)buffer; (void)length; (void)flags;
	return (ssize_t)qnx_offline_error();
}

ssize_t sendto(int fd, const void *buffer, size_t length, int flags,
		const struct sockaddr *address, socklen_t addressLength) {
	(void)fd; (void)buffer; (void)length; (void)flags;
	(void)address; (void)addressLength;
	return (ssize_t)qnx_offline_error();
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
	(void)fd; (void)message; (void)flags;
	return (ssize_t)qnx_offline_error();
}

int getaddrinfo(const char *node, const char *service,
		const struct addrinfo *hints, struct addrinfo **result) {
	(void)node; (void)service; (void)hints;
	if (result)
		*result = NULL;
	errno = ENETDOWN;
	return EAI_FAIL;
}

void freeaddrinfo(struct addrinfo *result) {
	(void)result;
}

const char *gai_strerror(int error) {
	(void)error;
	return "network unavailable in offline QNX build";
}
