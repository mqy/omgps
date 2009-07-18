#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <assert.h>
#include <glib.h>

#if (HAVE_SYS_CAPABILITY_H)
#undef _POSIX_SOURCE
#include <sys/capability.h>
#endif

#include "config.h"
#include "util.h"
#include "network.h"
#include "customized.h"

/**
 * Ping: reference <Unix network programming>, volume 1, third edition.
 */
static uint16_t in_checkksum(uint16_t *addr, int len)
{
	int nleft = len;
	uint32_t sum = 0;
	uint16_t *w = addr;
	uint16_t answer = 0;

	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}
	if (nleft == 1) {
		*(unsigned char *) (&answer) = *(unsigned char *) w;
		sum += answer;
	}
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	answer = ~sum;
	return (answer);
}

/**
 * FIXME: resolve_timeout
 */
struct addrinfo * get_remote_addr(char *host, char * port, int family, int socktype,
	int protocol, int resolve_timeout)
{
	struct addrinfo hints, *info = NULL;
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	hints.ai_protocol = protocol;
	hints.ai_flags = AI_CANONNAME;

	int ret = getaddrinfo(host, port, &hints, &info);
	if (ret != 0) {
		if (info) {
			freeaddrinfo(info);
			info = NULL;
		}
	}
	return info;
}

/**
 * return sock fd, < 0: error.
 * unit of timeouts: second.
 */
int connect_remote_with_timeouts(char *host, char *port, int family, int socktype, int protocol,
	int resolve_timeout, int connect_timeout, int send_timeout, int recv_timeout)
{
	struct timeval c_timeout, s_timeout, r_timeout;
	int sock_fd = -1, flags, ret = 0;

	struct addrinfo *rp, *addr_info;

	addr_info = get_remote_addr(host, port, family, socktype, protocol, resolve_timeout);
	if (addr_info == NULL)
		return -1;

	c_timeout.tv_sec = connect_timeout;
	c_timeout.tv_usec = 0;

	s_timeout.tv_sec = send_timeout;
	s_timeout.tv_usec = 0;

	r_timeout.tv_sec = recv_timeout;
	r_timeout.tv_usec = 0;

	for (rp = addr_info; rp != NULL; rp = rp->ai_next) {

		sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

		if (sock_fd == -1) {
			ret = -2;
			goto END;
		}

		flags = fcntl(sock_fd, F_GETFL, 0);
		if (flags < 0) {
			ret = -3;
			goto END;
		}

		if (fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
			ret = -4;
			goto END;
		}

		/* Ignore setsockopt() failures -- use default */
		setsockopt(sock_fd, IPPROTO_TCP, SO_SNDTIMEO, &s_timeout, sizeof(struct timeval));
		setsockopt(sock_fd, IPPROTO_TCP, SO_RCVTIMEO, &r_timeout, sizeof(struct timeval));

		ret = connect(sock_fd, addr_info->ai_addr, addr_info->ai_addrlen);

		if (ret == 0) {
			if (fcntl(sock_fd, F_SETFL, flags) < 0) {
				ret = -5;
				goto END;
			}
		} else if (ret < 0 && errno != EINPROGRESS) {
			ret = -6;
			goto END;
		}

		/* non-blocking listen */
		fd_set rs, ws, es;
		FD_ZERO(&rs);
		FD_SET(sock_fd, &rs);
		ws = es = rs;

		ret = select(sock_fd + 1, &rs, &ws, &es, &c_timeout);
		if (ret < 0) {
			ret = -7;
			goto END;
		} else if (0 == ret) {
			ret = -8;
			goto END;
		}

		if (!FD_ISSET(sock_fd, &rs) && !FD_ISSET(sock_fd, &ws)) {
			ret = -9;
			goto END;
		}

		int err;
		socklen_t len = sizeof(int);
		if (getsockopt(sock_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
			ret = -10;
			goto END;
		}

		if (err != 0) {
			ret = -11;
			goto END;
		}

		/* socket is ready for read/write, reset to blocking mode */
		if (fcntl(sock_fd, F_SETFL, flags) < 0) {
			ret = -12;
			goto END;
		}
	}

END:

	freeaddrinfo(addr_info);
	addr_info = NULL;

	if (ret < 0) {
		if (sock_fd > 0) {
			close(sock_fd);
			sock_fd = -1;
		}
		return ret;
	} else {
		return sock_fd;
	}
}

gboolean can_ping()
{
#if (HAVE_SYS_CAPABILITY_H)
	cap_flag_value_t cap = CAP_CLEAR;
	cap_t caps = cap_get_proc();
	if (caps == NULL)
		return 0;

	cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &cap);
	return (cap == CAP_CLEAR);
#else
	/* (1) root should have this capability
	 * (2) don't differentiate between getuid() with geteuid() */
	return (getuid() == 0);
#endif
}

