/* Icecast
 *
 * This program is distributed under the GNU General Public License, version 2.
 * A copy of this license is included with this source.
 *
 * Copyright 2000-2004, Jack Moffitt <jack@xiph.org, 
 *                      Michael Smith <msmith@xiph.org>,
 *                      oddsock <oddsock@xiph.org>,
 *                      Karl Heyes <karl@xiph.org>
 *                      and others (see AUTHORS for details).
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_POLL
#include <sys/poll.h>
#endif

#ifdef _MSC_VER
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
# ifdef HAVE_SYS_SOCKET_H
#  include <sys/socket.h>
# endif
# ifndef SCN_OFF_T
#  define SCN_OFF_T SCNdMAX
# endif
# ifndef PRI_OFF_T
#  define PRI_OFF_T PRIdMAX
# endif
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_BINARY
#define O_BINARY 0
#endif

#include "thread/thread.h"
#include "avl/avl.h"
#include "httpp/httpp.h"
#include "net/sock.h"

#include "fserve.h"
#include "connection.h"
#include "global.h"
#include "refbuf.h"
#include "client.h"
#include "stats.h"
#include "format.h"
#include "logging.h"
#include "cfgfile.h"
#include "util.h"
#include "admin.h"
#include "slave.h"

#include "format_mp3.h"

#undef CATMODULE
#define CATMODULE "fserve"

#define BUFSIZE 4096

static spin_t pending_lock;
static avl_tree *mimetypes = NULL;
static avl_tree *fh_cache = NULL;
#ifndef HAVE_PREAD
static mutex_t seekread_lock;
#endif

typedef struct {
    char *ext;
    char *type;
} mime_type;

typedef struct {
    fbinfo finfo;
    mutex_t lock;
    int refcount;
    int peak;
    int max;
    icefile_handle f;
    time_t stats_update;
    long stats;
    format_plugin_t *format;
    struct rate_calc *out_bitrate;
    avl_tree *clients;
} fh_node;

int fserve_running;

static int _delete_mapping(void *mapping);
static int prefile_send (client_t *client);
static int file_send (client_t *client);
static int _compare_fh(void *arg, void *a, void *b);
static int _delete_fh (void *mapping);

void fserve_initialize(void)
{
    ice_config_t *config = config_get_config();

    mimetypes = NULL;
    thread_spin_create (&pending_lock);
#ifndef HAVE_PREAD
    thread_mutex_create (&seekread_lock);
#endif
    fh_cache = avl_tree_new (_compare_fh, NULL);

    fserve_recheck_mime_types (config);
    config_release_config();

    stats_event_flags (NULL, "file_connections", "0", STATS_COUNTERS);
    fserve_running = 1;
    INFO0("file serving started");
}

void fserve_shutdown(void)
{
    fserve_running = 0;
    if (mimetypes)
        avl_tree_free (mimetypes, _delete_mapping);
    if (fh_cache)
    {
        int count = 20;
        while (fh_cache->length && count)
        {
            DEBUG1 ("waiting for %u entries to clear", fh_cache->length);
            thread_sleep (100000);
            count--;
        }
        avl_tree_free (fh_cache, _delete_fh);
    }

    thread_spin_destroy (&pending_lock);
#ifndef HAVE_PREAD
    thread_mutex_destroy (&seekread_lock);
#endif
    INFO0("file serving stopped");
}


/* string returned needs to be free'd */
char *fserve_content_type (const char *path)
{
    char *ext = util_get_extension(path);
    mime_type exttype = { NULL, NULL };
    void *result;
    char *type;

    if (ext == NULL)
        return strdup ("text/html");
    exttype.ext = strdup (ext);

    thread_spin_lock (&pending_lock);
    if (mimetypes && !avl_get_by_key (mimetypes, &exttype, &result))
    {
        mime_type *mime = result;
        free (exttype.ext);
        type = strdup (mime->type);
    }
    else {
        free (exttype.ext);
        /* Fallbacks for a few basic ones */
        if(!strcmp(ext, "ogg"))
            type = strdup ("application/ogg");
        else if(!strcmp(ext, "mp3"))
            type = strdup ("audio/mpeg");
        else if(!strcmp(ext, "html"))
            type = strdup ("text/html");
        else if(!strcmp(ext, "css"))
            type = strdup ("text/css");
        else if(!strcmp(ext, "txt"))
            type = strdup ("text/plain");
        else if(!strcmp(ext, "jpg"))
            type = strdup ("image/jpeg");
        else if(!strcmp(ext, "png"))
            type = strdup ("image/png");
        else if(!strcmp(ext, "m3u"))
            type = strdup ("audio/x-mpegurl");
        else if(!strcmp(ext, "aac"))
            type = strdup ("audio/aac");
        else
            type = strdup ("application/octet-stream");
    }
    thread_spin_unlock (&pending_lock);
    return type;
}


static int _compare_fh(void *arg, void *a, void *b)
{
    fh_node *x = a, *y = b;
    int r = strcmp (x->finfo.mount, y->finfo.mount);

    if (r) return r;
    r = (int)x->finfo.flags - y->finfo.flags;
    if (r) return r;
    return 0;
}


