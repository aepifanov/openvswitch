/*
 * Copyright (c) 2009, 2010, 2011, 2012, 2013 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>
#include "dpif.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef THREADED
#include <signal.h>
#include <pthread.h>

#include "socket-util.h"
#include "fatal-signal.h"
#include "dispatch.h"
#endif

#include "csum.h"
#include "dpif.h"
#include "dpif-provider.h"
#include "dummy.h"
#include "dynamic-string.h"
#include "flow.h"
#include "hmap.h"
#include "list.h"
#include "netdev.h"
#include "netdev-vport.h"
#include "netlink.h"
#include "odp-util.h"
#include "ofp-print.h"
#include "ofpbuf.h"
#include "packets.h"
#include "poll-loop.h"
#include "random.h"
#include "shash.h"
#include "sset.h"
#include "timeval.h"
#include "util.h"
#include "vlog.h"

VLOG_DEFINE_THIS_MODULE(dpif_netdev);
/* We could use these macros instead of using #ifdef and #endif every time we
 * need to call the pthread_mutex_lock/unlock.
#ifdef THREADED
#define LOCK(mutex) pthread_mutex_lock(mutex)
#define UNLOCK(mutex) pthread_mutex_unlock(mutex)
#else
#define LOCK(mutex)
#define UNLOCK(mutex)
#endif
*/

/* Configuration parameters. */
enum { MAX_PORTS = 256 };       /* Maximum number of ports. */
enum { MAX_FLOWS = 65536 };     /* Maximum number of flows in flow table. */

/* Enough headroom to add a vlan tag, plus an extra 2 bytes to allow IP
 * headers to be aligned on a 4-byte boundary.  */
enum { DP_NETDEV_HEADROOM = 2 + VLAN_HEADER_LEN };

/* Queues. */
enum { N_QUEUES = 2 };          /* Number of queues for dpif_recv(). */
enum { MAX_QUEUE_LEN = 128 };   /* Maximum number of packets per queue. */
enum { QUEUE_MASK = MAX_QUEUE_LEN - 1 };
BUILD_ASSERT_DECL(IS_POW2(MAX_QUEUE_LEN));

struct dp_netdev_upcall {
    struct dpif_upcall upcall;  /* Queued upcall information. */
    struct ofpbuf buf;          /* ofpbuf instance for upcall.packet. */
};

struct dp_netdev_queue {
    struct dp_netdev_upcall upcalls[MAX_QUEUE_LEN];
    unsigned int head, tail;
};

/* Datapath based on the network device interface from netdev.h. */
struct dp_netdev {
    const struct dpif_class *class;
    char *name;
    int open_cnt;
    bool destroyed;

#ifdef THREADED
    /* The pipe is used to signal the presence of a packet on the queue.
     * - dpif_netdev_recv_wait() waits on p[0]
     * - dpif_netdev_recv() extract from queue and read p[0]
     * - dp_netdev_output_control() send to queue and write p[1]
     */

    int pipe[2];    /* signal a packet on the queue */

    pthread_mutex_t table_mutex;    /* mutex for the flow table */
    pthread_mutex_t port_list_mutex;    /* port list mutex */

    /* The access to this queue is protected by the table_mutex mutex */
#endif
    struct dp_netdev_queue queues[N_QUEUES];
    struct hmap flow_table;     /* Flow table. */

    /* Statistics. */
    long long int n_hit;        /* Number of flow table matches. */
    long long int n_missed;     /* Number of flow table misses. */
    long long int n_lost;       /* Number of misses not passed to client. */

    /* Ports. */
    struct dp_netdev_port *ports[MAX_PORTS];
    struct list port_list;
    unsigned int serial;
};

/* A port in a netdev-based datapath. */
struct dp_netdev_port {
    int port_no;                /* Index into dp_netdev's 'ports'. */
    struct list node;           /* Element in dp_netdev's 'port_list'. */
    struct netdev *netdev;
    char *type;                 /* Port type as requested by user. */
#ifdef THREADED
    struct pollfd *poll_fd;     /* To manage the poll loop in the thread. */
#endif
};

/* A flow in dp_netdev's 'flow_table'. */
struct dp_netdev_flow {
    struct hmap_node node;      /* Element in dp_netdev's 'flow_table'. */
    struct flow key;

    /* Statistics. */
    long long int used;         /* Last used time, in monotonic msecs. */
    long long int packet_count; /* Number of packets matched. */
    long long int byte_count;   /* Number of bytes matched. */
    uint8_t tcp_flags;          /* Bitwise-OR of seen tcp_flags values. */

    /* Actions. */
    struct nlattr *actions;
    size_t actions_len;
};

/* Interface to netdev-based datapath. */
struct dpif_netdev {
    struct dpif dpif;
    struct dp_netdev *dp;
    unsigned int dp_serial;
};

#ifdef THREADED
/* XXX global Descriptor of the thread that manages the datapaths. */
pthread_t thread_p;
#endif

/* All netdev-based datapaths. */
static struct shash dp_netdevs = SHASH_INITIALIZER(&dp_netdevs);

/* Maximum port MTU seen so far. */
static int max_mtu = ETH_PAYLOAD_MAX;

static int get_port_by_number(struct dp_netdev *, uint32_t port_no,
                              struct dp_netdev_port **portp);
static int get_port_by_name(struct dp_netdev *, const char *devname,
                            struct dp_netdev_port **portp);
static void dp_netdev_free(struct dp_netdev *);
static void dp_netdev_flow_flush(struct dp_netdev *);
static int do_add_port(struct dp_netdev *, const char *devname,
                       const char *type, uint32_t port_no);
static int do_del_port(struct dp_netdev *, uint32_t port_no);
static int dpif_netdev_open(const struct dpif_class *, const char *name,
                            bool create, struct dpif **);
static int dp_netdev_output_userspace(struct dp_netdev *, const struct ofpbuf *,
                                    int queue_no, const struct flow *,
                                    const struct nlattr *userdata);
static void dp_netdev_execute_actions(struct dp_netdev *,
                                      struct ofpbuf *, struct flow *,
                                      const struct nlattr *actions,
                                      size_t actions_len);

static struct dpif_netdev *
dpif_netdev_cast(const struct dpif *dpif)
{
    ovs_assert(dpif->dpif_class->open == dpif_netdev_open);
    return CONTAINER_OF(dpif, struct dpif_netdev, dpif);
}

