
/**
 * Copyright (c) 2011, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. 
 *
 * @file
 * @author  Willem Thiart himself@willemthiart.com
 */

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <uv.h>
#include <sys/time.h>

#include "block.h"
#include "bt.h"
#include "bt_piece_db.h"
#include "bt_diskcache.h"
#include "bt_filedumper.h"
#include "bt_string.h"
#include "bt_sha1.h"
#include "bt_selector_random.h"
#include "tracker_client.h"
#include "torrentfile_reader.h"
#include "readfile.h"
#include "config.h"
#include "networkfuncs.h"
#include "linked_list_queue.h"

#define PROGRAM_NAME "bt"

typedef struct {
    /* bitorrent client */
    void* bc;

    /* piece db*/
    void* db;

    /* file dumper */
    void* fd;

    /* disk cache */
    void* dc;

    /* configuration */
    void* cfg;

    /* queue of announces to try */
    void* announces;

    /* tracker client */
    void* tc;

    uv_mutex_t mutex;
} bt_t;

typedef struct {
    bt_t* bt;
    char fname[256];
    int fname_len;
    int flen;
} torrent_reader_t;

uv_loop_t *loop;

static void __on_tc_done(void* data, int status);
static void __on_tc_add_peer(void* callee,
        char* peer_id,
        unsigned int peer_id_len,
        char* ip,
        unsigned int ip_len,
        unsigned int port);

static struct option const long_opts[] = {
    { "archive", no_argument, NULL, 'a'},
    /* The bounded network interface for net communications */
    { "verify-download", no_argument, NULL, 'e'},
    { "shutdown-when-complete", no_argument, NULL, 's'},
    { "show-config", no_argument, NULL, 'c'},
    { "pwp_listen_port", required_argument, NULL, 'p'},
    { "torrent_file_report_only", required_argument, NULL, 't'},
    { NULL, 0, NULL, 0}
};

static void __log(void *udata, void *src, char *buf)
{
    char stamp[32];
    int fd = (unsigned long) udata;
    struct timeval tv;

    printf("%s\n", buf);
    gettimeofday(&tv, NULL);
    sprintf(stamp, "%d,%0.2f,", (int) tv.tv_sec, (float) tv.tv_usec / 100000);
    write(fd, stamp, strlen(stamp));
    write(fd, buf, strlen(buf));
}

/**
 * Try to connect to this list of announces
 * @return 0 when no attempts could be made */
static int __trackerclient_try_announces(bt_t* bt)
{
    void* tc;
    char* a; /*  announcement */
    int waiting_for_response_from_connection = 0;

    /*  connect to one of the announces */
    if (0 == llqueue_count(bt->announces))
    {
        return 0;
    }

    bt->tc = tc = trackerclient_new(__on_tc_done, __on_tc_add_peer, bt);
    trackerclient_set_cfg(tc,bt->cfg);

    while ((a = llqueue_poll(bt->announces)))
    {
        if (1 == trackerclient_supports_uri(tc, a))
        {
            printf("Trying: %s\n", a);

            if (0 == trackerclient_connect_to_uri(tc, a))
            {
                printf("ERROR: connecting to %s\n", a);
                goto skip;
            }
            waiting_for_response_from_connection = 1;
            free(a);
            break;
        }
        else
        {
            printf("ERROR: No support for URI: %s\n", a);
        }
skip:
        free(a);
    }

    if (0 == waiting_for_response_from_connection)
    {
        return 0;
    }

    return 1;
}

/**
 * Tracker client is done. */
static void __on_tc_done(void* data, int status)
{
    bt_t* bt = data;

    if (0 == __trackerclient_try_announces(bt))
    {
        printf("No connections made, quitting\n");
        exit(0);
    }
}

static void* on_call_exclusively(void* me, void* cb_ctx, void **lock, void* udata,
        void* (*cb)(void* me, void* udata))
{
    void* result;

    if (NULL == *lock)
    {
        *lock = malloc(sizeof(uv_mutex_t));
        uv_mutex_init(*lock);
    }

    uv_mutex_lock(*lock);
    result = cb(me,udata);
    uv_mutex_unlock(*lock);
    return result;
}

