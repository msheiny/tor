/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2011, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file connection.c
 * \brief General high-level functions to handle reading and writing
 * on connections.
 **/

#include "or.h"
#include "buffers.h"
#include "circuitbuild.h"
#include "circuitlist.h"
#include "circuituse.h"
#include "config.h"
#include "connection.h"
#include "connection_edge.h"
#include "connection_or.h"
#include "control.h"
#include "cpuworker.h"
#include "directory.h"
#include "dirserv.h"
#include "dns.h"
#include "dnsserv.h"
#include "geoip.h"
#include "main.h"
#include "policies.h"
#include "reasons.h"
#include "relay.h"
#include "rendclient.h"
#include "rendcommon.h"
#include "rephist.h"
#include "router.h"
#include "routerparse.h"

static connection_t *connection_create_listener(
                               struct sockaddr *listensockaddr,
                               socklen_t listensocklen, int type,
                               char* address);
static void connection_init(time_t now, connection_t *conn, int type,
                            int socket_family);
static int connection_init_accepted_conn(connection_t *conn,
                                         uint8_t listener_type);
static int connection_handle_listener_read(connection_t *conn, int new_type);
static int connection_bucket_should_increase(int bucket,
                                             or_connection_t *conn);
static int connection_finished_flushing(connection_t *conn);
static int connection_flushed_some(connection_t *conn);
static int connection_finished_connecting(connection_t *conn);
static int connection_reached_eof(connection_t *conn);
static int connection_read_to_buf(connection_t *conn, int *max_to_read,
                                  int *socket_error);
static int connection_process_inbuf(connection_t *conn, int package_partial);
static void client_check_address_changed(int sock);
static void set_constrained_socket_buffers(int sock, int size);

static const char *connection_proxy_state_to_string(int state);
static int connection_read_https_proxy_response(connection_t *conn);
static void connection_send_socks5_connect(connection_t *conn);

/** The last IPv4 address that our network interface seemed to have been
 * binding to, in host order.  We use this to detect when our IP changes. */
static uint32_t last_interface_ip = 0;
/** A list of uint32_ts for addresses we've used in outgoing connections.
 * Used to detect IP address changes. */
static smartlist_t *outgoing_addrs = NULL;

/**************************************************************/

/**
 * Return the human-readable name for the connection type <b>type</b>
 */
const char *
conn_type_to_string(int type)
{
  static char buf[64];
  switch (type) {
    case CONN_TYPE_OR_LISTENER: return "OR listener";
    case CONN_TYPE_OR: return "OR";
    case CONN_TYPE_EXIT: return "Exit";
    case CONN_TYPE_AP_LISTENER: return "Socks listener";
    case CONN_TYPE_AP_TRANS_LISTENER:
      return "Transparent pf/netfilter listener";
    case CONN_TYPE_AP_NATD_LISTENER: return "Transparent natd listener";
    case CONN_TYPE_AP_DNS_LISTENER: return "DNS listener";
    case CONN_TYPE_AP: return "Socks";
    case CONN_TYPE_DIR_LISTENER: return "Directory listener";
    case CONN_TYPE_DIR: return "Directory";
    case CONN_TYPE_CPUWORKER: return "CPU worker";
    case CONN_TYPE_CONTROL_LISTENER: return "Control listener";
    case CONN_TYPE_CONTROL: return "Control";
    default:
      log_warn(LD_BUG, "unknown connection type %d", type);
      tor_snprintf(buf, sizeof(buf), "unknown [%d]", type);
      return buf;
  }
}

/**
 * Return the human-readable name for the connection state <b>state</b>
 * for the connection type <b>type</b>
 */
const char *
conn_state_to_string(int type, int state)
{
  static char buf[96];
  switch (type) {
    case CONN_TYPE_OR_LISTENER:
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_AP_TRANS_LISTENER:
    case CONN_TYPE_AP_NATD_LISTENER:
    case CONN_TYPE_AP_DNS_LISTENER:
    case CONN_TYPE_DIR_LISTENER:
    case CONN_TYPE_CONTROL_LISTENER:
      if (state == LISTENER_STATE_READY)
        return "ready";
      break;
    case CONN_TYPE_OR:
      switch (state) {
        case OR_CONN_STATE_CONNECTING: return "connect()ing";
        case OR_CONN_STATE_PROXY_HANDSHAKING: return "handshaking (proxy)";
        case OR_CONN_STATE_TLS_HANDSHAKING: return "handshaking (TLS)";
        case OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING:
          return "renegotiating (TLS)";
        case OR_CONN_STATE_TLS_SERVER_RENEGOTIATING:
          return "waiting for renegotiation (TLS)";
        case OR_CONN_STATE_OR_HANDSHAKING: return "handshaking (Tor)";
        case OR_CONN_STATE_OPEN: return "open";
      }
      break;
    case CONN_TYPE_EXIT:
      switch (state) {
        case EXIT_CONN_STATE_RESOLVING: return "waiting for dest info";
        case EXIT_CONN_STATE_CONNECTING: return "connecting";
        case EXIT_CONN_STATE_OPEN: return "open";
        case EXIT_CONN_STATE_RESOLVEFAILED: return "resolve failed";
      }
      break;
    case CONN_TYPE_AP:
      switch (state) {
        case AP_CONN_STATE_SOCKS_WAIT: return "waiting for socks info";
        case AP_CONN_STATE_NATD_WAIT: return "waiting for natd dest info";
        case AP_CONN_STATE_RENDDESC_WAIT: return "waiting for rendezvous desc";
        case AP_CONN_STATE_CONTROLLER_WAIT: return "waiting for controller";
        case AP_CONN_STATE_CIRCUIT_WAIT: return "waiting for circuit";
        case AP_CONN_STATE_CONNECT_WAIT: return "waiting for connect response";
        case AP_CONN_STATE_RESOLVE_WAIT: return "waiting for resolve response";
        case AP_CONN_STATE_OPEN: return "open";
      }
      break;
    case CONN_TYPE_DIR:
      switch (state) {
        case DIR_CONN_STATE_CONNECTING: return "connecting";
        case DIR_CONN_STATE_CLIENT_SENDING: return "client sending";
        case DIR_CONN_STATE_CLIENT_READING: return "client reading";
        case DIR_CONN_STATE_CLIENT_FINISHED: return "client finished";
        case DIR_CONN_STATE_SERVER_COMMAND_WAIT: return "waiting for command";
        case DIR_CONN_STATE_SERVER_WRITING: return "writing";
      }
      break;
    case CONN_TYPE_CPUWORKER:
      switch (state) {
        case CPUWORKER_STATE_IDLE: return "idle";
        case CPUWORKER_STATE_BUSY_ONION: return "busy with onion";
      }
      break;
    case CONN_TYPE_CONTROL:
      switch (state) {
        case CONTROL_CONN_STATE_OPEN: return "open (protocol v1)";
        case CONTROL_CONN_STATE_NEEDAUTH:
          return "waiting for authentication (protocol v1)";
      }
      break;
  }

  log_warn(LD_BUG, "unknown connection state %d (type %d)", state, type);
  tor_snprintf(buf, sizeof(buf),
               "unknown state [%d] on unknown [%s] connection",
               state, conn_type_to_string(type));
  return buf;
}

/** Allocate and return a new dir_connection_t, initialized as by
 * connection_init(). */
dir_connection_t *
dir_connection_new(int socket_family)
{
  dir_connection_t *dir_conn = tor_malloc_zero(sizeof(dir_connection_t));
  connection_init(time(NULL), TO_CONN(dir_conn), CONN_TYPE_DIR, socket_family);
  return dir_conn;
}

/** Allocate and return a new or_connection_t, initialized as by
 * connection_init(). */
or_connection_t *
or_connection_new(int socket_family)
{
  or_connection_t *or_conn = tor_malloc_zero(sizeof(or_connection_t));
  time_t now = time(NULL);
  connection_init(now, TO_CONN(or_conn), CONN_TYPE_OR, socket_family);

  or_conn->timestamp_last_added_nonpadding = time(NULL);
  or_conn->next_circ_id = crypto_rand_int(1<<15);

  or_conn->active_circuit_pqueue = smartlist_create();
  or_conn->active_circuit_pqueue_last_recalibrated = cell_ewma_get_tick();

  return or_conn;
}

/** Allocate and return a new edge_connection_t, initialized as by
 * connection_init(). */
edge_connection_t *
edge_connection_new(int type, int socket_family)
{
  edge_connection_t *edge_conn = tor_malloc_zero(sizeof(edge_connection_t));
  tor_assert(type == CONN_TYPE_EXIT || type == CONN_TYPE_AP);
  connection_init(time(NULL), TO_CONN(edge_conn), type, socket_family);
  if (type == CONN_TYPE_AP)
    edge_conn->socks_request = tor_malloc_zero(sizeof(socks_request_t));
  return edge_conn;
}

/** Allocate and return a new control_connection_t, initialized as by
 * connection_init(). */
control_connection_t *
control_connection_new(int socket_family)
{
  control_connection_t *control_conn =
    tor_malloc_zero(sizeof(control_connection_t));
  connection_init(time(NULL),
                  TO_CONN(control_conn), CONN_TYPE_CONTROL, socket_family);
  log_notice(LD_CONTROL, "New control connection opened.");
  return control_conn;
}

/** Allocate, initialize, and return a new connection_t subtype of <b>type</b>
 * to make or receive connections of address family <b>socket_family</b>.  The
 * type should be one of the CONN_TYPE_* constants. */
connection_t *
connection_new(int type, int socket_family)
{
  switch (type) {
    case CONN_TYPE_OR:
      return TO_CONN(or_connection_new(socket_family));

    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      return TO_CONN(edge_connection_new(type, socket_family));

    case CONN_TYPE_DIR:
      return TO_CONN(dir_connection_new(socket_family));

    case CONN_TYPE_CONTROL:
      return TO_CONN(control_connection_new(socket_family));

    default: {
      connection_t *conn = tor_malloc_zero(sizeof(connection_t));
      connection_init(time(NULL), conn, type, socket_family);
      return conn;
    }
  }
}

/** Initializes conn. (you must call connection_add() to link it into the main
 * array).
 *
 * Set conn-\>type to <b>type</b>. Set conn-\>s and conn-\>conn_array_index to
 * -1 to signify they are not yet assigned.
 *
 * If conn is not a listener type, allocate buffers for it. If it's
 * an AP type, allocate space to store the socks_request.
 *
 * Assign a pseudorandom next_circ_id between 0 and 2**15.
 *
 * Initialize conn's timestamps to now.
 */
static void
connection_init(time_t now, connection_t *conn, int type, int socket_family)
{
  static uint64_t n_connections_allocated = 1;

  switch (type) {
    case CONN_TYPE_OR:
      conn->magic = OR_CONNECTION_MAGIC;
      break;
    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      conn->magic = EDGE_CONNECTION_MAGIC;
      break;
    case CONN_TYPE_DIR:
      conn->magic = DIR_CONNECTION_MAGIC;
      break;
    case CONN_TYPE_CONTROL:
      conn->magic = CONTROL_CONNECTION_MAGIC;
      break;
    default:
      conn->magic = BASE_CONNECTION_MAGIC;
      break;
  }

  conn->s = -1; /* give it a default of 'not used' */
  conn->conn_array_index = -1; /* also default to 'not used' */
  conn->global_identifier = n_connections_allocated++;

  conn->type = type;
  conn->socket_family = socket_family;
  if (!connection_is_listener(conn)) { /* listeners never use their buf */
    conn->inbuf = buf_new();
    conn->outbuf = buf_new();
  }

  conn->timestamp_created = now;
  conn->timestamp_lastread = now;
  conn->timestamp_lastwritten = now;
}

/** Create a link between <b>conn_a</b> and <b>conn_b</b>. */
void
connection_link_connections(connection_t *conn_a, connection_t *conn_b)
{
  tor_assert(conn_a->s < 0);
  tor_assert(conn_b->s < 0);

  conn_a->linked = 1;
  conn_b->linked = 1;
  conn_a->linked_conn = conn_b;
  conn_b->linked_conn = conn_a;
}

/** Deallocate memory used by <b>conn</b>. Deallocate its buffers if
 * necessary, close its socket if necessary, and mark the directory as dirty
 * if <b>conn</b> is an OR or OP connection.
 */
static void
_connection_free(connection_t *conn)
{
  void *mem;
  size_t memlen;
  if (!conn)
    return;

  switch (conn->type) {
    case CONN_TYPE_OR:
      tor_assert(conn->magic == OR_CONNECTION_MAGIC);
      mem = TO_OR_CONN(conn);
      memlen = sizeof(or_connection_t);
      break;
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      tor_assert(conn->magic == EDGE_CONNECTION_MAGIC);
      mem = TO_EDGE_CONN(conn);
      memlen = sizeof(edge_connection_t);
      break;
    case CONN_TYPE_DIR:
      tor_assert(conn->magic == DIR_CONNECTION_MAGIC);
      mem = TO_DIR_CONN(conn);
      memlen = sizeof(dir_connection_t);
      break;
    case CONN_TYPE_CONTROL:
      tor_assert(conn->magic == CONTROL_CONNECTION_MAGIC);
      mem = TO_CONTROL_CONN(conn);
      memlen = sizeof(control_connection_t);
      break;
    default:
      tor_assert(conn->magic == BASE_CONNECTION_MAGIC);
      mem = conn;
      memlen = sizeof(connection_t);
      break;
  }

  if (conn->linked) {
    log_info(LD_GENERAL, "Freeing linked %s connection [%s] with %d "
             "bytes on inbuf, %d on outbuf.",
             conn_type_to_string(conn->type),
             conn_state_to_string(conn->type, conn->state),
             (int)buf_datalen(conn->inbuf), (int)buf_datalen(conn->outbuf));
  }

  if (!connection_is_listener(conn)) {
    buf_free(conn->inbuf);
    buf_free(conn->outbuf);
  } else {
    if (conn->socket_family == AF_UNIX) {
      /* For now only control ports can be Unix domain sockets
       * and listeners at the same time */
      tor_assert(conn->type == CONN_TYPE_CONTROL_LISTENER);

      if (unlink(conn->address) < 0 && errno != ENOENT) {
        log_warn(LD_NET, "Could not unlink %s: %s", conn->address,
                         strerror(errno));
      }
    }
  }

  tor_free(conn->address);

  if (connection_speaks_cells(conn)) {
    or_connection_t *or_conn = TO_OR_CONN(conn);
    tor_tls_free(or_conn->tls);
    or_conn->tls = NULL;
    or_handshake_state_free(or_conn->handshake_state);
    or_conn->handshake_state = NULL;
    smartlist_free(or_conn->active_circuit_pqueue);
    tor_free(or_conn->nickname);
  }
  if (CONN_IS_EDGE(conn)) {
    edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
    tor_free(edge_conn->chosen_exit_name);
    if (edge_conn->socks_request) {
      memset(edge_conn->socks_request, 0xcc, sizeof(socks_request_t));
      tor_free(edge_conn->socks_request);
    }

    rend_data_free(edge_conn->rend_data);
  }
  if (conn->type == CONN_TYPE_CONTROL) {
    control_connection_t *control_conn = TO_CONTROL_CONN(conn);
    tor_free(control_conn->incoming_cmd);
  }

  tor_free(conn->read_event); /* Probably already freed by connection_free. */
  tor_free(conn->write_event); /* Probably already freed by connection_free. */

  if (conn->type == CONN_TYPE_DIR) {
    dir_connection_t *dir_conn = TO_DIR_CONN(conn);
    tor_free(dir_conn->requested_resource);

    tor_zlib_free(dir_conn->zlib_state);
    if (dir_conn->fingerprint_stack) {
      SMARTLIST_FOREACH(dir_conn->fingerprint_stack, char *, cp, tor_free(cp));
      smartlist_free(dir_conn->fingerprint_stack);
    }

    cached_dir_decref(dir_conn->cached_dir);
    rend_data_free(dir_conn->rend_data);
  }

  if (conn->s >= 0) {
    log_debug(LD_NET,"closing fd %d.",conn->s);
    tor_close_socket(conn->s);
    conn->s = -1;
  }

  if (conn->type == CONN_TYPE_OR &&
      !tor_digest_is_zero(TO_OR_CONN(conn)->identity_digest)) {
    log_warn(LD_BUG, "called on OR conn with non-zeroed identity_digest");
    connection_or_remove_from_identity_map(TO_OR_CONN(conn));
  }

  memset(mem, 0xCC, memlen); /* poison memory */
  tor_free(mem);
}