static struct dp_netdev *
get_dp_netdev(const struct dpif *dpif)
{
    return dpif_netdev_cast(dpif)->dp;
}

static int
dpif_netdev_enumerate(struct sset *all_dps)
{
    struct shash_node *node;

    SHASH_FOR_EACH(node, &dp_netdevs) {
        sset_add(all_dps, node->name);
    }
    return 0;
}

static bool
dpif_netdev_class_is_dummy(const struct dpif_class *class)
{
    return class != &dpif_netdev_class;
}

static const char *
dpif_netdev_port_open_type(const struct dpif_class *class, const char *type)
{
    return strcmp(type, "internal") ? type
                  : dpif_netdev_class_is_dummy(class) ? "dummy"
                  : "tap";
}

static struct dpif *
create_dpif_netdev(struct dp_netdev *dp)
{
    uint16_t netflow_id = hash_string(dp->name, 0);
    struct dpif_netdev *dpif;

    dp->open_cnt++;

    dpif = xmalloc(sizeof *dpif);
    dpif_init(&dpif->dpif, dp->class, dp->name, netflow_id >> 8, netflow_id);
    dpif->dp = dp;
    dpif->dp_serial = dp->serial;

    return &dpif->dpif;
}

static int
choose_port(struct dp_netdev *dp, const char *name)
{
    int port_no;

    if (dp->class != &dpif_netdev_class) {
        const char *p;
        int start_no = 0;

        /* If the port name begins with "br", start the number search at
         * 100 to make writing tests easier. */
        if (!strncmp(name, "br", 2)) {
            start_no = 100;
        }

        /* If the port name contains a number, try to assign that port number.
         * This can make writing unit tests easier because port numbers are
         * predictable. */
        for (p = name; *p != '\0'; p++) {
            if (isdigit((unsigned char) *p)) {
                port_no = start_no + strtol(p, NULL, 10);
                if (port_no > 0 && port_no < MAX_PORTS
                    && !dp->ports[port_no]) {
                    return port_no;
                }
                break;
            }
        }
    }

    for (port_no = 1; port_no < MAX_PORTS; port_no++) {
        if (!dp->ports[port_no]) {
            return port_no;
        }
    }

    return -1;
}

static int
create_dp_netdev(const char *name, const struct dpif_class *class,
                 struct dp_netdev **dpp)
{
    struct dp_netdev *dp;
    int error;
    int i;

    dp = xzalloc(sizeof *dp);
    dp->class = class;
    dp->name = xstrdup(name);
    dp->open_cnt = 0;
#ifdef THREADED
    error = pipe(dp->pipe);
    if (error) {
        VLOG_ERR("Unable to create datapath thread pipe: %s", strerror(errno));
        return errno;
    }
    if (set_nonblocking(dp->pipe[0]) || set_nonblocking(dp->pipe[1])) {
        VLOG_ERR("Unable to set nonblocking on datapath thread pipe: %s",
                 strerror(errno));
        return errno;
    }
    VLOG_DBG("Datapath thread pipe created (%d, %d)", dp->pipe[0], dp->pipe[1]);

    pthread_mutex_init(&dp->table_mutex, NULL);
    pthread_mutex_init(&dp->port_list_mutex, NULL);
#endif
    for (i = 0; i < N_QUEUES; i++) {
        dp->queues[i].head = dp->queues[i].tail = 0;
    }
    hmap_init(&dp->flow_table);
    list_init(&dp->port_list);

    error = do_add_port(dp, name, "internal", OVSP_LOCAL);
    if (error) {
        dp_netdev_free(dp);
        return error;
    }

    shash_add(&dp_netdevs, name, dp);

    *dpp = dp;
    return 0;
}

#ifdef THREADED
static void * dp_thread_body(void *args OVS_UNUSED);

/* This is the function that is called in response of a fatal signal (e.g.
 * SIGTERM) */
static void
dpif_netdev_exit_hook(void *aux OVS_UNUSED)
{
    if (pthread_cancel(thread_p) == 0) {
        pthread_join(thread_p, NULL);
    }
}

static int
dpif_netdev_init(void)
{
    static int error = -1;

    if (error < 0) {
        fatal_signal_add_hook(dpif_netdev_exit_hook, NULL, NULL, true);
        error = pthread_create(&thread_p, NULL, dp_thread_body, NULL);
        if (error != 0) {
            VLOG_ERR("Unable to create datapath thread: %s", strerror(errno));
            error = errno;
        } else {
            VLOG_DBG("Datapath thread started");
        }
    }
    return error;
}
#endif

static int
dpif_netdev_open(const struct dpif_class *class, const char *name,
                 bool create, struct dpif **dpifp)
{
    struct dp_netdev *dp;

    dp = shash_find_data(&dp_netdevs, name);
    if (!dp) {
        if (!create) {
            return ENODEV;
        } else {
            int error = create_dp_netdev(name, class, &dp);
            if (error) {
                return error;
            }
            ovs_assert(dp != NULL);
        }
    } else {
        if (dp->class != class) {
            return EINVAL;
        } else if (create) {
            return EEXIST;
        }
    }

    *dpifp = create_dpif_netdev(dp);
#ifdef THREADED
    dpif_netdev_init();
#endif
    return 0;
}

/* table_mutex must be locked in THREADED mode.
 */
static void
dp_netdev_purge_queues(struct dp_netdev *dp)
{
    int i;

    for (i = 0; i < N_QUEUES; i++) {
        struct dp_netdev_queue *q = &dp->queues[i];

        while (q->tail != q->head) {
            struct dp_netdev_upcall *u = &q->upcalls[q->tail++ & QUEUE_MASK];
            ofpbuf_uninit(&u->buf);
        }
    }
}

static void
dp_netdev_free(struct dp_netdev *dp)
{
    struct dp_netdev_port *port, *next;

    dp_netdev_flow_flush(dp);
#ifdef THREADED
    pthread_mutex_lock(&dp->port_list_mutex);
#endif
    LIST_FOR_EACH_SAFE (port, next, node, &dp->port_list) {
        do_del_port(dp, port->port_no);
    }
#ifdef THREADED
    pthread_mutex_unlock(&dp->port_list_mutex);
    pthread_mutex_lock(&dp->table_mutex);
#endif
    dp_netdev_purge_queues(dp);
    hmap_destroy(&dp->flow_table);
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
    pthread_mutex_destroy(&dp->table_mutex);
    pthread_mutex_destroy(&dp->port_list_mutex);
#endif
    free(dp->name);
    free(dp);
}