static int _delete_fh (void *mapping)
{
    fh_node *fh = mapping;
    if (fh->refcount)
        WARN2 ("handle for %s has refcount %d", fh->finfo.mount, fh->refcount);
    else
        thread_mutex_destroy (&fh->lock);

    file_close (&fh->f);
    if (fh->format)
    {
        free (fh->format->mount);
        format_plugin_clear (fh->format, NULL);
        free (fh->format);
    }
    avl_tree_free (fh->clients, NULL);
    rate_free (fh->out_bitrate);
    free (fh->finfo.mount);
    free (fh->finfo.fallback);
    free (fh);

    return 1;
}


static int remove_fh_from_cache (fh_node *fh)
{
    int ret = 0;
    avl_tree_wlock (fh_cache);
    thread_mutex_lock (&fh->lock);
    if (fh->refcount) fh->refcount--;
    if (fh->stats)
        stats_set_args (fh->stats, "listeners", "%ld", fh->refcount);
    if (fh->refcount == 0)
    {
        avl_delete (fh_cache, fh, NULL);
        ret = 1;
    }
    thread_mutex_unlock (&fh->lock);
    avl_tree_unlock (fh_cache);
    return ret;
}


static void remove_from_fh (fh_node *fh, client_t *client)
{
    avl_delete (fh->clients, client, NULL);
}


static fh_node *find_fh (fbinfo *finfo)
{
    fh_node fh, *result = NULL;
    if (finfo->mount == NULL)
        finfo->mount = "";
    memcpy (&fh.finfo, finfo, sizeof (fbinfo));
    if (avl_get_by_key (fh_cache, &fh, (void**)&result) == 0)
        return result;
    return NULL;
}


static void fh_add_client (fh_node *fh, client_t *client)
{
    fh->refcount++;
    if (fh->stats)
    {
        stats_lock (fh->stats, NULL);
        stats_set_args (fh->stats, "listeners", "%ld", fh->refcount);
        if (fh->refcount > fh->peak)
        {
            fh->peak = fh->refcount;
            stats_set_args (fh->stats, "listener_peak", "%ld", fh->peak);
        }
        stats_release (fh->stats);
    }
    avl_insert (fh->clients, client);
    if (fh->format)
    {
        if (fh->format->create_client_data && client->format_data == NULL)
            fh->format->create_client_data (fh->format, client);
        if (fh->format->write_buf_to_client)
            client->check_buffer = fh->format->write_buf_to_client;
    }
    DEBUG2 ("refcount now %d for %s", fh->refcount, fh->finfo.mount);
}


/* find/create handle and return it with the structure in a locked state */
static fh_node *open_fh (fbinfo *finfo)
{
    fh_node *fh, *result;

    if (finfo->mount == NULL)
        finfo->mount = "";
    fh = calloc (1, sizeof (fh_node));
    memcpy (&fh->finfo, finfo, sizeof (fbinfo));
    if (avl_get_by_key (fh_cache, fh, (void**)&result) == 0)
    {
        free (fh);
        thread_mutex_lock (&result->lock);
        avl_tree_unlock (fh_cache);
        if ((finfo->flags & FS_FALLBACK) && result->finfo.type != finfo->type && finfo->type != FORMAT_TYPE_UNDEFINED)
        {
            WARN1 ("format mismatched for %s", finfo->mount);
            thread_mutex_unlock (&result->lock);
            return NULL;
        }
        return result;
    }

    // insert new one
    if (fh->finfo.mount[0])
    {
        char *fullpath= util_get_path_from_normalised_uri (fh->finfo.mount, fh->finfo.flags&FS_USE_ADMIN);
        char *contenttype = fserve_content_type (fullpath);
        format_type_t type = format_get_type (contenttype);

        free (contenttype);
        if (fh->finfo.type == FORMAT_TYPE_UNDEFINED)
            fh->finfo.type = type;
        if (finfo->flags & FS_FALLBACK)
        {
            if (fh->finfo.type != type && fh->finfo.type != FORMAT_TYPE_UNDEFINED)
            {
                avl_tree_unlock (fh_cache);
                free (fullpath);
                free (fh);
                WARN1 ("format mismatched for %s", finfo->mount);
                return NULL;
            }
            INFO2 ("lookup of fallback file \"%s\" (%d)", finfo->mount, finfo->limit);
        }
        else
            INFO1 ("lookup of \"%s\"", finfo->mount);
        if (file_open (&fh->f, fullpath) < 0)
        {
            INFO1 ("Failed to open \"%s\"", fullpath);
            if (finfo->flags & FS_FALLBACK)
            {
                avl_tree_unlock (fh_cache);
                free (fullpath);
                free (fh);
                return NULL;
            }
        }
        free (fullpath);
        if (fh->finfo.type != FORMAT_TYPE_UNDEFINED)
        {
            fh->format = calloc (1, sizeof (format_plugin_t));
            fh->format->type = fh->finfo.type;
            fh->format->mount = strdup (fh->finfo.mount);
            if (format_get_plugin (fh->format, NULL) < 0)
            {
                avl_tree_unlock (fh_cache);
                free (fh->format);
                free (fh);
                return NULL;
            }
            if (fh->finfo.limit)
                fh->out_bitrate = rate_setup (10000, 1000);
        }
    }
    fh->clients = avl_tree_new (client_compare, NULL);
    thread_mutex_create (&fh->lock);
    thread_mutex_lock (&fh->lock);
    avl_insert (fh_cache, fh);
    avl_tree_unlock (fh_cache);

    fh->refcount = 0;
    fh->peak = 0;
    if (fh->finfo.limit)
    {
        int len = strlen (finfo->mount) + 10;
        char *str = alloca (len);
        snprintf (str, len, "%s-%s", (finfo->flags & FS_FALLBACK) ? "fallback" : "file", finfo->mount);
        fh->stats = stats_handle (str);
        stats_set_flags (fh->stats, "fallback", "file", STATS_COUNTERS|STATS_HIDDEN);
        stats_set_flags (fh->stats, "outgoing_kbitrate", "0", STATS_COUNTERS|STATS_HIDDEN);
        stats_set_flags (fh->stats, "listeners", "1", STATS_GENERAL|STATS_HIDDEN);
        stats_set_flags (fh->stats, "listener_peak", "1", STATS_GENERAL|STATS_HIDDEN);
        stats_release (fh->stats);
    }
    fh->finfo.mount = strdup (finfo->mount);
    if (finfo->fallback)
        fh->finfo.fallback = strdup (finfo->fallback);

    return fh;
}


