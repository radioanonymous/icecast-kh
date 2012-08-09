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

/** 
 * Client authentication functions
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include "auth.h"
#include "auth_htpasswd.h"
#include "auth_radio.h"
#include "auth_cmd.h"
#include "auth_url.h"
#include "source.h"
#include "client.h"
#include "cfgfile.h"
#include "stats.h"
#include "httpp/httpp.h"
#include "fserve.h"
#include "admin.h"
#include "global.h"

#include "logging.h"
#define CATMODULE "auth"

struct _auth_thread_t
{
    thread_type *thread;
    void *data;
    unsigned int id;
    struct auth_tag *auth;
};

static volatile int thread_id;
static rwlock_t auth_lock;
int allow_auth;

static void *auth_run_thread (void *arg);
static int  auth_postprocess_listener (auth_client *auth_user);
static void auth_postprocess_source (auth_client *auth_user);
static int  wait_for_auth (client_t *client);


struct _client_functions auth_release_ops =
{
    wait_for_auth,
    client_destroy
};


static int wait_for_auth (client_t *client)
{
    DEBUG0 ("client finished with auth");
    return -1;
}


void auth_check_http (client_t *client)
{
    const char *header;
    char *username, *password;

    /* process any auth headers if any available */
    header = httpp_getvar (client->parser, "authorization");
    if (header == NULL)
        return;

    if (strncmp(header, "Basic ", 6) == 0)
    {
        /* This will look something like "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==" */
        char *tmp, *userpass = util_base64_decode (header+6);
        if (userpass == NULL)
        {
            WARN1("Base64 decode of Authorization header \"%s\" failed",
                    header+6);
            return;
        }

        tmp = strchr(userpass, ':');
        if (tmp == NULL)
        {
            free (userpass);
            return;
        }

        *tmp = 0;
        username = userpass;
        password = tmp+1;
        client->username = strdup (username);
        client->password = strdup (password);
        free (userpass);
        return;
    }
    WARN1 ("unhandled authorization header: %s", header);
}


static auth_client *auth_client_setup (const char *mount, client_t *client)
{
    ice_config_t *config = config_get_config_unlocked();
    auth_client *auth_user = calloc (1, sizeof(auth_client));

    auth_user->mount = strdup (mount);
    auth_user->hostname = strdup (config->hostname);
    auth_user->port = config->port;
    auth_user->client = client;
    return auth_user;
}


static void queue_auth_client (auth_client *auth_user, mount_proxy *mountinfo)
{
    auth_t *auth;

    if (auth_user == NULL || mountinfo == NULL)
        return;
    auth = mountinfo->auth;
    thread_mutex_lock (&auth->lock);
    auth_user->next = NULL;
    auth_user->auth = auth;
    *auth->tailp = auth_user;
    auth->tailp = &auth_user->next;
    auth->pending_count++;
    if (auth->refcount > auth->handlers)
        DEBUG0 ("max authentication handlers allocated");
    else
    {
        int i;
        for (i=0; i<auth->handlers; i++)
        {
            if (auth->handles [i].thread == NULL)
            {
                DEBUG1 ("starting auth thread %d", i);
                auth->refcount++;
                auth->handles [i].thread = thread_create ("auth thread", auth_run_thread,
                        &auth->handles [i], THREAD_DETACHED);
                break;
            }
        }
    }
    DEBUG2 ("auth on %s has %d pending", auth->mount, auth->pending_count);
    thread_mutex_unlock (&auth->lock);
}


/* release the auth. It is referred to by multiple structures so this is
 * refcounted and only actual freed after the last use
 */
void auth_release (auth_t *authenticator)
{
    if (authenticator == NULL)
        return;

    thread_mutex_lock (&authenticator->lock);
    authenticator->refcount--;
    DEBUG2 ("...refcount on auth_t %s is now %d", authenticator->mount, authenticator->refcount);
    if (authenticator->refcount)
    {
        thread_mutex_unlock (&authenticator->lock);
        return;
    }

    /* cleanup auth threads attached to this auth */
    authenticator->running = 0;
    while (authenticator->handlers)
    {
        if (authenticator->release_thread_data)
            authenticator->release_thread_data (authenticator,
                    authenticator->handles [authenticator->handlers-1].data);
        authenticator->handlers--;
    }
    free (authenticator->handles);

    if (authenticator->release)
        authenticator->release (authenticator);
    xmlFree (authenticator->type);
    xmlFree (authenticator->realm);
    xmlFree (authenticator->rejected_mount);
    thread_mutex_unlock (&authenticator->lock);
    thread_mutex_destroy (&authenticator->lock);
    free (authenticator->mount);
    free (authenticator);
}