static void
dpif_netdev_close(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    ovs_assert(dp->open_cnt > 0);
    if (--dp->open_cnt == 0 && dp->destroyed) {
        shash_find_and_delete(&dp_netdevs, dp->name);
        dp_netdev_free(dp);
    }
    free(dpif);
}

static int
dpif_netdev_destroy(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    dp->destroyed = true;
    return 0;
}

static int
dpif_netdev_get_stats(const struct dpif *dpif, struct dpif_dp_stats *stats)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
    stats->n_flows = hmap_count(&dp->flow_table);
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
    stats->n_hit = dp->n_hit;
    stats->n_missed = dp->n_missed;
    stats->n_lost = dp->n_lost;
    return 0;
}

static int
do_add_port(struct dp_netdev *dp, const char *devname, const char *type,
            uint32_t port_no)
{
    struct dp_netdev_port *port;
    struct netdev *netdev;
    const char *open_type;
    int mtu;
    int error;

    /* XXX reject devices already in some dp_netdev. */

    /* Open and validate network device. */
    open_type = dpif_netdev_port_open_type(dp->class, type);
    error = netdev_open(devname, open_type, &netdev);
    if (error) {
        return error;
    }
    /* XXX reject loopback devices */
    /* XXX reject non-Ethernet devices */

    error = netdev_listen(netdev);
    if (error
        && !(error == EOPNOTSUPP && dpif_netdev_class_is_dummy(dp->class))) {
        VLOG_ERR("%s: cannot receive packets on this network device (%s)",
                 devname, strerror(errno));
        netdev_close(netdev);
        return error;
    }

    error = netdev_turn_flags_on(netdev, NETDEV_PROMISC, false);
    if (error) {
        netdev_close(netdev);
        return error;
    }

    port = xmalloc(sizeof *port);
    port->port_no = port_no;
    port->netdev = netdev;
    port->type = xstrdup(type);
#ifdef THREADED
    port->poll_fd = NULL;
#endif

    error = netdev_get_mtu(netdev, &mtu);
    if (!error && mtu > max_mtu) {
        max_mtu = mtu;
    }

#ifdef THREADED
    pthread_mutex_lock(&dp->port_list_mutex);
#endif
    list_push_back(&dp->port_list, &port->node);
#ifdef THREADED
    pthread_mutex_unlock(&dp->port_list_mutex);
#endif
    dp->ports[port_no] = port;
    dp->serial++;

    return 0;
}

static int
dpif_netdev_port_add(struct dpif *dpif, struct netdev *netdev,
                     uint32_t *port_nop)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    int port_no;

    if (*port_nop != UINT32_MAX) {
        if (*port_nop >= MAX_PORTS) {
            return EFBIG;
        } else if (dp->ports[*port_nop]) {
            return EBUSY;
        }
        port_no = *port_nop;
    } else {
        port_no = choose_port(dp, netdev_vport_get_dpif_port(netdev));
    }
    if (port_no >= 0) {
        *port_nop = port_no;
        return do_add_port(dp, netdev_vport_get_dpif_port(netdev),
                           netdev_get_type(netdev), port_no);
    }
    return EFBIG;
}

static int
dpif_netdev_port_del(struct dpif *dpif, uint32_t port_no)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    int error;

    if (port_no == OVSP_LOCAL) {
        return EINVAL;
    } else {
#ifdef THREADED
        pthread_mutex_lock(&dp->port_list_mutex);
#endif        
        error = do_del_port(dp, port_no);
#ifdef THREADED
        pthread_mutex_unlock(&dp->port_list_mutex);
#endif        
    }
    return error;
}

static bool
is_valid_port_number(uint32_t port_no)
{
    return port_no < MAX_PORTS;
}

static int
get_port_by_number(struct dp_netdev *dp,
                   uint32_t port_no, struct dp_netdev_port **portp)
{
    if (!is_valid_port_number(port_no)) {
        *portp = NULL;
        return EINVAL;
    } else {
        *portp = dp->ports[port_no];
        return *portp ? 0 : ENOENT;
    }
}

static int
get_port_by_name(struct dp_netdev *dp,
                 const char *devname, struct dp_netdev_port **portp)
{
    struct dp_netdev_port *port;

#ifdef THREADED
    pthread_mutex_lock(&dp->port_list_mutex);
#endif
    LIST_FOR_EACH (port, node, &dp->port_list) {
        if (!strcmp(netdev_vport_get_dpif_port(port->netdev), devname)) {
            *portp = port;
#ifdef THREADED
            pthread_mutex_unlock(&dp->port_list_mutex);
#endif
            return 0;
        }
    }
#ifdef THREADED
    pthread_mutex_unlock(&dp->port_list_mutex);
#endif
    return ENOENT;
}

/* In THREADED mode, must be called with port_list_mutex held. */
static int
do_del_port(struct dp_netdev *dp, uint32_t port_no)
{
    struct dp_netdev_port *port;
    char *name;
    int error;

    error = get_port_by_number(dp, port_no, &port);
    if (error) {
        return error;
    }

    list_remove(&port->node);
    dp->ports[port->port_no] = NULL;
    dp->serial++;

    name = xstrdup(netdev_vport_get_dpif_port(port->netdev));
    netdev_close(port->netdev);
    free(port->type);

    free(name);
    free(port);

    return 0;
}

static void
answer_port_query(const struct dp_netdev_port *port,
                  struct dpif_port *dpif_port)
{
    dpif_port->name = xstrdup(netdev_vport_get_dpif_port(port->netdev));
    dpif_port->type = xstrdup(port->type);
    dpif_port->port_no = port->port_no;
}

static int
dpif_netdev_port_query_by_number(const struct dpif *dpif, uint32_t port_no,
                                 struct dpif_port *dpif_port)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_port *port;
    int error;

    error = get_port_by_number(dp, port_no, &port);
    if (!error && dpif_port) {
        answer_port_query(port, dpif_port);
    }
    return error;
}

static int
dpif_netdev_port_query_by_name(const struct dpif *dpif, const char *devname,
                               struct dpif_port *dpif_port)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_port *port;
    int error;

    error = get_port_by_name(dp, devname, &port);
    if (!error && dpif_port) {
        answer_port_query(port, dpif_port);
    }
    return error;
}