/** Make sure <b>conn</b> isn't in any of the global conn lists; then free it.
 */
void
connection_free(connection_t *conn)
{
  if (!conn)
    return;
  tor_assert(!connection_is_on_closeable_list(conn));
  tor_assert(!connection_in_array(conn));
  if (conn->linked_conn) {
    log_err(LD_BUG, "Called with conn->linked_conn still set.");
    tor_fragile_assert();
    conn->linked_conn->linked_conn = NULL;
    if (! conn->linked_conn->marked_for_close &&
        conn->linked_conn->reading_from_linked_conn)
      connection_start_reading(conn->linked_conn);
    conn->linked_conn = NULL;
  }
  if (connection_speaks_cells(conn)) {
    if (!tor_digest_is_zero(TO_OR_CONN(conn)->identity_digest)) {
      connection_or_remove_from_identity_map(TO_OR_CONN(conn));
    }
  }
  if (conn->type == CONN_TYPE_CONTROL) {
    TO_CONTROL_CONN(conn)->event_mask = 0;
    control_update_global_event_mask();
  }
  connection_unregister_events(conn);
  _connection_free(conn);
}

/** Call _connection_free() on every connection in our array, and release all
 * storage held by connection.c. This is used by cpuworkers and dnsworkers
 * when they fork, so they don't keep resources held open (especially
 * sockets).
 *
 * Don't do the checks in connection_free(), because they will
 * fail.
 */
void
connection_free_all(void)
{
  smartlist_t *conns = get_connection_array();

  /* We don't want to log any messages to controllers. */
  SMARTLIST_FOREACH(conns, connection_t *, conn,
    if (conn->type == CONN_TYPE_CONTROL)
      TO_CONTROL_CONN(conn)->event_mask = 0);

  control_update_global_event_mask();

  /* Unlink everything from the identity map. */
  connection_or_clear_identity_map();

  SMARTLIST_FOREACH(conns, connection_t *, conn, _connection_free(conn));

  if (outgoing_addrs) {
    SMARTLIST_FOREACH(outgoing_addrs, void*, addr, tor_free(addr));
    smartlist_free(outgoing_addrs);
    outgoing_addrs = NULL;
  }
}

/** Do any cleanup needed:
 *   - Directory conns that failed to fetch a rendezvous descriptor
 *     need to inform pending rendezvous streams.
 *   - OR conns need to call rep_hist_note_*() to record status.
 *   - AP conns need to send a socks reject if necessary.
 *   - Exit conns need to call connection_dns_remove() if necessary.
 *   - AP and Exit conns need to send an end cell if they can.
 *   - DNS conns need to fail any resolves that are pending on them.
 *   - OR and edge connections need to be unlinked from circuits.
 */
void
connection_about_to_close_connection(connection_t *conn)
{
  circuit_t *circ;
  dir_connection_t *dir_conn;
  or_connection_t *or_conn;
  edge_connection_t *edge_conn;
  time_t now = time(NULL);

  tor_assert(conn->marked_for_close);

  if (CONN_IS_EDGE(conn)) {
    edge_conn = TO_EDGE_CONN(conn);
    if (!edge_conn->edge_has_sent_end) {
      log_warn(LD_BUG, "(Harmless.) Edge connection (marked at %s:%d) "
               "hasn't sent end yet?",
               conn->marked_for_close_file, conn->marked_for_close);
      tor_fragile_assert();
    }
  }

  switch (conn->type) {
    case CONN_TYPE_DIR:
      dir_conn = TO_DIR_CONN(conn);
      if (conn->state < DIR_CONN_STATE_CLIENT_FINISHED) {
        /* It's a directory connection and connecting or fetching
         * failed: forget about this router, and maybe try again. */
        connection_dir_request_failed(dir_conn);
      }
      /* If we were trying to fetch a v2 rend desc and did not succeed,
       * retry as needed. (If a fetch is successful, the connection state
       * is changed to DIR_PURPOSE_HAS_FETCHED_RENDDESC to mark that
       * refetching is unnecessary.) */
      if (conn->purpose == DIR_PURPOSE_FETCH_RENDDESC_V2 &&
          dir_conn->rend_data &&
          strlen(dir_conn->rend_data->onion_address) ==
              REND_SERVICE_ID_LEN_BASE32)
        rend_client_refetch_v2_renddesc(dir_conn->rend_data);
      break;
    case CONN_TYPE_OR:
      or_conn = TO_OR_CONN(conn);
      /* Remember why we're closing this connection. */
      if (conn->state != OR_CONN_STATE_OPEN) {
        /* Inform any pending (not attached) circs that they should
         * give up. */
        circuit_n_conn_done(TO_OR_CONN(conn), 0);
        /* now mark things down as needed */
        if (connection_or_nonopen_was_started_here(or_conn)) {
          or_options_t *options = get_options();
          rep_hist_note_connect_failed(or_conn->identity_digest, now);
          entry_guard_register_connect_status(or_conn->identity_digest,0,
                                              !options->HTTPSProxy, now);
          if (conn->state >= OR_CONN_STATE_TLS_HANDSHAKING) {
            int reason = tls_error_to_orconn_end_reason(or_conn->tls_error);
            control_event_or_conn_status(or_conn, OR_CONN_EVENT_FAILED,
                                         reason);
            if (!authdir_mode_tests_reachability(options))
              control_event_bootstrap_problem(
                orconn_end_reason_to_control_string(reason), reason);
          }
        }
      } else if (conn->hold_open_until_flushed) {
        /* We only set hold_open_until_flushed when we're intentionally
         * closing a connection. */
        rep_hist_note_disconnect(or_conn->identity_digest, now);
        control_event_or_conn_status(or_conn, OR_CONN_EVENT_CLOSED,
                tls_error_to_orconn_end_reason(or_conn->tls_error));
      } else if (!tor_digest_is_zero(or_conn->identity_digest)) {
        rep_hist_note_connection_died(or_conn->identity_digest, now);
        control_event_or_conn_status(or_conn, OR_CONN_EVENT_CLOSED,
                tls_error_to_orconn_end_reason(or_conn->tls_error));
      }
      /* Now close all the attached circuits on it. */
      circuit_unlink_all_from_or_conn(TO_OR_CONN(conn),
                                      END_CIRC_REASON_OR_CONN_CLOSED);
      break;
    case CONN_TYPE_AP:
      edge_conn = TO_EDGE_CONN(conn);
      if (edge_conn->socks_request->has_finished == 0) {
        /* since conn gets removed right after this function finishes,
         * there's no point trying to send back a reply at this point. */
        log_warn(LD_BUG,"Closing stream (marked at %s:%d) without sending"
                 " back a socks reply.",
                 conn->marked_for_close_file, conn->marked_for_close);
      }
      if (!edge_conn->end_reason) {
        log_warn(LD_BUG,"Closing stream (marked at %s:%d) without having"
                 " set end_reason.",
                 conn->marked_for_close_file, conn->marked_for_close);
      }
      if (edge_conn->dns_server_request) {
        log_warn(LD_BUG,"Closing stream (marked at %s:%d) without having"
                 " replied to DNS request.",
                 conn->marked_for_close_file, conn->marked_for_close);
        dnsserv_reject_request(edge_conn);
      }
      control_event_stream_bandwidth(edge_conn);
      control_event_stream_status(edge_conn, STREAM_EVENT_CLOSED,
                                  edge_conn->end_reason);
      circ = circuit_get_by_edge_conn(edge_conn);
      if (circ)
        circuit_detach_stream(circ, edge_conn);
      break;
    case CONN_TYPE_EXIT:
      edge_conn = TO_EDGE_CONN(conn);
      circ = circuit_get_by_edge_conn(edge_conn);
      if (circ)
        circuit_detach_stream(circ, edge_conn);
      if (conn->state == EXIT_CONN_STATE_RESOLVING) {
        connection_dns_remove(edge_conn);
      }
      break;
  }
}

/** Return true iff connection_close_immediate() has been called on this
 * connection. */
#define CONN_IS_CLOSED(c) \
  ((c)->linked ? ((c)->linked_conn_is_closed) : ((c)->s < 0))

/** Close the underlying socket for <b>conn</b>, so we don't try to
 * flush it. Must be used in conjunction with (right before)
 * connection_mark_for_close().
 */
void
connection_close_immediate(connection_t *conn)
{
  assert_connection_ok(conn,0);
  if (CONN_IS_CLOSED(conn)) {
    log_err(LD_BUG,"Attempt to close already-closed connection.");
    tor_fragile_assert();
    return;
  }
  if (conn->outbuf_flushlen) {
    log_info(LD_NET,"fd %d, type %s, state %s, %d bytes on outbuf.",
             conn->s, conn_type_to_string(conn->type),
             conn_state_to_string(conn->type, conn->state),
             (int)conn->outbuf_flushlen);
  }

  connection_unregister_events(conn);

  if (conn->s >= 0)
    tor_close_socket(conn->s);
  conn->s = -1;
  if (conn->linked)
    conn->linked_conn_is_closed = 1;
  if (!connection_is_listener(conn)) {
    buf_clear(conn->outbuf);
    conn->outbuf_flushlen = 0;
  }
}

/** Mark <b>conn</b> to be closed next time we loop through
 * conn_close_if_marked() in main.c. */
void
_connection_mark_for_close(connection_t *conn, int line, const char *file)
{
  assert_connection_ok(conn,0);
  tor_assert(line);
  tor_assert(line < 1<<16); /* marked_for_close can only fit a uint16_t. */
  tor_assert(file);

  if (conn->marked_for_close) {
    log(LOG_WARN,LD_BUG,"Duplicate call to connection_mark_for_close at %s:%d"
        " (first at %s:%d)", file, line, conn->marked_for_close_file,
        conn->marked_for_close);
    tor_fragile_assert();
    return;
  }

  conn->marked_for_close = line;
  conn->marked_for_close_file = file;
  add_connection_to_closeable_list(conn);

  /* in case we're going to be held-open-til-flushed, reset
   * the number of seconds since last successful write, so
   * we get our whole 15 seconds */
  conn->timestamp_lastwritten = time(NULL);
}

/** Find each connection that has hold_open_until_flushed set to
 * 1 but hasn't written in the past 15 seconds, and set
 * hold_open_until_flushed to 0. This means it will get cleaned
 * up in the next loop through close_if_marked() in main.c.
 */
void
connection_expire_held_open(void)
{
  time_t now;
  smartlist_t *conns = get_connection_array();

  now = time(NULL);

  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    /* If we've been holding the connection open, but we haven't written
     * for 15 seconds...
     */
    if (conn->hold_open_until_flushed) {
      tor_assert(conn->marked_for_close);
      if (now - conn->timestamp_lastwritten >= 15) {
        int severity;
        if (conn->type == CONN_TYPE_EXIT ||
            (conn->type == CONN_TYPE_DIR &&
             conn->purpose == DIR_PURPOSE_SERVER))
          severity = LOG_INFO;
        else
          severity = LOG_NOTICE;
        log_fn(severity, LD_NET,
               "Giving up on marked_for_close conn that's been flushing "
               "for 15s (fd %d, type %s, state %s).",
               conn->s, conn_type_to_string(conn->type),
               conn_state_to_string(conn->type, conn->state));
        conn->hold_open_until_flushed = 0;
      }
    }
  });
}

/** Create an AF_INET listenaddr struct.
 * <b>listenaddress</b> provides the host and optionally the port information
 * for the new structure.  If no port is provided in <b>listenaddress</b> then
 * <b>listenport</b> is used.
 *
 * If not NULL <b>readable_address</b> will contain a copy of the host part of
 * <b>listenaddress</b>.
 *
 * The listenaddr struct has to be freed by the caller.
 */
static struct sockaddr_in *
create_inet_sockaddr(const char *listenaddress, uint16_t listenport,
                     char **readable_address, socklen_t *socklen_out) {
  struct sockaddr_in *listenaddr = NULL;
  uint32_t addr;
  uint16_t usePort = 0;

  if (parse_addr_port(LOG_WARN,
                      listenaddress, readable_address, &addr, &usePort)<0) {
    log_warn(LD_CONFIG,
             "Error parsing/resolving ListenAddress %s", listenaddress);
    goto err;
  }
  if (usePort==0)
    usePort = listenport;

  listenaddr = tor_malloc_zero(sizeof(struct sockaddr_in));
  listenaddr->sin_addr.s_addr = htonl(addr);
  listenaddr->sin_family = AF_INET;
  listenaddr->sin_port = htons((uint16_t) usePort);

  *socklen_out = sizeof(struct sockaddr_in);

  return listenaddr;

 err:
  tor_free(listenaddr);
  return NULL;
}

#ifdef HAVE_SYS_UN_H
/** Create an AF_UNIX listenaddr struct.
 * <b>listenaddress</b> provides the path to the Unix socket.
 *
 * Eventually <b>listenaddress</b> will also optionally contain user, group,
 * and file permissions for the new socket.  But not yet. XXX
 * Also, since we do not create the socket here the information doesn't help
 * here.
 *
 * If not NULL <b>readable_address</b> will contain a copy of the path part of
 * <b>listenaddress</b>.
 *
 * The listenaddr struct has to be freed by the caller.
 */
static struct sockaddr_un *
create_unix_sockaddr(const char *listenaddress, char **readable_address,
                     socklen_t *len_out)
{
  struct sockaddr_un *sockaddr = NULL;

  sockaddr = tor_malloc_zero(sizeof(struct sockaddr_un));
  sockaddr->sun_family = AF_UNIX;
  strncpy(sockaddr->sun_path, listenaddress, sizeof(sockaddr->sun_path));

  if (readable_address)
    *readable_address = tor_strdup(listenaddress);

  *len_out = sizeof(struct sockaddr_un);
  return sockaddr;
}
#else
static struct sockaddr *
create_unix_sockaddr(const char *listenaddress, char **readable_address,
                     socklen_t *len_out)
{
  (void)listenaddress;
  (void)readable_address;
  log_fn(LOG_ERR, LD_BUG,
         "Unix domain sockets not supported, yet we tried to create one.");
  *len_out = 0;
  tor_assert(0);
};
#endif /* HAVE_SYS_UN_H */

/** Warn that an accept or a connect has failed because we're running up
 * against our ulimit.  Rate-limit these warnings so that we don't spam
 * the log. */
static void
warn_too_many_conns(void)
{
#define WARN_TOO_MANY_CONNS_INTERVAL (6*60*60)
  static ratelim_t last_warned = RATELIM_INIT(WARN_TOO_MANY_CONNS_INTERVAL);
  char *m;
  if ((m = rate_limit_log(&last_warned, approx_time()))) {
    int n_conns = get_n_open_sockets();
    log_warn(LD_NET,"Failing because we have %d connections already. Please "
             "raise your ulimit -n.%s", n_conns, m);
    tor_free(m);
    control_event_general_status(LOG_WARN, "TOO_MANY_CONNECTIONS CURRENT=%d",
                                 n_conns);
  }
}

