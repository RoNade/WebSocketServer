#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libwebsockets.h>

// created for each message

struct msg {
    void* payload;
    size_t len;
};

// created for each client connecting to us

struct per_session_data {
    struct per_session_data* pss_list; // next link
    struct lws* wsi;
    uint32_t tail;
    char publishing;
};

// created for each vhost out protocol is used with

struct per_vhost_data {
    struct lws_context* context;
    struct lws_vhost* vhost;
    const struct lws_protocols *protocols;
    struct per_session_data* pss_list; // head link
    struct lws_ring* ring;
};

// destory a message when all sucribers received a copy

static void destroy_message(void* _msg)
{
    struct msg* msg = _msg;
    free(msg->payload);
    msg->payload = NULL;
    msg->len = 0;
}

// function called by the libwebsocket service every time a request occurs

static int callback_broker
(
    struct lws* wsi,
    enum lws_callback_reasons reason,
    void* user,
    void* in ,
    size_t len
)
{
    struct per_session_data* pss = user;
    struct lws_vhost* vhost = lws_get_vhost(wsi);
    const struct lws_protocols* protocols = lws_get_protocol(wsi);
    struct per_vhost_data* vhd = lws_protocol_vh_priv_get(vhost, protocols);

    const struct msg* pmsg;
    struct msg amsg;
    char buf[32];
    int n, m;

    switch (reason)
    {
        case LWS_CALLBACK_PROTOCOL_INIT:
        {
            vhd = lws_protocol_vh_priv_zalloc(vhost, protocols, sizeof(struct per_vhost_data));

            vhd->context = lws_get_context(wsi);
            vhd->protocols = protocols;
            vhd->vhost = vhost;

            // create a circular buffer for message queuing

            vhd->ring = lws_ring_create(sizeof(struct msg), 8, destroy_message);

            if (!vhd->ring)
            {
                return 1;
            }

            break;
        }
        case LWS_CALLBACK_PROTOCOL_DESTROY:
        {
            lws_ring_destroy(vhd->ring);

            break;
        }
        case  LWS_CALLBACK_ESTABLISHED:
        {
            pss->tail = lws_ring_get_oldest_tail(vhd->ring);
            pss->wsi = wsi;

            // find out if client is a publisher or a subscriber

            if (lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI) > 0)
            {
                pss->publishing = !strcmp(buf, "/publisher");
            }

            // client is not a publisher

            if (!pss->publishing)
            {
                // add subscribers to the list of live pss

                lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
            }

            break;
        }
        case LWS_CALLBACK_CLOSED:
        {
            // remove the pss from the list of live pss when a session closes

            lws_ll_fwd_remove(struct per_session_data, pss_list, pss, vhd->pss_list);

            break;
        }
        case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            // sending pending message to subscribers if any

            if (pss->publishing)
            {
                break;
            }

            // retrieve pending message

            pmsg = lws_ring_get_element(vhd->ring, &pss->tail);

            if (!pmsg)
            {
                break;
            }

            // LWS_PRE bytes are already allocated in the payload for protocol info

            m = lws_write(wsi, pmsg->payload + LWS_PRE, pmsg->len, LWS_WRITE_TEXT);

            if (m < pmsg->len)
            {
                lwsl_err("ERROR %d writing to ws socket\n", m);
                return -1;
            }

            // consume message once sent and move ring tail

            lws_ring_consume_and_update_oldest_tail
            (
                vhd->ring, // lws_ring object
                struct per_session_data, // type of objects keeping track of tail
                &pss->tail, // tail of currently active session
                1, // number of payload being consumed
                vhd->pss_list, // head of list of tail tracking objects
                tail, // member name of tail in tracking objects
                pss_list // member name of next object in tracking objects
            );

            // check if there is any message still pending

            if (lws_ring_get_element(vhd->ring, &pss->tail))
            {
                // come back when the connection socket is able
                // to accept another write packet without blocking

                // if it already was able to take another packet without blocking
                // you'll get this callback at the next call to the service loop

                lws_callback_on_writable(pss->wsi);
            }

            break;
        }
        case LWS_CALLBACK_RECEIVE:
        {
            // adding messages from publishers to lws_ring object

            if (!pss->publishing)
            {
                break;
            }

            // we ignore publishing when no subscribers are connected

            if (!vhd->pss_list)
            {
                break;
            }

            // check how much free space is left in the ring buffer

            n = lws_ring_get_count_free_elements(vhd->ring);

            if (!n)
            {
                lwsl_user("Dropping message, no more free space!!\n");
                break;
            }

            amsg.len = len;

            // we over-allocate with LWS_PRE for protocol info insertion

            amsg.payload = malloc(LWS_PRE + len);

            if (!amsg.payload)
            {
                lwsl_user("Dropping message, out of memory!!\n");
                break;
            }

            memcpy(amsg.payload + LWS_PRE, in, len);

            // add message to lws_ring object

            if (!lws_ring_insert(vhd->ring, &amsg, 1))
            {
                destroy_message(&amsg);
                lwsl_user("Dropping message, failed insertion!!");
                break;
            }

            // let every subscriber know we want to write
            // something to them as soon as they are ready

            lws_start_foreach_llp(struct per_session_data **, ppss, vhd->pss_list)
            {
                if (!(*ppss)->publishing)
                {
                    lws_callback_on_writable((*ppss)->wsi);
                }
            }
            lws_end_foreach_llp(ppss, pss_list);

            break;
        }
        default:
        {
            printf("Reason: %d\n", (int)reason);
            printf("unhandled callback\n");
            break;
        }
    }

    return 0;
}

 // this sets a per-vhost, per-protocol option name:value pair
 // the effect is to set this protocol to be the default one for the vhost
 
static const struct lws_protocol_vhost_options pvo_opt = {
        NULL,
        NULL,
        "default",
        "1"
};
 
static const struct lws_protocol_vhost_options pvo = {
        NULL,
        &pvo_opt,
        "lws-broker",
        ""
};

// list of supported protocols and callbacks

static struct lws_protocols protocols[] = {
    {
        "lws-broker",
        callback_broker,
        sizeof(struct per_session_data),
        128,
        0,
        NULL,
        0
    },
    {
        // marks the end of the protocol list

        NULL,
        NULL,
        0,
        0
    }
};

int main(void)
{
    // server url will be http://localhost:9000

    struct lws_context_creation_info info = {
        .port = 9000,
        .protocols = protocols,
        .pvo = &pvo,
        .gid = -1,
        .uid = -1
    };

    // create a libwebsocket context representing this server

    struct lws_context* context = lws_create_context(&info);

    if (context == NULL)
    {
        lwsl_err("libwebsocket init failed\n");
        return 1;
    }

    printf("starting server...\n");

    // infinite loop, to end this server send SIGTERM (Ctrl + C)

    while (1)
    {
        // libwebsocket_service will process all waiting events with
        // their callback functions and then wait for 10 miliseconds
        
        // this is a single threaded web server and this will keep our
        // server from generating load while there are no requests to process

        lws_service(context, 10);
    }

    printf("stopping server...");

    lws_context_destroy(context);

    return 0;
}