static int
dpif_netdev_get_max_ports(const struct dpif *dpif OVS_UNUSED)
{
    return MAX_PORTS;
}

static void
dp_netdev_free_flow(struct dp_netdev *dp, struct dp_netdev_flow *flow)
{
#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
    hmap_remove(&dp->flow_table, &flow->node);
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
    free(flow->actions);
    free(flow);
}

static void
dp_netdev_flow_flush(struct dp_netdev *dp)
{
    struct dp_netdev_flow *flow, *next;

    HMAP_FOR_EACH_SAFE (flow, next, node, &dp->flow_table) {
        dp_netdev_free_flow(dp, flow);
    }
}

static int
dpif_netdev_flow_flush(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    dp_netdev_flow_flush(dp);
    return 0;
}

struct dp_netdev_port_state {
    uint32_t port_no;
    char *name;
};

static int
dpif_netdev_port_dump_start(const struct dpif *dpif OVS_UNUSED, void **statep)
{
    *statep = xzalloc(sizeof(struct dp_netdev_port_state));
    return 0;
}

static int
dpif_netdev_port_dump_next(const struct dpif *dpif, void *state_,
                           struct dpif_port *dpif_port)
{
    struct dp_netdev_port_state *state = state_;
    struct dp_netdev *dp = get_dp_netdev(dpif);
    uint32_t port_no;

    for (port_no = state->port_no; port_no < MAX_PORTS; port_no++) {
        struct dp_netdev_port *port = dp->ports[port_no];
        if (port) {
            free(state->name);
            state->name = xstrdup(netdev_vport_get_dpif_port(port->netdev));
            dpif_port->name = state->name;
            dpif_port->type = port->type;
            dpif_port->port_no = port->port_no;
            state->port_no = port_no + 1;
            return 0;
        }
    }
    return EOF;
}

static int
dpif_netdev_port_dump_done(const struct dpif *dpif OVS_UNUSED, void *state_)
{
    struct dp_netdev_port_state *state = state_;
    free(state->name);
    free(state);
    return 0;
}

static int
dpif_netdev_port_poll(const struct dpif *dpif_, char **devnamep OVS_UNUSED)
{
    struct dpif_netdev *dpif = dpif_netdev_cast(dpif_);
    if (dpif->dp_serial != dpif->dp->serial) {
        dpif->dp_serial = dpif->dp->serial;
        return ENOBUFS;
    } else {
        return EAGAIN;
    }
}

static void
dpif_netdev_port_poll_wait(const struct dpif *dpif_)
{
    struct dpif_netdev *dpif = dpif_netdev_cast(dpif_);
    if (dpif->dp_serial != dpif->dp->serial) {
        poll_immediate_wake();
    }
}

static struct dp_netdev_flow *
dp_netdev_lookup_flow(struct dp_netdev *dp, const struct flow *key)
{
    struct dp_netdev_flow *flow;

#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
    HMAP_FOR_EACH_WITH_HASH (flow, node, flow_hash(key, 0), &dp->flow_table) {
        if (flow_equal(&flow->key, key)) {
#ifdef THREADED
            pthread_mutex_unlock(&dp->table_mutex);
#endif
            return flow;
        }
    }
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
    return NULL;
}

static void
get_dpif_flow_stats(struct dp_netdev_flow *flow, struct dpif_flow_stats *stats)
{
    stats->n_packets = flow->packet_count;
    stats->n_bytes = flow->byte_count;
    stats->used = flow->used;
    stats->tcp_flags = flow->tcp_flags;
}

static int
dpif_netdev_flow_from_nlattrs(const struct nlattr *key, uint32_t key_len,
                              struct flow *flow)
{
    if (odp_flow_key_to_flow(key, key_len, flow) != ODP_FIT_PERFECT) {
        /* This should not happen: it indicates that odp_flow_key_from_flow()
         * and odp_flow_key_to_flow() disagree on the acceptable form of a
         * flow.  Log the problem as an error, with enough details to enable
         * debugging. */
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);

        if (!VLOG_DROP_ERR(&rl)) {
            struct ds s;

            ds_init(&s);
            odp_flow_key_format(key, key_len, &s);
            VLOG_ERR("internal error parsing flow key %s", ds_cstr(&s));
            ds_destroy(&s);
        }

        return EINVAL;
    }

    if (flow->in_port < OFPP_MAX
        ? flow->in_port >= MAX_PORTS
        : flow->in_port != OFPP_LOCAL && flow->in_port != OFPP_NONE) {
        return EINVAL;
    }

    return 0;
}

static int
dpif_netdev_flow_get(const struct dpif *dpif,
                     const struct nlattr *nl_key, size_t nl_key_len,
                     struct ofpbuf **actionsp, struct dpif_flow_stats *stats)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_flow *flow;
    struct flow key;
    int error;

    error = dpif_netdev_flow_from_nlattrs(nl_key, nl_key_len, &key);
    if (error) {
        return error;
    }

    flow = dp_netdev_lookup_flow(dp, &key);
    if (!flow) {
        return ENOENT;
    }

    if (stats) {
        get_dpif_flow_stats(flow, stats);
    }
    if (actionsp) {
        *actionsp = ofpbuf_clone_data(flow->actions, flow->actions_len);
    }
    return 0;
}

static int
set_flow_actions(struct dp_netdev_flow *flow,
                 const struct nlattr *actions, size_t actions_len)
{
    flow->actions = xrealloc(flow->actions, actions_len);
    flow->actions_len = actions_len;
    memcpy(flow->actions, actions, actions_len);
    return 0;
}

static int
dp_netdev_flow_add(struct dp_netdev *dp, const struct flow *key,
                   const struct nlattr *actions, size_t actions_len)
{
    struct dp_netdev_flow *flow;
    int error;

    flow = xzalloc(sizeof *flow);
    flow->key = *key;

    error = set_flow_actions(flow, actions, actions_len);
    if (error) {
        free(flow);
        return error;
    }

#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
    hmap_insert(&dp->flow_table, &flow->node, flow_hash(&flow->key, 0));
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
    return 0;
}

static void
clear_stats(struct dp_netdev_flow *flow)
{
    flow->used = 0;
    flow->packet_count = 0;
    flow->byte_count = 0;
    flow->tcp_flags = 0;
}