/** Bind a new non-blocking socket listening to the socket described
 * by <b>listensockaddr</b>.
 *
 * <b>address</b> is only used for logging purposes and to add the information
 * to the conn.
 */
static connection_t *
connection_create_listener(struct sockaddr *listensockaddr, socklen_t socklen,
                           int type, char* address)
{
  connection_t *conn;
  int s; /* the socket we're going to make */
  uint16_t usePort = 0;
  int start_reading = 0;

  if (get_n_open_sockets() >= get_options()->_ConnLimit-1) {
    warn_too_many_conns();
    return NULL;
  }

  if (listensockaddr->sa_family == AF_INET) {
    int is_tcp = (type != CONN_TYPE_AP_DNS_LISTENER);
#ifndef MS_WINDOWS
    int one=1;
#endif
    if (is_tcp)
      start_reading = 1;

    usePort = ntohs( (uint16_t)
                     ((struct sockaddr_in *)listensockaddr)->sin_port);

    log_notice(LD_NET, "Opening %s on %s:%d",
               conn_type_to_string(type), address, usePort);

    s = tor_open_socket(PF_INET,
                        is_tcp ? SOCK_STREAM : SOCK_DGRAM,
                        is_tcp ? IPPROTO_TCP: IPPROTO_UDP);
    if (s < 0) {
      log_warn(LD_NET,"Socket creation failed.");
      goto err;
    }

#ifndef MS_WINDOWS
    /* REUSEADDR on normal places means you can rebind to the port
     * right after somebody else has let it go. But REUSEADDR on win32
     * means you can bind to the port _even when somebody else
     * already has it bound_. So, don't do that on Win32. */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void*) &one,
               (socklen_t)sizeof(one));
#endif

    if (bind(s,listensockaddr,socklen) < 0) {
      const char *helpfulhint = "";
      int e = tor_socket_errno(s);
      if (ERRNO_IS_EADDRINUSE(e))
        helpfulhint = ". Is Tor already running?";
      log_warn(LD_NET, "Could not bind to %s:%u: %s%s", address, usePort,
               tor_socket_strerror(e), helpfulhint);
      tor_close_socket(s);
      goto err;
    }

    if (is_tcp) {
      if (listen(s,SOMAXCONN) < 0) {
        log_warn(LD_NET, "Could not listen on %s:%u: %s", address, usePort,
                 tor_socket_strerror(tor_socket_errno(s)));
        tor_close_socket(s);
        goto err;
      }
    }
#ifdef HAVE_SYS_UN_H
  } else if (listensockaddr->sa_family == AF_UNIX) {
    start_reading = 1;

    /* For now only control ports can be Unix domain sockets
     * and listeners at the same time */
    tor_assert(type == CONN_TYPE_CONTROL_LISTENER);

    log_notice(LD_NET, "Opening %s on %s",
               conn_type_to_string(type), address);

    if (unlink(address) < 0 && errno != ENOENT) {
      log_warn(LD_NET, "Could not unlink %s: %s", address,
                       strerror(errno));
      goto err;
    }
    s = tor_open_socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
      log_warn(LD_NET,"Socket creation failed: %s.", strerror(errno));
      goto err;
    }

    if (bind(s, listensockaddr, (socklen_t)sizeof(struct sockaddr_un)) == -1) {
      log_warn(LD_NET,"Bind to %s failed: %s.", address,
               tor_socket_strerror(tor_socket_errno(s)));
      goto err;
    }

    if (listen(s,SOMAXCONN) < 0) {
      log_warn(LD_NET, "Could not listen on %s: %s", address,
               tor_socket_strerror(tor_socket_errno(s)));
      tor_close_socket(s);
      goto err;
    }
#endif /* HAVE_SYS_UN_H */
  } else {
      log_err(LD_BUG,"Got unexpected address family %d.",
              listensockaddr->sa_family);
      tor_assert(0);
  }

  set_socket_nonblocking(s);

  conn = connection_new(type, listensockaddr->sa_family);
  conn->socket_family = listensockaddr->sa_family;
  conn->s = s;
  conn->address = tor_strdup(address);
  conn->port = usePort;

  if (connection_add(conn) < 0) { /* no space, forget it */
    log_warn(LD_NET,"connection_add for listener failed. Giving up.");
    connection_free(conn);
    goto err;
  }

  log_debug(LD_NET,"%s listening on port %u.",
            conn_type_to_string(type), usePort);

  conn->state = LISTENER_STATE_READY;
  if (start_reading) {
    connection_start_reading(conn);
  } else {
    tor_assert(type == CONN_TYPE_AP_DNS_LISTENER);
    dnsserv_configure_listener(conn);
  }

  return conn;

 err:
  return NULL;
}

/** Do basic sanity checking on a newly received socket. Return 0
 * if it looks ok, else return -1. */
static int
check_sockaddr(struct sockaddr *sa, int len, int level)
{
  int ok = 1;

  if (sa->sa_family == AF_INET) {
    struct sockaddr_in *sin=(struct sockaddr_in*)sa;
    if (len != sizeof(struct sockaddr_in)) {
      log_fn(level, LD_NET, "Length of address not as expected: %d vs %d",
             len,(int)sizeof(struct sockaddr_in));
      ok = 0;
    }
    if (sin->sin_addr.s_addr == 0 || sin->sin_port == 0) {
      log_fn(level, LD_NET,
             "Address for new connection has address/port equal to zero.");
      ok = 0;
    }
  } else if (sa->sa_family == AF_INET6) {
    struct sockaddr_in6 *sin6=(struct sockaddr_in6*)sa;
    if (len != sizeof(struct sockaddr_in6)) {
      log_fn(level, LD_NET, "Length of address not as expected: %d vs %d",
             len,(int)sizeof(struct sockaddr_in6));
      ok = 0;
    }
    if (tor_mem_is_zero((void*)sin6->sin6_addr.s6_addr, 16) ||
        sin6->sin6_port == 0) {
      log_fn(level, LD_NET,
             "Address for new connection has address/port equal to zero.");
      ok = 0;
    }
  } else {
    ok = 0;
  }
  return ok ? 0 : -1;
}

/** Check whether the socket family from an accepted socket <b>got</b> is the
 * same as the one that <b>listener</b> is waiting for.  If it isn't, log
 * a useful message and return -1.  Else return 0.
 *
 * This is annoying, but can apparently happen on some Darwins. */
static int
check_sockaddr_family_match(sa_family_t got, connection_t *listener)
{
  if (got != listener->socket_family) {
    log_info(LD_BUG, "A listener connection returned a socket with a "
             "mismatched family. %s for addr_family %d gave us a socket "
             "with address family %d.  Dropping.",
             conn_type_to_string(listener->type),
             (int)listener->socket_family,
             (int)got);
    return -1;
  }
  return 0;
}

/** The listener connection <b>conn</b> told poll() it wanted to read.
 * Call accept() on conn-\>s, and add the new connection if necessary.
 */
static int
connection_handle_listener_read(connection_t *conn, int new_type)
{
  int news; /* the new socket */
  connection_t *newconn;
  /* information about the remote peer when connecting to other routers */
  char addrbuf[256];
  struct sockaddr *remote = (struct sockaddr*)addrbuf;
  /* length of the remote address. Must be whatever accept() needs. */
  socklen_t remotelen = (socklen_t)sizeof(addrbuf);
  or_options_t *options = get_options();

  tor_assert((size_t)remotelen >= sizeof(struct sockaddr_in));
  memset(addrbuf, 0, sizeof(addrbuf));

  news = tor_accept_socket(conn->s,remote,&remotelen);
  if (news < 0) { /* accept() error */
    int e = tor_socket_errno(conn->s);
    if (ERRNO_IS_ACCEPT_EAGAIN(e)) {
      return 0; /* he hung up before we could accept(). that's fine. */
    } else if (ERRNO_IS_ACCEPT_RESOURCE_LIMIT(e)) {
      warn_too_many_conns();
      return 0;
    }
    /* else there was a real error. */
    log_warn(LD_NET,"accept() failed: %s. Closing listener.",
             tor_socket_strerror(e));
    connection_mark_for_close(conn);
    return -1;
  }
  log_debug(LD_NET,
            "Connection accepted on socket %d (child of fd %d).",
            news,conn->s);

  set_socket_nonblocking(news);

  if (options->ConstrainedSockets)
    set_constrained_socket_buffers(news, (int)options->ConstrainedSockSize);

  if (check_sockaddr_family_match(remote->sa_family, conn) < 0) {
    tor_close_socket(news);
    return 0;
  }

  if (conn->socket_family == AF_INET || conn->socket_family == AF_INET6) {
    tor_addr_t addr;
    uint16_t port;
    if (check_sockaddr(remote, remotelen, LOG_INFO)<0) {
      log_info(LD_NET,
               "accept() returned a strange address; trying getsockname().");
      remotelen=sizeof(addrbuf);
      memset(addrbuf, 0, sizeof(addrbuf));
      if (getsockname(news, remote, &remotelen)<0) {
        int e = tor_socket_errno(news);
        log_warn(LD_NET, "getsockname() for new connection failed: %s",
                 tor_socket_strerror(e));
      } else {
        if (check_sockaddr((struct sockaddr*)addrbuf, remotelen,
                              LOG_WARN) < 0) {
          log_warn(LD_NET,"Something's wrong with this conn. Closing it.");
          tor_close_socket(news);
          return 0;
        }
      }
    }

    if (check_sockaddr_family_match(remote->sa_family, conn) < 0) {
      tor_close_socket(news);
      return 0;
    }

    tor_addr_from_sockaddr(&addr, remote, &port);

    /* process entrance policies here, before we even create the connection */
    if (new_type == CONN_TYPE_AP) {
      /* check sockspolicy to see if we should accept it */
      if (socks_policy_permits_address(&addr) == 0) {
        log_notice(LD_APP,
                   "Denying socks connection from untrusted address %s.",
                   fmt_addr(&addr));
        tor_close_socket(news);
        return 0;
      }
    }
    if (new_type == CONN_TYPE_DIR) {
      /* check dirpolicy to see if we should accept it */
      if (dir_policy_permits_address(&addr) == 0) {
        log_notice(LD_DIRSERV,"Denying dir connection from address %s.",
                   fmt_addr(&addr));
        tor_close_socket(news);
        return 0;
      }
    }

    newconn = connection_new(new_type, conn->socket_family);
    newconn->s = news;

    /* remember the remote address */
    tor_addr_copy(&newconn->addr, &addr);
    newconn->port = port;
    newconn->address = tor_dup_addr(&addr);

  } else if (conn->socket_family == AF_UNIX) {
    /* For now only control ports can be Unix domain sockets
     * and listeners at the same time */
    tor_assert(conn->type == CONN_TYPE_CONTROL_LISTENER);

    newconn = connection_new(new_type, conn->socket_family);
    newconn->s = news;

    /* remember the remote address -- do we have anything sane to put here? */
    tor_addr_make_unspec(&newconn->addr);
    newconn->port = 1;
    newconn->address = tor_strdup(conn->address);
  } else {
    tor_assert(0);
  };

  if (connection_add(newconn) < 0) { /* no space, forget it */
    connection_free(newconn);
    return 0; /* no need to tear down the parent */
  }

  if (connection_init_accepted_conn(newconn, conn->type) < 0) {
    if (! newconn->marked_for_close)
      connection_mark_for_close(newconn);
    return 0;
  }
  return 0;
}

/** Initialize states for newly accepted connection <b>conn</b>.
 * If conn is an OR, start the TLS handshake.
 * If conn is a transparent AP, get its original destination
 * and place it in circuit_wait.
 */
static int
connection_init_accepted_conn(connection_t *conn, uint8_t listener_type)
{
  connection_start_reading(conn);

  switch (conn->type) {
    case CONN_TYPE_OR:
      control_event_or_conn_status(TO_OR_CONN(conn), OR_CONN_EVENT_NEW, 0);
      return connection_tls_start_handshake(TO_OR_CONN(conn), 1);
    case CONN_TYPE_AP:
      switch (listener_type) {
        case CONN_TYPE_AP_LISTENER:
          conn->state = AP_CONN_STATE_SOCKS_WAIT;
          break;
        case CONN_TYPE_AP_TRANS_LISTENER:
          TO_EDGE_CONN(conn)->is_transparent_ap = 1;
          conn->state = AP_CONN_STATE_CIRCUIT_WAIT;
          return connection_ap_process_transparent(TO_EDGE_CONN(conn));
        case CONN_TYPE_AP_NATD_LISTENER:
          TO_EDGE_CONN(conn)->is_transparent_ap = 1;
          conn->state = AP_CONN_STATE_NATD_WAIT;
          break;
      }
      break;
    case CONN_TYPE_DIR:
      conn->purpose = DIR_PURPOSE_SERVER;
      conn->state = DIR_CONN_STATE_SERVER_COMMAND_WAIT;
      break;
    case CONN_TYPE_CONTROL:
      conn->state = CONTROL_CONN_STATE_NEEDAUTH;
      break;
  }
  return 0;
}

/** Take conn, make a nonblocking socket; try to connect to
 * addr:port (they arrive in *host order*). If fail, return -1 and if
 * applicable put your best guess about errno into *<b>socket_error</b>.
 * Else assign s to conn-\>s: if connected return 1, if EAGAIN return 0.
 *
 * address is used to make the logs useful.
 *
 * On success, add conn to the list of polled connections.
 */
int
connection_connect(connection_t *conn, const char *address,
                   const tor_addr_t *addr, uint16_t port, int *socket_error)
{
  int s, inprogress = 0;
  char addrbuf[256];
  struct sockaddr *dest_addr = (struct sockaddr*) addrbuf;
  socklen_t dest_addr_len;
  or_options_t *options = get_options();
  int protocol_family;

  if (get_n_open_sockets() >= get_options()->_ConnLimit-1) {
    warn_too_many_conns();
    return -1;
  }

  if (tor_addr_family(addr) == AF_INET6)
    protocol_family = PF_INET6;
  else
    protocol_family = PF_INET;

  s = tor_open_socket(protocol_family,SOCK_STREAM,IPPROTO_TCP);
  if (s < 0) {
    *socket_error = tor_socket_errno(-1);
    log_warn(LD_NET,"Error creating network socket: %s",
             tor_socket_strerror(*socket_error));
    return -1;
  }

  if (options->OutboundBindAddress && !tor_addr_is_loopback(addr)) {
    struct sockaddr_in ext_addr;

    memset(&ext_addr, 0, sizeof(ext_addr));
    ext_addr.sin_family = AF_INET;
    ext_addr.sin_port = 0;
    if (!tor_inet_aton(options->OutboundBindAddress, &ext_addr.sin_addr)) {
      log_warn(LD_CONFIG,"Outbound bind address '%s' didn't parse. Ignoring.",
               options->OutboundBindAddress);
    } else {
      if (bind(s, (struct sockaddr*)&ext_addr,
               (socklen_t)sizeof(ext_addr)) < 0) {
        *socket_error = tor_socket_errno(s);
        log_warn(LD_NET,"Error binding network socket: %s",
                 tor_socket_strerror(*socket_error));
        tor_close_socket(s);
        return -1;
      }
    }
  }

  set_socket_nonblocking(s);

  if (options->ConstrainedSockets)
    set_constrained_socket_buffers(s, (int)options->ConstrainedSockSize);

  memset(addrbuf,0,sizeof(addrbuf));
  dest_addr = (struct sockaddr*) addrbuf;
  dest_addr_len = tor_addr_to_sockaddr(addr, port, dest_addr, sizeof(addrbuf));
  tor_assert(dest_addr_len > 0);

  log_debug(LD_NET, "Connecting to %s:%u.",
            escaped_safe_str_client(address), port);

  if (connect(s, dest_addr, dest_addr_len) < 0) {
    int e = tor_socket_errno(s);
    if (!ERRNO_IS_CONN_EINPROGRESS(e)) {
      /* yuck. kill it. */
      *socket_error = e;
      log_info(LD_NET,
               "connect() to %s:%u failed: %s",
               escaped_safe_str_client(address),
               port, tor_socket_strerror(e));
      tor_close_socket(s);
      return -1;
    } else {
      inprogress = 1;
    }
  }

  if (!server_mode(options))
    client_check_address_changed(s);

  /* it succeeded. we're connected. */
  log_fn(inprogress?LOG_DEBUG:LOG_INFO, LD_NET,
         "Connection to %s:%u %s (sock %d).",
         escaped_safe_str_client(address),
         port, inprogress?"in progress":"established", s);
  conn->s = s;
  if (connection_add(conn) < 0) /* no space, forget it */
    return -1;
  return inprogress ? 0 : 1;
}