/**
 * Need root privilege
 * @return:
 * > 0 -- ok,
 * < 0 -- failed.
 * Ping: reference <Unix network programming>, volume 1, third edition.
 */
int ping (char *host)
{
	#define BUF_SIZE	1500
	#define ICMP_REQUEST_DATA_LEN 56

	struct addrinfo *ai = get_remote_addr(host, NULL, AF_INET, SOCK_RAW, IPPROTO_ICMP, 5);
	if (ai == NULL)
		return -2;

	pid_t pid = getpid() & 0xffff; /* ICMP ID field is 16 bits */

	char send_buf[BUF_SIZE];
	char recv_buf[BUF_SIZE];
	char control_buf[BUF_SIZE];

	int sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);

	struct timeval timeout = {5, 0};
	setsockopt(sockfd, IPPROTO_ICMP, SO_SNDTIMEO, &timeout, sizeof(struct timeval));
	setsockopt(sockfd, IPPROTO_ICMP, SO_RCVTIMEO, &timeout, sizeof(struct timeval));

	/* don't need special permissions any more */
	setuid(getuid());

	struct icmp *icmp;
	icmp = (struct icmp *) send_buf;
	icmp->icmp_type = ICMP_ECHO;
	icmp->icmp_code = 0;
	icmp->icmp_id = pid;
	icmp->icmp_seq = 0;
	memset(icmp->icmp_data, 0xa5, ICMP_REQUEST_DATA_LEN);
	gettimeofday((struct timeval *) icmp->icmp_data, NULL);
	int len = 8 + ICMP_REQUEST_DATA_LEN;
	icmp->icmp_cksum = 0;
	icmp->icmp_cksum = in_checkksum((u_short *) icmp, len);

	int ret = -1;

	if (sendto(sockfd, send_buf, len, 0, ai->ai_addr, ai->ai_addrlen) <= 0) {
		ret = -4;
	} else {
		struct msghdr msg;
		struct iovec iov;
		iov.iov_base = recv_buf;
		iov.iov_len = sizeof(recv_buf);
		msg.msg_name = (char *)calloc(1, ai->ai_addrlen);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control_buf;
		msg.msg_namelen = ai->ai_addrlen;
		msg.msg_controllen = sizeof(control_buf);

		if ((len = recvmsg(sockfd, &msg, 0)) <= 0) {
			ret = -5;
		} else {
			struct ip *ip = (struct ip *) recv_buf;
			int header_len = ip->ip_hl << 2;

			struct icmp *icmp = (struct icmp *) (recv_buf + header_len);
			int icmp_payload_len = len - header_len;
			if ((ip->ip_p == IPPROTO_ICMP) && (icmp_payload_len >= 16) &&
				(icmp->icmp_type == ICMP_ECHOREPLY) && (icmp->icmp_id == pid)) {
				ret = 1;
			} else {
				ret = -6;
			}
		}

		free(msg.msg_name);
	}

	free(ai);
	close(sockfd);

	return ret;
}

/**
 * return TRUE if the count of network interfaces that are (non-loopback and up) > 0
 * Only count IPv4.
 */
