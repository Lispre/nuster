/*
 * nuster cache filter related variables and functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/cfgparse.h>
#include <common/standard.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/stream.h>
#include <proto/proto_http.h>
#include <proto/stream_interface.h>

#include <nuster/cache.h>
#include <nuster/nuster.h>

static int _nst_cache_filter_init(struct proxy *px, struct flt_conf *fconf) {
    return 0;
}

static void _nst_cache_filter_deinit(struct proxy *px, struct flt_conf *fconf) {
    struct nuster_flt_conf *conf = fconf->conf;

    if(conf) {
        free(conf);
    }
    fconf->conf = NULL;
}

static int _nst_cache_filter_check(struct proxy *px, struct flt_conf *fconf) {
    if(px->mode != PR_MODE_HTTP) {
        ha_warning("Proxy [%s] : mode should be http to enable cache\n", px->id);
    }
    return 0;
}

static int _nst_cache_filter_attach(struct stream *s, struct filter *filter) {
    struct nuster_flt_conf *conf = FLT_CONF(filter);

    /* disable cache if state is not NUSTER_STATUS_ON */
    if(global.nuster.cache.status != NUSTER_STATUS_ON || conf->status != NUSTER_STATUS_ON) {
        return 0;
    }
    if(!filter->ctx) {
        struct nst_cache_ctx *ctx = pool_alloc(global.nuster.cache.pool.ctx);
        if(ctx == NULL ) {
            return 0;
        }
        ctx->state   = NST_CACHE_CTX_STATE_INIT;
        ctx->rule    = NULL;
        ctx->stash   = NULL;
        ctx->entry   = NULL;
        ctx->data    = NULL;
        ctx->element = NULL;
        ctx->pid     = -1;
        ctx->sov     = 0;
        filter->ctx  = ctx;
    }
    register_data_filter(s, &s->req, filter);
    register_data_filter(s, &s->res, filter);
    return 1;
}

static void _nst_cache_filter_detach(struct stream *s, struct filter *filter) {
    if(filter->ctx) {
        struct nuster_rule_stash *stash = NULL;
        struct nst_cache_ctx *ctx       = filter->ctx;

        nst_cache_stats_update_request(ctx->state);

        if(ctx->state == NST_CACHE_CTX_STATE_CREATE) {
            nst_cache_abort(ctx);
        }
        while(ctx->stash) {
            stash      = ctx->stash;
            ctx->stash = ctx->stash->next;
            free(stash->key);
            pool_free(global.nuster.cache.pool.stash, stash);
        }
        if(ctx->req.host.data) {
            nst_cache_memory_free(global.nuster.cache.pool.chunk, ctx->req.host.data);
        }
        if(ctx->req.path.data) {
            nst_cache_memory_free(global.nuster.cache.pool.chunk, ctx->req.path.data);
        }
        pool_free(global.nuster.cache.pool.ctx, ctx);
    }
}