static int fill_http_headers (client_t *client, const char *path, struct stat *file_buf)
{
    char *type;
    off_t content_length = 0;
    const char *range = httpp_getvar (client->parser, "range");
    refbuf_t *ref = client->refbuf;


    if (file_buf)
        content_length = file_buf->st_size;
    /* full http range handling is currently not done but we deal with the common case */
    if (range)
    {
        off_t new_content_len = 0, rangenumber = 0;
        int ret = 0;

        if (strncasecmp (range, "bytes=", 6) == 0)
            ret = sscanf (range+6, "%" SCN_OFF_T "-", &rangenumber);

        if (ret == 1 && rangenumber>=0 && rangenumber < content_length)
        {
            /* Date: is required on all HTTP1.1 responses */
            char currenttime[50];
            time_t now;
            struct tm result;
            off_t endpos;
            fh_node * fh = client->shared_data;

            ret = lseek (fh->f, rangenumber, SEEK_SET);
            if (ret == -1)
                return -1;

            client->intro_offset = rangenumber;
            new_content_len = content_length - rangenumber;
            endpos = rangenumber + new_content_len - 1;
            if (endpos < 0)
                endpos = 0;

            now = client->worker->current_time.tv_sec;
            strftime(currenttime, 50, "%a, %d-%b-%Y %X GMT",
                    gmtime_r (&now, &result));
            client->respcode = 206;
            type = fserve_content_type (path);
            snprintf (ref->data, BUFSIZE,
                    "HTTP/1.1 206 Partial Content\r\n"
                    "Date: %s\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Content-Length: %" PRI_OFF_T "\r\n"
                    "Content-Range: bytes %" PRI_OFF_T
                    "-%" PRI_OFF_T 
                    "/%" PRI_OFF_T "\r\n"
                    "Content-Type: %s\r\n\r\n",
                    currenttime,
                    new_content_len,
                    rangenumber,
                    endpos,
                    content_length,
                    type);
        }
        else
            return -1;
    }
    else
    {
        type = fserve_content_type (path);
        client->respcode = 200;
        if (content_length)
            snprintf (ref->data, BUFSIZE,
                    "HTTP/1.0 200 OK\r\n"
                    "Accept-Ranges: bytes\r\n"
                    "Content-Type: %s\r\n"
                    "Content-Length: %" PRI_OFF_T "\r\n"
                    "\r\n",
                    type,
                    content_length);
        else
            snprintf (ref->data, BUFSIZE,
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: %s\r\n"
                    "\r\n",
                    type);
    }
    free (type);
    client->refbuf->len = strlen (ref->data);
    client->pos = 0;
    ref->flags |= WRITE_BLOCK_GENERIC;
    return 0;
}


/* client has requested a file, so check for it and send the file.  Do not
 * refer to the client_t afterwards.  return 0 for success, -1 on error.
 */