/** Convert state number to string representation for logging purposes.
 */
static const char *
connection_proxy_state_to_string(int state)
{
  static const char *unknown = "???";
  static const char *states[] = {
    "PROXY_NONE",
    "PROXY_HTTPS_WANT_CONNECT_OK",
    "PROXY_SOCKS4_WANT_CONNECT_OK",
    "PROXY_SOCKS5_WANT_AUTH_METHOD_NONE",
    "PROXY_SOCKS5_WANT_AUTH_METHOD_RFC1929",
    "PROXY_SOCKS5_WANT_AUTH_RFC1929_OK",
    "PROXY_SOCKS5_WANT_CONNECT_OK",
    "PROXY_CONNECTED",
  };

  if (state < PROXY_NONE || state > PROXY_CONNECTED)
    return unknown;

  return states[state];
}

/** Write a proxy request of <b>type</b> (socks4, socks5, https) to conn
 * for conn->addr:conn->port, authenticating with the auth details given
 * in the configuration (if available). SOCKS 5 and HTTP CONNECT proxies
 * support authentication.
 *
 * Returns -1 if conn->addr is incompatible with the proxy protocol, and
 * 0 otherwise.
 *
 * Use connection_read_proxy_handshake() to complete the handshake.
 */
int
connection_proxy_connect(connection_t *conn, int type)
{
  or_options_t *options;

  tor_assert(conn);

  options = get_options();

  switch (type) {
    case PROXY_CONNECT: {
      char buf[1024];
      char *base64_authenticator=NULL;
      const char *authenticator = options->HTTPSProxyAuthenticator;

      /* Send HTTP CONNECT and authentication (if available) in
       * one request */

      if (authenticator) {
        base64_authenticator = alloc_http_authenticator(authenticator);
        if (!base64_authenticator)
          log_warn(LD_OR, "Encoding https authenticator failed");
      }

      if (base64_authenticator) {
        tor_snprintf(buf, sizeof(buf), "CONNECT %s:%d HTTP/1.1\r\n"
                     "Proxy-Authorization: Basic %s\r\n\r\n",
                     fmt_addr(&conn->addr),
                     conn->port, base64_authenticator);
        tor_free(base64_authenticator);
      } else {
        tor_snprintf(buf, sizeof(buf), "CONNECT %s:%d HTTP/1.0\r\n\r\n",
                     fmt_addr(&conn->addr), conn->port);
      }

      connection_write_to_buf(buf, strlen(buf), conn);
      conn->proxy_state = PROXY_HTTPS_WANT_CONNECT_OK;
      break;
    }

    case PROXY_SOCKS4: {
      unsigned char buf[9];
      uint16_t portn;
      uint32_t ip4addr;

      /* Send a SOCKS4 connect request with empty user id */

      if (tor_addr_family(&conn->addr) != AF_INET) {
        log_warn(LD_NET, "SOCKS4 client is incompatible with IPv6");
        return -1;
      }

      ip4addr = tor_addr_to_ipv4n(&conn->addr);
      portn = htons(conn->port);

      buf[0] = 4; /* version */
      buf[1] = SOCKS_COMMAND_CONNECT; /* command */
      memcpy(buf + 2, &portn, 2); /* port */
      memcpy(buf + 4, &ip4addr, 4); /* addr */
      buf[8] = 0; /* userid (empty) */

      connection_write_to_buf((char *)buf, sizeof(buf), conn);
      conn->proxy_state = PROXY_SOCKS4_WANT_CONNECT_OK;
      break;
    }

    case PROXY_SOCKS5: {
      unsigned char buf[4]; /* fields: vers, num methods, method list */

      /* Send a SOCKS5 greeting (connect request must wait) */

      buf[0] = 5; /* version */

      /* number of auth methods */
      if (options->Socks5ProxyUsername) {
        buf[1] = 2;
        buf[2] = 0x00; /* no authentication */
        buf[3] = 0x02; /* rfc1929 Username/Passwd auth */
        conn->proxy_state = PROXY_SOCKS5_WANT_AUTH_METHOD_RFC1929;
      } else {
        buf[1] = 1;
        buf[2] = 0x00; /* no authentication */
        conn->proxy_state = PROXY_SOCKS5_WANT_AUTH_METHOD_NONE;
      }

      connection_write_to_buf((char *)buf, 2 + buf[1], conn);
      break;
    }

    default:
      log_err(LD_BUG, "Invalid proxy protocol, %d", type);
      tor_fragile_assert();
      return -1;
  }

  log_debug(LD_NET, "set state %s",
            connection_proxy_state_to_string(conn->proxy_state));

  return 0;
}

/** Read conn's inbuf. If the http response from the proxy is all
 * here, make sure it's good news, then return 1. If it's bad news,
 * return -1. Else return 0 and hope for better luck next time.
 */
static int
connection_read_https_proxy_response(connection_t *conn)
{
  char *headers;
  char *reason=NULL;
  int status_code;
  time_t date_header;

  switch (fetch_from_buf_http(conn->inbuf,
                              &headers, MAX_HEADERS_SIZE,
                              NULL, NULL, 10000, 0)) {
    case -1: /* overflow */
      log_warn(LD_PROTOCOL,
               "Your https proxy sent back an oversized response. Closing.");
      return -1;
    case 0:
      log_info(LD_NET,"https proxy response not all here yet. Waiting.");
      return 0;
    /* case 1, fall through */
  }

  if (parse_http_response(headers, &status_code, &date_header,
                          NULL, &reason) < 0) {
    log_warn(LD_NET,
             "Unparseable headers from proxy (connecting to '%s'). Closing.",
             conn->address);
    tor_free(headers);
    return -1;
  }
  if (!reason) reason = tor_strdup("[no reason given]");

  if (status_code == 200) {
    log_info(LD_NET,
             "HTTPS connect to '%s' successful! (200 %s) Starting TLS.",
             conn->address, escaped(reason));
    tor_free(reason);
    return 1;
  }
  /* else, bad news on the status code */
  log_warn(LD_NET,
           "The https proxy sent back an unexpected status code %d (%s). "
           "Closing.",
           status_code, escaped(reason));
  tor_free(reason);
  return -1;
}

/** Send SOCKS5 CONNECT command to <b>conn</b>, copying <b>conn->addr</b>
 * and <b>conn->port</b> into the request.
 */
static void
connection_send_socks5_connect(connection_t *conn)
{
  unsigned char buf[1024];
  size_t reqsize = 6;
  uint16_t port = htons(conn->port);

  buf[0] = 5; /* version */
  buf[1] = SOCKS_COMMAND_CONNECT; /* command */
  buf[2] = 0; /* reserved */

  if (tor_addr_family(&conn->addr) == AF_INET) {
    uint32_t addr = tor_addr_to_ipv4n(&conn->addr);

    buf[3] = 1;
    reqsize += 4;
    memcpy(buf + 4, &addr, 4);
    memcpy(buf + 8, &port, 2);
  } else { /* AF_INET6 */
    buf[3] = 4;
    reqsize += 16;
    memcpy(buf + 4, tor_addr_to_in6(&conn->addr), 16);
    memcpy(buf + 20, &port, 2);
  }

  connection_write_to_buf((char *)buf, reqsize, conn);

  conn->proxy_state = PROXY_SOCKS5_WANT_CONNECT_OK;
}

/** Call this from connection_*_process_inbuf() to advance the proxy
 * handshake.
 *
 * No matter what proxy protocol is used, if this function returns 1, the
 * handshake is complete, and the data remaining on inbuf may contain the
 * start of the communication with the requested server.
 *
 * Returns 0 if the current buffer contains an incomplete response, and -1
 * on error.
 */
int
connection_read_proxy_handshake(connection_t *conn)
{
  int ret = 0;
  char *reason = NULL;

  log_debug(LD_NET, "enter state %s",
            connection_proxy_state_to_string(conn->proxy_state));

  switch (conn->proxy_state) {
    case PROXY_HTTPS_WANT_CONNECT_OK:
      ret = connection_read_https_proxy_response(conn);
      if (ret == 1)
        conn->proxy_state = PROXY_CONNECTED;
      break;

    case PROXY_SOCKS4_WANT_CONNECT_OK:
      ret = fetch_from_buf_socks_client(conn->inbuf,
                                        conn->proxy_state,
                                        &reason);
      if (ret == 1)
        conn->proxy_state = PROXY_CONNECTED;
      break;

    case PROXY_SOCKS5_WANT_AUTH_METHOD_NONE:
      ret = fetch_from_buf_socks_client(conn->inbuf,
                                        conn->proxy_state,
                                        &reason);
      /* no auth needed, do connect */
      if (ret == 1) {
        connection_send_socks5_connect(conn);
        ret = 0;
      }
      break;

    case PROXY_SOCKS5_WANT_AUTH_METHOD_RFC1929:
      ret = fetch_from_buf_socks_client(conn->inbuf,
                                        conn->proxy_state,
                                        &reason);

      /* send auth if needed, otherwise do connect */
      if (ret == 1) {
        connection_send_socks5_connect(conn);
        ret = 0;
      } else if (ret == 2) {
        unsigned char buf[1024];
        size_t reqsize, usize, psize;
        const char *user, *pass;

        user = get_options()->Socks5ProxyUsername;
        pass = get_options()->Socks5ProxyPassword;
        tor_assert(user && pass);

        /* XXX len of user and pass must be <= 255 !!! */
        usize = strlen(user);
        psize = strlen(pass);
        tor_assert(usize <= 255 && psize <= 255);
        reqsize = 3 + usize + psize;

        buf[0] = 1; /* negotiation version */
        buf[1] = usize;
        memcpy(buf + 2, user, usize);
        buf[2 + usize] = psize;
        memcpy(buf + 3 + usize, pass, psize);

        connection_write_to_buf((char *)buf, reqsize, conn);

        conn->proxy_state = PROXY_SOCKS5_WANT_AUTH_RFC1929_OK;
        ret = 0;
      }
      break;

    case PROXY_SOCKS5_WANT_AUTH_RFC1929_OK:
      ret = fetch_from_buf_socks_client(conn->inbuf,
                                        conn->proxy_state,
                                        &reason);
      /* send the connect request */
      if (ret == 1) {
        connection_send_socks5_connect(conn);
        ret = 0;
      }
      break;

    case PROXY_SOCKS5_WANT_CONNECT_OK:
      ret = fetch_from_buf_socks_client(conn->inbuf,
                                        conn->proxy_state,
                                        &reason);
      if (ret == 1)
        conn->proxy_state = PROXY_CONNECTED;
      break;

    default:
      log_err(LD_BUG, "Invalid proxy_state for reading, %d",
              conn->proxy_state);
      tor_fragile_assert();
      ret = -1;
      break;
  }

  log_debug(LD_NET, "leaving state %s",
            connection_proxy_state_to_string(conn->proxy_state));

  if (ret < 0) {
    if (reason) {
      log_warn(LD_NET, "Proxy Client: unable to connect to %s:%d (%s)",
                conn->address, conn->port, escaped(reason));
      tor_free(reason);
    } else {
      log_warn(LD_NET, "Proxy Client: unable to connect to %s:%d",
                conn->address, conn->port);
    }
  } else if (ret == 1) {
    log_info(LD_NET, "Proxy Client: connection to %s:%d successful",
              conn->address, conn->port);
  }

  return ret;
}

/**
 * Launch any configured listener connections of type <b>type</b>.  (A
 * listener is configured if <b>port_option</b> is non-zero.  If any
 * ListenAddress configuration options are given in <b>cfg</b>, create a
 * connection binding to each one.  Otherwise, create a single
 * connection binding to the address <b>default_addr</b>.)
 *
 * Only launch the listeners of this type that are not already open, and
 * only close listeners that are no longer wanted.  Existing listeners
 * that are still configured are not touched.
 *
 * If <b>disable_all_conns</b> is set, then never open new conns, and
 * close the existing ones.
 *
 * Add all old conns that should be closed to <b>replaced_conns</b>.
 * Add all new connections to <b>new_conns</b>.
 */
static int
retry_listeners(int type, config_line_t *cfg,
                int port_option, const char *default_addr,
                smartlist_t *replaced_conns,
                smartlist_t *new_conns,
                int disable_all_conns,
                int socket_family)
{
  smartlist_t *launch = smartlist_create(), *conns;
  int free_launch_elts = 1;
  int r;
  config_line_t *c;
  connection_t *conn;
  config_line_t *line;

  tor_assert(socket_family == AF_INET || socket_family == AF_UNIX);

  if (cfg && port_option) {
    for (c = cfg; c; c = c->next) {
      smartlist_add(launch, c);
    }
    free_launch_elts = 0;
  } else if (port_option) {
    line = tor_malloc_zero(sizeof(config_line_t));
    line->key = tor_strdup("");
    line->value = tor_strdup(default_addr);
    smartlist_add(launch, line);
  }

  /*
  SMARTLIST_FOREACH(launch, config_line_t *, l,
                    log_fn(LOG_NOTICE, "#%s#%s", l->key, l->value));
  */

  conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type != type ||
        conn->socket_family != socket_family ||
        conn->marked_for_close)
      continue;
    /* Okay, so this is a listener.  Is it configured? */
    line = NULL;
    SMARTLIST_FOREACH(launch, config_line_t *, wanted,
      {
        char *address=NULL;
        uint16_t port;
        switch (socket_family) {
          case AF_INET:
            if (!parse_addr_port(LOG_WARN,
                                 wanted->value, &address, NULL, &port)) {
              int addr_matches = !strcasecmp(address, conn->address);
              tor_free(address);
              if (! port)
                port = port_option;
              if (port == conn->port && addr_matches) {
                line = wanted;
                break;
              }
            }
            break;
          case AF_UNIX:
            if (!strcasecmp(wanted->value, conn->address)) {
              line = wanted;
              break;
            }
            break;
          default:
            tor_assert(0);
        }
      });
    if (!line || disable_all_conns) {
      /* This one isn't configured. Close it. */
      log_notice(LD_NET, "Closing no-longer-configured %s on %s:%d",
                 conn_type_to_string(type), conn->address, conn->port);
      if (replaced_conns) {
        smartlist_add(replaced_conns, conn);
      } else {
        connection_close_immediate(conn);
        connection_mark_for_close(conn);
      }
    } else {
      /* It's configured; we don't need to launch it. */
//      log_debug(LD_NET, "Already have %s on %s:%d",
//                conn_type_to_string(type), conn->address, conn->port);
      smartlist_remove(launch, line);
      if (free_launch_elts)
        config_free_lines(line);
    }
  });

  /* Now open all the listeners that are configured but not opened. */
  r = 0;
  if (!disable_all_conns) {
    SMARTLIST_FOREACH_BEGIN(launch, config_line_t *, cfg_line) {
        char *address = NULL;
        struct sockaddr *listensockaddr;
        socklen_t listensocklen = 0;

        switch (socket_family) {
          case AF_INET:
            listensockaddr = (struct sockaddr *)
                             create_inet_sockaddr(cfg_line->value,
                                                  (uint16_t) port_option,
                                                  &address, &listensocklen);
            break;
          case AF_UNIX:
            listensockaddr = (struct sockaddr *)
                             create_unix_sockaddr(cfg_line->value,
                                                  &address, &listensocklen);
            break;
          default:
            tor_assert(0);
        }

        if (listensockaddr) {
          conn = connection_create_listener(listensockaddr, listensocklen,
                                            type, address);
          tor_free(listensockaddr);
          tor_free(address);
        } else
          conn = NULL;

        if (!conn) {
          r = -1;
        } else {
          if (new_conns)
            smartlist_add(new_conns, conn);
        }
    } SMARTLIST_FOREACH_END(cfg_line);
  }

  if (free_launch_elts) {
    SMARTLIST_FOREACH(launch, config_line_t *, cfg_line,
                      config_free_lines(cfg_line));
  }
  smartlist_free(launch);

  return r;
}

