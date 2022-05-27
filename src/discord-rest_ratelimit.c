#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "discord.h"
#include "discord-internal.h"

#include "cog-utils.h"
#include "clock.h"

#define CHASH_VALUE_FIELD   bucket
#define CHASH_BUCKETS_FIELD routes
#include "chash.h"

/* chash heap-mode (auto-increase hashtable) */
#define RATELIMITER_TABLE_HEAP   1
#define RATELIMITER_TABLE_BUCKET struct _discord_route
#define RATELIMITER_TABLE_FREE_KEY(_key)
#define RATELIMITER_TABLE_HASH(_key, _hash)  chash_string_hash(_key, _hash)
#define RATELIMITER_TABLE_FREE_VALUE(_value) free(_value)
#define RATELIMITER_TABLE_COMPARE(_cmp_a, _cmp_b)                             \
    chash_string_compare(_cmp_a, _cmp_b)
#define RATELIMITER_TABLE_INIT(route, _key, _value)                           \
    memcpy(route.key, _key, sizeof(route.key));                               \
    route.bucket = _value

struct _discord_route {
    /** key formed from a request's route */
    char key[DISCORD_ROUTE_LEN];
    /** this route's bucket match */
    struct discord_bucket *bucket;
    /** the route state in the hashtable (see chash.h 'State enums') */
    int state;
};

#define KEY_PUSH(key, len, ...)                                               \
    do {                                                                      \
        *len += snprintf(key + *len, DISCORD_ROUTE_LEN - (size_t)*len,        \
                         ":" __VA_ARGS__);                                    \
        ASSERT_NOT_OOB(*len, DISCORD_ROUTE_LEN);                              \
    } while (0)

/* determine which ratelimit group a request belongs to by generating its key.
 * see: https://discord.com/developers/docs/topics/rate-limits */
void
discord_ratelimiter_build_key(enum http_method method,
                              char key[DISCORD_ROUTE_LEN],
                              const char endpoint_fmt[],
                              va_list args)
{
    /* generated key length */
    int keylen = 0;
    /* split endpoint sections */
    const char *curr = endpoint_fmt, *prev = "";
    size_t currlen = 0;

    KEY_PUSH(key, &keylen, "%d", method);
    do {
        u64snowflake id_arg = 0ULL;
        size_t i;

        curr += 1 + currlen;
        currlen = strcspn(curr, "/");

        /* reactions and sub-routes share the same bucket */
        if (0 == strncmp(prev, "reactions", 9)) break;

        /* consume variadic arguments */
        for (i = 0; i < currlen; ++i) {
            if ('%' == curr[i]) {
                const char *type = &curr[i + 1];

                switch (*type) {
                default:
                    VASSERT_S(0 == strncmp(type, PRIu64, sizeof(PRIu64) - 1),
                              "Internal error: Missing check for '%%%s'",
                              type);

                    id_arg = va_arg(args, u64snowflake);
                    break;
                case 's':
                    (void)va_arg(args, char *);
                    break;
                case 'd':
                    (void)va_arg(args, int);
                    break;
                }
            }
        }

        /* push section to key's string, in case of a major parameter the
         * literal ID will be pushed */
        if (0 == strncmp(curr, "%" PRIu64, currlen)
            && (0 == strncmp(prev, "channels", 8)
                || 0 == strncmp(prev, "guilds", 6)))
        {
            KEY_PUSH(key, &keylen, "%" PRIu64, id_arg);
        }
        else {
            KEY_PUSH(key, &keylen, "%.*s", (int)currlen, curr);
        }

        prev = curr;

    } while (curr[currlen] != '\0');
}

#undef KEY_PUSH

/* initialize bucket and assign it to ratelimiter hashtable */
static struct discord_bucket *
_discord_bucket_init(struct discord_ratelimiter *rl,
                     const char key[],
                     const struct ua_szbuf_readonly *hash,
                     const long limit)
{
    struct discord_bucket *b = calloc(1, sizeof *b);
    int len = snprintf(b->hash, sizeof(b->hash), "%.*s", (int)hash->size,
                       hash->start);

    ASSERT_NOT_OOB(len, sizeof(b->hash));

    b->remaining = 1;
    b->limit = limit;

    QUEUE_INIT(&b->pending_queue);

    chash_assign(rl, key, b, RATELIMITER_TABLE);

    return b;
}