int fserve_client_create (client_t *httpclient, const char *path)
{
    struct stat file_buf;
    char *fullpath;
    int m3u_requested = 0, m3u_file_available = 1;
    int xspf_requested = 0, xspf_file_available = 1;
    int ret = -1;
    ice_config_t *config;
    fbinfo finfo;

    fullpath = util_get_path_from_normalised_uri (path, 0);
    DEBUG2 ("checking for file %s (%s)", path, fullpath);

    if (strcmp (util_get_extension (fullpath), "m3u") == 0)
        m3u_requested = 1;

    if (strcmp (util_get_extension (fullpath), "xspf") == 0)
        xspf_requested = 1;

    /* check for the actual file */
    if (stat (fullpath, &file_buf) != 0)
    {
        /* the m3u can be generated, but send an m3u file if available */
        if (m3u_requested == 0 && xspf_requested == 0)
        {
            if (redirect_client (path, httpclient) == 0)
            {
                if ((httpclient->flags & CLIENT_SKIP_ACCESSLOG) == 0)
                    WARN2 ("req for file \"%s\" %s", fullpath, strerror (errno));
                ret = client_send_404 (httpclient, "The file you requested could not be found");
            }
            free (fullpath);
            return ret;
        }
        m3u_file_available = 0;
        xspf_file_available = 0;
    }

    httpclient->refbuf->len = PER_CLIENT_REFBUF_SIZE;

    if (m3u_requested && m3u_file_available == 0)
    {
        const char  *host = httpp_getvar (httpclient->parser, "host"),
                    *args = httpp_getvar (httpclient->parser, HTTPP_VAR_QUERYARGS),
                    *at = "", *user = "", *pass ="";
        char *sourceuri = strdup (path);
        char *dot = strrchr (sourceuri, '.');
        char *protocol = "http";
        const char *agent = httpp_getvar (httpclient->parser, "user-agent");

        if (agent)
        {
            if (strstr (agent, "QTS") || strstr (agent, "QuickTime"))
                protocol = "icy";
        }
        /* at least a couple of players (fb2k/winamp) are reported to send a 
         * host header but without the port number. So if we are missing the
         * port then lets treat it as if no host line was sent */
        if (host && strchr (host, ':') == NULL)
            host = NULL;

        *dot = 0;
        if (httpclient->username && httpclient->password)
        {
            at = "@";
            user = httpclient->username;
            pass = httpclient->password;
        }
        httpclient->respcode = 200;
        if (host == NULL)
        {
            config = config_get_config();
            snprintf (httpclient->refbuf->data, BUFSIZE,
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: audio/x-mpegurl\r\n\r\n"
                    "%s://%s%s%s%s%s:%d%s%s\r\n", 
                    protocol,
                    user, at[0]?":":"", pass, at, 
                    config->hostname, config->port,
                    sourceuri,
                    args?args:"");
            config_release_config();
        }
        else
        {
            snprintf (httpclient->refbuf->data, BUFSIZE,
                    "HTTP/1.0 200 OK\r\n"
                    "Content-Type: audio/x-mpegurl\r\n\r\n"
                    "%s://%s%s%s%s%s%s%s\r\n",
                    protocol,
                    user, at[0]?":":"", pass, at, 
                    host, 
                    sourceuri,
                    args?args:"");
        }
        httpclient->refbuf->len = strlen (httpclient->refbuf->data);
        free (sourceuri);
        free (fullpath);
        return fserve_setup_client_fb (httpclient, NULL);
    }
    if (xspf_requested && xspf_file_available == 0)
    {
        xmlDocPtr doc;
        char *reference = strdup (path);
        char *eol = strrchr (reference, '.');
        if (eol)
            *eol = '\0';
        doc = stats_get_xml (0, reference);
        free (reference);
        free (fullpath);
        return admin_send_response (doc, httpclient, XSLT, "xspf.xsl");
    }

    /* on demand file serving check */
    config = config_get_config();
    if (config->fileserve == 0)
    {
        config_release_config();
        DEBUG1 ("on demand file \"%s\" refused", fullpath);
        free (fullpath);
        return client_send_404 (httpclient, "The file you requested could not be found");
    }
    config_release_config();

    if (S_ISREG (file_buf.st_mode) == 0)
    {
        WARN1 ("found requested file but there is no handler for it: %s", fullpath);
        free (fullpath);
        return client_send_404 (httpclient, "The file you requested could not be found");
    }

    free (fullpath);
    finfo.flags = 0;
    finfo.mount = (char *)path;
    finfo.fallback = NULL;
    finfo.limit = 0;
    finfo.type = FORMAT_TYPE_UNDEFINED;
    stats_event_inc (NULL, "file_connections");

    return fserve_setup_client_fb (httpclient, &finfo);
}


// fh must be locked before calling this
static void fh_release (fh_node *fh)
{
    if (fh->finfo.mount[0])
        DEBUG3 ("refcount now %d on %s%s", fh->refcount, (fh->stats)?"file-": "", fh->finfo.mount);
    thread_mutex_unlock (&fh->lock);
    if (remove_fh_from_cache (fh))
    {
        fh->peak = 0;
        if (fh->stats) // drop fallback file stats
        {
            int len = strlen (fh->finfo.mount) + 10;
            char *str = alloca (len);
            snprintf (str, len, "%s-%s", (fh->finfo.flags & FS_FALLBACK) ? "fallback" : "file", fh->finfo.mount);
            stats_set (fh->stats, "fallback", NULL); // is the fallback stat hack needed now?
            stats_event (str, NULL, NULL);
        }
        _delete_fh (fh);
    }
}


static void _free_fserve_buffers (client_t *client)
{
    refbuf_t *buf = client->refbuf;
    while (buf)
    {
        refbuf_t *old = buf;
        buf = old->next;
        old->next = NULL;
        refbuf_release (old);
    }
    client->refbuf = NULL;
}


