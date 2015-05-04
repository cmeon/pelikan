#include <util/bb_core.h>

#include <bb_stats.h>
#include <time/bb_time.h>

#if defined TARGET_SLIMCACHE
#include <slimcache/bb_process.h>
#elif defined TARGET_TWEMCACHE
#include <twemcache/bb_process.h>
#endif

#include <cc_debug.h>
#include <cc_event.h>
#include <channel/cc_tcp.h>
#include <stream/cc_sockio.h>

#include <netdb.h>

/**
 * TODO: use function pointers to accommodate different channel types when we
 * extend to beyond just TCP
 */

static struct context {
    struct event_base  *evb;
    int                timeout;
} context;

static struct context *ctx = &context;
static struct buf_sock *serversock; /* server buf_sock */
static channel_handler_t handlers;
static channel_handler_t *hdl = &handlers;

static void
_close(struct buf_sock *s)
{
    log_info("core close on buf_sock %p", s);

    event_deregister(ctx->evb, s->ch->sd);
    hdl->term(s->ch);
    request_return((struct request **)&s->data);
    buf_sock_return(&s);
}

static rstatus_t
_write(struct buf_sock *s)
{
    rstatus_t status;

    log_verb("writing on buf_sock %p", s);

    ASSERT(s != NULL);
    ASSERT(s->wbuf != NULL && s->rbuf != NULL);

    status = buf_tcp_write(s);

    return status;
}

static void
_post_write(struct buf_sock *s)
{
    log_verb("post write processing on buf_sock %p", s);

    //stats_thread_incr_by(data_written, nbyte);

    /* left-shift rbuf and wbuf */
    buf_lshift(s->rbuf);
    buf_lshift(s->wbuf);
}

static void
_event_write(struct buf_sock *s)
{
    rstatus_t status;
    struct conn *c = s->ch;

    status = _write(s);
    if (status == CC_ERETRY || status == CC_EAGAIN) { /* retry write */
        event_add_write(ctx->evb, hdl->id(c), s);
    } else if (status == CC_ERROR) {
        c->state = CONN_CLOSING;
    }
    _post_write(s);
}

static rstatus_t
_read(struct buf_sock *s)
{
    log_verb("reading on buf_sock %p", s);

    ASSERT(s != NULL);
    ASSERT(s->wbuf != NULL && s->rbuf != NULL);

    rstatus_t status;

    status = buf_tcp_read(s);
    if (status == CC_ENOMEM) {
        log_debug("not enough room in rbuf: start %p, rpos %p, wpos %p, end %p",
                s->rbuf->begin, s->rbuf->rpos, s->rbuf->wpos, s->rbuf->end);
        status = CC_ERETRY; /* retry when we cannot read due to buffer full */
    }

    return status;
}

static void
_post_read(struct buf_sock *s)
{
    rstatus_t status;
    struct request *req;

    log_verb("post read processing on buf_sock %p", s);

    //stats_thread_incr_by(data_read, nbyte);

    if (s->data != NULL) {
        req = s->data;
    } else {
        req = request_borrow();
        s->data = req;
    }

    if (req == NULL) {
        log_error("cannot acquire request: OOM");
        status = compose_rsp_msg(s->wbuf, RSP_SERVER_ERROR, false);
        if (status != CC_OK) {
            //s->err = status;

            log_error("failed to send server error, status: %d", status);
        }

        goto done;
    }

    if (req->swallow) {
        status = parse_swallow(s->rbuf);
        if (status == CC_OK) {
            request_reset(req);
        } else { /* CC_UNFIN */
            goto done;
        }
    }

    while (buf_rsize(s->rbuf) > 0) {
        /* parsing */
        log_verb("%"PRIu32" bytes left", buf_rsize(s->rbuf));

        status = parse_req(req, s->rbuf);
        if (status == CC_UNFIN) {
            goto done;
        }

        if (status != CC_OK) { /* parsing errors are all client errors */
            log_warn("illegal request received, status: %d", status);

            status = compose_rsp_msg(s->wbuf, RSP_CLIENT_ERROR, false);
            if (status != CC_OK) {
                log_error("failed to send client error, status: %d", status);
            }

            goto done;
        }

        /* processing */
        log_verb("wbuf free: %"PRIu32" B", buf_wsize(s->wbuf));
        status = process_request(req, s->wbuf);
        log_verb("wbuf free: %"PRIu32" B", buf_wsize(s->wbuf));

        if (status == CC_ENOMEM) {
            log_debug("wbuf full, try again later");
            goto done;
        }
        if (status == CC_ERDHUP) {
            log_info("peer called quit");
            s->ch->state = CONN_CLOSING;
            goto done;
        }

        if (status != CC_OK) {
            log_error("process request failed for other reason: %d", status);

            status = compose_rsp_msg(s->wbuf, RSP_SERVER_ERROR, false);
            if (status != CC_OK) {
                /* NOTE(yao): this processing logic does NOT work for large
                 * values, which will easily overflow wbuf and therefore always
                 * fail. Here we can do this because the values are very small
                 * relative to the size wbuf.
                 *
                 * The right way of handling write of any size value is to copy
                 * data directly from our data store on heap to the channel.
                 *
                 * If we want to be less aggressive in raising errors, we can
                 * re-process the current request when wbuf is full. This will
                 * require small modification to this function and struct request.
                 */
                log_error("failed to send server error, status: %d", status);
            }

            goto done;
        }

        request_reset(req);
    }

done:
    /* TODO: call stream write directly to save one event */
    if (buf_rsize(s->wbuf) > 0) {
        _event_write(s);
    }
}