/** Launch listeners for each port you should have open.  Only launch
 * listeners who are not already open, and only close listeners we no longer
 * want.
 *
 * Add all old conns that should be closed to <b>replaced_conns</b>.
 * Add all new connections to <b>new_conns</b>.
 */
int
retry_all_listeners(smartlist_t *replaced_conns,
                    smartlist_t *new_conns)
{
  or_options_t *options = get_options();

  if (retry_listeners(CONN_TYPE_OR_LISTENER, options->ORListenAddress,
                      options->ORPort, "0.0.0.0",
                      replaced_conns, new_conns, options->ClientOnly,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_DIR_LISTENER, options->DirListenAddress,
                      options->DirPort, "0.0.0.0",
                      replaced_conns, new_conns, options->ClientOnly,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_AP_LISTENER, options->SocksListenAddress,
                      options->SocksPort, "127.0.0.1",
                      replaced_conns, new_conns, 0,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_AP_TRANS_LISTENER, options->TransListenAddress,
                      options->TransPort, "127.0.0.1",
                      replaced_conns, new_conns, 0,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_AP_NATD_LISTENER, options->NATDListenAddress,
                      options->NATDPort, "127.0.0.1",
                      replaced_conns, new_conns, 0,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_AP_DNS_LISTENER, options->DNSListenAddress,
                      options->DNSPort, "127.0.0.1",
                      replaced_conns, new_conns, 0,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_CONTROL_LISTENER,
                      options->ControlListenAddress,
                      options->ControlPort, "127.0.0.1",
                      replaced_conns, new_conns, 0,
                      AF_INET)<0)
    return -1;
  if (retry_listeners(CONN_TYPE_CONTROL_LISTENER,
                      options->ControlSocket,
                      options->ControlSocket ? 1 : 0, NULL,
                      replaced_conns, new_conns, 0,
                      AF_UNIX)<0)
    return -1;

  return 0;
}

/** Return 1 if we should apply rate limiting to <b>conn</b>,
 * and 0 otherwise. Right now this just checks if it's an internal
 * IP address or an internal connection. */
static int
connection_is_rate_limited(connection_t *conn)
{
  if (conn->linked || /* internal connection */
      tor_addr_family(&conn->addr) == AF_UNSPEC || /* no address */
      tor_addr_is_internal(&conn->addr, 0)) /* internal address */
    return 0;
  else
    return 1;
}

extern int global_read_bucket, global_write_bucket;
extern int global_relayed_read_bucket, global_relayed_write_bucket;

/** Did either global write bucket run dry last second? If so,
 * we are likely to run dry again this second, so be stingy with the
 * tokens we just put in. */
static int write_buckets_empty_last_second = 0;

/** How many seconds of no active local circuits will make the
 * connection revert to the "relayed" bandwidth class? */
#define CLIENT_IDLE_TIME_FOR_PRIORITY 30

/** Return 1 if <b>conn</b> should use tokens from the "relayed"
 * bandwidth rates, else 0. Currently, only OR conns with bandwidth
 * class 1, and directory conns that are serving data out, count.
 */
static int
connection_counts_as_relayed_traffic(connection_t *conn, time_t now)
{
  if (conn->type == CONN_TYPE_OR &&
      TO_OR_CONN(conn)->client_used + CLIENT_IDLE_TIME_FOR_PRIORITY < now)
    return 1;
  if (conn->type == CONN_TYPE_DIR && DIR_CONN_IS_SERVER(conn))
    return 1;
  return 0;
}

/** Helper function to decide how many bytes out of <b>global_bucket</b>
 * we're willing to use for this transaction. <b>base</b> is the size
 * of a cell on the network; <b>priority</b> says whether we should
 * write many of them or just a few; and <b>conn_bucket</b> (if
 * non-negative) provides an upper limit for our answer. */
static ssize_t
connection_bucket_round_robin(int base, int priority,
                              ssize_t global_bucket, ssize_t conn_bucket)
{
  ssize_t at_most;
  ssize_t num_bytes_high = (priority ? 32 : 16) * base;
  ssize_t num_bytes_low = (priority ? 4 : 2) * base;

  /* Do a rudimentary round-robin so one circuit can't hog a connection.
   * Pick at most 32 cells, at least 4 cells if possible, and if we're in
   * the middle pick 1/8 of the available bandwidth. */
  at_most = global_bucket / 8;
  at_most -= (at_most % base); /* round down */
  if (at_most > num_bytes_high) /* 16 KB, or 8 KB for low-priority */
    at_most = num_bytes_high;
  else if (at_most < num_bytes_low) /* 2 KB, or 1 KB for low-priority */
    at_most = num_bytes_low;

  if (at_most > global_bucket)
    at_most = global_bucket;

  if (conn_bucket >= 0 && at_most > conn_bucket)
    at_most = conn_bucket;

  if (at_most < 0)
    return 0;
  return at_most;
}

/** How many bytes at most can we read onto this connection? */
static ssize_t
connection_bucket_read_limit(connection_t *conn, time_t now)
{
  int base = connection_speaks_cells(conn) ?
               CELL_NETWORK_SIZE : RELAY_PAYLOAD_SIZE;
  int priority = conn->type != CONN_TYPE_DIR;
  int conn_bucket = -1;
  int global_bucket = global_read_bucket;

  if (connection_speaks_cells(conn)) {
    or_connection_t *or_conn = TO_OR_CONN(conn);
    if (conn->state == OR_CONN_STATE_OPEN)
      conn_bucket = or_conn->read_bucket;
  }

  if (!connection_is_rate_limited(conn)) {
    /* be willing to read on local conns even if our buckets are empty */
    return conn_bucket>=0 ? conn_bucket : 1<<14;
  }

  if (connection_counts_as_relayed_traffic(conn, now) &&
      global_relayed_read_bucket <= global_read_bucket)
    global_bucket = global_relayed_read_bucket;

  return connection_bucket_round_robin(base, priority,
                                       global_bucket, conn_bucket);
}

/** How many bytes at most can we write onto this connection? */
ssize_t
connection_bucket_write_limit(connection_t *conn, time_t now)
{
  int base = connection_speaks_cells(conn) ?
               CELL_NETWORK_SIZE : RELAY_PAYLOAD_SIZE;
  int priority = conn->type != CONN_TYPE_DIR;
  int conn_bucket = (int)conn->outbuf_flushlen;
  int global_bucket = global_write_bucket;

  if (!connection_is_rate_limited(conn)) {
    /* be willing to write to local conns even if our buckets are empty */
    return conn->outbuf_flushlen;
  }

  if (connection_speaks_cells(conn)) {
    /* use the per-conn write limit if it's lower, but if it's less
     * than zero just use zero */
    or_connection_t *or_conn = TO_OR_CONN(conn);
    if (conn->state == OR_CONN_STATE_OPEN)
      if (or_conn->write_bucket < conn_bucket)
        conn_bucket = or_conn->write_bucket >= 0 ?
                        or_conn->write_bucket : 0;
  }

  if (connection_counts_as_relayed_traffic(conn, now) &&
      global_relayed_write_bucket <= global_write_bucket)
    global_bucket = global_relayed_write_bucket;

  return connection_bucket_round_robin(base, priority,
                                       global_bucket, conn_bucket);
}

/** Return 1 if the global write buckets are low enough that we
 * shouldn't send <b>attempt</b> bytes of low-priority directory stuff
 * out to <b>conn</b>. Else return 0.

 * Priority is 1 for v1 requests (directories and running-routers),
 * and 2 for v2 requests (statuses and descriptors). But see FFFF in
 * directory_handle_command_get() for why we don't use priority 2 yet.
 *
 * There are a lot of parameters we could use here:
 * - global_relayed_write_bucket. Low is bad.
 * - global_write_bucket. Low is bad.
 * - bandwidthrate. Low is bad.
 * - bandwidthburst. Not a big factor?
 * - attempt. High is bad.
 * - total bytes queued on outbufs. High is bad. But I'm wary of
 *   using this, since a few slow-flushing queues will pump up the
 *   number without meaning what we meant to mean. What we really
 *   mean is "total directory bytes added to outbufs recently", but
 *   that's harder to quantify and harder to keep track of.
 */
int
global_write_bucket_low(connection_t *conn, size_t attempt, int priority)
{
  int smaller_bucket = global_write_bucket < global_relayed_write_bucket ?
                       global_write_bucket : global_relayed_write_bucket;
  if (authdir_mode(get_options()) && priority>1)
    return 0; /* there's always room to answer v2 if we're an auth dir */

  if (!connection_is_rate_limited(conn))
    return 0; /* local conns don't get limited */

  if (smaller_bucket < (int)attempt)
    return 1; /* not enough space no matter the priority */

  if (write_buckets_empty_last_second)
    return 1; /* we're already hitting our limits, no more please */

  if (priority == 1) { /* old-style v1 query */
    /* Could we handle *two* of these requests within the next two seconds? */
    or_options_t *options = get_options();
    int64_t can_write = (int64_t)smaller_bucket
      + 2*(options->RelayBandwidthRate ? options->RelayBandwidthRate :
                                         options->BandwidthRate);
    if (can_write < 2*(int64_t)attempt)
      return 1;
  } else { /* v2 query */
    /* no further constraints yet */
  }
  return 0;
}

/** We just read <b>num_read</b> and wrote <b>num_written</b> bytes
 * onto <b>conn</b>. Decrement buckets appropriately. */
static void
connection_buckets_decrement(connection_t *conn, time_t now,
                             size_t num_read, size_t num_written)
{
  if (num_written >= INT_MAX || num_read >= INT_MAX) {
    log_err(LD_BUG, "Value out of range. num_read=%lu, num_written=%lu, "
             "connection type=%s, state=%s",
             (unsigned long)num_read, (unsigned long)num_written,
             conn_type_to_string(conn->type),
             conn_state_to_string(conn->type, conn->state));
    if (num_written >= INT_MAX) num_written = 1;
    if (num_read >= INT_MAX) num_read = 1;
    tor_fragile_assert();
  }

  /* Count bytes of answering direct and tunneled directory requests */
  if (conn->type == CONN_TYPE_DIR && conn->purpose == DIR_PURPOSE_SERVER) {
    if (num_read > 0)
      rep_hist_note_dir_bytes_read(num_read, now);
    if (num_written > 0)
      rep_hist_note_dir_bytes_written(num_written, now);
  }

  if (!connection_is_rate_limited(conn))
    return; /* local IPs are free */
  if (num_read > 0) {
    rep_hist_note_bytes_read(num_read, now);
  }
  if (num_written > 0) {
    rep_hist_note_bytes_written(num_written, now);
  }
  if (conn->type == CONN_TYPE_EXIT)
    rep_hist_note_exit_bytes(conn->port, num_written, num_read);

  if (connection_counts_as_relayed_traffic(conn, now)) {
    global_relayed_read_bucket -= (int)num_read;
    global_relayed_write_bucket -= (int)num_written;
  }
  global_read_bucket -= (int)num_read;
  global_write_bucket -= (int)num_written;
  if (connection_speaks_cells(conn) && conn->state == OR_CONN_STATE_OPEN) {
    TO_OR_CONN(conn)->read_bucket -= (int)num_read;
    TO_OR_CONN(conn)->write_bucket -= (int)num_written;
  }
}

/** If we have exhausted our global buckets, or the buckets for conn,
 * stop reading. */
static void
connection_consider_empty_read_buckets(connection_t *conn)
{
  const char *reason;

  if (global_read_bucket <= 0) {
    reason = "global read bucket exhausted. Pausing.";
  } else if (connection_counts_as_relayed_traffic(conn, approx_time()) &&
             global_relayed_read_bucket <= 0) {
    reason = "global relayed read bucket exhausted. Pausing.";
  } else if (connection_speaks_cells(conn) &&
             conn->state == OR_CONN_STATE_OPEN &&
             TO_OR_CONN(conn)->read_bucket <= 0) {
    reason = "connection read bucket exhausted. Pausing.";
  } else
    return; /* all good, no need to stop it */

  LOG_FN_CONN(conn, (LOG_DEBUG, LD_NET, "%s", reason));
  conn->read_blocked_on_bw = 1;
  connection_stop_reading(conn);
}

/** If we have exhausted our global buckets, or the buckets for conn,
 * stop writing. */
static void
connection_consider_empty_write_buckets(connection_t *conn)
{
  const char *reason;

  if (global_write_bucket <= 0) {
    reason = "global write bucket exhausted. Pausing.";
  } else if (connection_counts_as_relayed_traffic(conn, approx_time()) &&
             global_relayed_write_bucket <= 0) {
    reason = "global relayed write bucket exhausted. Pausing.";
  } else if (connection_speaks_cells(conn) &&
             conn->state == OR_CONN_STATE_OPEN &&
             TO_OR_CONN(conn)->write_bucket <= 0) {
    reason = "connection write bucket exhausted. Pausing.";
  } else
    return; /* all good, no need to stop it */

  LOG_FN_CONN(conn, (LOG_DEBUG, LD_NET, "%s", reason));
  conn->write_blocked_on_bw = 1;
  connection_stop_writing(conn);
}

/** Initialize the global read bucket to options-\>BandwidthBurst. */
void
connection_bucket_init(void)
{
  or_options_t *options = get_options();
  /* start it at max traffic */
  global_read_bucket = (int)options->BandwidthBurst;
  global_write_bucket = (int)options->BandwidthBurst;
  if (options->RelayBandwidthRate) {
    global_relayed_read_bucket = (int)options->RelayBandwidthBurst;
    global_relayed_write_bucket = (int)options->RelayBandwidthBurst;
  } else {
    global_relayed_read_bucket = (int)options->BandwidthBurst;
    global_relayed_write_bucket = (int)options->BandwidthBurst;
  }
}

/** Refill a single <b>bucket</b> called <b>name</b> with bandwidth rate
 * <b>rate</b> and bandwidth burst <b>burst</b>, assuming that
 * <b>seconds_elapsed</b> seconds have passed since the last call.
 **/
static void
connection_bucket_refill_helper(int *bucket, int rate, int burst,
                                int seconds_elapsed, const char *name)
{
  int starting_bucket = *bucket;
  if (starting_bucket < burst && seconds_elapsed) {
    if (((burst - starting_bucket)/seconds_elapsed) < rate) {
      *bucket = burst;  /* We would overflow the bucket; just set it to
                         * the maximum. */
    } else {
      int incr = rate*seconds_elapsed;
      *bucket += incr;
      if (*bucket > burst || *bucket < starting_bucket) {
        /* If we overflow the burst, or underflow our starting bucket,
         * cap the bucket value to burst. */
        /* XXXX this might be redundant now, but it doesn't show up
         * in profiles.  Remove it after analysis. */
        *bucket = burst;
      }
    }
    log(LOG_DEBUG, LD_NET,"%s now %d.", name, *bucket);
  }
}