static void file_release (client_t *client)
{
    fh_node *fh = client->shared_data;
    int ret = -1;

    if (fh)
    {
        thread_mutex_lock (&fh->lock);
        if (fh->finfo.flags & FS_FALLBACK)
            stats_event_dec (NULL, "listeners");
        remove_from_fh (fh, client);
        fh_release (fh);
    }
    _free_fserve_buffers (client);
    if (client->flags & CLIENT_AUTHENTICATED)
    {
        const char *mount = httpp_getvar (client->parser, HTTPP_VAR_URI);
        ice_config_t *config = config_get_config ();
        mount_proxy *mountinfo = config_find_mount (config, mount);
        if (mountinfo && mountinfo->access_log.name)
            logging_access_id (&mountinfo->access_log, client);
        ret = auth_release_listener (client, mount, mountinfo);
        config_release_config();
    }
    if (ret < 0)
    {
        client->flags &= ~CLIENT_AUTHENTICATED;
        client_destroy (client);
    }
    global_reduce_bitrate_sampling (global.out_bitrate);
}


struct _client_functions buffer_content_ops =
{
    prefile_send,
    file_release
};


struct _client_functions file_content_ops =
{
    file_send,
    file_release
};


static int fserve_move_listener (client_t *client)
{
    fh_node *fh = client->shared_data;
    int ret = 0;
    fbinfo f;

    _free_fserve_buffers (client);
    thread_mutex_lock (&fh->lock);
    f.flags = fh->finfo.flags|FS_OVERRIDE;
    f.limit = fh->finfo.limit;
    f.mount = fh->finfo.fallback;
    f.fallback = fh->finfo.mount;
    f.type = fh->finfo.type;
    if (move_listener (client, &f) < 0)
    {
        thread_mutex_unlock (&fh->lock);
        WARN1 ("moved failed, terminating listener on %s", fh->finfo.mount);
        ret = -1;
    }
    else
    {
        remove_from_fh (fh, client);
        fh_release (fh);
    }
    return ret;
}

struct _client_functions throttled_file_content_ops;

static int prefile_send (client_t *client)
{
    refbuf_t *refbuf = client->refbuf;
    int loop = 8, bytes, written = 0;
    worker_t *worker = client->worker;

    while (loop)
    {
        fh_node *fh = client->shared_data;
        loop--;
        if (fserve_running == 0 || client->connection.error)
            return -1;
        if (refbuf == NULL || client->pos == refbuf->len)
        {
            if (fh && fh->finfo.fallback)
                return fserve_move_listener (client);

            if (refbuf == NULL || refbuf->next == NULL)
            {
                if (fh)
                {
                    if (file_in_use (fh->f)) // is there a file to read from
                    {
                        int len = 8192;
                        if (fh->finfo.limit)
                            client->ops = &throttled_file_content_ops;
                        else
                            client->ops = &file_content_ops;
                        refbuf_release (client->refbuf);
                        client->refbuf = refbuf_new(len);
                        client->pos = len;
                        return client->ops->process (client);
                    }
                }
                if (client->respcode)
                    return -1;
                thread_mutex_lock (&fh->lock);
                fh_release (fh);
                return client_send_404 (client, NULL);
            }
            else
            {
                refbuf = refbuf->next;
                client->refbuf->next = NULL;
                refbuf_release (client->refbuf);
                client->refbuf = refbuf;
            }
            client->pos = 0;
        }
        if (refbuf->flags & WRITE_BLOCK_GENERIC)
            bytes = format_generic_write_to_client (client);
        else 
            bytes = client->check_buffer (client);
        if (bytes < 0)
        {
            client->schedule_ms = worker->time_ms + (written ? 150 : 300);
            return 0;
        }
        written += bytes;
        global_add_bitrates (global.out_bitrate, bytes, worker->time_ms);
        if (written > 30000)
            break;
    }
    return 0;
}


/* fast send routine */
static int file_send (client_t *client)
{
    refbuf_t *refbuf = client->refbuf;
    int loop = 6, bytes, written = 0, ret = 0;
    fh_node *fh = client->shared_data;
    worker_t *worker = client->worker;
    time_t now;

    client->schedule_ms = worker->time_ms;
    now = worker->current_time.tv_sec;
    /* slowdown if max bandwidth is exceeded, but allow for short-lived connections to avoid 
     * this, eg admin requests */
    if (throttle_sends > 1 && now - client->connection.con_time > 1)
    {
        client->schedule_ms += 300;
        loop = 1; 
    }
    while (loop && written < 30000)
    {
        loop--;
        if (fserve_running == 0 || client->connection.error)
            return -1;
        if (client->connection.discon_time && now >= client->connection.discon_time)
            return -1;
        if (client->pos == refbuf->len)
        {
            ret = pread (fh->f, refbuf->data, 8192, client->intro_offset);
            if (ret <= 0)
                return -1;
            refbuf->len = ret;
            client->intro_offset += ret;
            client->pos = 0;
        }
        bytes = client->check_buffer (client);
        if (bytes < 0)
        {
            client->schedule_ms += (written ? 120 : 250);
            break;
        }
        written += bytes;
        client->schedule_ms += 3;
    }
    return 0;
}