static void
_tcpserver(struct buf_sock *ss)
{
    struct buf_sock *s;
    struct conn *sc = ss->ch;

    s = buf_sock_borrow();
    if (s == NULL) {
        log_error("establish connection failed: cannot allocate buf_sock, "
                "reject connection request");
        ss->hdl->reject(sc); /* server rejects connection by closing it */
        return;
    }

    if (!ss->hdl->accept(sc, s->ch)) {
        return;
    }

    s->owner = ctx;
    s->hdl = hdl;
    event_add_read(ctx->evb, hdl->id(s->ch), s);
}

static void
_event_read(struct buf_sock *s)
{
    rstatus_t status;
    struct conn *c = s->ch;

    if (c->level == CHANNEL_META) {
        _tcpserver(s);
    } else if (c->level == CHANNEL_BASE) {
        status = _read(s);
        if (status == CC_ERROR) {
            c->state = CONN_CLOSING;
        }
        /* retry is unnecessary when we use level-triggered epoll
        if (status == CC_ERETRY) {
            event_add_read(ctx->evb, hdl->id(c), s);
        }
        */
        _post_read(s);
    } else {
        NOT_REACHED();
    }
}

static void
core_event(void *arg, uint32_t events)
{
    struct buf_sock *s = arg;

    log_verb("event %06"PRIX32" on buf_sock %p", events, s);

    if (events & EVENT_ERR) {
        INCR(event_error);
        _close(s);

        return;
    }

    if (events & EVENT_READ) {
        log_verb("processing read event on buf_sock %p", s);

        INCR(event_read);
        _event_read(s);
    }

    if (events & EVENT_WRITE) {
        log_verb("processing write event on buf_sock %p", s);

        INCR(event_write);
        _event_write(s);
    }

    if (s->ch->state == CONN_CLOSING ||
            (s->ch->state == CONN_EOF && buf_rsize(s->wbuf) == 0)) {
        _close(s);
    }
}

rstatus_t
core_setup(struct addrinfo *ai)
{
    struct conn *c;

    ctx->timeout = 100;
    ctx->evb = event_base_create(1024, core_event);
    if (ctx->evb == NULL) {
        return CC_ERROR;
    }

    hdl->accept = tcp_accept;
    hdl->reject = tcp_reject;
    hdl->open = tcp_listen;
    hdl->term = tcp_close;
    hdl->recv = tcp_recv;
    hdl->send = tcp_send;
    hdl->id = conn_id;

    /**
     * Here we give server socket a buf_sock purely because it is difficult to
     * write code in the core event loop that would accommodate different types
     * of structs at the moment. However, this doesn't have to be the case in
     * the future. We can choose to wrap different types in a common header-
     * one that contains a type field and a pointer to the actual struct, or
     * define common fields, like how posix sockaddr structs are used.
     */
    serversock = buf_sock_borrow();
    if (serversock == NULL) {
        log_error("cannot get server tcp buf_sock object");

        return CC_ERROR;
    }

    serversock->hdl = hdl;
    c = serversock->ch;
    if (!hdl->open(ai, c)) {
        log_error("server connection setup failed");

        return CC_ERROR;
    }
    c->level = CHANNEL_META;

    event_add_read(ctx->evb, hdl->id(c), serversock);

    return CC_OK;
}

void
core_teardown(void)
{
    buf_sock_return(&serversock);
    event_base_destroy(&ctx->evb);
}

rstatus_t
core_evwait(void)
{
    int n;

    n = event_wait(ctx->evb, ctx->timeout);
    if (n < 0) {
        return n;
    }

    INCR(event_loop);
    INCR_N(event_total, n);
    time_update();

    return CC_OK;
}