static void auth_client_free (auth_client *auth_user)
{
    if (auth_user == NULL)
        return;
    if (auth_user->client)
    {
        client_t *client = auth_user->client;

        if (client->respcode)
            client->connection.error = 1;
        client_send_401 (client, auth_user->auth->realm);
        auth_user->client = NULL;
    }
    free (auth_user->hostname);
    free (auth_user->mount);
    free (auth_user);
}


/* verify that the listener is still connected. */
static int is_listener_connected (client_t *client)
{
    int ret = 1;
    if (client)
    {
        if (sock_active (client->connection.sock) == 0)
            ret = 0;
    }
    return ret;
}


/* wrapper function for auth thread to authenticate new listener
 * connection details
 */
static void auth_new_listener (auth_client *auth_user)
{
    client_t *client = auth_user->client;

    /* make sure there is still a client at this point, a slow backend request
     * can be avoided if client has disconnected */
    if (allow_auth == 0 || is_listener_connected (client) == 0)
    {
        DEBUG0 ("dropping listener connection");
        client->respcode = 400;
        return;
    }
    if (auth_user->auth->authenticate)
    {
        switch (auth_user->auth->authenticate (auth_user))
        {
            case AUTH_OK:
            case AUTH_FAILED:
                break;
            default:
                return;
        }
    }
    if (auth_postprocess_listener (auth_user) < 0)
        DEBUG0 ("listener connection failed");
}


/* wrapper function for auth thread to drop listener connections
 */
static void auth_remove_listener (auth_client *auth_user)
{
    if (auth_user->auth->release_listener)
        auth_user->auth->release_listener (auth_user);
    auth_user->auth = NULL;

    /* client is going, so auth is not an issue at this point */
    if (auth_user->client)
    {
        client_t *client = auth_user->client;
        if (client->worker)
            client_send_404 (client, NULL);
        else
        {
            client->flags &= ~CLIENT_AUTHENTICATED;
            client_destroy (auth_user->client);
        }
        auth_user->client = NULL;
    }
}


/* Called from auth thread to process any request for source client
 * authentication. Only applies to source clients, not relays.
 */
static void stream_auth_callback (auth_client *auth_user)
{
    client_t *client = auth_user->client;

    if (auth_user->auth->stream_auth)
        auth_user->auth->stream_auth (auth_user);

    if (client->flags & CLIENT_AUTHENTICATED)
        auth_postprocess_source (auth_user);
    else
        WARN1 ("Failed auth for source \"%s\"", auth_user->mount);
}


/* Callback from auth thread to handle a stream start event, this applies
 * to both source clients and relays.
 */
static void stream_start_callback (auth_client *auth_user)
{
    auth_t *auth = auth_user->auth;

    if (auth->stream_start)
        auth->stream_start (auth_user);
}


/* Callback from auth thread to handle a stream start event, this applies
 * to both source clients and relays.
 */
static void stream_end_callback (auth_client *auth_user)
{
    auth_t *auth = auth_user->auth;

    if (auth->stream_end)
        auth->stream_end (auth_user);
}


