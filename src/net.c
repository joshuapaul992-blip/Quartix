/*
 * Quartix - HTTPS GET over BearSSL (Windows XP x64 has no native TLS 1.2).
 * Follows redirects, handles Content-Length, chunked and read-to-close bodies,
 * and streams the body to a callback. Certificates are checked against the
 * bundled Mozilla trust anchors and the host name.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"

extern const br_x509_trust_anchor QX_TAs[];
extern const size_t QX_TAs_NUM;

#define MAX_REDIRECTS 5
#define HEADER_MAX 16384
#define TIMEOUT_MS 30000

typedef struct conn {
    SOCKET s;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    /* read-ahead buffer for header/chunk parsing */
    unsigned char buf[4096];
    size_t pos, len;
} conn;

#ifdef QX_TESTING
/* Test builds may trust one extra CA from QUARTIX_TEST_CA (a PEM file), so the
 * tests can run a local HTTPS server. Release builds never compile this in. */
static br_x509_trust_anchor *test_tas;
static size_t test_tas_num;

static void dn_append(void *ctx, const void *data, size_t len)
{
    br_x509_trust_anchor *ta = ctx;
    unsigned char *p = realloc(ta->dn.data, ta->dn.len + len);
    if (!p)
        return;
    memcpy(p + ta->dn.len, data, len);
    ta->dn.data = p;
    ta->dn.len += len;
}

static void der_append(void *ctx, const void *data, size_t len)
{
    struct { unsigned char *p; size_t n; } *d = ctx;
    unsigned char *p = realloc(d->p, d->n + len);
    if (!p)
        return;
    memcpy(p + d->n, data, len);
    d->p = p;
    d->n += len;
}

static void load_test_ca(void)
{
    static int done;
    char path[MAX_PATH], *pem;
    FILE *f;
    long n;
    br_pem_decoder_context pc;
    struct { unsigned char *p; size_t n; } der = {NULL, 0};
    size_t off = 0;
    int inside = 0;

    if (done)
        return;
    done = 1;
    if (!GetEnvironmentVariableA("QUARTIX_TEST_CA", path, sizeof(path)) || !(f = fopen(path, "rb")))
        return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    pem = malloc((size_t)n);
    if (!pem || fread(pem, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(pem);
        return;
    }
    fclose(f);

    br_pem_decoder_init(&pc);
    while (off < (size_t)n) {
        size_t k = br_pem_decoder_push(&pc, pem + off, (size_t)n - off);
        off += k;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            inside = 1;
            br_pem_decoder_setdest(&pc, der_append, &der);
            break;
        case BR_PEM_END_OBJ:
            if (inside && der.n) {
                br_x509_decoder_context dc;
                br_x509_pkey *pk;
                br_x509_trust_anchor *ta;
                test_tas = realloc(test_tas, (test_tas_num + 1) * sizeof(*test_tas));
                ta = &test_tas[test_tas_num];
                memset(ta, 0, sizeof(*ta));
                br_x509_decoder_init(&dc, dn_append, ta);
                br_x509_decoder_push(&dc, der.p, der.n);
                pk = br_x509_decoder_get_pkey(&dc);
                if (pk && pk->key_type == BR_KEYTYPE_EC) {
                    ta->flags = BR_X509_TA_CA;
                    ta->pkey.key_type = BR_KEYTYPE_EC;
                    ta->pkey.key.ec.curve = pk->key.ec.curve;
                    ta->pkey.key.ec.q = malloc(pk->key.ec.qlen);
                    memcpy(ta->pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
                    ta->pkey.key.ec.qlen = pk->key.ec.qlen;
                    test_tas_num++;
                }
            }
            der.n = 0;
            inside = 0;
            break;
        default:
            break;
        }
    }
    free(der.p);
    free(pem);
}

/* The minimal X.509 engine takes one anchor list, so test builds use a merged copy. */
static const br_x509_trust_anchor *anchors(size_t *num)
{
    static br_x509_trust_anchor *all;
    load_test_ca();
    if (!test_tas_num) {
        *num = QX_TAs_NUM;
        return QX_TAs;
    }
    if (!all) {
        all = malloc((QX_TAs_NUM + test_tas_num) * sizeof(*all));
        memcpy(all, QX_TAs, QX_TAs_NUM * sizeof(*all));
        memcpy(all + QX_TAs_NUM, test_tas, test_tas_num * sizeof(*all));
    }
    *num = QX_TAs_NUM + test_tas_num;
    return all;
}
#else
static const br_x509_trust_anchor *anchors(size_t *num)
{
    *num = QX_TAs_NUM;
    return QX_TAs;
}
#endif

static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int n = recv(*(SOCKET *)ctx, (char *)buf, (int)(len > 65536 ? 65536 : len), 0);
    return n > 0 ? n : -1;
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int n = send(*(SOCKET *)ctx, (const char *)buf, (int)(len > 65536 ? 65536 : len), 0);
    return n > 0 ? n : -1;
}