/** A second has rolled over; increment buckets appropriately. */
void
connection_bucket_refill(int seconds_elapsed, time_t now)
{
  or_options_t *options = get_options();
  smartlist_t *conns = get_connection_array();
  int relayrate, relayburst;

  if (options->RelayBandwidthRate) {
    relayrate = (int)options->RelayBandwidthRate;
    relayburst = (int)options->RelayBandwidthBurst;
  } else {
    relayrate = (int)options->BandwidthRate;
    relayburst = (int)options->BandwidthBurst;
  }

  tor_assert(seconds_elapsed >= 0);

  write_buckets_empty_last_second =
    global_relayed_write_bucket <= 0 || global_write_bucket <= 0;

  /* refill the global buckets */
  connection_bucket_refill_helper(&global_read_bucket,
                                  (int)options->BandwidthRate,
                                  (int)options->BandwidthBurst,
                                  seconds_elapsed, "global_read_bucket");
  connection_bucket_refill_helper(&global_write_bucket,
                                  (int)options->BandwidthRate,
                                  (int)options->BandwidthBurst,
                                  seconds_elapsed, "global_write_bucket");
  connection_bucket_refill_helper(&global_relayed_read_bucket,
                                  relayrate, relayburst, seconds_elapsed,
                                  "global_relayed_read_bucket");
  connection_bucket_refill_helper(&global_relayed_write_bucket,
                                  relayrate, relayburst, seconds_elapsed,
                                  "global_relayed_write_bucket");

  /* refill the per-connection buckets */
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (connection_speaks_cells(conn)) {
      or_connection_t *or_conn = TO_OR_CONN(conn);
      if (connection_bucket_should_increase(or_conn->read_bucket, or_conn)) {
        connection_bucket_refill_helper(&or_conn->read_bucket,
                                        or_conn->bandwidthrate,
                                        or_conn->bandwidthburst,
                                        seconds_elapsed,
                                        "or_conn->read_bucket");
      }
      if (connection_bucket_should_increase(or_conn->write_bucket, or_conn)) {
        connection_bucket_refill_helper(&or_conn->write_bucket,
                                        or_conn->bandwidthrate,
                                        or_conn->bandwidthburst,
                                        seconds_elapsed,
                                        "or_conn->write_bucket");
      }
    }

    if (conn->read_blocked_on_bw == 1 /* marked to turn reading back on now */
        && global_read_bucket > 0 /* and we're allowed to read */
        && (!connection_counts_as_relayed_traffic(conn, now) ||
            global_relayed_read_bucket > 0) /* even if we're relayed traffic */
        && (!connection_speaks_cells(conn) ||
            conn->state != OR_CONN_STATE_OPEN ||
            TO_OR_CONN(conn)->read_bucket > 0)) {
        /* and either a non-cell conn or a cell conn with non-empty bucket */
      LOG_FN_CONN(conn, (LOG_DEBUG,LD_NET,
                         "waking up conn (fd %d) for read", conn->s));
      conn->read_blocked_on_bw = 0;
      connection_start_reading(conn);
    }

    if (conn->write_blocked_on_bw == 1
        && global_write_bucket > 0 /* and we're allowed to write */
        && (!connection_counts_as_relayed_traffic(conn, now) ||
            global_relayed_write_bucket > 0) /* even if it's relayed traffic */
        && (!connection_speaks_cells(conn) ||
            conn->state != OR_CONN_STATE_OPEN ||
            TO_OR_CONN(conn)->write_bucket > 0)) {
      LOG_FN_CONN(conn, (LOG_DEBUG,LD_NET,
                         "waking up conn (fd %d) for write", conn->s));
      conn->write_blocked_on_bw = 0;
      connection_start_writing(conn);
    }
  });
}

/** Is the <b>bucket</b> for connection <b>conn</b> low enough that we
 * should add another pile of tokens to it?
 */
static int
connection_bucket_should_increase(int bucket, or_connection_t *conn)
{
  tor_assert(conn);

  if (conn->_base.state != OR_CONN_STATE_OPEN)
    return 0; /* only open connections play the rate limiting game */
  if (bucket >= conn->bandwidthburst)
    return 0;

  return 1;
}

/** Read bytes from conn-\>s and process them.
 *
 * This function gets called from conn_read() in main.c, either
 * when poll() has declared that conn wants to read, or (for OR conns)
 * when there are pending TLS bytes.
 *
 * It calls connection_read_to_buf() to bring in any new bytes,
 * and then calls connection_process_inbuf() to process them.
 *
 * Mark the connection and return -1 if you want to close it, else
 * return 0.
 */
static int
connection_handle_read_impl(connection_t *conn)
{
  int max_to_read=-1, try_to_read;
  size_t before, n_read = 0;
  int socket_error = 0;

  if (conn->marked_for_close)
    return 0; /* do nothing */

  conn->timestamp_lastread = approx_time();

  switch (conn->type) {
    case CONN_TYPE_OR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_OR);
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_AP_TRANS_LISTENER:
    case CONN_TYPE_AP_NATD_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_AP);
    case CONN_TYPE_DIR_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_DIR);
    case CONN_TYPE_CONTROL_LISTENER:
      return connection_handle_listener_read(conn, CONN_TYPE_CONTROL);
    case CONN_TYPE_AP_DNS_LISTENER:
      /* This should never happen; eventdns.c handles the reads here. */
      tor_fragile_assert();
      return 0;
  }

 loop_again:
  try_to_read = max_to_read;
  tor_assert(!conn->marked_for_close);

  before = buf_datalen(conn->inbuf);
  if (connection_read_to_buf(conn, &max_to_read, &socket_error) < 0) {
    /* There's a read error; kill the connection.*/
    if (conn->type == CONN_TYPE_OR &&
        conn->state == OR_CONN_STATE_CONNECTING) {
      connection_or_connect_failed(TO_OR_CONN(conn),
                                   errno_to_orconn_end_reason(socket_error),
                                   tor_socket_strerror(socket_error));
    }
    if (CONN_IS_EDGE(conn)) {
      edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
      connection_edge_end_errno(edge_conn);
      if (edge_conn->socks_request) /* broken, don't send a socks reply back */
        edge_conn->socks_request->has_finished = 1;
    }
    connection_close_immediate(conn); /* Don't flush; connection is dead. */
    connection_mark_for_close(conn);
    return -1;
  }
  n_read += buf_datalen(conn->inbuf) - before;
  if (CONN_IS_EDGE(conn) && try_to_read != max_to_read) {
    /* instruct it not to try to package partial cells. */
    if (connection_process_inbuf(conn, 0) < 0) {
      return -1;
    }
    if (!conn->marked_for_close &&
        connection_is_reading(conn) &&
        !conn->inbuf_reached_eof &&
        max_to_read > 0)
      goto loop_again; /* try reading again, in case more is here now */
  }
  /* one last try, packaging partial cells and all. */
  if (!conn->marked_for_close &&
      connection_process_inbuf(conn, 1) < 0) {
    return -1;
  }
  if (conn->linked_conn) {
    /* The other side's handle_write() will never actually get called, so
     * we need to invoke the appropriate callbacks ourself. */
    connection_t *linked = conn->linked_conn;

    if (n_read) {
      /* Probably a no-op, since linked conns typically don't count for
       * bandwidth rate limiting. But do it anyway so we can keep stats
       * accurately. Note that since we read the bytes from conn, and
       * we're writing the bytes onto the linked connection, we count
       * these as <i>written</i> bytes. */
      connection_buckets_decrement(linked, approx_time(), 0, n_read);

      if (connection_flushed_some(linked) < 0)
        connection_mark_for_close(linked);
      if (!connection_wants_to_flush(linked))
        connection_finished_flushing(linked);
    }

    if (!buf_datalen(linked->outbuf) && conn->active_on_link)
      connection_stop_reading_from_linked_conn(conn);
  }
  /* If we hit the EOF, call connection_reached_eof(). */
  if (!conn->marked_for_close &&
      conn->inbuf_reached_eof &&
      connection_reached_eof(conn) < 0) {
    return -1;
  }
  return 0;
}

int
connection_handle_read(connection_t *conn)
{
  int res;

  tor_gettimeofday_cache_clear();
  res = connection_handle_read_impl(conn);
  return res;
}

/** Pull in new bytes from conn-\>s or conn-\>linked_conn onto conn-\>inbuf,
 * either directly or via TLS. Reduce the token buckets by the number of bytes
 * read.
 *
 * If *max_to_read is -1, then decide it ourselves, else go with the
 * value passed to us. When returning, if it's changed, subtract the
 * number of bytes we read from *max_to_read.
 *
 * Return -1 if we want to break conn, else return 0.
 */
static int
connection_read_to_buf(connection_t *conn, int *max_to_read, int *socket_error)
{
  int result;
  ssize_t at_most = *max_to_read;
  size_t slack_in_buf, more_to_read;
  size_t n_read = 0, n_written = 0;

  if (at_most == -1) { /* we need to initialize it */
    /* how many bytes are we allowed to read? */
    at_most = connection_bucket_read_limit(conn, approx_time());
  }

  slack_in_buf = buf_slack(conn->inbuf);
 again:
  if ((size_t)at_most > slack_in_buf && slack_in_buf >= 1024) {
    more_to_read = at_most - slack_in_buf;
    at_most = slack_in_buf;
  } else {
    more_to_read = 0;
  }

  if (connection_speaks_cells(conn) &&
      conn->state > OR_CONN_STATE_PROXY_HANDSHAKING) {
    int pending;
    or_connection_t *or_conn = TO_OR_CONN(conn);
    size_t initial_size;
    if (conn->state == OR_CONN_STATE_TLS_HANDSHAKING ||
        conn->state == OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING) {
      /* continue handshaking even if global token bucket is empty */
      return connection_tls_continue_handshake(or_conn);
    }

    log_debug(LD_NET,
              "%d: starting, inbuf_datalen %ld (%d pending in tls object)."
              " at_most %ld.",
              conn->s,(long)buf_datalen(conn->inbuf),
              tor_tls_get_pending_bytes(or_conn->tls), (long)at_most);

    initial_size = buf_datalen(conn->inbuf);
    /* else open, or closing */
    result = read_to_buf_tls(or_conn->tls, at_most, conn->inbuf);
    if (TOR_TLS_IS_ERROR(result) || result == TOR_TLS_CLOSE)
      or_conn->tls_error = result;
    else
      or_conn->tls_error = 0;

    switch (result) {
      case TOR_TLS_CLOSE:
      case TOR_TLS_ERROR_IO:
        log_debug(LD_NET,"TLS connection closed %son read. Closing. "
                 "(Nickname %s, address %s)",
                 result == TOR_TLS_CLOSE ? "cleanly " : "",
                 or_conn->nickname ? or_conn->nickname : "not set",
                 conn->address);
        return result;
      CASE_TOR_TLS_ERROR_ANY_NONIO:
        log_debug(LD_NET,"tls error [%s]. breaking (nickname %s, address %s).",
                 tor_tls_err_to_string(result),
                 or_conn->nickname ? or_conn->nickname : "not set",
                 conn->address);
        return result;
      case TOR_TLS_WANTWRITE:
        connection_start_writing(conn);
        return 0;
      case TOR_TLS_WANTREAD: /* we're already reading */
      case TOR_TLS_DONE: /* no data read, so nothing to process */
        result = 0;
        break; /* so we call bucket_decrement below */
      default:
        break;
    }
    pending = tor_tls_get_pending_bytes(or_conn->tls);
    if (pending) {
      /* If we have any pending bytes, we read them now.  This *can*
       * take us over our read allotment, but really we shouldn't be
       * believing that SSL bytes are the same as TCP bytes anyway. */
      int r2 = read_to_buf_tls(or_conn->tls, pending, conn->inbuf);
      if (r2<0) {
        log_warn(LD_BUG, "apparently, reading pending bytes can fail.");
        return -1;
      }
    }
    result = (int)(buf_datalen(conn->inbuf)-initial_size);
    tor_tls_get_n_raw_bytes(or_conn->tls, &n_read, &n_written);
    log_debug(LD_GENERAL, "After TLS read of %d: %ld read, %ld written",
              result, (long)n_read, (long)n_written);
  } else if (conn->linked) {
    if (conn->linked_conn) {
      result = move_buf_to_buf(conn->inbuf, conn->linked_conn->outbuf,
                               &conn->linked_conn->outbuf_flushlen);
    } else {
      result = 0;
    }
    //log_notice(LD_GENERAL, "Moved %d bytes on an internal link!", result);
    /* If the other side has disappeared, or if it's been marked for close and
     * we flushed its outbuf, then we should set our inbuf_reached_eof. */
    if (!conn->linked_conn ||
        (conn->linked_conn->marked_for_close &&
         buf_datalen(conn->linked_conn->outbuf) == 0))
      conn->inbuf_reached_eof = 1;

    n_read = (size_t) result;
  } else {
    /* !connection_speaks_cells, !conn->linked_conn. */
    int reached_eof = 0;
    CONN_LOG_PROTECT(conn,
        result = read_to_buf(conn->s, at_most, conn->inbuf, &reached_eof,
                             socket_error));
    if (reached_eof)
      conn->inbuf_reached_eof = 1;

//  log_fn(LOG_DEBUG,"read_to_buf returned %d.",read_result);

    if (result < 0)
      return -1;
    n_read = (size_t) result;
  }

  if (n_read > 0) { /* change *max_to_read */
    /*XXXX021 check for overflow*/
    *max_to_read = (int)(at_most - n_read);
  }

  if (conn->type == CONN_TYPE_AP) {
    edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
    /*XXXX021 check for overflow*/
    edge_conn->n_read += (int)n_read;
  }

  connection_buckets_decrement(conn, approx_time(), n_read, n_written);

  if (more_to_read && result == at_most) {
    slack_in_buf = buf_slack(conn->inbuf);
    at_most = more_to_read;
    goto again;
  }

  /* Call even if result is 0, since the global read bucket may
   * have reached 0 on a different conn, and this guy needs to
   * know to stop reading. */
  connection_consider_empty_read_buckets(conn);
  if (n_written > 0 && connection_is_writing(conn))
    connection_consider_empty_write_buckets(conn);

  return 0;
}

/** A pass-through to fetch_from_buf. */
int
connection_fetch_from_buf(char *string, size_t len, connection_t *conn)
{
  return fetch_from_buf(string, len, conn->inbuf);
}

/** Return conn-\>outbuf_flushlen: how many bytes conn wants to flush
 * from its outbuf. */
int
connection_wants_to_flush(connection_t *conn)
{
  return conn->outbuf_flushlen > 0;
}

/** Are there too many bytes on edge connection <b>conn</b>'s outbuf to
 * send back a relay-level sendme yet? Return 1 if so, 0 if not. Used by
 * connection_edge_consider_sending_sendme().
 */
int
connection_outbuf_too_full(connection_t *conn)
{
  return (conn->outbuf_flushlen > 10*CELL_PAYLOAD_SIZE);
}

/** Try to flush more bytes onto conn-\>s.
 *
 * This function gets called either from conn_write() in main.c
 * when poll() has declared that conn wants to write, or below
 * from connection_write_to_buf() when an entire TLS record is ready.
 *
 * Update conn-\>timestamp_lastwritten to now, and call flush_buf
 * or flush_buf_tls appropriately. If it succeeds and there are no more
 * more bytes on conn->outbuf, then call connection_finished_flushing
 * on it too.
 *
 * If <b>force</b>, then write as many bytes as possible, ignoring bandwidth
 * limits.  (Used for flushing messages to controller connections on fatal
 * errors.)
 *
 * Mark the connection and return -1 if you want to close it, else
 * return 0.
 */