static int _nst_cache_filter_http_headers(struct stream *s, struct filter *filter,
        struct http_msg *msg) {

    struct channel *req         = msg->chn;
    struct channel *res         = &s->res;
    struct proxy *px            = s->be;
    struct stream_interface *si = &s->si[1];
    struct nst_cache_ctx *ctx   = filter->ctx;
    struct nuster_rule *rule    = NULL;
    char *key                   = NULL;
    uint64_t hash               = 0;

    if(!(msg->chn->flags & CF_ISRESP)) {

        nst_cache_housekeeping();

        /* check http method */
        if(s->txn->meth == HTTP_METH_OTHER) {
            ctx->state = NST_CACHE_CTX_STATE_BYPASS;
        }

        /* request */
        if(ctx->state == NST_CACHE_CTX_STATE_INIT) {
            if(!nst_cache_prebuild_key(ctx, s, msg)) {
                return 1;
            }
            list_for_each_entry(rule, &px->nuster.rules, list) {
                nuster_debug("[CACHE] Checking rule: %s\n", rule->name);
                /* disabled? */
                if(*rule->state == NUSTER_RULE_DISABLED) {
                    continue;
                }
                /* build key */
                key = nst_cache_build_key(ctx, rule->key, s, msg);
                if(!key) {
                    return 1;
                }
                nuster_debug("[CACHE] Got key: %s\n", key);
                hash = nuster_hash(key);

                /* stash key */
                if(!nst_cache_stash_rule(ctx, rule, key, hash)) {
                    return 1;
                }
                /* check if cache exists  */
                nuster_debug("[CACHE] Checking key existence: ");
                ctx->data = nst_cache_exists(key, hash);
                if(ctx->data) {
                    nuster_debug("EXIST\n[CACHE] Hit\n");
                    /* OK, cache exists */
                    ctx->state = NST_CACHE_CTX_STATE_HIT;
                    break;
                }
                nuster_debug("NOT EXIST\n");
                /* no, there's no cache yet */

                /* test acls to see if we should cache it */
                nuster_debug("[CACHE] [REQ] Checking if rule pass: ");
                if(nuster_test_rule(rule, s, msg->chn->flags & CF_ISRESP)) {
                    nuster_debug("PASS\n");
                    ctx->state = NST_CACHE_CTX_STATE_PASS;
                    ctx->rule  = rule;
                    break;
                }
                nuster_debug("FAIL\n");
            }
        }

        if(ctx->state == NST_CACHE_CTX_STATE_HIT) {
            nst_cache_hit(s, si, req, res, ctx->data);
        }

    } else {
        /* response */
        if(ctx->state == NST_CACHE_CTX_STATE_INIT) {
            nuster_debug("[CACHE] [RES] Checking if rule pass: ");
            list_for_each_entry(rule, &px->nuster.rules, list) {
                /* test acls to see if we should cache it */
                if(nuster_test_rule(rule, s, msg->chn->flags & CF_ISRESP)) {
                    nuster_debug("PASS\n");
                    ctx->state = NST_CACHE_CTX_STATE_PASS;
                    ctx->rule  = rule;
                    break;
                }
                nuster_debug("FAIL\n");
            }
        }

        if(ctx->state == NST_CACHE_CTX_STATE_PASS) {
            struct nuster_rule_stash *stash = ctx->stash;
            struct nuster_rule_code *cc     = ctx->rule->code;
            int valid                       = 0;

            ctx->pid = px->uuid;

            /* check if code is valid */
            nuster_debug("[CACHE] [RES] Checking status code: ");
            if(!cc) {
                valid = 1;
            }
            while(cc) {
                if(cc->code == s->txn->status) {
                    valid = 1;
                    break;
                }
                cc = cc->next;
            }
            if(!valid) {
                nuster_debug("FAIL\n");
                return 1;
            }

            /* get cache key */
            while(stash) {
                if(ctx->stash->rule == ctx->rule) {
                    key  = stash->key;
                    hash = stash->hash;
                    break;
                }
                stash = stash->next;
            }

            if(!key) {
                return 1;
            }

            ctx->sov = msg->sov;
            nuster_debug("PASS\n[CACHE] To create\n");

            /* start to build cache */
            nst_cache_create(ctx, key, hash);
        }

    }

    return 1;
}

static int _nst_cache_filter_http_forward_data(struct stream *s, struct filter *filter,
        struct http_msg *msg, unsigned int len) {

    struct nst_cache_ctx *ctx = filter->ctx;

    int forward = len;
    if(ctx->state == NST_CACHE_CTX_STATE_CREATE && (msg->chn->flags & CF_ISRESP)) {
        if(ctx->sov > 0) {
            forward  = ctx->sov;
            ctx->sov = 0;
        }
        if(!nst_cache_update(ctx, msg, forward)) {
            ctx->entry->state = NST_CACHE_ENTRY_STATE_INVALID;
            ctx->state        = NST_CACHE_CTX_STATE_PASS;
        }
    }
    return forward;
}

static int _nst_cache_filter_http_end(struct stream *s, struct filter *filter,
        struct http_msg *msg) {

    struct nst_cache_ctx *ctx = filter->ctx;

    if(ctx->state == NST_CACHE_CTX_STATE_CREATE && (msg->chn->flags & CF_ISRESP)) {
        nst_cache_finish(ctx);
    }
    return 1;
}

struct flt_ops nst_cache_filter_ops = {
    /* Manage cache filter, called for each filter declaration */
    .init   = _nst_cache_filter_init,
    .deinit = _nst_cache_filter_deinit,
    .check  = _nst_cache_filter_check,

    .attach = _nst_cache_filter_attach,
    .detach = _nst_cache_filter_detach,

    /* Filter HTTP requests and responses */
    .http_headers      = _nst_cache_filter_http_headers,
    .http_forward_data = _nst_cache_filter_http_forward_data,
    .http_end          = _nst_cache_filter_http_end,

};