static int
dpif_netdev_flow_put(struct dpif *dpif, const struct dpif_flow_put *put)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_flow *flow;
    struct flow key;
    int error;
    int n_flows;

    error = dpif_netdev_flow_from_nlattrs(put->key, put->key_len, &key);
    if (error) {
        return error;
    }

    flow = dp_netdev_lookup_flow(dp, &key);
    if (!flow) {
        if (put->flags & DPIF_FP_CREATE) {
#ifdef THREADED
            pthread_mutex_lock(&dp->table_mutex);
#endif
            n_flows = hmap_count(&dp->flow_table);
#ifdef THREADED
            pthread_mutex_unlock(&dp->table_mutex);
#endif
            if (n_flows < MAX_FLOWS) {
                if (put->stats) {
                    memset(put->stats, 0, sizeof *put->stats);
                }
                return dp_netdev_flow_add(dp, &key, put->actions,
                                          put->actions_len);
            } else {
                return EFBIG;
            }
        } else {
            return ENOENT;
        }
    } else {
        if (put->flags & DPIF_FP_MODIFY) {
            int error = set_flow_actions(flow, put->actions, put->actions_len);
            if (!error) {
                if (put->stats) {
                    get_dpif_flow_stats(flow, put->stats);
                }
                if (put->flags & DPIF_FP_ZERO_STATS) {
                    clear_stats(flow);
                }
            }
            return error;
        } else {
            return EEXIST;
        }
    }
}

static int
dpif_netdev_flow_del(struct dpif *dpif, const struct dpif_flow_del *del)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_flow *flow;
    struct flow key;
    int error;

    error = dpif_netdev_flow_from_nlattrs(del->key, del->key_len, &key);
    if (error) {
        return error;
    }

    flow = dp_netdev_lookup_flow(dp, &key);
    if (flow) {
        if (del->stats) {
            get_dpif_flow_stats(flow, del->stats);
        }
        dp_netdev_free_flow(dp, flow);
        return 0;
    } else {
        return ENOENT;
    }
}

struct dp_netdev_flow_state {
    uint32_t bucket;
    uint32_t offset;
    struct nlattr *actions;
    struct odputil_keybuf keybuf;
    struct dpif_flow_stats stats;
};

static int
dpif_netdev_flow_dump_start(const struct dpif *dpif OVS_UNUSED, void **statep)
{
    struct dp_netdev_flow_state *state;

    *statep = state = xmalloc(sizeof *state);
    state->bucket = 0;
    state->offset = 0;
    state->actions = NULL;
    return 0;
}

static int
dpif_netdev_flow_dump_next(const struct dpif *dpif, void *state_,
                           const struct nlattr **key, size_t *key_len,
                           const struct nlattr **actions, size_t *actions_len,
                           const struct dpif_flow_stats **stats)
{
    struct dp_netdev_flow_state *state = state_;
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_flow *flow;
    struct hmap_node *node;

#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
    node = hmap_at_position(&dp->flow_table, &state->bucket, &state->offset);
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
    if (!node) {
        return EOF;
    }

    flow = CONTAINER_OF(node, struct dp_netdev_flow, node);

    if (key) {
        struct ofpbuf buf;

        ofpbuf_use_stack(&buf, &state->keybuf, sizeof state->keybuf);
        odp_flow_key_from_flow(&buf, &flow->key, flow->key.in_port);

        *key = buf.data;
        *key_len = buf.size;
    }

    if (actions) {
        free(state->actions);
        state->actions = xmemdup(flow->actions, flow->actions_len);

        *actions = state->actions;
        *actions_len = flow->actions_len;
    }

    if (stats) {
        get_dpif_flow_stats(flow, &state->stats);
        *stats = &state->stats;
    }

    return 0;
}

static int
dpif_netdev_flow_dump_done(const struct dpif *dpif OVS_UNUSED, void *state_)
{
    struct dp_netdev_flow_state *state = state_;

    free(state->actions);
    free(state);
    return 0;
}

static int
dpif_netdev_execute(struct dpif *dpif, const struct dpif_execute *execute)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct ofpbuf copy;
    struct flow key;
    int error;

    if (execute->packet->size < ETH_HEADER_LEN ||
        execute->packet->size > UINT16_MAX) {
        return EINVAL;
    }

    /* Make a deep copy of 'packet', because we might modify its data. */
    ofpbuf_init(&copy, DP_NETDEV_HEADROOM + execute->packet->size);
    ofpbuf_reserve(&copy, DP_NETDEV_HEADROOM);
    ofpbuf_put(&copy, execute->packet->data, execute->packet->size);

    flow_extract(&copy, 0, 0, NULL, -1, &key);
    error = dpif_netdev_flow_from_nlattrs(execute->key, execute->key_len,
                                          &key);
    if (!error) {
        dp_netdev_execute_actions(dp, &copy, &key,
                                  execute->actions, execute->actions_len);
    }

    ofpbuf_uninit(&copy);
    return error;
}

static int
dpif_netdev_recv_set(struct dpif *dpif OVS_UNUSED, bool enable OVS_UNUSED)
{
    return 0;
}

static int
dpif_netdev_queue_to_priority(const struct dpif *dpif OVS_UNUSED,
                              uint32_t queue_id, uint32_t *priority)
{
    *priority = queue_id;
    return 0;
}

static struct dp_netdev_queue *
find_nonempty_queue(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    int i;

    for (i = 0; i < N_QUEUES; i++) {
        struct dp_netdev_queue *q = &dp->queues[i];
        if (q->head != q->tail) {
            return q;
        }
    }
    return NULL;
}

static int
dpif_netdev_recv(struct dpif *dpif, struct dpif_upcall *upcall,
                 struct ofpbuf *buf)
{
    struct dp_netdev_queue *q;
#ifdef THREADED
    struct dp_netdev *dp = get_dp_netdev(dpif);
    char c;
    pthread_mutex_lock(&dp->table_mutex);
#endif
    q = find_nonempty_queue(dpif);
    if (q) {
        struct dp_netdev_upcall *u = &q->upcalls[q->tail++ & QUEUE_MASK];

        *upcall = u->upcall;
        upcall->packet = buf;

        ofpbuf_uninit(buf);
        *buf = u->buf;

#ifdef THREADED
        /* Read a byte from the pipe to signal that a packet has been
         * received. */
        if (read(dp->pipe[0], &c, 1) < 0) {
            VLOG_ERR("Error reading from the pipe: %s", strerror(errno));
        }
        pthread_mutex_unlock(&dp->table_mutex);
#endif
        return 0;
    } else {
#ifdef THREADED
        pthread_mutex_unlock(&dp->table_mutex);
#endif
        return EAGAIN;
    }
}