static int net_init(void)
{
    static int ready;
    WSADATA wsa;
    if (!ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return -1;
        ready = 1;
    }
    return 0;
}

/* Splits https://host[:port]/path. Only https is accepted. */
static int parse_url(const char *url, char *host, size_t hcap, char *port, char *path, size_t pcap)
{
    const char *h, *slash, *colon;
    size_t hl;
    if (strncmp(url, "https://", 8) != 0)
        return -1;
    h = url + 8;
    slash = strchr(h, '/');
    if (!slash)
        slash = h + strlen(h);
    colon = memchr(h, ':', (size_t)(slash - h));
    hl = (size_t)((colon ? colon : slash) - h);
    if (hl == 0 || hl >= hcap)
        return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    if (colon) {
        size_t pl = (size_t)(slash - colon - 1);
        if (pl == 0 || pl > 5)
            return -1;
        memcpy(port, colon + 1, pl);
        port[pl] = 0;
    } else {
        strcpy(port, "443");
    }
    if (strlen(*slash ? slash : "/") >= pcap)
        return -1;
    strcpy(path, *slash ? slash : "/");
    return 0;
}

static int conn_open(conn *c, const char *host, const char *port)
{
    struct addrinfo hints, *res = NULL, *ai;
    DWORD tmo = TIMEOUT_MS;
    const br_x509_trust_anchor *tas;
    size_t ntas;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        qx_err("cannot resolve %s", host);
        return -1;
    }
    c->s = INVALID_SOCKET;
    for (ai = res; ai; ai = ai->ai_next) {
        c->s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c->s == INVALID_SOCKET)
            continue;
        setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
        setsockopt(c->s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof(tmo));
        if (connect(c->s, ai->ai_addr, (int)ai->ai_addrlen) == 0)
            break;
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (c->s == INVALID_SOCKET) {
        qx_err("cannot connect to %s", host);
        return -1;
    }

    tas = anchors(&ntas);
    br_ssl_client_init_full(&c->sc, &c->xc, tas, ntas);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
    br_ssl_client_reset(&c->sc, host, 0);
    br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->s, sock_write, &c->s);
    c->pos = c->len = 0;
    return 0;
}

static void conn_close(conn *c)
{
    if (c->s != INVALID_SOCKET) {
        closesocket(c->s);
        c->s = INVALID_SOCKET;
    }
}

static void report_tls_error(conn *c)
{
    int err = br_ssl_engine_last_error(&c->sc.eng);
    if (err >= BR_ERR_X509_OK && err < BR_ERR_X509_OK + 64)
        qx_err("TLS certificate check failed (BearSSL error %d)", err);
    else if (err != BR_ERR_OK)
        qx_err("TLS error (BearSSL error %d)", err);
    else
        qx_err("connection closed");
}

/* Returns the number of bytes read into out (>0), 0 at end of stream, -1 on error. */
static int conn_read(conn *c, void *out, size_t n)
{
    int r;
    if (c->pos < c->len) {
        size_t k = c->len - c->pos < n ? c->len - c->pos : n;
        memcpy(out, c->buf + c->pos, k);
        c->pos += k;
        return (int)k;
    }
    r = br_sslio_read(&c->io, out, n > 65536 ? 65536 : n);
    if (r > 0)
        return r;
    /* A clean close after the body is a normal end of stream. */
    return br_ssl_engine_last_error(&c->sc.eng) == BR_ERR_OK ? 0 : -1;
}

static int conn_getc(conn *c)
{
    unsigned char ch;
    if (c->pos == c->len) {
        int r = br_sslio_read(&c->io, c->buf, sizeof(c->buf));
        if (r <= 0)
            return -1;
        c->pos = 0;
        c->len = (size_t)r;
    }
    ch = c->buf[c->pos++];
    return ch;
}

static int read_line(conn *c, char *line, size_t cap)
{
    size_t n = 0;
    int ch;
    while ((ch = conn_getc(c)) >= 0) {
        if (ch == '\n') {
            if (n > 0 && line[n - 1] == '\r')
                n--;
            line[n] = 0;
            return 0;
        }
        if (n + 1 >= cap)
            return -1;
        line[n++] = (char)ch;
    }
    return -1;
}

static int header_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    return _strnicmp(line, name, n) == 0 && line[n] == ':';
}

