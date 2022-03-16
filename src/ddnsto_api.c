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
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include "sys-time.h"

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "fdevent.h"
#include "http_chunk.h"
#include "http_header.h"
#include "keyvalue.h"
#include "response.h"

#include <fdevent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include "mod_ddnsto.h"

extern handler_conn_ctx * mod_ddnsto_handler_ctx_init (plugin_data * const p);
extern handler_t wait_request_ready(request_st *r, chunkqueue * const cq);
extern void ddnsto_response_json(request_st *r, const char* resp_str, size_t resp_len);

static handler_t wol_handle_fdevent(void *ctx, int revents) {
	handler_conn_ctx *hctx = (handler_conn_ctx*)ctx;
	hctx->rd_revents |= revents;
	joblist_append(hctx->con);
	return HANDLER_FINISHED;
}

static void child(int socket) {
  const char hello[] = "hello parent, I am child";
  write(socket, hello, sizeof(hello)); /* NB. this includes nul */
  close(socket);
}

// prepare to read, call many times (TODO)
static handler_t wol_process_rd_revents(handler_conn_ctx * const hctx, request_st * const r, int revents) {
  const bufsize = 4096;
  const int fd = hctx->fdn->fd;
  ssize_t n;
  unsigned char buf[bufsize];

  // TODO read to end
  n = read(fd, buf, bufsize);
  if (n < 0) {
    switch (errno) {
      case EAGAIN:
     #ifdef EWOULDBLOCK
     #if EWOULDBLOCK != EAGAIN
      case EWOULDBLOCK:
     #endif
     #endif
      case EINTR:
        return HANDLER_GO_ON;
      default:
        r->resp_body_finished = 1;
        r->http_status = 500;

        log_perror(r->conf.errh, __FILE__, __LINE__,
          "read() %d %d", r->con->fd, fd);

        return HANDLER_ERROR;
    }
  }

  log_error(r->conf.errh, __FILE__, __LINE__,
    "wol rd n=%ld buf=%s\n", n, (char*)buf);
  hctx->fd_read_ok = 1;

  return HANDLER_GO_ON;
}

// prepare to write
static handler_t wol_process_wr_revents (handler_conn_ctx * const hctx, request_st * const r, int revents) {
  log_error(r->conf.errh, __FILE__, __LINE__,
    "wol wd");
  return HANDLER_GO_ON;
}

handler_t handle_ddnsto_wol(request_st *r, plugin_data * const p) {
  time_t now = time(NULL);
  int fd[2];
  static const int parentsocket = 0;
  static const int childsocket = 1;
  pid_t pid;

  handler_conn_ctx * hctx = r->plugin_ctx[p->id];
  if(NULL == hctx) {
    hctx = mod_ddnsto_handler_ctx_init(p);
    hctx->create_ts = now;
    hctx->fd = -1;
		hctx->ev = r->con->srv->ev;
		hctx->r = r;
    hctx->con = r->con;
    r->plugin_ctx[p->id] = hctx;
  } 

  const int rd_revents = hctx->rd_revents;
  const int wr_revents = hctx->wr_revents;
  if (rd_revents) {
    hctx->rd_revents = 0;
    handler_t rc = wol_process_rd_revents(hctx, r, rd_revents);
    if (rc != HANDLER_GO_ON) {
      return rc;
    }
  }
  if (wr_revents) {
    hctx->wr_revents = 0;
    handler_t rc = wol_process_wr_revents(hctx, r, wr_revents);
    if (rc != HANDLER_GO_ON) {
      return rc;
    }
  }

  if(0 == hctx->command_ready) {
    chunkqueue * const cq = &r->reqbody_queue;
    handler_t hret = wait_request_ready(r, cq);
    if (HANDLER_GO_ON != hret) {
      return hret;
    }

    buffer *body = chunkqueue_read_squash(cq, r->conf.errh);
    const char *body_str = body->ptr + body->used - (uint32_t)r->reqbody_length - 1;

    log_error(r->conf.errh, __FILE__, __LINE__, "wol chunkqueue is ready, body=%s", body_str);
    hctx->command_ready = 1;
  }

  if (-1 == hctx->fd) {
    socketpair(PF_LOCAL, SOCK_STREAM, 0, fd);
    pid = fork();
    if (pid == 0) { 
        /* if fork returned zero, you are the child */
        close(fd[parentsocket]); /* Close the parent file descriptor */
        child(fd[childsocket]);
        exit(0);
    } else { 
        /* you are the parent */
        close(fd[childsocket]); /* Close the child file descriptor */
        hctx->fd = fd[parentsocket];
        hctx->fdn = fdevent_register(hctx->ev, hctx->fd, wol_handle_fdevent, hctx);
        if (-1 == fdevent_fcntl_set_nb(hctx->fd)) {
          log_perror(r->conf.errh, __FILE__, __LINE__, "fcntl failed");
          r->resp_body_finished = 1;
          r->http_status = 500;
          return HANDLER_ERROR;
        }

			  //fdevent_fdnode_event_set(hctx->ev, hctx->fdn, FDEVENT_OUT);
		    fdevent_fdnode_event_set(hctx->ev, hctx->fdn, FDEVENT_IN | FDEVENT_RDHUP);
    }
  }

  if (1 == hctx->fd_read_ok) {
    ddnsto_response_json(r, "{}", 2);
    r->resp_body_finished = 1; 
    return HANDLER_FINISHED;
  }

  return HANDLER_WAIT_FOR_EVENT;
}