static void
dpif_netdev_recv_wait(struct dpif *dpif)
{
#ifdef THREADED
    struct dp_netdev *dp = get_dp_netdev(dpif);

    poll_fd_wait(dp->pipe[0], POLLIN);
#else
    if (find_nonempty_queue(dpif)) {
        poll_immediate_wake();
    } else {
        /* No messages ready to be received, and dp_wait() will ensure that we
         * wake up to queue new messages, so there is nothing to do. */
    }
#endif
}

static void
dpif_netdev_recv_purge(struct dpif *dpif)
{
    struct dpif_netdev *dpif_netdev = dpif_netdev_cast(dpif);
#ifdef THREADED
    struct dp_netdev *dp = get_dp_netdev(dpif);
    pthread_mutex_lock(&dp->table_mutex);
#endif
    dp_netdev_purge_queues(dpif_netdev->dp);
#ifdef THREADED
    pthread_mutex_unlock(&dp->table_mutex);
#endif
}

static void
dp_netdev_flow_used(struct dp_netdev_flow *flow, const struct ofpbuf *packet)
{
    flow->used = time_msec();
    flow->packet_count++;
    flow->byte_count += packet->size;
    flow->tcp_flags |= packet_get_tcp_flags(packet, &flow->key);
}

static void
dp_netdev_port_input(struct dp_netdev *dp, struct dp_netdev_port *port,
                     struct ofpbuf *packet)
{
    struct dp_netdev_flow *flow;
    struct flow key;

    if (packet->size < ETH_HEADER_LEN) {
        return;
    }
    flow_extract(packet, 0, 0, NULL, port->port_no, &key);
    flow = dp_netdev_lookup_flow(dp, &key);
    if (flow) {
        dp_netdev_flow_used(flow, packet);
        dp_netdev_execute_actions(dp, packet, &key,
                                  flow->actions, flow->actions_len);
        dp->n_hit++;
    } else {
        dp->n_missed++;
        dp_netdev_output_userspace(dp, packet, DPIF_UC_MISS, &key, NULL);
    }
}

#ifdef THREADED
static void
dpif_netdev_run(struct dpif *dpif OVS_UNUSED)
{
}

static void
dpif_netdev_wait(struct dpif *dpif OVS_UNUSED)
{
}
#else
static void
dpif_netdev_run(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_port *port;
    struct ofpbuf packet;

    ofpbuf_init(&packet, DP_NETDEV_HEADROOM + VLAN_ETH_HEADER_LEN + max_mtu);

    LIST_FOR_EACH (port, node, &dp->port_list) {
        int error;

        /* Reset packet contents. */
        ofpbuf_clear(&packet);
        ofpbuf_reserve(&packet, DP_NETDEV_HEADROOM);

        error = netdev_recv(port->netdev, &packet);
        if (!error) {
            dp_netdev_port_input(dp, port, &packet);
        } else if (error != EAGAIN && error != EOPNOTSUPP) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 5);
            VLOG_ERR_RL(&rl, "error receiving data from %s: %s",
                        netdev_vport_get_dpif_port(port->netdev),
                        strerror(error));
        }
    }
    ofpbuf_uninit(&packet);
}

static void
dpif_netdev_wait(struct dpif *dpif)
{
    struct dp_netdev *dp = get_dp_netdev(dpif);
    struct dp_netdev_port *port;

    LIST_FOR_EACH (port, node, &dp->port_list) {
        netdev_recv_wait(port->netdev);
    }
}
#endif

#ifdef THREADED
/*
 * pcap callback argument
 */
struct dispatch_arg {
    struct dp_netdev *dp;   /* update statistics */
    struct dp_netdev_port *port;    /* argument to flow identifier function */
    struct ofpbuf buf;      /* used to process the packet */
};

/* Process a packet.
 *
 * The port_input function will send immediately if it finds a flow match and
 * the associated action is ODPAT_OUTPUT or ODPAT_OUTPUT_GROUP.
 * If a flow is not found or for the other actions, the packet is copied.
 */
static void
process_pkt(u_char *arg_p, const struct pkthdr *hdr, const u_char *packet)
{
    struct dispatch_arg *arg = (struct dispatch_arg *)arg_p;
    struct ofpbuf *buf = &arg->buf;

    /* set packet size and data pointer */
    buf->size = hdr->caplen; /* XXX Must the size be equal to hdr->len or
                              * hdr->caplen */
    buf->data = (void*)packet;

    dp_netdev_port_input(arg->dp, arg->port, buf);

    return;
}

