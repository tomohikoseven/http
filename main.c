#include<stdio.h>
#include<stdarg.h>
#include<stdlib.h>
#include<signal.h>
#include<errno.h>
#include<string.h>
#include<ctype.h>
#include<sys/stat.h>
#include<time.h>
#include<sys/types.h>
#include<fcntl.h>
#include<unistd.h>

/* define */
#define SERVER_NAME "LittleHTTP"
#define SERVER_VERSION "1.0"
#define HTTP_MINOR_VERSION 0
#define BLOCK_BUF_SIZE 1024
#define LINE_BUF_SIZE 4096
#define MAX_REQUEST_BODY_LENGTH (1024 * 1024)
#define TIME_BUF_SIZE 64

#ifdef _DEBUG
#define dbg( fmt, ... ) printf( "[DEBUG] Line:%d %s() " fmt, __LINE__, __func__, __VA_ARGS__ );
#else
#define dbg( fmt, ... )
#endif

/* Data structure */
struct HTTPHeaderField
{
    char    *name;
    char    *value;
    struct HTTPHeaderField *next;
};

struct HTTPRequest
{
    int     protocol_minor_version;
    char    *method;
    char    *path;
    struct HTTPHeaderField  *header;
    char    *body;
    long    length;
};

struct FileInfo
{
    char *path;     /* file path */
    long size;      /* file size */
    int  ok;        /* Do exists file? yes:not 0, no:0 */
};

static void     log_exit( char *fmt, ... );
static void*    xmalloc( size_t sz );
static void     signal_exit( int sig );
static void     trap_signal( int sig, sighandler_t handler );
static void     install_signal_handlers( void );
static void     usage( int argc, char *argv[] );
static void     chk_doc_root( char *path );
static void     free_request( struct HTTPRequest *req );
static void     upcase( char *p );
static void     read_request_line( struct HTTPRequest *req, FILE *in );
static struct HTTPRequest* read_request( FILE *in );
static void     respond_to( struct HTTPRequest  *req, FILE *out, char *docroot );
static long     content_length( struct HTTPRequest *req );
static char*    lookup_header_field_value( struct HTTPRequest *req, char *name );
static void     not_found(struct HTTPRequest *req, FILE *out);
static char*    guess_content_type(struct FileInfo *info);
static void     method_not_allowed(struct HTTPRequest *req, FILE *out);
static void     not_implemented(struct HTTPRequest *req, FILE *out);
static char*    getRequestMethod( char **method, char *buf );

static void
install_signal_handlers
(
    void
)
{
    trap_signal( SIGPIPE, signal_exit );
}

static void
trap_signal
(
    int sig,
    sighandler_t    handler
)
{
    struct sigaction act;
    int ret = 0;

    memset( &act, 0x00, sizeof( act ) );

    act.sa_handler = handler;
    act.sa_flags   = SA_RESTART;
    sigemptyset( &act.sa_mask );

    ret = sigaction( sig, &act, NULL );
    if( ret < 0 )
    {
        log_exit( "sigaction() failed: %s", strerror( errno ) );
    }
}

static void
signal_exit
(
    int sig
)
{
    log_exit( "exit by signal %d", sig );
}

static void*
xmalloc
(
    size_t sz
)
{
    void    *p = NULL;

    dbg( "sz=%d\n", sz );

    p = malloc( sz );
    if( NULL == p )
    {
        log_exit( "failed to allocate memory" );
    }
    memset( p, 0x00, sz );

    return p;
}

static void
log_exit
(
    char *fmt,
    ...
)
{
    va_list ap;

    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    fputc( '\n', stderr );
    va_end( ap );
    exit(1);
}

static void
usage
(
    int argc,
    char *argv[]
)
{
    dbg( "argc=%d, argv=%p\n", argc, argv );

    if( 2 != argc )
    {
        fprintf( stderr, "Usage: %s <docroot>\n", argv[0] );
        exit( 1 );
    }
}

static void
chk_doc_root
(
    char *path
)
{
    struct stat buf;
    int ret = 0;

    dbg( "path=%s\n", path );

    memset( &buf, 0x00, sizeof( buf ) );

    /* Get file status */
    ret = stat( path, &buf );
    if( 0 != ret )
    {
        perror("stat");
        exit( 1 );
    }

    /* Directory check */
    if( !S_ISDIR(buf.st_mode) )
    {
        fprintf( stderr, "%s not document root!\n", path );
        exit( 1 );
    }
}

static void
upcase
(
    char *p
)
{
    int size = 0;
    int i    = 0;

    dbg( "p=%s\n", p );

    size = strlen( p );
    for( i = 0; i < size; i++ )
    {
        p[i] = toupper( p[i] );
    }
}

