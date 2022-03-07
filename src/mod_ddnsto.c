/* fstatat() fdopendir() */
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE-0 < 700
#undef  _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
/* NetBSD dirent.h improperly hides fdopendir() (POSIX.1-2008) declaration
 * which should be visible with _XOPEN_SOURCE 700 or _POSIX_C_SOURCE 200809L */
#ifdef __NetBSD__
#define _NETBSD_SOURCE
#endif
#endif

#include "first.h"

#include <stdlib.h>
#include <string.h>
#include "sys-time.h"

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "fdevent.h"
#include "http_chunk.h"
#include "http_header.h"
#include "keyvalue.h"
#include "response.h"
#include "kvec.h"
#include "queue.h"
#include "kstring.h"
#include "cJSON.h"

#include "plugin.h"

#define COMMAND_TAG_LEN 32
#define COMMAND_DEF 50
#define COMMAND_MAX 200
typedef struct command {
  uint32_t        local_id;
  uint32_t        remote_id;
  char            remote_tag[COMMAND_TAG_LEN];
  unix_time64_t   create_ts;
  kstring_t       val;
} command_t;

typedef struct ddnsto_command* command_item;
typedef kvec_t(command_item) command_array;

typedef struct {
  /* not used */
	char json;
  uint32_t local_id_start;
} plugin_config;

typedef struct handler_conn {
	plugin_config conf;

  STAILQ_ENTRY(handler_conn) conn_entry;

  int command_ready;
  int command_from;
  int command_size;
  unix_time64_t create_ts;
} handler_conn_ctx;

STAILQ_HEAD(handler_conn_list_head, handler_conn);

typedef struct {
	PLUGIN_DATA;
	plugin_config defaults;
	plugin_config conf;
	int processing;

  struct handler_conn_list_head handler_conn_list;
  command_array commands;
} plugin_data;

static handler_conn_ctx * mod_ddnsto_handler_ctx_init (plugin_data * const p) {
    handler_conn_ctx *hctx = calloc(1, sizeof(*hctx));
    force_assert(hctx);
    memcpy(&hctx->conf, &p->conf, sizeof(plugin_config));
    return hctx;
}

static void mod_ddnsto_handler_ctx_free (handler_conn_ctx *hctx) {
    free(hctx);
}

static handler_t mod_echo_request_body(request_st * const r) {
    chunkqueue * const cq = &r->reqbody_queue;
    chunkqueue_remove_finished_chunks(cq); /* unnecessary? */
    off_t cqlen = chunkqueue_length(cq);
    if ((r->conf.stream_response_body & FDEVENT_STREAM_RESPONSE_BUFMIN)
        && r->resp_body_started) {
        if (chunkqueue_length(&r->write_queue) > 65536 - 4096) {
            /* wait for more data to be sent to client */
            return HANDLER_WAIT_FOR_EVENT;
        }
        else {
            if (cqlen > 65536) {
                cqlen = 65536;
                joblist_append(r->con);
            }
        }
    }

    if (0 != http_chunk_transfer_cqlen(r, cq, (size_t)cqlen))
        return HANDLER_ERROR;

    if (cq->bytes_out == (off_t)r->reqbody_length) {
        /* sent all request body input */
        http_response_backend_done(r);
        return HANDLER_FINISHED;
    }

    cqlen = chunkqueue_length(cq);
    if (cq->bytes_in != (off_t)r->reqbody_length && cqlen < 65536 - 16384) {
        /*(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST)*/
        if (!(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_POLLIN)) {
            r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_POLLIN;
            r->con->is_readable = 1; /* trigger optimistic read from client */
        }
    }
    return HANDLER_WAIT_FOR_EVENT;
}

static void mod_ddnsto_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* ddnsto.activate */
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_ddnsto_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_ddnsto_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_ddnsto_patch_config(request_st * const r, plugin_data * const p) {
    memcpy(&p->conf, &p->defaults, sizeof(plugin_config));
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_ddnsto_merge_config(&p->conf, p->cvlist + p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_ddnsto_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("ddnsto.activate"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_ddnsto"))
        return HANDLER_ERROR;

    log_error(srv->errh, __FILE__, __LINE__, "nconfig=%d %d %d", p->nconfig, !p->cvlist[0].v.u2[1], p->cvlist[0].v.u2[0]);

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        log_error(srv->errh, __FILE__, __LINE__, "nconfig=%d %d %d %d", p->nconfig, i, p->cvlist[i].v.u2[0], cpv->k_id);
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 0: /* ddnsto.activate */
              default:/* should not happen */
                break;
            }
        }
    }

    p->defaults.json = 0;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_ddnsto_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

INIT_FUNC(mod_ddnsto_init) {
    plugin_data *p = (plugin_data*)calloc(1, sizeof(plugin_data));
    kv_init(p->commands);
    return p;
}

SUBREQUEST_FUNC(mod_ddnsto_subrequest);
REQUEST_FUNC(mod_ddnsto_reset);

URIHANDLER_FUNC(mod_ddnsto_handle_uri_clean) {
    plugin_data *p = p_d;

    if (NULL == r->handler_module) {
        return HANDLER_GO_ON;
    }

    if (buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wait/"))
          || buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wake/"))
        ) {
        r->handler_module = p->self;
        r->resp_body_started = 1;
    } 
    return HANDLER_GO_ON;
}