/* The auth thread main loop. */
static void *auth_run_thread (void *arg)
{
    auth_thread_t *handler = arg;
    auth_t *auth = handler->auth;

    DEBUG2 ("Authentication thread %d started for %s", handler->id, auth->mount);
    thread_rwlock_rlock (&auth_lock);

    while (1)
    {
        thread_mutex_lock (&auth->lock);
        if (auth->head)
        {
            auth_client *auth_user = auth->head;

            DEBUG2 ("%d client(s) pending on %s", auth->pending_count, auth->mount);
            auth->head = auth_user->next;
            if (auth->head == NULL)
                auth->tailp = &auth->head;
            auth->pending_count--;
            thread_mutex_unlock (&auth->lock);
            auth_user->next = NULL;

            /* associate per-thread data with auth_user here */
            auth_user->thread_data = handler->data;
            auth_user->handler = handler->id;

            if (auth_user->process)
                auth_user->process (auth_user);

            auth_client_free (auth_user);

            continue;
        }
        handler->thread = NULL;
        thread_mutex_unlock (&auth->lock);
        break;
    }
    DEBUG1 ("Authenication thread %d shutting down", handler->id);
    auth_release (auth);
    thread_rwlock_unlock (&auth_lock);
    return NULL;
}


int move_listener (client_t *client, struct _fbinfo *finfo)
{
    source_t *source;
    mount_proxy *minfo;
    int rate = finfo->limit, loop = 20, ret = -1;
    ice_config_t *config = config_get_config();
    struct _fbinfo where;

    memcpy (&where, finfo, sizeof (where));
    where.mount = strdup (finfo->mount);
    avl_tree_rlock (global.source_tree);
    do
    {
        minfo = config_find_mount (config, where.mount);

        if (rate == 0 && minfo && minfo->limit_rate)
            rate = minfo->limit_rate;
        source = source_find_mount_raw (where.mount);

        if (source == NULL && minfo == NULL)
            break;
        if (source)
        {
            thread_rwlock_wlock (&source->lock);
            if (source_available (source))
            {
                // an unused on-demand relay will still have an unitialised type
                if (source->format->type == finfo->type || source->format->type == FORMAT_TYPE_UNDEFINED)
                {
                    config_release_config();
                    avl_tree_unlock (global.source_tree);
                    source_setup_listener (source, client);
                    client->flags |= CLIENT_HAS_MOVED;
                    thread_rwlock_unlock (&source->lock);
                    return 0;
                }
            }
            thread_rwlock_unlock (&source->lock);
        }
        if (minfo && minfo->fallback_mount)
        {
            free (where.mount);
            where.mount = strdup (minfo->fallback_mount);
        }
        else
        {
            break;
        }
    } while (loop--);

    avl_tree_unlock (global.source_tree);
    config_release_config();
    if (where.mount && ((client->flags & CLIENT_IS_SLAVE) == 0))
    {
        if (finfo->flags & FS_OVERRIDE)
        {
            free (where.mount);
            where.mount = strdup (where.fallback);
            where.fallback = NULL;
            where.flags &= ~FS_OVERRIDE;
        }
        if (where.limit == 0)
        {
            if (rate == 0)
                if (sscanf (where.mount, "%*[^[][%d]", &rate) == 1)
                    rate = rate * 1000/8;
            where.limit = rate;
        }
        ret = fserve_setup_client_fb (client, &where);
    }
    free (where.mount);
    return ret;
}


/* Add listener to the pending lists of either the source or fserve thread. This can be run
 * from the connection or auth thread context. return -1 to indicate that client has been
 * terminated, 0 for receiving content.
 */
static int add_authenticated_listener (const char *mount, mount_proxy *mountinfo, client_t *client)
{
    int ret = 0;

    client->flags |= CLIENT_AUTHENTICATED;

    /* some win32 setups do not do TCP win scaling well, so allow an override */
    if (mountinfo && mountinfo->so_sndbuf > 0)
        sock_set_send_buffer (client->connection.sock, mountinfo->so_sndbuf);

    /* check whether we are processing a streamlist request for slaves */
    if (strcmp (mount, "/admin/streams") == 0)
    {
        client->flags |= CLIENT_IS_SLAVE;
        if (client->parser->req_type == httpp_req_stats)
        {
            stats_add_listener (client, STATS_SLAVE|STATS_GENERAL);
            return 0;
        }
        mount = httpp_get_query_param (client->parser, "mount");
        if (mount == NULL)
        {
            command_list_mounts (client, TEXT);
            return 0;
        }
        mountinfo = config_find_mount (config_get_config_unlocked(), mount);
    }

    /* Here we are parsing the URI request to see if the extension is .xsl, if
     * so, then process this request as an XSLT request
     */
    if (util_check_valid_extension (mount) == XSLT_CONTENT)
    {
        /* If the file exists, then transform it, otherwise, write a 404 */
        DEBUG0("Stats request, sending XSL transformed stats");
        return stats_transform_xslt (client, mount);
    }

    ret = source_add_listener (mount, mountinfo, client);

    if (ret == -2)
    {
        if (mountinfo && mountinfo->file_seekable == 0)
        {
            DEBUG1 ("disable seek on file matching %s", mountinfo->mountname);
            httpp_deletevar (client->parser, "range");
            client->flags |= CLIENT_NO_CONTENT_LENGTH;
        }
        ret = fserve_client_create (client, mount);
    }
    return ret;
}