static int __dispatch_from_buffer(
        void *callee,
        void *peer_nethandle,
        const unsigned char* buf,
        unsigned int len)
{
    bt_t* bt = callee;

    uv_mutex_lock(&bt->mutex);
    bt_dm_dispatch_from_buffer(bt->bc,peer_nethandle,buf,len);
    uv_mutex_unlock(&bt->mutex);
    return 1;
}

static int __on_peer_connect(
        void *callee,
        void* peer_nethandle,
        char *ip,
        const int port)
{
    bt_t* bt = callee;

    uv_mutex_lock(&bt->mutex);
    bt_dm_peer_connect(bt->bc,peer_nethandle,ip,port);
    uv_mutex_unlock(&bt->mutex);

    return 1;
}

static void __on_peer_connect_fail(
    void *callee,
    void* peer_nethandle)
{
    bt_t* bt = callee;

    uv_mutex_lock(&bt->mutex);
    bt_dm_peer_connect_fail(bt->bc,peer_nethandle);
    uv_mutex_unlock(&bt->mutex);
}

/**
 * Tracker client wants to add peer. */
static void __on_tc_add_peer(void* callee,
        char* peer_id,
        unsigned int peer_id_len,
        char* ip,
        unsigned int ip_len,
        unsigned int port)
{
    bt_t* bt = callee;
    void* peer;
    void* netdata;
    void* peer_nethandle;
    char ip_string[32];

    peer_nethandle = NULL;
    sprintf(ip_string,"%.*s", ip_len, ip);

#if 0 /* debug */
    printf("adding peer: %s %d\n", ip_string, port);
#endif

    uv_mutex_lock(&bt->mutex);

    if (0 == peer_connect(bt,
                &netdata,
                &peer_nethandle,
                ip_string, port,
                __dispatch_from_buffer,
                __on_peer_connect,
                __on_peer_connect_fail))
    {

    }

    peer = bt_dm_add_peer(bt->bc, peer_id, peer_id_len, ip, ip_len, port, peer_nethandle);

    uv_mutex_unlock(&bt->mutex);
}

static void __usage(int status)
{
    if (status != EXIT_SUCCESS)
    {
        fprintf(stderr, ("Try `%s --help' for more information.\n"),
                PROGRAM_NAME);
    }
    else
    {
        printf("\
Usage: %s [OPTION]... TORRENT_FILE\n", PROGRAM_NAME);
        fputs(("\
Download torrent indicated by TORRENT_FILE. \n\n\
Mandatory arguments to long options are mandatory for short options too. \n\
  -e, --verify-download        check downloaded files and quit \n\
  -t, --torrent_file_report_only    only report the contents of the torrent file \n\
  -b                                                    \n "), stdout);
        exit(status);
    }
}

static int __tfr_event(void* udata, const char* key)
{
    return 1;
}

static int __tfr_event_str(void* udata, const char* key, const char* val, int len)
{
    torrent_reader_t* me = udata;

#if 0 /* debugging */
    printf("%s %.*s\n", key, len, val);
#endif

    if (!strcmp(key,"announce"))
    {
        /* add to queue. We'll try them out by polling the queue elsewhere */
        llqueue_offer(me->bt->announces, strndup(val,len));
    }
    else if (!strcmp(key,"infohash"))
    {
        char hash[21];

        bt_str2sha1hash(hash, val, len);
        config_set_va(me->bt->cfg,"infohash","%.*s", 20, hash);
    }
    else if (!strcmp(key,"pieces"))
    {
        int i;

        for (i=0; i < len; i += 20)
        {
            bt_piecedb_add(me->bt->db,val + i);
            printf("pieces: %d\n", bt_piecedb_get_length(me->bt->db));
        }

        config_set_va(me->bt->cfg, "npieces", "%d",
                bt_piecedb_get_length(me->bt->db));
    }
    else if (!strcmp(key,"file path"))
    {
        assert(len < 256);
        strncpy(me->fname,val,len);
        me->fname_len = len;
        bt_piecedb_increase_piece_space(me->bt->db, me->flen);
        bt_filedumper_add_file(me->bt->fd, me->fname, me->fname_len, me->flen);
    }

    return 1;
}