static int fserve_change_worker (client_t *client)
{
    worker_t *this_worker = client->worker, *worker;
    int ret = 0;

    if (this_worker->move_allocations == 0 || worker_count < 2)
        return 0;
    thread_rwlock_rlock (&workers_lock);
    worker = worker_selected ();
    if (worker && worker != client->worker)
    {
        long diff = this_worker->count - worker->count;
        if (diff > 15)
        {
            this_worker->move_allocations--;
            ret = client_change_worker (client, worker);
            if (ret)
                DEBUG2 ("moving listener from %p to %p", this_worker, worker);
        }
    }
    thread_rwlock_unlock (&workers_lock);
    return ret;
}


/* send routine for files sent at a target bitrate, eg fallback files. */
static int throttled_file_send (client_t *client)
{
    refbuf_t *refbuf = client->refbuf;
    int  bytes;
    fh_node *fh = client->shared_data;
    time_t now;
    worker_t *worker = client->worker;
    unsigned long secs; 
    unsigned int  rate = 0;
    unsigned int limit = fh->finfo.limit;

    if (fserve_running == 0 || client->connection.error)
        return -1;
    now = worker->current_time.tv_sec;
    secs = now - client->timer_start; 
    client->schedule_ms = worker->time_ms;
    if (client->connection.discon_time && now >= client->connection.discon_time)
        return -1;
    if (fh->finfo.fallback)
        return fserve_move_listener (client);

    if (fserve_change_worker (client)) // allow for balancing
        return 1;

    if (client->flags & CLIENT_WANTS_FLV) /* increase limit for flv clients as wrapping takes more space */
        limit = (unsigned long)(limit * 1.01);
    if (secs)
        rate = (client->counter+1400)/secs;
    // DEBUG3 ("counter %lld, duration %ld, limit %u", client->counter, secs, rate);
    if (rate > limit || secs < 3)
    {
        if (limit >= 1400)
            client->schedule_ms += 1000/(limit/1400);
        else
            client->schedule_ms += 50; // should not happen but guard against it
        rate_add (fh->out_bitrate, 0, worker->time_ms);
        if (secs > 2)
        {
            global_add_bitrates (global.out_bitrate, 0, worker->time_ms);
            return 0;
        }
    }
    if (fh->stats_update <= now)
    {
        int update_stats = 0;
        thread_mutex_lock (&fh->lock);
        if (fh->stats_update <= now)
        {
            // only the first client should do this
            fh->stats_update = now + 5;
            update_stats = 1;
        }
        thread_mutex_unlock (&fh->lock);
        if (update_stats)
            stats_set_args (fh->stats, "outgoing_kbitrate", "%ld",
                    (long)((8 * rate_avg (fh->out_bitrate))/1024));
    }
    if (client->pos == refbuf->len)
    {
        //DEBUG1 ("reading another block from offset %ld", client->intro_offset);
        int ret = format_file_read (client, fh->format, fh->f);

        switch (ret)
        {
            case -1: /* loop fallback file  */
                // DEBUG0 ("loop of file triggered");
                client->intro_offset = 0;
                client->schedule_ms += 150;
                return 0;
            case -2: /* non-recoverable */
                // DEBUG0 ("major failure on read, better leave");
                return -1;
            default:  ;
        }
        client->pos = 0;
    }
    bytes = client->check_buffer (client);
    if (bytes < 0)
        bytes = 0;
    //DEBUG3 ("bytes %d, counter %ld, %ld", bytes, client->counter, client->worker->time_ms - (client->timer_start*1000));
    rate_add (fh->out_bitrate, bytes, worker->time_ms);
    global_add_bitrates (global.out_bitrate, bytes, worker->time_ms);
    if (limit > 2800)
        client->schedule_ms += (1000/(limit/1400*2));
    else
        client->schedule_ms += 50;

    /* progessive slowdown if max bandwidth is exceeded. */
    if (throttle_sends > 1)
        client->schedule_ms += 300;
    return 0;
}


struct _client_functions throttled_file_content_ops =
{
    throttled_file_send,
    file_release
};