void
discord_ratelimiter_init(struct discord_ratelimiter *rl, struct logconf *conf)
{
    struct ua_szbuf_readonly keynull = { "null", 4 }, keymiss = { "miss", 4 };

    __chash_init(rl, RATELIMITER_TABLE);

    logconf_branch(&rl->conf, conf, "DISCORD_RATELIMIT");

    /* global ratelimiting */
    rl->global_wait_ms = calloc(1, sizeof *rl->global_wait_ms);

    /* initialize 'singleton' buckets */
    rl->null = _discord_bucket_init(rl, "null", &keynull, 1L);
    rl->miss = _discord_bucket_init(rl, "miss", &keymiss, LONG_MAX);
}

static void
_discord_bucket_cancel(struct discord_ratelimiter *rl,
                       struct discord_bucket *b)
{
    struct discord_requestor *rqtor =
        CONTAINEROF(rl, struct discord_requestor, ratelimiter);

    /* cancel busy transfer */
    if (b->performing_req) discord_request_cancel(rqtor, b->performing_req);

    /* move pending tranfers to recycling */
    QUEUE_ADD(&rqtor->queues->recycling, &b->pending_queue);
    QUEUE_INIT(&b->pending_queue);
}

void
discord_ratelimiter_cleanup(struct discord_ratelimiter *rl)
{
    discord_ratelimiter_foreach_bucket(rl, &_discord_bucket_cancel);
    free(rl->global_wait_ms);
    __chash_free(rl, RATELIMITER_TABLE);
}

void
discord_ratelimiter_foreach_bucket(struct discord_ratelimiter *rl,
                                   void (*iter)(struct discord_ratelimiter *rl,
                                                struct discord_bucket *b))
{
    for (int i = 0; i < rl->capacity; ++i) {
        struct _discord_route *r = rl->routes + i;
        if (CHASH_FILLED == r->state) (*iter)(rl, r->bucket);
    }
}

static struct discord_bucket *
_discord_bucket_find(struct discord_ratelimiter *rl, const char key[])
{
    struct discord_bucket *b = NULL;
    int ret = chash_contains(rl, key, ret, RATELIMITER_TABLE);

    if (ret) {
        b = chash_lookup(rl, key, b, RATELIMITER_TABLE);
    }
    return b;
}

/* return ratelimit timeout timestamp for this bucket */
u64unix_ms
discord_bucket_get_timeout(struct discord_ratelimiter *rl,
                           struct discord_bucket *b)
{
    u64unix_ms reset = (b->remaining < 1) ? b->reset_tstamp : 0ULL;
    return (*rl->global_wait_ms > reset) ? *rl->global_wait_ms : reset;
}

static void
_discord_bucket_wake_cb(struct discord *client, struct discord_timer *timer)
{
    (void)client;
    struct discord_bucket *b = timer->data;

    b->performing_req = NULL;
    b->remaining = 1;
}

void
discord_bucket_try_timeout(struct discord_ratelimiter *rl,
                           struct discord_bucket *b)
{
    struct discord *client = CLIENT(rl, rest.requestor.ratelimiter);
    int64_t delay_ms = (int64_t)(b->reset_tstamp - cog_timestamp_ms());

    if (delay_ms < 0) delay_ms = 0;
    b->performing_req = DISCORD_BUCKET_TIMEOUT;

    _discord_timer_ctl(client, &client->rest.timers,
                       &(struct discord_timer){
                           .cb = &_discord_bucket_wake_cb,
                           .data = b,
                           .delay = delay_ms,
                           .flags = DISCORD_TIMER_DELETE_AUTO,
                       });

    logconf_info(&rl->conf, "[%.4s] RATELIMITING (wait %" PRId64 " ms)",
                 b->hash, delay_ms);
}

/* attempt to find a bucket associated key */
struct discord_bucket *
discord_bucket_get(struct discord_ratelimiter *rl, const char key[])
{
    struct discord_bucket *b;

    if (NULL != (b = _discord_bucket_find(rl, key))) {
        logconf_trace(&rl->conf, "[%.4s] Found a bucket match for '%s'!",
                      b->hash, key);

        return b;
    }

    logconf_trace(&rl->conf, "[null] Couldn't match known buckets to '%s'",
                  key);

    return rl->null;
}

static struct discord_bucket *
_discord_ratelimiter_get_match(struct discord_ratelimiter *rl,
                               const char key[],
                               struct ua_info *info)
{
    struct discord_bucket *b;

    /* create bucket if it doesn't exist yet */
    if (NULL == (b = _discord_bucket_find(rl, key))) {
        struct ua_szbuf_readonly hash =
            ua_info_get_header(info, "x-ratelimit-bucket");

        if (!hash.size) { /* bucket is not part of a ratelimiting group */
            b = rl->miss;
        }
        else {
            struct ua_szbuf_readonly limit =
                ua_info_get_header(info, "x-ratelimit-limit");
            long _limit =
                limit.size ? strtol(limit.start, NULL, 10) : LONG_MAX;

            b = _discord_bucket_init(rl, key, &hash, _limit);
        }
    }