static int auth_postprocess_listener (auth_client *auth_user)
{
    int ret;
    client_t *client = auth_user->client;
    auth_t *auth = auth_user->auth;
    ice_config_t *config;
    mount_proxy *mountinfo;
    const char *mount = auth_user->mount;

    if (client == NULL)
        return -1;

    if ((client->flags & CLIENT_AUTHENTICATED) == 0)
    {
        /* auth failed so do we place the listener elsewhere */
        auth_user->client = NULL;
        if (auth->rejected_mount)
            mount = auth->rejected_mount;
        else
        {
            client_send_401 (client, auth_user->auth->realm);
            return -1;
        }
    }
    config = config_get_config();
    mountinfo = config_find_mount (config, mount);
    ret = add_authenticated_listener (mount, mountinfo, client);
    config_release_config();
    auth_user->client = NULL;

    return ret;
}


/* Decide whether we need to start a source or just process a source
 * admin request.
 */
void auth_postprocess_source (auth_client *auth_user)
{
    client_t *client = auth_user->client;
    const char *mount = auth_user->mount;
    const char *req = httpp_getvar (client->parser, HTTPP_VAR_URI);

    auth_user->client = NULL;
    if (strcmp (req, "/admin.cgi") == 0 || strncmp ("/admin/metadata", req, 15) == 0)
    {
        DEBUG2 ("metadata request (%s, %s)", req, mount);
        admin_mount_request (client, "metadata");
    }
    else
    {
        DEBUG1 ("on mountpoint %s", mount);
        source_startup (client, mount);
    }
}


/* Add a listener. Check for any mount information that states any
 * authentication to be used.
 */
int auth_add_listener (const char *mount, client_t *client)
{
    int ret = 0;
    ice_config_t *config = config_get_config();
    mount_proxy *mountinfo = config_find_mount (config, mount);

    if ((client->flags & CLIENT_AUTHENTICATED) == 0)
    {
        if (mountinfo)
        {
            if (mountinfo->skip_accesslog)
                client->flags |= CLIENT_SKIP_ACCESSLOG;
            if (mountinfo->ban_client)
            {
                if (mountinfo->ban_client < 0)
                    client->flags |= CLIENT_IP_BAN_LIFT;
                connection_add_banned_ip (client->connection.ip, mountinfo->ban_client);
            }
            if (mountinfo->no_mount)
            {
                config_release_config ();
                return client_send_403 (client, "mountpoint unavailable");
            }
            if (mountinfo->redirect)
            {
                int len = strlen (mountinfo->redirect) + strlen (mount) + 3;
                char *addr = alloca (len);
                snprintf (addr, len, "%s%s", mountinfo->redirect, mount);
                config_release_config ();
                return client_send_302 (client, addr);
            }
            if (mountinfo->auth && mountinfo->auth->authenticate)
            {
                auth_client *auth_user;

                if (mountinfo->auth->running == 0 || mountinfo->auth->pending_count > 300)
                {
                    config_release_config ();
                    WARN0 ("too many clients awaiting authentication");
                    if (global.new_connections_slowdown < 10)
                        global.new_connections_slowdown++;
                    return client_send_403 (client, "busy, please try again later");
                }
                auth_user = auth_client_setup (mount, client);
                auth_user->process = auth_new_listener;
                client->flags &= ~CLIENT_ACTIVE;
                DEBUG0 ("adding client for authentication");
                queue_auth_client (auth_user, mountinfo);
                config_release_config ();
                return 0;
            }
        }
        else
        {
            if (strcmp (mount, "/admin/streams") == 0)
            {
                config_release_config ();
                return client_send_401 (client, NULL);
            }
        }
    }
    ret = add_authenticated_listener (mount, mountinfo, client);
    config_release_config ();
    return ret;
}