static int __tfr_event_int(void* udata, const char* key, int val)
{
    torrent_reader_t* me = udata;

#if 0 /* debugging */
    printf("%s %d\n", key, val);
#endif

    if (!strcmp(key,"file length"))
    {
        me->flen = val;
    }
    else if (!strcmp(key,"piece length"))
    {
        config_set_va(me->bt->cfg, "piece_length", "%d", val);
        bt_piecedb_set_piece_length(me->bt->db, val);
        bt_diskcache_set_piece_length(me->bt->dc, val);
        bt_filedumper_set_piece_length(me->bt->fd, val);
    }

    return 1;
}

/**
 *  Read metainfo file (ie. "torrent" file).
 *  This function will populate the piece database.
 *  @param bc bittorrent client
 *  @param db piece database 
 *  @param fd filedumper
 *  @return 1 on sucess; otherwise 0
 */
static int __read_torrent_file(bt_t* bt, const char* torrent_file)
{
    void* tf;
    int len;
    char* metainfo;
    torrent_reader_t r;

    memset(&r, 0, sizeof(torrent_reader_t));
    r.bt = bt;
    tf = tfr_new(__tfr_event, __tfr_event_str, __tfr_event_int, &r);
    metainfo = read_file(torrent_file,&len);
    tfr_read_metainfo(tf, metainfo, len);
    return 1;
}

static void __log_process_info()
{
    static long int last_run = 0;
    struct timeval tv;

#define SECONDS_SINCE_LAST_LOG 1
    gettimeofday(&tv, NULL);

    /*  run every n seconds */
    if (0 == last_run)
    {
        last_run = tv.tv_usec;
    }
    else
    {
        unsigned int diff;
        
        diff = abs(last_run - tv.tv_usec);
        if (diff >= SECONDS_SINCE_LAST_LOG)
            return;
        last_run = tv.tv_usec;
    }
}

static void __bt_periodic(uv_timer_t* handle, int status)
{
    bt_t* bt = handle->data;
    bt_dm_stats_t stat;

    if (bt->bc)
    {
        uv_mutex_lock(&bt->mutex);
        bt_dm_periodic(bt->bc, &stat);
        uv_mutex_unlock(&bt->mutex);
    }

    __log_process_info();

    // bt_piecedb_print_pieces_downloaded(bt_dm_get_piecedb(me));
    // TODO: show inactive peers
    // TODO: show number of invalid pieces
    printf("peers: %d (active:%d choking:%d failed:%d) "
            "pieces: (downloaded:%d completed:%d/%d) dl:%04dKB/s ul:%04dKB/s\r",
            stat.peers,
            stat.connected,
            stat.choking,
            stat.failed_connection,
            bt_piecedb_get_num_downloaded(bt->db),
            bt_piecedb_get_num_completed(bt->db),
            bt_piecedb_get_length(bt->db),
            stat.download_rate == 0 ? 0 : stat.download_rate / 1000,
            stat.upload_rate == 0 ? 0 : stat.upload_rate / 1000
            );
}