static const char *header_value(const char *line)
{
    const char *v = strchr(line, ':') + 1;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

static int deliver(qx_net_sink sink, void *ctx, const void *data, size_t n, uint64_t *total, uint64_t max)
{
    *total += n;
    if (*total > max) {
        qx_err("download is larger than expected");
        return -1;
    }
    return sink(ctx, data, n);
}

static int read_body(conn *c, int chunked, int64_t length, qx_net_sink sink, void *ctx, uint64_t max)
{
    unsigned char buf[16384];
    uint64_t total = 0;
    int r;

    if (chunked) {
        char line[128];
        for (;;) {
            unsigned long size;
            char *end;
            if (read_line(c, line, sizeof(line)))
                return -1;
            size = strtoul(line, &end, 16);
            if (end == line)
                return -1;
            if (size == 0)
                return 0; /* trailers are ignored */
            while (size > 0) {
                r = conn_read(c, buf, size < sizeof(buf) ? size : sizeof(buf));
                if (r <= 0 || deliver(sink, ctx, buf, (size_t)r, &total, max))
                    return -1;
                size -= (unsigned long)r;
            }
            if (read_line(c, line, sizeof(line)) || line[0])
                return -1;
        }
    }
    if (length >= 0) {
        if ((uint64_t)length > max) {
            qx_err("download is larger than expected");
            return -1;
        }
        while (length > 0) {
            r = conn_read(c, buf, (size_t)length < sizeof(buf) ? (size_t)length : sizeof(buf));
            if (r <= 0 || deliver(sink, ctx, buf, (size_t)r, &total, max))
                return -1;
            length -= r;
        }
        return 0;
    }
    while ((r = conn_read(c, buf, sizeof(buf))) > 0)
        if (deliver(sink, ctx, buf, (size_t)r, &total, max))
            return -1;
    return r < 0 ? -1 : 0;
}

/* Resolves a Location header against the current URL. */
static int resolve_location(const char *base_host, const char *base_port, const char *loc, char *out, size_t cap)
{
    int n;
    if (strncmp(loc, "https://", 8) == 0)
        n = snprintf(out, cap, "%s", loc);
    else if (loc[0] == '/' && loc[1] != '/')
        n = strcmp(base_port, "443") == 0 ? snprintf(out, cap, "https://%s%s", base_host, loc)
                                           : snprintf(out, cap, "https://%s:%s%s", base_host, base_port, loc);
    else
        return -1; /* http:// and protocol-relative redirects are refused */
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int qx_https_get(const char *url, qx_net_sink sink, void *ctx, uint64_t max_bytes)
{
    static conn c;
    char cur[2048], host[256], port[8], path[2048], line[HEADER_MAX], req[2600];
    int hops;

    if (net_init())
        return -1;
    if (strlen(url) >= sizeof(cur))
        return -1;
    strcpy(cur, url);

    for (hops = 0; hops <= MAX_REDIRECTS; hops++) {
        int status, chunked = 0, n;
        int64_t length = -1;
        char location[2048] = "";

        if (parse_url(cur, host, sizeof(host), port, path, sizeof(path))) {
            qx_err("unsupported URL %s", cur);
            return -1;
        }
        if (conn_open(&c, host, port))
            return -1;
        n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Quartix/%s\r\n"
                     "Accept: */*\r\nConnection: close\r\n\r\n",
                     path, host, QX_VERSION_A);
        if (n <= 0 || (size_t)n >= sizeof(req) || br_sslio_write_all(&c.io, req, (size_t)n) ||
            br_sslio_flush(&c.io)) {
            report_tls_error(&c);
            conn_close(&c);
            return -1;
        }
        if (read_line(&c, line, sizeof(line)) || sscanf(line, "HTTP/1.%*d %d", &status) != 1) {
            report_tls_error(&c);
            conn_close(&c);
            return -1;
        }
        for (;;) {
            if (read_line(&c, line, sizeof(line))) {
                report_tls_error(&c);
                conn_close(&c);
                return -1;
            }
            if (!line[0])
                break;
            if (header_is(line, "Content-Length"))
                length = _strtoi64(header_value(line), NULL, 10);
            else if (header_is(line, "Transfer-Encoding") && strstr(header_value(line), "chunked"))
                chunked = 1;
            else if (header_is(line, "Location") && strlen(header_value(line)) < sizeof(location))
                strcpy(location, header_value(line));
        }

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            conn_close(&c);
            if (!location[0] || resolve_location(host, port, location, cur, sizeof(cur))) {
                qx_err("bad redirect from %s", host);
                return -1;
            }
            continue;
        }
        if (status != 200) {
            conn_close(&c);
            qx_err("server returned HTTP %d for %s", status, cur);
            return status == 404 ? 1 : -1;
        }
        n = read_body(&c, chunked, length, sink, ctx, max_bytes);
        if (n && br_ssl_engine_last_error(&c.sc.eng) != BR_ERR_OK)
            report_tls_error(&c);
        conn_close(&c);
        return n ? -1 : 0;
    }
    qx_err("too many redirects");
    return -1;
}