/* General listener client shutdown function. Here we free up the passed client but
 * if the client is authenticated and there's auth available then queue it.
 */
int auth_release_listener (client_t *client, const char *mount, mount_proxy *mountinfo)
{
    if (client->flags & CLIENT_AUTHENTICATED)
    {
        client_set_queue (client, NULL);

        if (mount && mountinfo && mountinfo->auth && mountinfo->auth->release_listener)
        {
            auth_client *auth_user = auth_client_setup (mount, client);
            client->flags &= ~CLIENT_ACTIVE;
            if (client->worker)
                client->ops = &auth_release_ops; // put into a wait state
            auth_user->process = auth_remove_listener;
            queue_auth_client (auth_user, mountinfo);
            return 0;
        }
        client->flags &= ~CLIENT_AUTHENTICATED;
    }
    return client_send_404 (client, NULL);
}


static int get_authenticator (auth_t *auth, config_options_t *options)
{
    if (auth->type == NULL)
    {
        WARN0 ("no authentication type defined");
        return -1;
    }
    do
    {
        DEBUG1 ("type is %s", auth->type);

        if (strcmp (auth->type, "url") == 0)
        {
#ifdef HAVE_AUTH_URL
            if (auth_get_url_auth (auth, options) < 0)
                return -1;
            break;
#else
            ERROR0 ("Auth URL disabled");
            return -1;
#endif
        }
        if (strcmp (auth->type, "command") == 0)
        {
#ifdef WIN32
            ERROR1("Authenticator type: \"%s\" not supported on win32 platform", auth->type);
            return -1;
#else
            if (auth_get_cmd_auth (auth, options) < 0)
                return -1;
            break;
#endif
        }
        if (strcmp (auth->type, "htpasswd") == 0)
        {
            if (auth_get_htpasswd_auth (auth, options) < 0)
                return -1;
            break;
        }
		if (strcmp (auth->type, "radio") == 0)
		{
			if (auth_get_radio_auth (auth, options) < 0)
				return -1;
			break;
		}

        ERROR1("Unrecognised authenticator type: \"%s\"", auth->type);
        return -1;
    } while (0);

    while (options)
    {
        if (strcmp (options->name, "allow_duplicate_users") == 0)
            auth->allow_duplicate_users = atoi (options->value);
        else if (strcmp(options->name, "realm") == 0)
            auth->realm = (char*)xmlStrdup (XMLSTR(options->value));
        else if (strcmp(options->name, "drop_existing_listener") == 0)
            auth->drop_existing_listener = atoi (options->value);
        else if (strcmp (options->name, "rejected_mount") == 0)
            auth->rejected_mount = (char*)xmlStrdup (XMLSTR(options->value));
        else if (strcmp(options->name, "handlers") == 0)
            auth->handlers = atoi (options->value);
        options = options->next;
    }
    if (auth->handlers < 1) auth->handlers = 3;
    if (auth->handlers > 100) auth->handlers = 100;
    return 0;
}