static struct HTTPHeaderField*
read_header_field
(
    FILE    *in
)
{
    struct HTTPHeaderField  *h = NULL;
    char    buf[LINE_BUF_SIZE];
    char    *p = NULL;

    dbg( "in=%p\n", in );

    memset( buf, 0x00, sizeof( buf ) );

    if( !fgets(buf, LINE_BUF_SIZE, in ) )
    {
        log_exit( "failed to read request header field: %s",
                    strerror( errno ) );
    }
    if( buf[0] == '\n' || (strcmp( buf, "\r\n") == 0) )
    {
        return NULL;
    }

    p = strchr( buf, ':' );
    if( !p )
    {
        log_exit( "parse error on request header field: %s", buf );
    }
    *p++ = '\0';
    h = xmalloc( sizeof( struct HTTPHeaderField) );
    h->name = xmalloc( p - buf );
    strcpy( h->name, buf );

    p += strspn( p, " \t" );
    h->value = xmalloc( strlen(p) + 1 );
    strcpy( h->value, p );

    return h;
}

static char*
lookup_header_field_value
(
    struct HTTPRequest *req,
    char *name
)
{
    struct HTTPHeaderField *h = NULL;

    dbg( "req=%p, name=%p\n", req, name );

    for( h = req->header; h; h = h->next )
    {
        if( strcasecmp( h->name, name ) == 0 )
        {
            return h->value;
        }
    }

    return NULL;
}

static long
content_length
(
    struct HTTPRequest *req
)
{
    char *val = NULL;
    long len  = 0L;

    dbg( "req=%p\n", req );

    val = lookup_header_field_value( req, "Content-Length" );
    if( NULL == val )
    {
        return 0;
    }

    len = atol( val );
    if( len < 0 )
    {
        log_exit( "negative Content-Length value" );
    }

    return len;
}

static struct HTTPRequest*
read_request
(
    FILE *in
)
{
    struct HTTPRequest     *req = NULL;
    struct HTTPHeaderField *h   = NULL;

    dbg( "in=%p\n", in );

    req = xmalloc( sizeof( struct HTTPRequest ) );
    read_request_line( req, in );
    req->header = NULL;

    while( ( h = read_header_field( in ) ) != NULL )
    {
        h->next = req->header;
        req->header = h;
    }

    req->length = content_length( req );
    if( req->length != 0 )
    {
        if( req->length > MAX_REQUEST_BODY_LENGTH )
        {
            log_exit( "request body too long" );
        }
        req->body = xmalloc( req->length );
        if( fread( req->body, req->length, 1, in ) < 1 )
        {
            log_exit( "failed to read request body" );
        }
    }
    else
    {
        req->body = NULL;
    }

    return req;
}

static inline void
free_fileinfo
(
    void *info
)
{
    dbg( "info=%p\n", info );
    free( info );
}

static void
output_common_header_fields
(
    struct HTTPRequest *req,
    FILE *out,
    char *status                /* response status */
)
{
    time_t      t;
    struct tm   *tm = NULL;
    char        buf[TIME_BUF_SIZE];

    dbg( "req=%p, out=%p, status=%p\n", req, out, status );

    memset( &t,   0x00, sizeof( t )   );
    memset( &buf, 0x00, sizeof( buf ) );

    t  = time(NULL);
    tm = gmtime( &t );
    if( tm == NULL )
    {
        log_exit( "gmtime() failed: %s", strerror( errno ) );
    }

    strftime( buf, TIME_BUF_SIZE, "%a, %d %b %Y %H:%M:%S GMT", tm   );

    fprintf(  out, "HTTP/1.%d %s\r\n", HTTP_MINOR_VERSION, status   );
    fprintf(  out, "Date: %s\r\n", buf                              );
    fprintf(  out, "Server: %s/%s\r\n", SERVER_NAME, SERVER_VERSION );
    fprintf(  out, "Connection: close\r\n"                          );
}

static char *
build_fspath
(
    char *docroot,
    char *urlpath
)
{
    char *path;

    dbg( "docroot=%p, urlpath=%p\n", docroot, urlpath );

    path = xmalloc(strlen(docroot) + 1 + strlen(urlpath) + 1);
    sprintf(path, "%s/%s", docroot, urlpath);
    return path;
}

