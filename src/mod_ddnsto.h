#ifndef __MOD_DDNSTO_H_
#define __MOD_DDNSTO_H_

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

  int      fd;
	int      rd_revents;
	int      wr_revents;
	fdnode   *fdn;
  int      fd_read_ok;

  request_st* r;
	struct      fdevents *ev;      /* dumb pointer */
	connection* con;               /* dumb pointer */

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

#endif