/* return 0 for success, -1 for fallback invalid */
int fserve_setup_client_fb (client_t *client, fbinfo *finfo)
{
    if (finfo)
    {
        mount_proxy *minfo;
        fh_node *fh;
        if (finfo->flags & FS_FALLBACK && finfo->limit == 0)
            return -1;
        avl_tree_wlock (fh_cache);
        fh = find_fh (finfo);
        minfo = config_find_mount (config_get_config(), finfo->mount);
        if (fh)
        {
            thread_mutex_lock (&fh->lock);
            avl_tree_unlock (fh_cache);
            client->shared_data = NULL;
            if (minfo)
            {
                if (minfo->max_listeners >= 0 && fh->refcount > minfo->max_listeners)
                {
                    thread_mutex_unlock (&fh->lock);
                    config_release_config();
                    return client_send_403redirect (client, finfo->mount, "max listeners reached");
                }
                if (check_duplicate_logins (finfo->mount, fh->clients, client, minfo->auth) == 0)
                {
                    thread_mutex_unlock (&fh->lock);
                    config_release_config();
                    return client_send_403 (client, "Account already in use");
                }
            }
            config_release_config();
        }
        else
        {
            if (minfo && minfo->max_listeners == 0)
            {
                config_release_config();
                fh_release (fh);
                client->shared_data = NULL;
                return client_send_403redirect (client, finfo->mount, "max listeners reached");
            }
            config_release_config();
            fh = open_fh (finfo);
            if (fh == NULL)
                return -1;
        }
        fh_add_client (fh, client);
        client->shared_data = fh;

        if (fh->finfo.limit)
        {
            client->timer_start = client->worker->current_time.tv_sec;
            if (client->connection.sent_bytes == 0)
                client->timer_start -= 2;
            client->counter = 0;
            client->intro_offset = 0;
            global_reduce_bitrate_sampling (global.out_bitrate);
        }
        thread_mutex_unlock (&fh->lock);
        if (client->respcode == 0)
            fill_http_headers (client, finfo->mount, NULL);
        client->mount = fh->finfo.mount;
    }
    if (client->check_buffer == NULL)
        client->check_buffer = format_generic_write_to_client;

    client->ops = &buffer_content_ops;
    client->flags &= ~CLIENT_HAS_INTRO_CONTENT;
    client->flags |= CLIENT_IN_FSERVE;
    if (client->flags & CLIENT_ACTIVE)
    {
        client->schedule_ms = client->worker->time_ms;
        if (finfo && finfo->flags & FS_FALLBACK)
            return 0; // prevent a recursive loop 
        return client->ops->process (client);
    }
    else
    {
        worker_t *worker = client->worker;
        client->flags |= CLIENT_ACTIVE;
        worker_wakeup (worker); /* worker may of already processed client but make sure */
    }
    return 0;
}


int fserve_setup_client (client_t *client)
{
    client->check_buffer = format_generic_write_to_client;
    return fserve_setup_client_fb (client, NULL);
}


int fserve_set_override (const char *mount, const char *dest, format_type_t type)
{
    fh_node fh, *result;

    fh.finfo.flags = FS_FALLBACK;
    fh.finfo.mount = (char *)mount;
    fh.finfo.fallback = NULL;
    fh.finfo.type = type;

    avl_tree_wlock (fh_cache);
    if (avl_get_by_key (fh_cache, &fh, (void**)&result) == 0 && result->finfo.type == type)
    {
        char *tmp = result->finfo.fallback;
        thread_mutex_lock (&result->lock);
        avl_delete (fh_cache, result, NULL);
        avl_tree_unlock (fh_cache);

        result->finfo.flags |= FS_OVERRIDE;
        result->finfo.fallback = strdup (dest);
        result->finfo.type = type;
        thread_mutex_unlock (&result->lock);
        free (tmp);
        INFO2 ("move clients from %s to %s", mount, dest);
        return 1;
    }
    avl_tree_unlock (fh_cache);
    return 0;
}

static int _delete_mapping(void *mapping) {
    mime_type *map = mapping;
    free(map->ext);
    free(map->type);
    free(map);

    return 1;
}

static int _compare_mappings(void *arg, void *a, void *b)
{
    return strcmp(
            ((mime_type *)a)->ext,
            ((mime_type *)b)->ext);
}

void fserve_recheck_mime_types (ice_config_t *config)
{
    FILE *mimefile;
    char line[4096];
    char *type, *ext, *cur;
    mime_type *mapping;
    avl_tree *new_mimetypes;

    if (config->mimetypes_fn == NULL)
        return;
    mimefile = fopen (config->mimetypes_fn, "r");
    if (mimefile == NULL)
    {
        WARN1 ("Cannot open mime types file %s", config->mimetypes_fn);
        return;
    }

    new_mimetypes = avl_tree_new(_compare_mappings, NULL);

    while(fgets(line, 4096, mimefile))
    {
        line[4095] = 0;

        if(*line == 0 || *line == '#')
            continue;

        type = line;

        cur = line;

        while(*cur != ' ' && *cur != '\t' && *cur)
            cur++;
        if(*cur == 0)
            continue;

        *cur++ = 0;

        while(1) {
            while(*cur == ' ' || *cur == '\t')
                cur++;
            if(*cur == 0)
                break;

            ext = cur;
            while(*cur != ' ' && *cur != '\t' && *cur != '\n' && *cur)
                cur++;
            *cur++ = 0;
            if(*ext)
            {
                void *tmp;
                /* Add a new extension->type mapping */
                mapping = malloc(sizeof(mime_type));
                mapping->ext = strdup(ext);
                mapping->type = strdup(type);
                if (!avl_get_by_key (new_mimetypes, mapping, &tmp))
                    avl_delete (new_mimetypes, mapping, _delete_mapping);
                if (avl_insert (new_mimetypes, mapping) != 0)
                    _delete_mapping (mapping);
            }
        }
    }
    fclose(mimefile);

    thread_spin_lock (&pending_lock);
    if (mimetypes)
        avl_tree_free (mimetypes, _delete_mapping);
    mimetypes = new_mimetypes;
    thread_spin_unlock (&pending_lock);
}