static struct FileInfo*
get_fileinfo
(
    char *docroot,
    char *urlpath
)
{
    struct FileInfo *info = NULL;
    struct stat st;

    dbg( "docroot=%p, urlpath=%p\n", docroot, urlpath );

    memset( &st, 0x00, sizeof( st ) );

    info       = xmalloc( sizeof( struct FileInfo ) );
    info->path = build_fspath( docroot, urlpath );
    info->ok   = 0;
    if ( lstat(info->path, &st) < 0 )
    {
        /* not exists file */
        return info;
    }
    if ( !S_ISREG( st.st_mode ) )
    {
        /* not exists file */
        return info;
    }

    /* exists file */
    info->ok   = 1;
    info->size = st.st_size;

    return info;
}

static void
not_found(struct HTTPRequest *req, FILE *out)
{
    dbg( "req=%p, out=%p\n", req, out );

    output_common_header_fields( req, out, "404 Not Found" );

    fprintf( out, "Content-Type: text/html\r\n" );
    fprintf( out, "\r\n"                        );

    if( 0 != strcmp( req->method, "HEAD" ) )
    {
        fprintf( out, "<html>\r\n"                                   );
        fprintf( out, "<header><title>Not Found</title><header>\r\n" );
        fprintf( out, "<body><p>File not found</p></body>\r\n"       );
        fprintf( out, "</html>\r\n"                                  );
    }
    fflush(out);
}

static char*
guess_content_type(struct FileInfo *info)
{
    dbg( "info=%p\n", info );
    return "text/plain";   /* FIXME */
}

static int
openBody
(
    char *path
)
{
    int fd = 0;

    fd = open( path, O_RDONLY );
    if( fd < 0 )
    {
        log_exit( "failed to open %s: %s", path, strerror( errno ) );
    }

    return fd;
}

static int
readBody
(
    int fd,
    char *buf,
    char *path
)
{
    ssize_t n;

    n = read( fd, buf, BLOCK_BUF_SIZE );
    if( n < 0 )
    {
        log_exit( "failed to read %s: %s", path, strerror( errno ) );
    }

    return n;
}

static void
writeBody
(
    char *buf,
    ssize_t n,
    FILE *out
)
{
    ssize_t m;

    memset( &m, 0x00, sizeof( m ) );

    m = (int)fwrite( buf, n, 1, out );
    if( 1 != m )
    {
        log_exit( "failed to write to socket: %s", strerror( errno ) );
    }
}

static void
readWriteBody
(
    int  fd,    /* read fd   */
    FILE *out,  /* output fd */
    char *path  /* body path */
)
{
    char    buf[BLOCK_BUF_SIZE];
    ssize_t n;

    memset( buf, 0x00, sizeof( buf ) );
    memset( &n,  0x00, sizeof( n )   );

    for( ;; )
    {
        n = readBody( fd, buf, path );
        if( n == 0 )
        {
            break;
        }

        writeBody( buf, n, out );
    }
}

static void
outputBodyFields
(
    struct FileInfo *info,      /* File Info    */
    struct HTTPRequest *req,    /* HTTP request */
    FILE *out                   /* output fd    */
)
{
    int     fd = 0;

    if( 0 != strcmp( req->method, "HEAD" ) )
    {
        fd = openBody( info->path );

        readWriteBody( fd, out, info->path );

        close( fd );
    }
}

static void
do_file_response
(
    struct HTTPRequest *req,    /* HTTP request */
    FILE *out,                  /* output fd    */
    char *docroot               /* docroot path */
)
{
    struct FileInfo *info = NULL;

    dbg( "req=%p, out=%p, docroot=%p\n", req, out, docroot );

    info = get_fileinfo( docroot, req->path );
    if( 0 == info->ok )
    {
        free_fileinfo( info );
        not_found( req, out );
        return ;
    }

    output_common_header_fields( req, out, "200 OK" );

    fprintf( out, "Content-Length: %ld\r\n", info->size              );
    fprintf( out, "Content-Type: %s\r\n", guess_content_type( info ) );
    fprintf( out, "\r\n"                                             );

    outputBodyFields( info, req, out );

    fflush( out );
    free_fileinfo( info );
}

static void
method_not_allowed(struct HTTPRequest *req, FILE *out)
{
    dbg( "req=%p, out=%p\n", req, out );

    output_common_header_fields( req, out, "405 Method Not Allowed" );

    fprintf( out, "Content-Type: text/html\r\n"                     );
    fprintf( out, "\r\n"                                            );
    fprintf( out, "<html>\r\n"                                      );
    fprintf( out, "<header>\r\n"                                    );
    fprintf( out, "<title>405 Method Not Allowed</title>\r\n"       );
    fprintf( out, "<header>\r\n"                                    );
    fprintf( out, "<body>\r\n"                                      );
    fprintf( out, "<p>The request method %s is not allowed</p>\r\n",
                    req->method                                     );
    fprintf( out, "</body>\r\n"                                     );
    fprintf( out, "</html>\r\n"                                     );

    fflush( out );
}