    logconf_debug(&rl->conf, "[%.4s] Match '%s' to bucket", b->hash, key);

    return b;
}

/* attempt to fill bucket's values with response header fields */
static void
_discord_bucket_populate(struct discord_ratelimiter *rl,
                         struct discord_bucket *b,
                         struct ua_info *info)
{
    struct ua_szbuf_readonly remaining = ua_info_get_header(
                                 info, "x-ratelimit-remaining"),
                             reset =
                                 ua_info_get_header(info, "x-ratelimit-reset"),
                             reset_after = ua_info_get_header(
                                 info, "x-ratelimit-reset-after");
    u64unix_ms now = cog_timestamp_ms();

    b->remaining = remaining.size ? strtol(remaining.start, NULL, 10) : 1L;

    /* use X-Ratelimit-Reset-After if available, X-Ratelimit-Reset otherwise */
    if (reset_after.size) {
        struct ua_szbuf_readonly global =
            ua_info_get_header(info, "x-ratelimit-global");
        u64unix_ms reset_tstamp =
            now + (u64unix_ms)(1000 * strtod(reset_after.start, NULL));

        if (global.size) /* lock all buckets */
            *rl->global_wait_ms = reset_tstamp;
        else /* lock single bucket, timeout at discord_rest_run() */
            b->reset_tstamp = reset_tstamp;
    }
    else if (reset.size) {
        struct ua_szbuf_readonly date = ua_info_get_header(info, "date");
        /* get approximate elapsed time since request */
        struct PsnipClockTimespec ts;
        /* the Discord time in milliseconds */
        u64unix_ms server;
        /* the Discord time + request's elapsed time */
        u64unix_ms offset;

        server = (u64unix_ms)(1000 * curl_getdate(date.start, NULL));
        psnip_clock_wall_get_time(&ts);
        offset = server + ts.nanoseconds / 1000000;

        /* reset timestamp =
         *   (system time)
         *      + (diff between Discord's reset timestamp and offset)
         */
        b->reset_tstamp =
            now + ((u64unix_ms)(1000 * strtod(reset.start, NULL)) - offset);
    }

    logconf_debug(&rl->conf, "[%.4s] Remaining = %ld | Reset = %" PRIu64,
                  b->hash, b->remaining, b->reset_tstamp);
}

/* in case of asynchronous requests, check if successive requests made from a
 * `null` singleton bucket can be matched to another bucket */
static void
_discord_ratelimiter_null_filter(struct discord_ratelimiter *rl,
                                 struct discord_bucket *b,
                                 const char key[])
{
    QUEUE(struct discord_request) queue, *qelem;
    struct discord_request *req;

    QUEUE_MOVE(&rl->null->pending_queue, &queue);
    QUEUE_INIT(&rl->null->pending_queue);

    while (!QUEUE_EMPTY(&queue)) {
        qelem = QUEUE_HEAD(&queue);
        QUEUE_REMOVE(qelem);

        req = QUEUE_DATA(qelem, struct discord_request, entry);
        if (0 == strcmp(req->key, key)) {
            QUEUE_INSERT_TAIL(&b->pending_queue, qelem);
            req->b = b;
        }
        else {
            QUEUE_INSERT_TAIL(&rl->null->pending_queue, qelem);
        }
    }
}

/* attempt to create and/or update bucket's values */
void
discord_ratelimiter_build(struct discord_ratelimiter *rl,
                          struct discord_bucket *b,
                          const char key[],
                          struct ua_info *info)
{
    /* try to match to existing, or create new bucket */
    if (b == rl->null) {
        b = _discord_ratelimiter_get_match(rl, key, info);
        _discord_ratelimiter_null_filter(rl, b, key);
    }
    /* populate bucket with response header values */
    _discord_bucket_populate(rl, b, info);
}

void
discord_bucket_add_request(struct discord_bucket *b,
                           struct discord_request *req,
                           bool high_priority)
{
    QUEUE_REMOVE(&req->entry);
    QUEUE_INIT(&req->entry);
    if (high_priority)
        QUEUE_INSERT_HEAD(&b->pending_queue, &req->entry);
    else
        QUEUE_INSERT_TAIL(&b->pending_queue, &req->entry);
    req->b = b;
}

struct discord_request *
discord_bucket_remove_request(struct discord_bucket *b)
{
    QUEUE(struct discord_request) *qelem = QUEUE_HEAD(&b->pending_queue);
    QUEUE_REMOVE(qelem);
    QUEUE_INIT(qelem);

    return QUEUE_DATA(qelem, struct discord_request, entry);
}