static int
connection_handle_write_impl(connection_t *conn, int force)
{
  int e;
  socklen_t len=(socklen_t)sizeof(e);
  int result;
  ssize_t max_to_write;
  time_t now = approx_time();
  size_t n_read = 0, n_written = 0;

  tor_assert(!connection_is_listener(conn));

  if (conn->marked_for_close || conn->s < 0)
    return 0; /* do nothing */

  if (conn->in_flushed_some) {
    log_warn(LD_BUG, "called recursively from inside conn->in_flushed_some");
    return 0;
  }

  conn->timestamp_lastwritten = now;

  /* Sometimes, "writable" means "connected". */
  if (connection_state_is_connecting(conn)) {
    if (getsockopt(conn->s, SOL_SOCKET, SO_ERROR, (void*)&e, &len) < 0) {
      log_warn(LD_BUG,
               "getsockopt() syscall failed?! Please report to tor-ops.");
      if (CONN_IS_EDGE(conn))
        connection_edge_end_errno(TO_EDGE_CONN(conn));
      connection_mark_for_close(conn);
      return -1;
    }
    if (e) {
      /* some sort of error, but maybe just inprogress still */
      if (!ERRNO_IS_CONN_EINPROGRESS(e)) {
        log_info(LD_NET,"in-progress connect failed. Removing. (%s)",
                 tor_socket_strerror(e));
        if (CONN_IS_EDGE(conn))
          connection_edge_end_errno(TO_EDGE_CONN(conn));
        if (conn->type == CONN_TYPE_OR)
          connection_or_connect_failed(TO_OR_CONN(conn),
                                       errno_to_orconn_end_reason(e),
                                       tor_socket_strerror(e));

        connection_close_immediate(conn);
        connection_mark_for_close(conn);
        return -1;
      } else {
        return 0; /* no change, see if next time is better */
      }
    }
    /* The connection is successful. */
    if (connection_finished_connecting(conn)<0)
      return -1;
  }

  max_to_write = force ? (ssize_t)conn->outbuf_flushlen
    : connection_bucket_write_limit(conn, now);

  if (connection_speaks_cells(conn) &&
      conn->state > OR_CONN_STATE_PROXY_HANDSHAKING) {
    or_connection_t *or_conn = TO_OR_CONN(conn);
    if (conn->state == OR_CONN_STATE_TLS_HANDSHAKING ||
        conn->state == OR_CONN_STATE_TLS_CLIENT_RENEGOTIATING) {
      connection_stop_writing(conn);
      if (connection_tls_continue_handshake(or_conn) < 0) {
        /* Don't flush; connection is dead. */
        connection_close_immediate(conn);
        connection_mark_for_close(conn);
        return -1;
      }
      return 0;
    } else if (conn->state == OR_CONN_STATE_TLS_SERVER_RENEGOTIATING) {
      return connection_handle_read(conn);
    }

    /* else open, or closing */
    result = flush_buf_tls(or_conn->tls, conn->outbuf,
                           max_to_write, &conn->outbuf_flushlen);

    /* If we just flushed the last bytes, check if this tunneled dir
     * request is done. */
    if (buf_datalen(conn->outbuf) == 0 && conn->dirreq_id)
      geoip_change_dirreq_state(conn->dirreq_id, DIRREQ_TUNNELED,
                                DIRREQ_OR_CONN_BUFFER_FLUSHED);

    switch (result) {
      CASE_TOR_TLS_ERROR_ANY:
      case TOR_TLS_CLOSE:
        log_info(LD_NET,result!=TOR_TLS_CLOSE?
                 "tls error. breaking.":"TLS connection closed on flush");
        /* Don't flush; connection is dead. */
        connection_close_immediate(conn);
        connection_mark_for_close(conn);
        return -1;
      case TOR_TLS_WANTWRITE:
        log_debug(LD_NET,"wanted write.");
        /* we're already writing */
        return 0;
      case TOR_TLS_WANTREAD:
        /* Make sure to avoid a loop if the receive buckets are empty. */
        log_debug(LD_NET,"wanted read.");
        if (!connection_is_reading(conn)) {
          connection_stop_writing(conn);
          conn->write_blocked_on_bw = 1;
          /* we'll start reading again when we get more tokens in our
           * read bucket; then we'll start writing again too.
           */
        }
        /* else no problem, we're already reading */
        return 0;
      /* case TOR_TLS_DONE:
       * for TOR_TLS_DONE, fall through to check if the flushlen
       * is empty, so we can stop writing.
       */
    }

    tor_tls_get_n_raw_bytes(or_conn->tls, &n_read, &n_written);
    log_debug(LD_GENERAL, "After TLS write of %d: %ld read, %ld written",
              result, (long)n_read, (long)n_written);
  } else {
    CONN_LOG_PROTECT(conn,
             result = flush_buf(conn->s, conn->outbuf,
                                max_to_write, &conn->outbuf_flushlen));
    if (result < 0) {
      if (CONN_IS_EDGE(conn))
        connection_edge_end_errno(TO_EDGE_CONN(conn));

      connection_close_immediate(conn); /* Don't flush; connection is dead. */
      connection_mark_for_close(conn);
      return -1;
    }
    n_written = (size_t) result;
  }

  if (conn->type == CONN_TYPE_AP) {
    edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
    /*XXXX021 check for overflow.*/
    edge_conn->n_written += (int)n_written;
  }

  connection_buckets_decrement(conn, approx_time(), n_read, n_written);

  if (result > 0) {
    /* If we wrote any bytes from our buffer, then call the appropriate
     * functions. */
    if (connection_flushed_some(conn) < 0)
      connection_mark_for_close(conn);
  }

  if (!connection_wants_to_flush(conn)) { /* it's done flushing */
    if (connection_finished_flushing(conn) < 0) {
      /* already marked */
      return -1;
    }
    return 0;
  }

  /* Call even if result is 0, since the global write bucket may
   * have reached 0 on a different conn, and this guy needs to
   * know to stop writing. */
  connection_consider_empty_write_buckets(conn);
  if (n_read > 0 && connection_is_reading(conn))
    connection_consider_empty_read_buckets(conn);

  return 0;
}

int
connection_handle_write(connection_t *conn, int force)
{
    int res;
    tor_gettimeofday_cache_clear();
    res = connection_handle_write_impl(conn, force);
    return res;
}

/** OpenSSL TLS record size is 16383; this is close. The goal here is to
 * push data out as soon as we know there's enough for a TLS record, so
 * during periods of high load we won't read entire megabytes from
 * input before pushing any data out. It also has the feature of not
 * growing huge outbufs unless something is slow. */
#define MIN_TLS_FLUSHLEN 15872

/** Append <b>len</b> bytes of <b>string</b> onto <b>conn</b>'s
 * outbuf, and ask it to start writing.
 *
 * If <b>zlib</b> is nonzero, this is a directory connection that should get
 * its contents compressed or decompressed as they're written.  If zlib is
 * negative, this is the last data to be compressed, and the connection's zlib
 * state should be flushed.
 *
 * If it's an OR conn and an entire TLS record is ready, then try to
 * flush the record now. Similarly, if it's a local control connection
 * and a 64k chunk is ready, try to flush it all, so we don't end up with
 * many megabytes of controller info queued at once.
 */
void
_connection_write_to_buf_impl(const char *string, size_t len,
                              connection_t *conn, int zlib)
{
  /* XXXX This function really needs to return -1 on failure. */
  int r;
  size_t old_datalen;
  if (!len && !(zlib<0))
    return;
  /* if it's marked for close, only allow write if we mean to flush it */
  if (conn->marked_for_close && !conn->hold_open_until_flushed)
    return;

  old_datalen = buf_datalen(conn->outbuf);
  if (zlib) {
    dir_connection_t *dir_conn = TO_DIR_CONN(conn);
    int done = zlib < 0;
    CONN_LOG_PROTECT(conn, r = write_to_buf_zlib(conn->outbuf,
                                                 dir_conn->zlib_state,
                                                 string, len, done));
  } else {
    CONN_LOG_PROTECT(conn, r = write_to_buf(string, len, conn->outbuf));
  }
  if (r < 0) {
    if (CONN_IS_EDGE(conn)) {
      /* if it failed, it means we have our package/delivery windows set
         wrong compared to our max outbuf size. close the whole circuit. */
      log_warn(LD_NET,
               "write_to_buf failed. Closing circuit (fd %d).", conn->s);
      circuit_mark_for_close(circuit_get_by_edge_conn(TO_EDGE_CONN(conn)),
                             END_CIRC_REASON_INTERNAL);
    } else {
      log_warn(LD_NET,
               "write_to_buf failed. Closing connection (fd %d).", conn->s);
      connection_mark_for_close(conn);
    }
    return;
  }

  connection_start_writing(conn);
  if (zlib) {
    conn->outbuf_flushlen += buf_datalen(conn->outbuf) - old_datalen;
  } else {
    ssize_t extra = 0;
    conn->outbuf_flushlen += len;

    /* Should we try flushing the outbuf now? */
    if (conn->in_flushed_some) {
      /* Don't flush the outbuf when the reason we're writing more stuff is
       * _because_ we flushed the outbuf.  That's unfair. */
      return;
    }

    if (conn->type == CONN_TYPE_OR &&
        conn->outbuf_flushlen-len < MIN_TLS_FLUSHLEN &&
        conn->outbuf_flushlen >= MIN_TLS_FLUSHLEN) {
      /* We just pushed outbuf_flushlen to MIN_TLS_FLUSHLEN or above;
       * we can send out a full TLS frame now if we like. */
      extra = conn->outbuf_flushlen - MIN_TLS_FLUSHLEN;
      conn->outbuf_flushlen = MIN_TLS_FLUSHLEN;
    } else if (conn->type == CONN_TYPE_CONTROL &&
               !connection_is_rate_limited(conn) &&
               conn->outbuf_flushlen-len < 1<<16 &&
               conn->outbuf_flushlen >= 1<<16) {
      /* just try to flush all of it */
    } else
      return; /* no need to try flushing */

    if (connection_handle_write(conn, 0) < 0) {
      if (!conn->marked_for_close) {
        /* this connection is broken. remove it. */
        log_warn(LD_BUG, "unhandled error on write for "
                 "conn (type %d, fd %d); removing",
                 conn->type, conn->s);
        tor_fragile_assert();
        /* do a close-immediate here, so we don't try to flush */
        connection_close_immediate(conn);
      }
      return;
    }
    if (extra) {
      conn->outbuf_flushlen += extra;
      connection_start_writing(conn);
    }
  }
}

/** Return a connection with given type, address, port, and purpose;
 * or NULL if no such connection exists. */
connection_t *
connection_get_by_type_addr_port_purpose(int type,
                                         const tor_addr_t *addr, uint16_t port,
                                         int purpose)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == type &&
        tor_addr_eq(&conn->addr, addr) &&
        conn->port == port &&
        conn->purpose == purpose &&
        !conn->marked_for_close)
      return conn;
  });
  return NULL;
}

/** Return the stream with id <b>id</b> if it is not already marked for
 * close.
 */
connection_t *
connection_get_by_global_id(uint64_t id)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->global_identifier == id)
      return conn;
  });
  return NULL;
}

/** Return a connection of type <b>type</b> that is not marked for close.
 */
connection_t *
connection_get_by_type(int type)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == type && !conn->marked_for_close)
      return conn;
  });
  return NULL;
}

/** Return a connection of type <b>type</b> that is in state <b>state</b>,
 * and that is not marked for close.
 */
connection_t *
connection_get_by_type_state(int type, int state)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == type && conn->state == state && !conn->marked_for_close)
      return conn;
  });
  return NULL;
}

/** Return a connection of type <b>type</b> that has rendquery equal
 * to <b>rendquery</b>, and that is not marked for close. If state
 * is non-zero, conn must be of that state too.
 */
connection_t *
connection_get_by_type_state_rendquery(int type, int state,
                                       const char *rendquery)
{
  smartlist_t *conns = get_connection_array();

  tor_assert(type == CONN_TYPE_DIR ||
             type == CONN_TYPE_AP || type == CONN_TYPE_EXIT);
  tor_assert(rendquery);

  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == type &&
        !conn->marked_for_close &&
        (!state || state == conn->state)) {
      if (type == CONN_TYPE_DIR &&
          TO_DIR_CONN(conn)->rend_data &&
          !rend_cmp_service_ids(rendquery,
                                TO_DIR_CONN(conn)->rend_data->onion_address))
        return conn;
      else if (CONN_IS_EDGE(conn) &&
               TO_EDGE_CONN(conn)->rend_data &&
               !rend_cmp_service_ids(rendquery,
                            TO_EDGE_CONN(conn)->rend_data->onion_address))
        return conn;
    }
  });
  return NULL;
}

/** Return an open, non-marked connection of a given type and purpose, or NULL
 * if no such connection exists. */
connection_t *
connection_get_by_type_purpose(int type, int purpose)
{
  smartlist_t *conns = get_connection_array();
  SMARTLIST_FOREACH(conns, connection_t *, conn,
  {
    if (conn->type == type &&
        !conn->marked_for_close &&
        (purpose == conn->purpose))
      return conn;
  });
  return NULL;
}

/** Return 1 if <b>conn</b> is a listener conn, else return 0. */
int
connection_is_listener(connection_t *conn)
{
  if (conn->type == CONN_TYPE_OR_LISTENER ||
      conn->type == CONN_TYPE_AP_LISTENER ||
      conn->type == CONN_TYPE_AP_TRANS_LISTENER ||
      conn->type == CONN_TYPE_AP_DNS_LISTENER ||
      conn->type == CONN_TYPE_AP_NATD_LISTENER ||
      conn->type == CONN_TYPE_DIR_LISTENER ||
      conn->type == CONN_TYPE_CONTROL_LISTENER)
    return 1;
  return 0;
}

/** Return 1 if <b>conn</b> is in state "open" and is not marked
 * for close, else return 0.
 */
int
connection_state_is_open(connection_t *conn)
{
  tor_assert(conn);

  if (conn->marked_for_close)
    return 0;

  if ((conn->type == CONN_TYPE_OR && conn->state == OR_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_AP && conn->state == AP_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_EXIT && conn->state == EXIT_CONN_STATE_OPEN) ||
      (conn->type == CONN_TYPE_CONTROL &&
       conn->state == CONTROL_CONN_STATE_OPEN))
    return 1;

  return 0;
}

/** Return 1 if conn is in 'connecting' state, else return 0. */
int
connection_state_is_connecting(connection_t *conn)
{
  tor_assert(conn);

  if (conn->marked_for_close)
    return 0;
  switch (conn->type)
    {
    case CONN_TYPE_OR:
      return conn->state == OR_CONN_STATE_CONNECTING;
    case CONN_TYPE_EXIT:
      return conn->state == EXIT_CONN_STATE_CONNECTING;
    case CONN_TYPE_DIR:
      return conn->state == DIR_CONN_STATE_CONNECTING;
    }

  return 0;
}

/** Allocates a base64'ed authenticator for use in http or https
 * auth, based on the input string <b>authenticator</b>. Returns it
 * if success, else returns NULL. */
char *
alloc_http_authenticator(const char *authenticator)
{
  /* an authenticator in Basic authentication
   * is just the string "username:password" */
  const size_t authenticator_length = strlen(authenticator);
  /* The base64_encode function needs a minimum buffer length
   * of 66 bytes. */
  const size_t base64_authenticator_length = (authenticator_length/48+1)*66;
  char *base64_authenticator = tor_malloc(base64_authenticator_length);
  if (base64_encode(base64_authenticator, base64_authenticator_length,
                    authenticator, authenticator_length) < 0) {
    tor_free(base64_authenticator); /* free and set to null */
  } else {
    /* remove extra \n at end of encoding */
    base64_authenticator[strlen(base64_authenticator) - 1] = 0;
  }
  return base64_authenticator;
}