int count_network_interfaces()
{
	struct ifaddrs *addrs = NULL, *i;
	if (getifaddrs(&addrs) == -1)
		return 0;

	int count = 0;
	for (i=addrs; i != NULL; i=i->ifa_next) {
		if (i->ifa_addr && (i->ifa_addr->sa_family == AF_INET)) {
			if (! (i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP))
				++count;
		}
	}
	freeifaddrs(addrs);
	return count;
}

/**
 * libcurl is not reliable under multi-threaded environment.
 * tile URLs should be HTTP protocol.
 *
 * http://host<:port>/path,  <url> is destroyed. path does not contain leading '/'
 */
int parse_http_url(char *url, char **host, char **port, char **path)
{
	if (! url || ! (url=trim(url)))
		return -1;

	char *phost, *pport, *ppath;

	char *p = strstr(url, "http://");
	if (p != url)
		return -2;

	phost = p + 7; // points to host
	if (*phost == '\0')
		return -3;

	p = strstr(phost, "/");
	if (! p)
		return -4; // need path

	ppath = p + 1; // points to path
	if (*ppath == '\0')
		return -5;

	*p = '\0'; // end host<:port>

	p = strstr(phost, ":");
	if (p) {
		pport = p + 1;
		*p = '\0'; // end host
		if (*pport == '\0')
			pport = "80";
		else {
			char c = *pport;
			for (; c; c = *(++pport)) {
				if (c <'0' || c > '9')
					return -6;
			}
		}
	} else {
		pport = "80";
	}

	*host = phost;
	*port = pport;
	*path = ppath;

	return 0;
}

static int read_http_header_line(int sock_fd, char *buf, int buflen)
{
	int len, i=0, state = 0;
	char c;

	while ((len = read(sock_fd, &c, 1)) >= 0) {
		if (state == 0) {
			if (c == '\r') {
				state = 1;
			} else {
				buf[i++] = c;
			}
		} else if (state == 1) {
			/* some site (e.g, yahoo map) returns bad header line: \r\r\n
			 * Even wireshark unable to recognize it correctly */
			if (c == '\r')
				;
			else if (c == '\n')
				break;
			else
				return -1;
		}
		if (i == buflen)
			return -2;
	}

	if (len < 0)
		return -3;

	buf[i] = '\0';

	return i;
}

static int write_fd(int fd, char *buf, int buf_len)
{
	int written = 0;
	int len;

	while((len = write(fd, &buf[written], buf_len - written)) > 0) {
		written += len;
		if (written == buf_len)
			break;
	}

	if (len < 0)
		return len;

	return 0;
}

/**
 * @ref: http://www.w3.org/Protocols/rfc2616/rfc2616.html
 * NOTE: just a simple implementation for downloading images
 * don't support (1) HTTPS (2) FTP (3) proxy
 */