int main(int argc, char **argv)
{
    char c;
    int o_verify_download,
        o_shutdown_when_complete,
        o_torrent_file_report_only,
        o_show_config;
    void *bc;
    char *str;
    config_t* cfg;
    bt_t bt;

    o_show_config = 0;
    o_verify_download = 0;
    o_shutdown_when_complete = 0;
    o_torrent_file_report_only = 0;
    bt.bc = bc = bt_dm_new();
    bt.cfg = cfg = bt_dm_get_config(bc);
    bt.announces = llqueue_new();

#if 0
    status = config_read(cfg, "yabtc", "config");
    setlocale(LC_ALL, " ");
    atexit (close_stdin);
#endif

    /* TODO: replace with gnugetops generator */
    while ((c = getopt_long(argc, argv, "cesi:p:", long_opts, NULL)) != -1)
    {
        switch (c)
        {
        case 's':
            o_shutdown_when_complete = 1;
            break;
        case 'p':
            config_set_va(cfg,"pwp_listen_port","%.*s",strlen(optarg), optarg);
            break;
        case 't':
            config_set_va(cfg,"torrent_file","%.*s",strlen(optarg), optarg);
            o_torrent_file_report_only = 1;
            break;
        case 'i':
            config_set_va(cfg,"bounded_iface","%.*s",strlen(optarg), optarg);
            break;
        case 'c':
            o_show_config = 1;
            break;
        case 'e':
            o_verify_download = 1;
            break;

        default:
            __usage(EXIT_FAILURE);
        }
    }

    /* do configuration */
    config_set_va(cfg, "shutdown_when_complete", "%d", o_shutdown_when_complete);
    config_set(cfg, "my_peerid", bt_generate_peer_id());
    assert(config_get(cfg, "my_peerid"));

    /*  logging */
    bt_dm_set_logging(
            bc, open("dump_log", O_CREAT | O_TRUNC | O_RDWR, 0666), __log);

    /* set network functions */
    bt_dm_cbs_t func = {
        .peer_connect = peer_connect,
        .peer_send = peer_send,
        .peer_disconnect = peer_disconnect, 
        .call_exclusively = on_call_exclusively
    };

    bt_dm_set_cbs(bc, &func, NULL);

    /* database for dumping pieces to disk */
    bt_piecedb_i pdb_funcs = {
        .get_piece = bt_piecedb_get
    };
    bt.fd = bt_filedumper_new();

    /* Disk Cache */
    bt.dc = bt_diskcache_new();
    /* point diskcache to filedumper */
    bt_diskcache_set_disk_blockrw(bt.dc,
            bt_filedumper_get_blockrw(bt.fd), bt.fd);
    //bt_diskcache_set_func_log(bc->dc, __log, bc);

    /* Piece DB */
    bt.db = bt_piecedb_new();
    bt_piecedb_set_diskstorage(bt.db,
            bt_diskcache_get_blockrw(bt.dc),
            NULL,//(void*)bt_filedumper_add_file,
            bt.dc);
    bt_dm_set_piece_db(bt.bc,&pdb_funcs,bt.db);

    if (o_torrent_file_report_only)
    {
        __read_torrent_file(&bt, config_get(cfg,"torrent_file"));
    }

    if (argc == optind)
    {
        printf("ERROR: Please specify torrent file\n");
        exit(EXIT_FAILURE);
    }
    else if (0 == __read_torrent_file(&bt,argv[optind]))
    {
        exit(EXIT_FAILURE);
    }

    if (o_show_config)
    {
        config_print(cfg);
    }

    bt_piecedb_print_pieces_downloaded(bt.db);

    /* Selector */
    bt_pieceselector_i ips = {
        .new = bt_random_selector_new,
        .peer_giveback_piece = bt_random_selector_giveback_piece,
        .have_piece = bt_random_selector_have_piece,
        .remove_peer = bt_random_selector_remove_peer,
        .add_peer = bt_random_selector_add_peer,
        .peer_have_piece = bt_random_selector_peer_have_piece,
        .get_npeers = bt_random_selector_get_npeers,
        .get_npieces = bt_random_selector_get_npieces,
        .poll_piece = bt_random_selector_poll_best_piece
    };

    bt_dm_set_piece_selector(bt.bc, &ips, NULL);

    /* start uv */
    loop = uv_default_loop();
    uv_mutex_init(&bt.mutex);

    /* create periodic timer */
    uv_timer_t *periodic_req;
    periodic_req = malloc(sizeof(uv_timer_t));
    periodic_req->data = &bt;
    uv_timer_init(loop, periodic_req);
    uv_timer_start(periodic_req, __bt_periodic, 0, 1000);

    /* try to connect to tracker */
    if (0 == __trackerclient_try_announces(&bt))
    {
        printf("No connections made, quitting\n");
        exit(0);
    }

    uv_run(loop, UV_RUN_DEFAULT);
    bt_dm_release(bc);
    return 1;
}