static void
not_implemented(struct HTTPRequest *req, FILE *out)
{
    dbg( "req=%p, out=%p\n", req, out );

    output_common_header_fields(req, out, "501 Not Implemented");

    fprintf( out, "Content-Type: text/html\r\n"                         );
    fprintf( out, "\r\n"                                                );
    fprintf( out, "<html>\r\n"                                          );
    fprintf( out, "<header>\r\n"                                        );
    fprintf( out, "<title>501 Not Implemented</title>\r\n"              );
    fprintf( out, "<header>\r\n"                                        );
    fprintf( out, "<body>\r\n"                                          );
    fprintf( out, "<p>The request method %s is not implemented</p>\r\n",
                    req->method                                         );
    fprintf( out, "</body>\r\n"                                         );
    fprintf( out, "</html>\r\n"                                         );

    fflush(out);
}

static void
respond_to
(
    struct HTTPRequest  *req,   /* HTTP request */
    FILE *out,                  /* output fd    */
    char *docroot               /* docroot path */
)
{
    dbg( "req=%p, out=%p, docroot=%p\n", req, out, docroot );

    if     ( strcmp( req->method, "GET"  ) == 0 )
    {
        do_file_response( req, out, docroot );
    }
    else if( strcmp( req->method, "HEAD" ) == 0 )
    {
        do_file_response( req, out, docroot );
    }
    else if( strcmp( req->method, "POST" ) == 0 )
    {
        method_not_allowed( req, out );
    }
    else
    {
        not_implemented( req, out );
    }
}

static void
service
(
    FILE    *in,        /* input fd     */
    FILE    *out,       /* output fd    */
    char    *docroot    /* docroot path */
)
{
    struct HTTPRequest  *req = NULL;

    dbg( "in=%p, out=%p, docroot=%p\n", in, out, docroot );

    req = read_request( in );

    respond_to( req, out, docroot );

    free_request( req );
}

static void
free_request
(
    struct HTTPRequest  *req
)
{
    struct HTTPHeaderField  *h      = NULL;
    struct HTTPHeaderField  *head   = NULL;

    dbg( "req=%p\n", req );

    head = req->header;
    while( head )
    {
        h    = head;
        head = head->next;

        free( h->name );
        free( h->value );
        h->name  = NULL;
        h->value = NULL;

        free( h );
        h = NULL;
    }
    free( req->method );
    free( req->path );
    free( req->body );
    req->method = NULL;
    req->path   = NULL;
    req->body   = NULL;

    free( req );
    req = NULL;
}

static char*
getRequestMethod
(
    char **method,
    char *buf
)
{
    char *p = NULL;

    p = strchr( buf, ' ' );
    if( NULL == p )
    {
        log_exit( "parse error on request line (1): %s", buf );
    }
    *p++ = '\0';

    *method = xmalloc( p - buf );
    strcpy( *method, buf );
    upcase( *method );

    return p;
}

static char*
getRequestPath
(
    char **path,
    char *buf
)
{
    char *p = NULL;

    p = strchr( buf, ' ' );
    if( !p )
    {
        log_exit( "parse error on request line (2): %s", buf );
    }
    *p++ = '\0';

    *path = xmalloc( p - buf );
    strcpy( *path, buf );

    return p;
}

static int
getRequestProtocolMinVersion
(
    char *buf
)
{
    char *p = NULL;

    p = buf;
    if( strncasecmp( p, "HTTP/1.", strlen( "HTTP/1." ) ) != 0 )
    {
        log_exit( "parse error on request line (2): %s", buf );
    }
    p += strlen( "HTTP/1." );
    return atoi( p );
}

static void
read_request_line
(
    struct HTTPRequest *req,
    FILE *in
)
{
    char buf[LINE_BUF_SIZE];
    char *p    = NULL;

    dbg( "req=%p, in=%p\n", req, in );

    memset( buf, 0x00, sizeof( buf ) );

    /* Input Request */
    printf(">");
    if( NULL == fgets( buf, LINE_BUF_SIZE, in ) )
    {
        log_exit( "no request line" );
    }

    /* Get Request Method */
    p = getRequestMethod( &(req->method), buf );

    /* Get Request Path */
    p = getRequestPath( &(req->path), p );

    /* Get Request Protocol Minor Version */
    req->protocol_minor_version =
        getRequestProtocolMinVersion( p );
}

int
main
(
    int argc,
    char *argv[]
)
{
    dbg( "argc=%d, argv=%p\n", argc, argv );

    usage( argc, argv );

    chk_doc_root( argv[1] );

    install_signal_handlers();

    service( stdin, stdout, argv[1] );

    return 0;
}