/** Given a socket handle, check whether the local address (sockname) of the
 * socket is one that we've connected from before.  If so, double-check
 * whether our address has changed and we need to generate keys.  If we do,
 * call init_keys().
 */
static void
client_check_address_changed(int sock)
{
  uint32_t iface_ip, ip_out; /* host order */
  struct sockaddr_in out_addr;
  socklen_t out_addr_len = (socklen_t) sizeof(out_addr);
  uint32_t *ip; /* host order */

  if (!last_interface_ip)
    get_interface_address(LOG_INFO, &last_interface_ip);
  if (!outgoing_addrs)
    outgoing_addrs = smartlist_create();

  if (getsockname(sock, (struct sockaddr*)&out_addr, &out_addr_len)<0) {
    int e = tor_socket_errno(sock);
    log_warn(LD_NET, "getsockname() to check for address change failed: %s",
             tor_socket_strerror(e));
    return;
  }

  /* If we've used this address previously, we're okay. */
  ip_out = ntohl(out_addr.sin_addr.s_addr);
  SMARTLIST_FOREACH(outgoing_addrs, uint32_t*, ip_ptr,
                    if (*ip_ptr == ip_out) return;
                    );

  /* Uh-oh.  We haven't connected from this address before. Has the interface
   * address changed? */
  if (get_interface_address(LOG_INFO, &iface_ip)<0)
    return;
  ip = tor_malloc(sizeof(uint32_t));
  *ip = ip_out;

  if (iface_ip == last_interface_ip) {
    /* Nope, it hasn't changed.  Add this address to the list. */
    smartlist_add(outgoing_addrs, ip);
  } else {
    /* The interface changed.  We're a client, so we need to regenerate our
     * keys.  First, reset the state. */
    log(LOG_NOTICE, LD_NET, "Our IP address has changed.  Rotating keys...");
    last_interface_ip = iface_ip;
    SMARTLIST_FOREACH(outgoing_addrs, void*, ip_ptr, tor_free(ip_ptr));
    smartlist_clear(outgoing_addrs);
    smartlist_add(outgoing_addrs, ip);
    /* Okay, now change our keys. */
    ip_address_changed(1);
  }
}

/** Some systems have limited system buffers for recv and xmit on
 * sockets allocated in a virtual server or similar environment. For a Tor
 * server this can produce the "Error creating network socket: No buffer
 * space available" error once all available TCP buffer space is consumed.
 * This method will attempt to constrain the buffers allocated for the socket
 * to the desired size to stay below system TCP buffer limits.
 */
static void
set_constrained_socket_buffers(int sock, int size)
{
  void *sz = (void*)&size;
  socklen_t sz_sz = (socklen_t) sizeof(size);
  if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, sz, sz_sz) < 0) {
    int e = tor_socket_errno(sock);
    log_warn(LD_NET, "setsockopt() to constrain send "
             "buffer to %d bytes failed: %s", size, tor_socket_strerror(e));
  }
  if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, sz, sz_sz) < 0) {
    int e = tor_socket_errno(sock);
    log_warn(LD_NET, "setsockopt() to constrain recv "
             "buffer to %d bytes failed: %s", size, tor_socket_strerror(e));
  }
}

/** Process new bytes that have arrived on conn-\>inbuf.
 *
 * This function just passes conn to the connection-specific
 * connection_*_process_inbuf() function. It also passes in
 * package_partial if wanted.
 */
static int
connection_process_inbuf(connection_t *conn, int package_partial)
{
  tor_assert(conn);

  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_process_inbuf(TO_OR_CONN(conn));
    case CONN_TYPE_EXIT:
    case CONN_TYPE_AP:
      return connection_edge_process_inbuf(TO_EDGE_CONN(conn),
                                           package_partial);
    case CONN_TYPE_DIR:
      return connection_dir_process_inbuf(TO_DIR_CONN(conn));
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_process_inbuf(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_process_inbuf(TO_CONTROL_CONN(conn));
    default:
      log_err(LD_BUG,"got unexpected conn type %d.", conn->type);
      tor_fragile_assert();
      return -1;
  }
}

/** Called whenever we've written data on a connection. */
static int
connection_flushed_some(connection_t *conn)
{
  int r = 0;
  tor_assert(!conn->in_flushed_some);
  conn->in_flushed_some = 1;
  if (conn->type == CONN_TYPE_DIR &&
      conn->state == DIR_CONN_STATE_SERVER_WRITING) {
    r = connection_dirserv_flushed_some(TO_DIR_CONN(conn));
  } else if (conn->type == CONN_TYPE_OR) {
    r = connection_or_flushed_some(TO_OR_CONN(conn));
  } else if (CONN_IS_EDGE(conn)) {
    r = connection_edge_flushed_some(TO_EDGE_CONN(conn));
  }
  conn->in_flushed_some = 0;
  return r;
}

/** We just finished flushing bytes from conn-\>outbuf, and there
 * are no more bytes remaining.
 *
 * This function just passes conn to the connection-specific
 * connection_*_finished_flushing() function.
 */
static int
connection_finished_flushing(connection_t *conn)
{
  tor_assert(conn);

  /* If the connection is closed, don't try to do anything more here. */
  if (CONN_IS_CLOSED(conn))
    return 0;

//  log_fn(LOG_DEBUG,"entered. Socket %u.", conn->s);

  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_finished_flushing(TO_OR_CONN(conn));
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_finished_flushing(TO_EDGE_CONN(conn));
    case CONN_TYPE_DIR:
      return connection_dir_finished_flushing(TO_DIR_CONN(conn));
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_finished_flushing(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_finished_flushing(TO_CONTROL_CONN(conn));
    default:
      log_err(LD_BUG,"got unexpected conn type %d.", conn->type);
      tor_fragile_assert();
      return -1;
  }
}

/** Called when our attempt to connect() to another server has just
 * succeeded.
 *
 * This function just passes conn to the connection-specific
 * connection_*_finished_connecting() function.
 */
static int
connection_finished_connecting(connection_t *conn)
{
  tor_assert(conn);
  switch (conn->type)
    {
    case CONN_TYPE_OR:
      return connection_or_finished_connecting(TO_OR_CONN(conn));
    case CONN_TYPE_EXIT:
      return connection_edge_finished_connecting(TO_EDGE_CONN(conn));
    case CONN_TYPE_DIR:
      return connection_dir_finished_connecting(TO_DIR_CONN(conn));
    default:
      log_err(LD_BUG,"got unexpected conn type %d.", conn->type);
      tor_fragile_assert();
      return -1;
  }
}

/** Callback: invoked when a connection reaches an EOF event. */
static int
connection_reached_eof(connection_t *conn)
{
  switch (conn->type) {
    case CONN_TYPE_OR:
      return connection_or_reached_eof(TO_OR_CONN(conn));
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      return connection_edge_reached_eof(TO_EDGE_CONN(conn));
    case CONN_TYPE_DIR:
      return connection_dir_reached_eof(TO_DIR_CONN(conn));
    case CONN_TYPE_CPUWORKER:
      return connection_cpu_reached_eof(conn);
    case CONN_TYPE_CONTROL:
      return connection_control_reached_eof(TO_CONTROL_CONN(conn));
    default:
      log_err(LD_BUG,"got unexpected conn type %d.", conn->type);
      tor_fragile_assert();
      return -1;
  }
}

/** Log how many bytes are used by buffers of different kinds and sizes. */
void
connection_dump_buffer_mem_stats(int severity)
{
  uint64_t used_by_type[_CONN_TYPE_MAX+1];
  uint64_t alloc_by_type[_CONN_TYPE_MAX+1];
  int n_conns_by_type[_CONN_TYPE_MAX+1];
  uint64_t total_alloc = 0;
  uint64_t total_used = 0;
  int i;
  smartlist_t *conns = get_connection_array();

  memset(used_by_type, 0, sizeof(used_by_type));
  memset(alloc_by_type, 0, sizeof(alloc_by_type));
  memset(n_conns_by_type, 0, sizeof(n_conns_by_type));

  SMARTLIST_FOREACH(conns, connection_t *, c,
  {
    int tp = c->type;
    ++n_conns_by_type[tp];
    if (c->inbuf) {
      used_by_type[tp] += buf_datalen(c->inbuf);
      alloc_by_type[tp] += buf_allocation(c->inbuf);
    }
    if (c->outbuf) {
      used_by_type[tp] += buf_datalen(c->outbuf);
      alloc_by_type[tp] += buf_allocation(c->outbuf);
    }
  });
  for (i=0; i <= _CONN_TYPE_MAX; ++i) {
    total_used += used_by_type[i];
    total_alloc += alloc_by_type[i];
  }

  log(severity, LD_GENERAL,
     "In buffers for %d connections: "U64_FORMAT" used/"U64_FORMAT" allocated",
      smartlist_len(conns),
      U64_PRINTF_ARG(total_used), U64_PRINTF_ARG(total_alloc));
  for (i=_CONN_TYPE_MIN; i <= _CONN_TYPE_MAX; ++i) {
    if (!n_conns_by_type[i])
      continue;
    log(severity, LD_GENERAL,
        "  For %d %s connections: "U64_FORMAT" used/"U64_FORMAT" allocated",
        n_conns_by_type[i], conn_type_to_string(i),
        U64_PRINTF_ARG(used_by_type[i]), U64_PRINTF_ARG(alloc_by_type[i]));
  }
}

/** Verify that connection <b>conn</b> has all of its invariants
 * correct. Trigger an assert if anything is invalid.
 */
void
assert_connection_ok(connection_t *conn, time_t now)
{
  (void) now; /* XXXX unused. */
  tor_assert(conn);
  tor_assert(conn->type >= _CONN_TYPE_MIN);
  tor_assert(conn->type <= _CONN_TYPE_MAX);
  switch (conn->type) {
    case CONN_TYPE_OR:
      tor_assert(conn->magic == OR_CONNECTION_MAGIC);
      break;
    case CONN_TYPE_AP:
    case CONN_TYPE_EXIT:
      tor_assert(conn->magic == EDGE_CONNECTION_MAGIC);
      break;
    case CONN_TYPE_DIR:
      tor_assert(conn->magic == DIR_CONNECTION_MAGIC);
      break;
    case CONN_TYPE_CONTROL:
      tor_assert(conn->magic == CONTROL_CONNECTION_MAGIC);
      break;
    default:
      tor_assert(conn->magic == BASE_CONNECTION_MAGIC);
      break;
  }

  if (conn->linked_conn) {
    tor_assert(conn->linked_conn->linked_conn == conn);
    tor_assert(conn->linked);
  }
  if (conn->linked)
    tor_assert(conn->s < 0);

  if (conn->outbuf_flushlen > 0) {
    tor_assert(connection_is_writing(conn) || conn->write_blocked_on_bw ||
            (CONN_IS_EDGE(conn) && TO_EDGE_CONN(conn)->edge_blocked_on_circ));
  }

  if (conn->hold_open_until_flushed)
    tor_assert(conn->marked_for_close);

  /* XXXX check: read_blocked_on_bw, write_blocked_on_bw, s, conn_array_index,
   * marked_for_close. */

  /* buffers */
  if (!connection_is_listener(conn)) {
    assert_buf_ok(conn->inbuf);
    assert_buf_ok(conn->outbuf);
  }

  if (conn->type == CONN_TYPE_OR) {
    or_connection_t *or_conn = TO_OR_CONN(conn);
    if (conn->state == OR_CONN_STATE_OPEN) {
      /* tor_assert(conn->bandwidth > 0); */
      /* the above isn't necessarily true: if we just did a TLS
       * handshake but we didn't recognize the other peer, or it
       * gave a bad cert/etc, then we won't have assigned bandwidth,
       * yet it will be open. -RD
       */
//      tor_assert(conn->read_bucket >= 0);
    }
//    tor_assert(conn->addr && conn->port);
    tor_assert(conn->address);
    if (conn->state > OR_CONN_STATE_PROXY_HANDSHAKING)
      tor_assert(or_conn->tls);
  }

  if (CONN_IS_EDGE(conn)) {
    edge_connection_t *edge_conn = TO_EDGE_CONN(conn);
    if (edge_conn->chosen_exit_optional || edge_conn->chosen_exit_retries) {
      tor_assert(conn->type == CONN_TYPE_AP);
      tor_assert(edge_conn->chosen_exit_name);
    }

    /* XXX unchecked: package window, deliver window. */
    if (conn->type == CONN_TYPE_AP) {

      tor_assert(edge_conn->socks_request);
      if (conn->state == AP_CONN_STATE_OPEN) {
        tor_assert(edge_conn->socks_request->has_finished);
        if (!conn->marked_for_close) {
          tor_assert(edge_conn->cpath_layer);
          assert_cpath_layer_ok(edge_conn->cpath_layer);
        }
      }
    }
    if (conn->type == CONN_TYPE_EXIT) {
      tor_assert(conn->purpose == EXIT_PURPOSE_CONNECT ||
                 conn->purpose == EXIT_PURPOSE_RESOLVE);
    }
  } else if (conn->type == CONN_TYPE_DIR) {
  } else {
    /* Purpose is only used for dir and exit types currently */
    tor_assert(!conn->purpose);
  }

  switch (conn->type)
    {
    case CONN_TYPE_OR_LISTENER:
    case CONN_TYPE_AP_LISTENER:
    case CONN_TYPE_AP_TRANS_LISTENER:
    case CONN_TYPE_AP_NATD_LISTENER:
    case CONN_TYPE_DIR_LISTENER:
    case CONN_TYPE_CONTROL_LISTENER:
    case CONN_TYPE_AP_DNS_LISTENER:
      tor_assert(conn->state == LISTENER_STATE_READY);
      break;
    case CONN_TYPE_OR:
      tor_assert(conn->state >= _OR_CONN_STATE_MIN);
      tor_assert(conn->state <= _OR_CONN_STATE_MAX);
      tor_assert(TO_OR_CONN(conn)->n_circuits >= 0);
      break;
    case CONN_TYPE_EXIT:
      tor_assert(conn->state >= _EXIT_CONN_STATE_MIN);
      tor_assert(conn->state <= _EXIT_CONN_STATE_MAX);
      tor_assert(conn->purpose >= _EXIT_PURPOSE_MIN);
      tor_assert(conn->purpose <= _EXIT_PURPOSE_MAX);
      break;
    case CONN_TYPE_AP:
      tor_assert(conn->state >= _AP_CONN_STATE_MIN);
      tor_assert(conn->state <= _AP_CONN_STATE_MAX);
      tor_assert(TO_EDGE_CONN(conn)->socks_request);
      break;
    case CONN_TYPE_DIR:
      tor_assert(conn->state >= _DIR_CONN_STATE_MIN);
      tor_assert(conn->state <= _DIR_CONN_STATE_MAX);
      tor_assert(conn->purpose >= _DIR_PURPOSE_MIN);
      tor_assert(conn->purpose <= _DIR_PURPOSE_MAX);
      break;
    case CONN_TYPE_CPUWORKER:
      tor_assert(conn->state >= _CPUWORKER_STATE_MIN);
      tor_assert(conn->state <= _CPUWORKER_STATE_MAX);
      break;
    case CONN_TYPE_CONTROL:
      tor_assert(conn->state >= _CONTROL_CONN_STATE_MIN);
      tor_assert(conn->state <= _CONTROL_CONN_STATE_MAX);
      break;
    default:
      tor_assert(0);
  }
}

