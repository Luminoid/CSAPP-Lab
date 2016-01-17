/* proxy.c - CS:APP Web proxy
 *
 * TEAM MEMBERS:
 * 
 * Step 1: Use proxy as a server to the web browser.
 * Step 2: Accept the request from the client.
 * Step 3: Send the request to the end server as a client.
 * Step 4: Receive the result from the server and send it back to the browser.
 * Step 5: Output log information.
 *
 */

#include "csapp.h"

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, int  *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size);
void proxy_server(int port);
void doit(int connfd, struct sockaddr_in *clientaddr);
void proxy_client(char *host, int port, int connfd, rio_t rio1, char buf[], struct sockaddr_in *clientaddr, char uri[]);
void *thread(void *vargp);

void Rio_writen_w(int fd, void *usrbuf, size_t n);
ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n);
ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen);
int open_clientfd_ts(char *hostname, int port);

/*
 * Global Constant
 */
static const char *ERROR_MSG = "HTTP/1.1 502 Bad Gateway\r\n";
static sem_t mutex1; // protect log function
static sem_t mutex2; // protect gethostbyname

/* Used for passing parameters to threads */
struct threadstr{
    int connfd;
    struct sockaddr_in clientaddr;
};

/* main - Main routine for the proxy program */
int main(int argc, char **argv) {
    printf("Proxy start!\n");
    /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }
    signal(SIGPIPE, SIG_IGN);
    Sem_init(&mutex1, 0, 1);
    Sem_init(&mutex2, 0, 1);
    int port = atoi(argv[1]);
    proxy_server(port); // Step 1: Use proxy as a server to the web browser

    exit(0);
}

/* An concurrent server */
void proxy_server(int port) {
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_in clientaddr;  /* Enough space for any address */
    struct threadstr *threadstr1;

    pthread_t tid;

    listenfd = open_listenfd(port);

    while (1) {
        clientlen = sizeof(clientaddr);

        threadstr1 = Malloc(sizeof(struct threadstr));
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen); // Step 2: Accept the request from the client.
        threadstr1->connfd = connfd;
        threadstr1->clientaddr = clientaddr;
        printf("proxy_server\n");

        Pthread_create(&tid, NULL, thread, (void *)threadstr1);
    }
}

/* Thread routine */
void *thread(void *vargp){
    struct threadstr threadstr1 = *((struct threadstr *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(threadstr1.connfd, &(threadstr1.clientaddr));
    close(threadstr1.connfd);
    return NULL;
}

/* Parse URI */
void doit(int connfd, struct sockaddr_in *clientaddr){
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE];
    char pathname[MAXLINE];
    int port;
    rio_t rio;
    /* Read request line and headers */
    Rio_readinitb(&rio, connfd);
    Rio_readlineb_w(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s %s", method, uri, version);
    if(strcmp(method, "GET") != 0){
        Rio_writen_w(connfd, (void *) ERROR_MSG, strlen(ERROR_MSG));
        printf("doit: method is not GET!\n");
        return;
    }

    if(parse_uri(uri, hostname, pathname, &port)==-1){
        printf("doit: parse_uri error!\n");
        return;
    }

    printf("Request hostname: %s, pathname: %s, port: %d\n", hostname, pathname, port);
    proxy_client(hostname, port, connfd, rio, buf, clientaddr, uri); // Step 3: Use proxy as a client to the end server.
}

/*
 * After establishing a connection with the end server,
 * the proxy sends the whole request to the server (Connection: close),
 * and then enters a loop that repeatedly reads a text line from the end server,
 * and send it back to the browser.
 */
void proxy_client(char *host, int port, int connfd, rio_t rio1, char buf[], struct sockaddr_in *clientaddr, char uri[]) {
    int clientfd;
    int size = 0; // the size of the content that server returns
    ssize_t linesize;
    char buf1[MAXLINE];
    char buf2[MAXLINE];
    char requestblock[MAXLINE];
    char logstring[MAXLINE];
    FILE *file;
    rio_t rio2;

    printf("proxy_client\n");
    requestblock[0] = '\0'; // initialization
    strcat(requestblock, buf);
    Rio_readlineb_w(&rio1, buf1, MAXLINE);
    while (strcmp(buf1, "\r\n")) {
        if(strstr(buf1, "Connection:") == NULL && strstr(buf1, "Proxy-Connection:") == NULL ){
            strcat(requestblock, buf1);
        }
        Rio_readlineb_w(&rio1, buf1, MAXLINE);
    }

    strcat(requestblock, "Connection: close\r\nProxy-Connection: close\r\n\r\n");
    // strcat(requestblock, "\r\n");

    // printf("%s", requestblock);

    // Step 4: Receive the result from the server and send it back to the browser.
    if((clientfd = open_clientfd_ts(host, port))<0){
        printf("proxy_client: open_client error!");
        // return -1;
    }

    // send the request to the end server
    Rio_writen_w(clientfd, requestblock, strlen(requestblock));

    // response
    Rio_readinitb(&rio2, clientfd);
    // response line
    linesize = Rio_readlineb_w(&rio2, buf2, MAXLINE);
    size += linesize;
    // response header
    while (strcmp(buf2, "\r\n") && linesize != 0){
        // printf("%s", buf2);
        Rio_writen_w(connfd, buf2, strlen(buf2));
        linesize = Rio_readlineb_w(&rio2, buf2, MAXLINE);
        size += linesize;
    }
    Rio_writen_w(connfd, "\r\n", 2);
    // response body
    while ((linesize = Rio_readnb_w(&rio2, buf2, MAXLINE)) != 0){
        size += linesize;
        Rio_writen_w(connfd, buf2, linesize);
    }

    close(clientfd);

    // Step 5: Output log information
    P(&mutex1);
    format_log_entry(logstring, clientaddr, uri, size);
    file = fopen("./proxy.log", "a");
    fputs(logstring, file);
    fputs("\r\n", file);
    fclose(file);
    V(&mutex1);
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port) {
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    *port = 80; /* default */
    if (*hostend == ':')
        *port = atoi(hostend + 1);

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    }
    else {
        pathbegin++;
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size) {
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 13, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %d", time_str, a, b, c, d, uri, size);
}

/**********************************
 * Wrappers for robust I/O routines
  **********************************/
void Rio_writen_w(int fd, void *usrbuf, size_t n)
{
    if (rio_writen(fd, usrbuf, n) != n){
        printf("Rio_writen error\n");
        return;
    }
}

ssize_t Rio_readnb_w(rio_t *rp, void *usrbuf, size_t n)
{
    ssize_t rc;

    if ((rc = rio_readnb(rp, usrbuf, n)) < 0){
        printf("Rio_readnb error\n");
        return 0;
    }
    return rc;
}

ssize_t Rio_readlineb_w(rio_t *rp, void *usrbuf, size_t maxlen)
{
    ssize_t rc;
    
    if ((rc = rio_readlineb(rp, usrbuf, maxlen)) < 0){
        printf("Rio_readlineb error\n");
        return 0;
    }
    return rc;
}

int open_clientfd_ts(char *hostname, int port)
{
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1; /* check errno for cause of error */

    /* Fill in the server's IP address and port */
    P(&mutex2);
    if ((hp = gethostbyname(hostname)) == NULL)
        return -2; /* check h_errno for cause of error */
    V(&mutex2);
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0],
          (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
        return -1;
    return clientfd;
}