/* Body of the thread that manages the datapaths */
static void*
dp_thread_body(void *args OVS_UNUSED)
{
    struct dp_netdev *dp;
    struct dp_netdev_port *port;
    struct dispatch_arg arg;
    int error;
    int n_fds;
    uint32_t batch = 50; /* max number of pkts processed by the dispatch */
    int processed;     /* actual number of pkts processed by the dispatch */

    sigset_t sigmask;

    /*XXX Since the poll involves all ports of all datapaths, the right fds
     * size should be MAX_PORTS * max_number_of_datapaths */
    struct pollfd fds[MAX_PORTS]; 
    
    /* mask the fatal signals. In this way the main thread is delegate to
     * manage this them. */
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGTERM);
    sigaddset(&sigmask, SIGALRM);
    sigaddset(&sigmask, SIGINT);
    sigaddset(&sigmask, SIGHUP);

    if (pthread_sigmask(SIG_BLOCK, &sigmask, NULL) != 0) {
        VLOG_ERR("Error setting thread sigmask: %s", errno);
    }

    ofpbuf_init(&arg.buf, DP_NETDEV_HEADROOM + VLAN_ETH_HEADER_LEN + max_mtu);
    for(;;) {
        struct shash_node *node;
        n_fds = 0;
        /* build the structure for poll */
        SHASH_FOR_EACH(node, &dp_netdevs) {
            dp = (struct dp_netdev *)node->data;
            pthread_mutex_lock(&dp->port_list_mutex);
            LIST_FOR_EACH (port, node, &dp->port_list) {
                /* insert an element in the fds structure */
                fds[n_fds].fd = netdev_get_fd(port->netdev);
                fds[n_fds].events = POLLIN;
                port->poll_fd = &fds[n_fds];
                n_fds++;
            }
            pthread_mutex_unlock(&dp->port_list_mutex);
        }

        error = poll(fds, n_fds, 2000);
        VLOG_DBG("dp_thread_body poll wakeup with cnt=%d", error);

        if (error < 0) {
            if (errno == EINTR) {
                /* XXX get this case in detach mode */
                continue;
            }
            VLOG_ERR("Datapath thread poll() error: %s\n", strerror(errno));
            /* XXX terminating the thread is probably not right */
            break;
        }
        pthread_testcancel();

        SHASH_FOR_EACH (node, &dp_netdevs) {
            dp = (struct dp_netdev *)node->data;
            arg.dp = dp;
            pthread_mutex_lock(&dp->port_list_mutex);
            LIST_FOR_EACH (port, node, &dp->port_list) {
                arg.port = port;
                arg.buf.size = 0;
                arg.buf.data = (char*)arg.buf.base + DP_NETDEV_HEADROOM;
                if (port->poll_fd) {
                    VLOG_DBG("fd %d revents 0x%x", port->poll_fd->fd, port->poll_fd->revents);
                }
                if (port->poll_fd && (port->poll_fd->revents & POLLIN)) {
                    /* call the dispatch and process the packet into
                     * its callback. We process 'batch' packets at time */
                    processed = netdev_dispatch(port->netdev, batch,
                                         process_pkt, (u_char *)&arg);
                    if (processed < 0) { /* pcap returns error */
                        static struct vlog_rate_limit rl =
                            VLOG_RATE_LIMIT_INIT(1, 5);
                        VLOG_ERR_RL(&rl, 
                                "error receiving data from XXX \n"); 
                    }
                } /* end of if poll */
            } /* end of port loop */
        pthread_mutex_unlock(&dp->port_list_mutex);
        } /* end of dp loop */
    } /* for ;; */

    ofpbuf_uninit(&arg.buf);
    return NULL;
}

#endif /* THREADED */

static void
dp_netdev_set_dl(struct ofpbuf *packet, const struct ovs_key_ethernet *eth_key)
{
    struct eth_header *eh = packet->l2;

    memcpy(eh->eth_src, eth_key->eth_src, sizeof eh->eth_src);
    memcpy(eh->eth_dst, eth_key->eth_dst, sizeof eh->eth_dst);
}

static void
dp_netdev_output_port(struct dp_netdev *dp, struct ofpbuf *packet,
                      uint32_t out_port)
{
    struct dp_netdev_port *p = dp->ports[out_port];
    if (p) {
        netdev_send(p->netdev, packet);
    }
}

static int
dp_netdev_output_userspace(struct dp_netdev *dp, const struct ofpbuf *packet,
                           int queue_no, const struct flow *flow,
                           const struct nlattr *userdata)
{
    struct dp_netdev_queue *q = &dp->queues[queue_no];
    if (q->head - q->tail < MAX_QUEUE_LEN) {
        struct dp_netdev_upcall *u = &q->upcalls[q->head++ & QUEUE_MASK];
        struct dpif_upcall *upcall = &u->upcall;
        struct ofpbuf *buf = &u->buf;
        size_t buf_size;
#ifdef THREADED
    char c;
#endif

        upcall->type = queue_no;

        /* Allocate buffer big enough for everything. */
        buf_size = ODPUTIL_FLOW_KEY_BYTES + 2 + packet->size;
        if (userdata) {
            buf_size += NLA_ALIGN(userdata->nla_len);
        }
        ofpbuf_init(buf, buf_size);

        /* Put ODP flow. */
        odp_flow_key_from_flow(buf, flow, flow->in_port);
        upcall->key = buf->data;
        upcall->key_len = buf->size;

        /* Put userdata. */
        if (userdata) {
            upcall->userdata = ofpbuf_put(buf, userdata,
                                          NLA_ALIGN(userdata->nla_len));
        }

        /* Put packet.
         *
         * We adjust 'data' and 'size' in 'buf' so that only the packet itself
         * is visible in 'upcall->packet'.  The ODP flow and (if present)
         * userdata become part of the headroom. */
        ofpbuf_put_zeros(buf, 2);
        buf->data = ofpbuf_put(buf, packet->data, packet->size);
        buf->size = packet->size;
        upcall->packet = buf;

#ifdef THREADED
    pthread_mutex_lock(&dp->table_mutex);
#endif
#ifdef THREADED
    /* Write a byte on the pipe to advertise that a packet is ready. */
    if (write(dp->pipe[1], &c, 1) < 0) {
        VLOG_ERR("Error writing on the pipe: %s", strerror(errno));
    }
    pthread_mutex_unlock(&dp->table_mutex);
#endif

        return 0;
    } else {
        dp->n_lost++;
        return ENOBUFS;
    }
}

static void
dp_netdev_sample(struct dp_netdev *dp,
                 struct ofpbuf *packet, struct flow *key,
                 const struct nlattr *action)
{
    const struct nlattr *subactions = NULL;
    const struct nlattr *a;
    size_t left;

    NL_NESTED_FOR_EACH_UNSAFE (a, left, action) {
        int type = nl_attr_type(a);

        switch ((enum ovs_sample_attr) type) {
        case OVS_SAMPLE_ATTR_PROBABILITY:
            if (random_uint32() >= nl_attr_get_u32(a)) {
                return;
            }
            break;

        case OVS_SAMPLE_ATTR_ACTIONS:
            subactions = a;
            break;

        case OVS_SAMPLE_ATTR_UNSPEC:
        case __OVS_SAMPLE_ATTR_MAX:
        default:
            NOT_REACHED();
        }
    }

    dp_netdev_execute_actions(dp, packet, key, nl_attr_get(subactions),
                              nl_attr_get_size(subactions));
}

static void
dp_netdev_action_userspace(struct dp_netdev *dp,
                          struct ofpbuf *packet, struct flow *key,
                          const struct nlattr *a)
{
    const struct nlattr *userdata;

    userdata = nl_attr_find_nested(a, OVS_USERSPACE_ATTR_USERDATA);
    dp_netdev_output_userspace(dp, packet, DPIF_UC_ACTION, key, userdata);
}

