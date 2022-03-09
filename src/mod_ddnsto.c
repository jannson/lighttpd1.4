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

#define CONN_TIMEOUT    (30)
#define COMMAND_TIMEOUT (20)
#define COMMAND_TAG_LEN (32)
#define COMMAND_DEF     (50)
#define COMMAND_MAX     (200)
typedef struct command {
  uint32_t        local_id;
  uint32_t        remote_id;
  char            remote_tag[COMMAND_TAG_LEN];
  time_t          create_ts;
  kstring_t       val;
} command_t;

typedef struct command* command_item;
typedef kvec_t(command_item) command_array;

typedef struct {
  /* not used */
	char json;
  uint32_t local_id_start;
} plugin_config;

typedef struct handler_conn {
	plugin_config conf;

  STAILQ_ENTRY(handler_conn) conn_entry;

  int      command_ready;
  uint32_t command_from;
  uint32_t command_size;
  int      conn_add;
  time_t   create_ts;

  request_st* r;
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
    STAILQ_INIT(&p->handler_conn_list);
    return hctx;
}

static void mod_ddnsto_handler_ctx_free(plugin_data * const p, handler_conn_ctx *hctx) {
    if(hctx->conn_add) {
      STAILQ_REMOVE(&p->handler_conn_list, hctx, handler_conn, conn_entry);
      hctx->conn_add = 0;
    }
    free(hctx);
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

static void http_status_set_error (request_st * const r, int status) {
    r->resp_body_finished = 1;
    r->http_status = status;
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

    if (NULL != r->handler_module) {
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

static command_t *create_command() {
  return NULL;
}

static void free_command(command_t *cmd) {
  ks_free(&cmd->val);
  free(cmd);
}

static void remove_timeout_commands(plugin_data * const p, time_t now) {
    now = now - COMMAND_TIMEOUT ;
    if(kv_size(p->commands) > 0) {
      command_t *cmd = NULL;
      size_t i, j = 0;
      for (i = 0; i < kv_size(p->commands); i++) {
        cmd = kv_A(p->commands, i);
        if(cmd->create_ts >= now) {
          // not timeout
          break;
        }
        free_command(cmd);
        kv_A(p->commands, i) = NULL;
      }
      if (i > 0) {
        for(; i < kv_size(p->commands); i++) {
          kv_A(p->commands, j) = kv_A(p->commands, i);
          j++;
        }
        kv_size(p->commands) = j;
      }
    }
}

static void response_json(request_st *r, const char* resp_str, size_t resp_len) {
    chunkqueue * const cq = &r->write_queue;
    buffer * const out = chunkqueue_append_buffer_open(cq);
    buffer_append_string_len(out, resp_str, resp_len);
    chunkqueue_append_buffer_commit(cq);

    buffer * const vb = 
      http_header_response_set_ptr(r, HTTP_HEADER_CONTENT_TYPE,
                                   CONST_STR_LEN("Content-Type")); 
    buffer_append_string_len(vb, CONST_STR_LEN("application/json; charset=utf-8"));
}

static handler_t wait_request_ready(request_st *r, chunkqueue * const cq) {
    chunkqueue_remove_finished_chunks(cq);
    while(cq->bytes_in != (off_t) r->reqbody_length) {
      if(!(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST_POLLIN)) {
        r->conf.stream_request_body |= FDEVENT_STREAM_REQUEST_POLLIN;
        r->con->is_readable = 1;
      }

      handler_t rc = r->con->reqbody_read(r);
      if(rc != HANDLER_GO_ON) {
        return rc;
      }

      if(-1 == r->reqbody_length 
          && !(r->conf.stream_request_body & FDEVENT_STREAM_REQUEST)) {
        // wait request finished if it's chunk
        return HANDLER_WAIT_FOR_EVENT;
      }
    }
    return HANDLER_GO_ON;
}

#define CMD_OUTPUT_FMT "{\"local_id\",%u, \"remote_id\":%u, \"remote_tag\":\"%s\", \"create_ts\":\"%llu\", \"get_ts\":\"%llu\", \"val\":\"%s\"}"

static handler_t handle_ddnsto_wait(request_st *r, void *p_d) {
    plugin_data * const p = p_d;
    time_t now = time(NULL);
    // create handler_conn_ctx per connection
    handler_conn_ctx * hctx = r->plugin_ctx[p->id];
    if(NULL == hctx) {
      hctx = mod_ddnsto_handler_ctx_init(p);
      hctx->create_ts = now;
      r->plugin_ctx[p->id] = hctx;
    } 
    if(0 == hctx->command_ready) {
      // not ready, read request
      chunkqueue * const cq = &r->reqbody_queue;
      handler_t hret = wait_request_ready(r, cq);
      if (HANDLER_GO_ON != hret) {
        return hret;
      }

      buffer *body = chunkqueue_read_squash(cq, r->conf.errh);
      cJSON *json = NULL;
      cJSON *tmp = NULL;
      int ret = 0;
      const char *body_str = body->ptr + body->used - (uint32_t)r->reqbody_length - 1;

      log_error(r->conf.errh, __FILE__, __LINE__, "chunkqueue is ready, body=%s", body_str);

      do {
        json = cJSON_Parse(body_str);
        if(NULL == json) {
          ret = -1;
          break;
        }
        tmp = cJSON_GetObjectItem(json, "from");
        if(NULL == tmp) {
          ret = -2;
          break;
        }
        hctx->command_from = (uint32_t)tmp->valueint;
        tmp = cJSON_GetObjectItem(json, "size");
        if(NULL == tmp) {
          hctx->command_size = COMMAND_DEF;
        } else {
          hctx->command_size = (uint32_t)tmp->valueint;
          if(hctx->command_size > COMMAND_MAX) {
            hctx->command_size = COMMAND_MAX;
          }
        }
      } while(0);
      if (NULL != json) {
        cJSON_Delete(json);
      }
      // chunk_buffer_release(body);
      log_error(r->conf.errh, __FILE__, __LINE__, "chunkqueue body finish ret=%d", ret);
      if(0 != ret) {
        http_status_set_error(r, 400);
        return HANDLER_FINISHED;
      }
      hctx->command_ready = 1;
    }

    remove_timeout_commands(p, now);

    uint32_t output_size = 0;
    kstring_t output = {0};
    if(kv_size(p->commands) > 0) {
      ksprintf(&output, "{\"success\":0,\"result\":[");
      command_t *cmd = NULL;
      for (size_t i = 0; i < kv_size(p->commands); i++) {
        cmd = kv_A(p->commands, i);
        if(cmd->local_id < hctx->command_from) {
          continue;
        }
        char *fmt = CMD_OUTPUT_FMT ;
        if(0 != output_size) {
          fmt = ","CMD_OUTPUT_FMT ;
        }
        ksprintf(&output, fmt
            , cmd->local_id
            , cmd->remote_id
            , cmd->remote_tag
            , (uint64_t)cmd->create_ts
            , (uint64_t)now
            , ks_str(&cmd->val)
            );
        output_size++;
        if(output_size >= hctx->command_size) {
          break;
        }
      }
      ksprintf(&output, "]}");
    }
    if(output_size > 0) {
      if(hctx->conn_add) {
        STAILQ_REMOVE(&p->handler_conn_list, hctx, handler_conn, conn_entry);
        hctx->conn_add = 0;
      }
      response_json(r, ks_str(&output), ks_len(&output));
      r->resp_body_finished = 1; 
      ks_free(&output);
      return HANDLER_FINISHED;
    }

    // no new items, check timeout
    if(hctx->create_ts < (now-COMMAND_TIMEOUT)) {
      // timeout
      if(hctx->conn_add) {
        STAILQ_REMOVE(&p->handler_conn_list, hctx, handler_conn, conn_entry);
        hctx->conn_add = 0;
      }
      response_json(r, CONST_STR_LEN("{\"success\":-1001,\"error\":\"timeout\"}"));
      r->resp_body_finished = 1; 
      return HANDLER_FINISHED;
    }

    // not timeout, add to conns
    if(0 == hctx->conn_add) {
      // add to conn list
      STAILQ_INSERT_TAIL(&p->handler_conn_list, hctx, conn_entry);
      hctx->conn_add = 1;
    }
    hctx->r = r;
    return HANDLER_WAIT_FOR_EVENT;
}

static handler_t handle_ddnsto_wake(request_st *r, void *p_d) {
    return HANDLER_FINISHED;
}

SUBREQUEST_FUNC(mod_ddnsto_subrequest) {
    plugin_data * const p = p_d;

    if (r->conf.log_request_handling) {
      log_error(r->conf.errh, __FILE__, __LINE__, "ddnsto subrequest URI: %s", r->uri.path.ptr);
    }

    if (buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wait/"))) {
        return handle_ddnsto_wait(r, p_d);
    } else if (buffer_eq_slen(&r->uri.path, CONST_STR_LEN("/api/ddnsto/wake/"))) {
        return handle_ddnsto_wake(r, p_d);
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
    plugin_data * const p = p_d;
    void ** const restrict dptr = &r->plugin_ctx[p->id];
    if (*dptr) {
        --p->processing;
        mod_ddnsto_handler_ctx_free(p, *dptr);
        *dptr = NULL;
    }
    return HANDLER_GO_ON;
}

TRIGGER_FUNC(mod_ddnsto_handle_trigger) {
    const plugin_data * const p = p_d;
    time_t now = time(NULL);
    now -= CONN_TIMEOUT;

    log_error(srv->errh, __FILE__, __LINE__, "ddnsto trigger");

    handler_conn_ctx *hctx = NULL;
    STAILQ_FOREACH(hctx, &p->handler_conn_list, conn_entry) {
      if (hctx->create_ts < now) {
        if(NULL != hctx->r->con) {
          log_error(srv->errh, __FILE__, __LINE__, "ddnsto append job");
          joblist_append(hctx->r->con);
        }
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