int auth_get_authenticator (xmlNodePtr node, void *x)
{
    auth_t *auth = calloc (1, sizeof (auth_t));
    config_options_t *options = NULL, **next_option = &options;
    xmlNodePtr option;
    int i;

    if (auth == NULL)
        return -1;

    option = node->xmlChildrenNode;
    while (option)
    {
        xmlNodePtr current = option;
        option = option->next;
        if (xmlStrcmp (current->name, XMLSTR("option")) == 0)
        {
            config_options_t *opt = calloc (1, sizeof (config_options_t));
            opt->name = (char *)xmlGetProp (current, XMLSTR("name"));
            if (opt->name == NULL)
            {
                free(opt);
                continue;
            }
            opt->value = (char *)xmlGetProp (current, XMLSTR("value"));
            if (opt->value == NULL)
            {
                xmlFree (opt->name);
                free (opt);
                continue;
            }
            *next_option = opt;
            next_option = &opt->next;
        }
        else
            if (xmlStrcmp (current->name, XMLSTR("text")) != 0)
                WARN1 ("unknown auth setting (%s)", current->name);
    }
    auth->type = (char *)xmlGetProp (node, XMLSTR("type"));
    if (get_authenticator (auth, options) < 0)
    {
        xmlFree (auth->type);
        free (auth);
        auth = NULL;
    }
    else
    {
        auth->tailp = &auth->head;
        thread_mutex_create (&auth->lock);

        /* allocate N threads */
        auth->handles = calloc (auth->handlers, sizeof (auth_thread_t));
        auth->refcount = 1;
        auth->running = 1;
        for (i=0; i<auth->handlers; i++)
        {
            if (auth->alloc_thread_data)
                auth->handles[i].data = auth->alloc_thread_data (auth);
            auth->handles[i].id = thread_id++;
            auth->handles[i].auth = auth;
        }
        *(auth_t**)x = auth;
    }

    while (options)
    {
        config_options_t *opt = options;
        options = opt->next;
        xmlFree (opt->name);
        xmlFree (opt->value);
        free (opt);
    }
    return 0;
}


/* Called when a source client connects and requires authentication via the
 * authenticator. This is called for both source clients and admin requests
 * that work on a specified mountpoint.
 */
int auth_stream_authenticate (client_t *client, const char *mount, mount_proxy *mountinfo)
{
    if (mountinfo && mountinfo->auth && mountinfo->auth->stream_auth)
    {
        auth_client *auth_user = auth_client_setup (mount, client);

        auth_user->process = stream_auth_callback;
        INFO1 ("request source auth for \"%s\"", mount);
        client->flags &= ~CLIENT_ACTIVE;
        queue_auth_client (auth_user, mountinfo);
        return 1;
    }
    return 0;
}


/* called when the stream starts, so that authentication engine can do any
 * cleanup/initialisation.
 */
void auth_stream_start (mount_proxy *mountinfo, const char *mount)
{
    if (mountinfo && mountinfo->auth && mountinfo->auth->stream_start)
    {
        auth_client *auth_user = auth_client_setup (mount, NULL);
        auth_user->process = stream_start_callback;
        INFO1 ("request source start for \"%s\"", mount);

        queue_auth_client (auth_user, mountinfo);
    }
}


/* Called when the stream ends so that the authentication engine can do
 * any authentication cleanup
 */
void auth_stream_end (mount_proxy *mountinfo, const char *mount)
{
    if (mountinfo && mountinfo->auth && mountinfo->auth->stream_end)
    {
        auth_client *auth_user = auth_client_setup (mount, NULL);
        auth_user->process = stream_end_callback;
        INFO1 ("request source end for \"%s\"", mount);

        queue_auth_client (auth_user, mountinfo);
    }
}


/* return -1 for failed, 0 for authenticated, 1 for pending
 */
int auth_check_source (client_t *client, const char *mount)
{
    ice_config_t *config = config_get_config();
    char *pass = config->source_password;
    char *user = "source";
    int ret = -1;
    mount_proxy *mountinfo = config_find_mount (config, mount);

    do
    {
        if (mountinfo)
        {
            ret = 1;
            if (auth_stream_authenticate (client, mount, mountinfo) > 0)
                break;
            ret = -1;
            if (mountinfo->password)
                pass = mountinfo->password;
            if (mountinfo->username && client->server_conn->shoutcast_compat == 0)
                user = mountinfo->username;
        }
        if (connection_check_pass (client->parser, user, pass) > 0)
            ret = 0;
    } while (0);
    config_release_config();
    return ret;
}


/* these are called at server start and termination */

void auth_initialise (void)
{
    thread_rwlock_create (&auth_lock);
    thread_id = 0;
    allow_auth = 1;
}

void auth_shutdown (void)
{
    if (allow_auth == 0)
        return;
    allow_auth = 0;
    thread_rwlock_wlock (&auth_lock);
    thread_rwlock_unlock (&auth_lock);
    thread_rwlock_destroy (&auth_lock);
    INFO0 ("Auth shutdown complete");
}