SUBREQUEST_FUNC(mod_ddnsto_subrequest) {
    plugin_data * const p = p_d;

    if (r->conf.log_request_handling) {
      log_error(r->conf.errh, __FILE__, __LINE__, "ddnsto subrequest URI: %s", r->uri.path.ptr);
    }

    if (buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wait/"))) {
        // create handler_conn_ctx per connection
        handler_conn_ctx * hctx = r->plugin_ctx[p->id];
        if(NULL == hctx) {
          hctx = mod_ddnsto_handler_ctx_init(p);
          hctx->create_ts = log_monotonic_secs + 1;
          hctx->jb = chunk_buffer_acquire();
          r->plugin_ctx[p->id] = hctx;
        } 
        if(0 == hctx->command_ready) {
          // not ready, read request
          chunkqueue * const cq = &r->reqbody_queue;
          chunkqueue_remove_finished_chunks(cq); /* unnecessary? */
          if (cq->bytes_in < (off_t)r->reqbody_length) {
            return HANDLER_WAIT_FOR_EVENT;
          }

          buffer *body = chunkqueue_read_squash(cq, r->conf.errh);
          cJSON *json = NULL;
          cJSON *tmp = NULL;
          int ret = 0;
          do {
            json = cJSON_Parse(body->ptr);
            if(NULL == json) {
              ret = -1;
              break;
            }
            tmp = cJSON_GetObjectItem(json, "from");
            if(NULL == tmp) {
              ret = -2;
              break;
            }
            hctx->command_from = tmp->valueint;
            tmp = cJSON_GetObjectItem(json, "size");
            if(NULL == tmp) {
              hctx->command_size = COMMAND_DEF;
            } else {
              hctx->command_size = tmp->valueint;
              if(hctx->command_size > COMMAND_MAX) {
                hctx->command_size = COMMAND_MAX;
              }
            }
          } while(0);
          if (NULL != json) {
            cJSON_Delete(json);
          }
          chunk_buffer_release(body);
          if(0 != ret) {
            http_status_set_error(r, 400);
            return HANDLER_FINISHED;
          }
          hctx->command_ready = 1;
        }

        if(kv_size(p->commands) > 0) {
          kstring_t output = {0};
          ksprintf(&output, "{\"success\":0,\"result\":[");
          command_t *cmd = NULL;
          for (size_t i=0; i < kv_size(p->commands); i++) {
            command_t *cmd = 
            ksprintf(&output, "", )
          }
				  ksprintf(&output, "]}");
        } else {
          // check timeout
        }

        r->resp_body_finished = 1; 
        return HANDLER_FINISHED;
    } else if (buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wake/"))) {
    } else {
      /* send error here */
        chunkqueue * const cq = &r->write_queue;
        buffer * const out = chunkqueue_append_buffer_open(cq);
        buffer_append_string_len(out, CONST_STR_LEN(
          "{\"success\":-1000,"
          "\"error\":\"not found\"}"
        ));
        chunkqueue_append_buffer_commit(cq);

        buffer * const vb = 
          http_header_response_set_ptr(r, HTTP_HEADER_CONTENT_TYPE,
                                       CONST_STR_LEN("Content-Type")); 
        buffer_append_string_len(vb, CONST_STR_LEN("application/json; charset=utf-8"));

        r->resp_body_finished = 1;
        return HANDLER_FINISHED;
    } 
}

REQUEST_FUNC(mod_ddnsto_reset) {
    void ** const restrict dptr = &r->plugin_ctx[((plugin_data *)p_d)->id];
    if (*dptr) {
        --((plugin_data *)p_d)->processing;
        mod_ddnsto_handler_ctx_free(*dptr);
        *dptr = NULL;
    }
    return HANDLER_GO_ON;
}

TRIGGER_FUNC(mod_ddnsto_handle_trigger) {
    const plugin_data * const p = p_d;
    const unix_time64_t cur_ts = log_monotonic_secs + 1;

    log_error(srv->errh, __FILE__, __LINE__, "ddnsto trigger");

    for (connection *con = srv->conns; con; con = con->next) {
        request_st * const r = &con->request;
        handler_conn_ctx *hctx = r->plugin_ctx[p->id];
        if (NULL == hctx || r->handler_module != p->self)
            continue;

        if ((hctx->create_ts + 3) < cur_ts) {
            hctx->create_ts = cur_ts;
            joblist_append(con);
            continue;
        }
    }

    return HANDLER_GO_ON;
}

FREE_FUNC(mod_ddnsto_free) {
    plugin_data * const p = p_d;
    kv_destroy(p->commands);
}

int mod_ddnsto_plugin_init(plugin *p);
int mod_ddnsto_plugin_init(plugin *p) {
	p->version                 = LIGHTTPD_VERSION_ID;
	p->name                    = "ddnsto";
	p->init                    = mod_ddnsto_init;
  p->handle_uri_clean        = mod_ddnsto_handle_uri_clean;
	p->handle_subrequest       = mod_ddnsto_subrequest;
	p->handle_request_reset    = mod_ddnsto_reset;
	p->set_defaults            = mod_ddnsto_set_defaults;
  p->handle_trigger          = mod_ddnsto_handle_trigger;
	p->cleanup                 = mod_ddnsto_free;

	return 0;
}

