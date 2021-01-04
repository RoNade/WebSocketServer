#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libwebsockets.h>

// function called by the libwebsocket service every time a request occurs

static int callback_http
(
    struct lws* wsi,
    enum lws_callback_reasons reason,
    void* user,
    void* in ,
    size_t len
)
{
    switch (reason)
    {    
        case LWS_CALLBACK_ESTABLISHED:
        {
            printf("Connection established\n");
            break;
        }
        case LWS_CALLBACK_HTTP:
        {
            char* requested_uri = (char*) in;
            printf("Requested URI: %s\n", requested_uri);

            if (strcmp(requested_uri, "/") == 0)
            {
                void* universal_response = "Hello, World!";
                lws_write(wsi, universal_response, strlen(universal_response), LWS_WRITE_HTTP);

                break;
            } 
            else
            {
                // try to get current working directory

                char cwd[1024];
                char* resource_path;

                if (getcwd(cwd, sizeof(cwd)) != NULL)
                {
                    // allocate enough memory for the resource path

                    resource_path = malloc(strlen(cwd) + strlen(requested_uri));

                    // join current working directory to the resource path

                    sprintf(resource_path, "%s%s", cwd, requested_uri);
                    printf("Resource path: %s\n", resource_path);

                    char* extension = strrchr(resource_path, '.');
                    char * mime;

                    // choose the mime type based on the file extension

                    if (extension == NULL)
                    {
                        mime = "text/plain";
                    }
                    else if (strcmp(extension, ".png") == 0)
                    {
                        mime = "image/png";
                    }
                    else if (strcmp(extension, ".jpg") == 0)
                    {
                        mime = "image/jpg";
                    }
                    else if (strcmp(extension, ".gif") == 0)
                    {
                        mime = "image/gif";
                    }
                    else if (strcmp(extension, ".html") == 0)
                    {
                        mime = "text/html";
                    }
                    else if (strcmp(extension, "css") == 0)
                    {
                        mime = "text/css";
                    }
                    else
                    {
                        mime = "text/plain";
                    }
                    
                    // non-exisiting resources return 404

                    lws_serve_http_file(wsi, resource_path, mime, NULL, 0);

                    free(resource_path);
                }
            }
        }
        case LWS_CALLBACK_RECEIVE:
        {
            char msg[len];
            memcpy(msg, in, len);
            msg[len] = '\0';

            printf("Data received: %s\n", msg);
            
            char response[] = "OK\0";
            lws_write(wsi, response, strlen(response), LWS_WRITE_TEXT);
            
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

// list of supported protocols and callbacks

static struct lws_protocols protocols[] = {
    {
        // first protocol must always be HTTP handler

        "http-only",
        callback_http,
        0
    },
    {
        // marks the end of the protocol list

        NULL,
        NULL,
        0
    }
};

int main(void)
{
    // server url will be http://localhost:9000

    struct lws_context_creation_info info = {
        .port = 9000,
        .protocols = protocols,
        .gid = -1,
        .uid = -1
    };

    // create a libwebsocket context representing this server

    struct lws_context* context = lws_create_context(&info);

    if (context == NULL)
    {
        fprintf(stderr, "libwebsocket init failed\n");
        return -1;
    }

    printf("starting server...\n");

    // infinite loop, to end this server send SIGTERM (Ctrl + C)

    while (1)
    {
        // libwebsocket_service will process all waiting events with
        // their callback functions and then wait for 50 miliseconds
        
        // this is a single threaded web server and this will keep our
        // server from generating load while there are no requests to process

        lws_service(context, 50);
    }

    printf("stopping server...");

    lws_context_destroy(context);

    return 0;
}