int fserve_kill_client (client_t *client, const char *mount, int response)
{
    int loop = 2, id;
    fbinfo finfo;
    xmlDocPtr doc;
    xmlNodePtr node;
    const char *idtext, *v = "0";
    char buf[50];

    finfo.flags = 0;
    finfo.mount = (char*)mount;
    finfo.limit = 0;
    finfo.fallback = NULL;

    idtext = httpp_get_query_param (client->parser, "id");
    if (idtext == NULL)
        return client_send_400 (client, "missing parameter id");

    id = atoi(idtext);

    doc = xmlNewDoc(XMLSTR("1.0"));
    node = xmlNewDocNode(doc, NULL, XMLSTR("iceresponse"), NULL);
    xmlDocSetRootElement(doc, node);
    snprintf (buf, sizeof(buf), "Client %d not found", id);

    avl_tree_rlock (fh_cache);
    while (1)
    {
        avl_node *node;
        fh_node *fh = find_fh (&finfo);
        if (fh)
        {
            thread_mutex_lock (&fh->lock);
            avl_tree_unlock (fh_cache);
            node = avl_get_first (fh->clients);
            while (node)
            {
                client_t *listener = (client_t *)node->key;
                if (listener->connection.id == id)
                {
                    listener->connection.error = 1;
                    snprintf (buf, sizeof(buf), "Client %d removed", id);
                    v = "1";
                    loop = 0;
                    break;
                }
                node = avl_get_next (node);
            }
            thread_mutex_unlock (&fh->lock);
            avl_tree_rlock (fh_cache);
        }
        if (loop == 0) break;
        loop--;
        if (loop == 1) finfo.flags = FS_FALLBACK;
    }
    avl_tree_unlock (fh_cache);
    xmlNewChild (node, NULL, XMLSTR("message"), XMLSTR(buf));
    xmlNewChild (node, NULL, XMLSTR("return"), XMLSTR(v));
    return admin_send_response (doc, client, response, "response.xsl");
}


int fserve_list_clients_xml (xmlNodePtr parent, fbinfo *finfo)
{
    int ret = 0;
    fh_node *fh;
    avl_node *anode;

    avl_tree_rlock (fh_cache);
    fh = find_fh (finfo);
    if (fh == NULL)
    {
        avl_tree_unlock (fh_cache);
        return 0;
    }
    thread_mutex_lock (&fh->lock);
    avl_tree_unlock (fh_cache);

    anode = avl_get_first (fh->clients);
    while (anode)
    {
        client_t *listener = (client_t *)anode->key;

        stats_listener_to_xml (listener, parent);
        ret++;
        anode = avl_get_next (anode);
    }
    thread_mutex_unlock (&fh->lock);
    return ret;
}


int fserve_list_clients (client_t *client, const char *mount, int response, int show_listeners)
{
    int ret;
    fbinfo finfo;
    xmlDocPtr doc;
    xmlNodePtr node, srcnode;

    finfo.flags = FS_FALLBACK;
    finfo.mount = (char*)mount;
    finfo.limit = 0;
    finfo.fallback = NULL;

    doc = xmlNewDoc(XMLSTR("1.0"));
    node = xmlNewDocNode(doc, NULL, XMLSTR("icestats"), NULL);
    xmlDocSetRootElement(doc, node);
    srcnode = xmlNewChild(node, NULL, XMLSTR("source"), NULL);
    xmlSetProp(srcnode, XMLSTR("mount"), XMLSTR(mount));

    ret = fserve_list_clients_xml (srcnode, &finfo);
    if (ret == 0 && finfo.flags&FS_FALLBACK)
    {
        finfo.flags = 0; // retry
        ret = fserve_list_clients_xml (srcnode, &finfo);
    }
    if (ret)
    {
        char buf [20];
        snprintf (buf, sizeof(buf), "%d", ret);
        xmlNewChild (srcnode, NULL, XMLSTR("listeners"), XMLSTR(buf));
        return admin_send_response (doc, client, response, "listclients.xsl");
    }
    xmlFree (doc);
    return client_send_400 (client, "mount does not exist");
}


int fserve_query_count (fbinfo *finfo)
{
    int ret = 0;
    fh_node *fh;

    avl_tree_rlock (fh_cache);
    fh = find_fh (finfo);
    if (fh)
        ret = fh->refcount;
    avl_tree_unlock (fh_cache);
    return ret;
}


int file_in_use (icefile_handle f)
{
    return f != -1;
}


void file_close (icefile_handle *f)
{
   if (*f != -1)
       close (*f);
   *f = -1;
}


int file_open (icefile_handle *f, const char *fn)
{
    *f = open (fn, O_RDONLY|O_CLOEXEC|O_BINARY);
    return (*f) < 0 ? -1 : 0;
}


#ifndef HAVE_PREAD
ssize_t pread (icefile_handle f, void *data, size_t count, off_t offset)
{
    ssize_t bytes = -1;

    // we do not want another thread to modifiy handle between seek and read 
    // win32 may be able to use the overlapped io struct in ReadFile
    thread_mutex_lock (&seekread_lock);
    if (lseek (f, offset, SEEK_SET) != (off_t)-1)
        bytes = read (f, data, count);
    thread_mutex_unlock (&seekread_lock);
    return bytes;
}
#endif