static void
execute_set_action(struct ofpbuf *packet, const struct nlattr *a)
{
    enum ovs_key_attr type = nl_attr_type(a);
    const struct ovs_key_ipv4 *ipv4_key;
    const struct ovs_key_ipv6 *ipv6_key;
    const struct ovs_key_tcp *tcp_key;
    const struct ovs_key_udp *udp_key;

    switch (type) {
    case OVS_KEY_ATTR_PRIORITY:
    case OVS_KEY_ATTR_SKB_MARK:
    case OVS_KEY_ATTR_TUNNEL:
        /* not implemented */
        break;

    case OVS_KEY_ATTR_ETHERNET:
        dp_netdev_set_dl(packet,
                   nl_attr_get_unspec(a, sizeof(struct ovs_key_ethernet)));
        break;

    case OVS_KEY_ATTR_IPV4:
        ipv4_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv4));
        packet_set_ipv4(packet, ipv4_key->ipv4_src, ipv4_key->ipv4_dst,
                        ipv4_key->ipv4_tos, ipv4_key->ipv4_ttl);
        break;

    case OVS_KEY_ATTR_IPV6:
        ipv6_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_ipv6));
        packet_set_ipv6(packet, ipv6_key->ipv6_proto, ipv6_key->ipv6_src,
                        ipv6_key->ipv6_dst, ipv6_key->ipv6_tclass,
                        ipv6_key->ipv6_label, ipv6_key->ipv6_hlimit);
        break;

    case OVS_KEY_ATTR_TCP:
        tcp_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_tcp));
        packet_set_tcp_port(packet, tcp_key->tcp_src, tcp_key->tcp_dst);
        break;

     case OVS_KEY_ATTR_UDP:
        udp_key = nl_attr_get_unspec(a, sizeof(struct ovs_key_udp));
        packet_set_udp_port(packet, udp_key->udp_src, udp_key->udp_dst);
        break;

     case OVS_KEY_ATTR_MPLS:
         set_mpls_lse(packet, nl_attr_get_be32(a));
         break;

     case OVS_KEY_ATTR_UNSPEC:
     case OVS_KEY_ATTR_ENCAP:
     case OVS_KEY_ATTR_ETHERTYPE:
     case OVS_KEY_ATTR_IN_PORT:
     case OVS_KEY_ATTR_VLAN:
     case OVS_KEY_ATTR_ICMP:
     case OVS_KEY_ATTR_ICMPV6:
     case OVS_KEY_ATTR_ARP:
     case OVS_KEY_ATTR_ND:
     case __OVS_KEY_ATTR_MAX:
     default:
        NOT_REACHED();
    }
}

static void
dp_netdev_execute_actions(struct dp_netdev *dp,
                          struct ofpbuf *packet, struct flow *key,
                          const struct nlattr *actions,
                          size_t actions_len)
{
    const struct nlattr *a;
    unsigned int left;

    NL_ATTR_FOR_EACH_UNSAFE (a, left, actions, actions_len) {
        int type = nl_attr_type(a);

        switch ((enum ovs_action_attr) type) {
        case OVS_ACTION_ATTR_OUTPUT:
            dp_netdev_output_port(dp, packet, nl_attr_get_u32(a));
            break;

        case OVS_ACTION_ATTR_USERSPACE:
            dp_netdev_action_userspace(dp, packet, key, a);
            break;

        case OVS_ACTION_ATTR_PUSH_VLAN: {
            const struct ovs_action_push_vlan *vlan = nl_attr_get(a);
            eth_push_vlan(packet, vlan->vlan_tci);
            break;
        }

        case OVS_ACTION_ATTR_POP_VLAN:
            eth_pop_vlan(packet);
            break;

        case OVS_ACTION_ATTR_PUSH_MPLS: {
            const struct ovs_action_push_mpls *mpls = nl_attr_get(a);
            push_mpls(packet, mpls->mpls_ethertype, mpls->mpls_lse);
            break;
         }

        case OVS_ACTION_ATTR_POP_MPLS:
            pop_mpls(packet, nl_attr_get_be16(a));
            break;

        case OVS_ACTION_ATTR_SET:
            execute_set_action(packet, nl_attr_get(a));
            break;

        case OVS_ACTION_ATTR_SAMPLE:
            dp_netdev_sample(dp, packet, key, a);
            break;

        case OVS_ACTION_ATTR_UNSPEC:
        case __OVS_ACTION_ATTR_MAX:
            NOT_REACHED();
        }
    }
}

const struct dpif_class dpif_netdev_class = {
    "netdev",
    dpif_netdev_enumerate,
    dpif_netdev_port_open_type,
    dpif_netdev_open,
    dpif_netdev_close,
    dpif_netdev_destroy,
    dpif_netdev_run,
    dpif_netdev_wait,
    dpif_netdev_get_stats,
    dpif_netdev_port_add,
    dpif_netdev_port_del,
    dpif_netdev_port_query_by_number,
    dpif_netdev_port_query_by_name,
    dpif_netdev_get_max_ports,
    NULL,                       /* port_get_pid */
    dpif_netdev_port_dump_start,
    dpif_netdev_port_dump_next,
    dpif_netdev_port_dump_done,
    dpif_netdev_port_poll,
    dpif_netdev_port_poll_wait,
    dpif_netdev_flow_get,
    dpif_netdev_flow_put,
    dpif_netdev_flow_del,
    dpif_netdev_flow_flush,
    dpif_netdev_flow_dump_start,
    dpif_netdev_flow_dump_next,
    dpif_netdev_flow_dump_done,
    dpif_netdev_execute,
    NULL,                       /* operate */
    dpif_netdev_recv_set,
    dpif_netdev_queue_to_priority,
    dpif_netdev_recv,
    dpif_netdev_recv_wait,
    dpif_netdev_recv_purge,
};

static void
dpif_dummy_register__(const char *type)
{
    struct dpif_class *class;

    class = xmalloc(sizeof *class);
    *class = dpif_netdev_class;
    class->type = xstrdup(type);
    dp_register_provider(class);
}

void
dpif_dummy_register(bool override)
{
    if (override) {
        struct sset types;
        const char *type;

        sset_init(&types);
        dp_enumerate_types(&types);
        SSET_FOR_EACH (type, &types) {
            if (!dp_unregister_provider(type)) {
                dpif_dummy_register__(type);
            }
        }
        sset_destroy(&types);
    }

    dpif_dummy_register__("dummy");
}
