#ifndef NETWORK_H_
#define NETWORK_H_

#include <glib.h>

#define PING_IPV4_ADDRESS	"208.67.222.222"

extern int count_network_interfaces();

extern struct addrinfo * get_remote_addr(char *host, char * port, int family, int socktype,
		int protocol, int resolve_timeout);

extern int connect_remote_with_timeouts(char *host, char *port, int family, int socktype, int protocol,
		int resolv_timeout, int connect_timeout, int send_timeout, int recv_timeout);

extern gboolean can_ping();
extern int ping (char *host);
extern gboolean guess_network_is_connecting();

extern int parse_http_url(char *url, char **host, char **port, char **path);

/* make sure enough space for error messages! */
#define HTTP_GET_RESULT_ERR_BUF_LEN	128

typedef struct __http_get_result_t
{
	int error_no;
	int http_code;
	int content_length;
	char content_type[32];
	char err_buf[HTTP_GET_RESULT_ERR_BUF_LEN];
} http_get_result_t;

typedef enum
{
	HTTP_GET_ERROR_NONE,
	HTTP_GET_ERROR_URL,
	HTTP_GET_ERROR_CONNECT,
	HTTP_GET_ERROR_READ_REMOTE,
	HTTP_GET_ERROR_WRITE_REMOTE,
	HTTP_GET_ERROR_NOT_200_OK,
	HTTP_GET_ERROR_WRITE_FILE,
} HTTP_GET_ERROR_NO_T;

#define HTTP_GET_ERROR_URL_ERR			"bad url format"
#define HTTP_GET_ERROR_CONNECT_ERR		"failed to connect remote host"
#define HTTP_GET_ERROR_READ_REMOTE_ERR	"failed to read from remote host"
#define HTTP_GET_ERROR_WRITE_REMOTE_ERR	"failed to send request to remote host"
#define HTTP_GET_ERROR_NOT_200_OK_ERR	"remote host returns HTTP code"
#define HTTP_GET_ERROR_WRITE_FILE_ERR	"failed to write data to local file"

extern void http_get(char *url, int fd, int con_timeout, int timeout, http_get_result_t *result);


#endif /* NETWORK_H_ */