void http_get(char *url, int fd, int con_timeout, int timeout, http_get_result_t *result)
{
	char *_url = strdup(url);

	/* no error */
	result->error_no = HTTP_GET_ERROR_NONE;
	result->content_length = 0;
	result->content_type[0] = '\0';
	result->http_code = 0;

	char *host, *port, *path;
	int sock_fd = 0;

	if (parse_http_url(_url, &host, &port, &path) < 0) {
		result->error_no = HTTP_GET_ERROR_URL;
		goto END;
	}

	sock_fd = connect_remote_with_timeouts(host, port, AF_UNSPEC, SOCK_STREAM, 0,
			10, con_timeout, timeout, timeout);

	if (sock_fd <= 0) {
		result->error_no = HTTP_GET_ERROR_CONNECT;
		goto END;
	}

	char buf[1024];
	snprintf(buf, sizeof(buf), "GET /%s HTTP/1.1\r\n"
		"User-Agent: %s\r\n"
		"Host: %s\r\n"
		"Accept: */*\r\n"
		"\r\n", path, "omgps", host);

	/* send request */
	if (write_fd(sock_fd, buf, strlen(buf)) < 0) {
		close(sock_fd);
		result->error_no = HTTP_GET_ERROR_WRITE_REMOTE;
		goto END;
	}

	char header_buf[256];

	/* status line */
	int ret = read_http_header_line(sock_fd, header_buf, sizeof(header_buf));
	if (ret <= 0) {
		result->error_no = HTTP_GET_ERROR_READ_REMOTE;
		goto END;
	}

	//log_debug("status line=%s\n", header_buf);

	char http_version[32], reason_phrase[64];
	int code;
	if (sscanf(header_buf, "%s %d %s", http_version, &code, reason_phrase) != 3) {
		result->error_no = HTTP_GET_ERROR_READ_REMOTE;
		goto END;
	}

	result->http_code = code;

	if (code != 200) {
		result->error_no = HTTP_GET_ERROR_NOT_200_OK;
		goto END;
	}

	int content_length = 0;
	char *content_type = NULL;

	char *p;

	#define CL "Content-Length:"
	#define CT "Content-Type:"
	#define CL_LEN 15
	#define CT_LEN 13

	/* read other headers, until <= 0 */
	while ((ret = read_http_header_line(sock_fd, header_buf, sizeof(header_buf))) > 0) {
		if ((p = strstr(header_buf, CL)) != NULL) {
			sscanf(&header_buf[CL_LEN], "%d", &content_length);
		} else if ((p = strstr(header_buf, CT)) != NULL) {
			content_type = trim(strdup(&header_buf[CT_LEN]));
		}
	}

	/* bad http header */
	if (ret < 0 || content_length <= 0 || ! content_type)  {
		result->error_no = HTTP_GET_ERROR_READ_REMOTE;
		goto END;
	} else {
		/* ret == 0: last header line: empty line with \r\n */
	}

	result->content_length = content_length;
	strcpy(result->content_type, content_type);

	int len, total = 0;

	/* read real data */
	while ((len = read(sock_fd, buf, sizeof(buf))) > 0) {
		total += len;
		if (total > content_length) {
			result->error_no = HTTP_GET_ERROR_READ_REMOTE;
			goto END;
		}

		if (write_fd(fd, buf, len) < 0) {
			result->error_no = HTTP_GET_ERROR_WRITE_FILE;
			goto END;
		}
		if (total == content_length)
			break;
	}

	if (len < 0) {
		result->error_no = HTTP_GET_ERROR_READ_REMOTE;
		goto END;
	}

END:

	free(_url);

	switch(result->error_no) {
	case HTTP_GET_ERROR_URL:
		strcpy(result->err_buf, HTTP_GET_ERROR_URL_ERR);
		break;
	case HTTP_GET_ERROR_CONNECT:
		strcpy(result->err_buf, HTTP_GET_ERROR_CONNECT_ERR);
		break;
	case HTTP_GET_ERROR_READ_REMOTE:
		strcpy(result->err_buf, HTTP_GET_ERROR_READ_REMOTE_ERR);
		break;
	case HTTP_GET_ERROR_WRITE_REMOTE:
		strcpy(result->err_buf, HTTP_GET_ERROR_WRITE_REMOTE_ERR);
		break;
	case HTTP_GET_ERROR_NOT_200_OK:
		sprintf(result->err_buf, "%s: %d", HTTP_GET_ERROR_NOT_200_OK_ERR, result->http_code);
		break;
	case HTTP_GET_ERROR_WRITE_FILE:
		strcpy(result->err_buf, HTTP_GET_ERROR_WRITE_FILE_ERR);
		break;
	default:
		break;
	}

	if (sock_fd > 0)
		close(sock_fd);
}

gboolean guess_network_is_connecting()
{
	gboolean failed = FALSE;

	if (count_network_interfaces() >= 0) {
		if (can_ping() && ping(PING_IPV4_ADDRESS) <= 0)
			failed = TRUE;
	} else {
		failed = TRUE;
	}
	return failed;
}
