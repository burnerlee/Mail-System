#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#define ISVALIDSOCKET(s) ((s) >= 0)
#define CLOSESOCKET(s) close(s)
#define SOCKET int
#define GETSOCKETERRNO() (errno)
#define MAXINPUT 512
#define MAXRESPONSE 1024
#define MAX_REQUEST_SIZE 2047

//Asks user for an input with a prompt.
void get_input(const char *prompt, char *buffer)
{
    printf("%s", prompt);
    buffer[0] = 0;
    fgets(buffer, MAXINPUT, stdin);
    const int read = strlen(buffer);
    if (read > 0)
    {
        buffer[read - 1] = 0;
    }
}

SOCKET create_listening_socket(const char *port)
{

    //Configuring the local server address using the port number provided
    //and storing it in server_address.
    printf("Configuring Server address...\n");
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *server_address;
    if (getaddrinfo(0, port, &hints, &server_address))
    {
        fprintf(stderr, "getaddrinfo failed with error %d\n", GETSOCKETERRNO());
        exit(1);
    }

    //Printing the server address for debugging purposes.
    printf("Server address is: ");
    char address_buffer[1024];
    char service_buffer[1024];
    getnameinfo(server_address->ai_addr, server_address->ai_addrlen, service_buffer,
                sizeof(service_buffer), address_buffer, sizeof(address_buffer),
                NI_NUMERICHOST | NI_NUMERICSERV);
    printf("%s %s\n", service_buffer, address_buffer);

    //Creating the socket.
    SOCKET server_socket = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
    if (!ISVALIDSOCKET(server_socket))
    {
        fprintf(stderr, "socket() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    //Binding the socket to the server address
    if (bind(server_socket, server_address->ai_addr, server_address->ai_addrlen))
    {
        fprintf(stderr, "bind() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    //Freeing the server_address structure.
    freeaddrinfo(server_address);

    //Start listening on the socket.
    printf("Listening on server...\n");
    if (listen(server_socket, 10) < 0)
    {
        fprintf(stderr, "listen() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    return server_socket;
}

//Defining a structure to hold data of clients who connect to the server.
struct client_info
{
    socklen_t address_length;
    struct sockaddr_storage address;
    SOCKET socket;
    char request[MAX_REQUEST_SIZE];
    int received;
    struct client_info *next;
    char email[MAXINPUT];
};

//Initialising the clients pointer to struc client_info.
static struct client_info *clients = 0;

//Returns client_info by socket, creates a new client if it doesn't exist.
struct client_info *get_client(SOCKET s)
{
    struct client_info *ci = clients;

    while (ci)
    {
        if (ci->socket == s)
            break;
        ci = ci->next;
    }

    if (ci)
        return ci;

    struct client_info *n = (struct client_info *)calloc(1, sizeof(struct client_info));

    if (!n)
    {
        fprintf(stderr, "Out of memory.\n");
        exit(1);
    }

    n->address_length = sizeof(n->address);
    n->next = clients;
    clients = n;
    return n;
}

//Disconnects the client from the server.
void drop_client(struct client_info *client)
{
    CLOSESOCKET(client->socket);
    struct client_info **p = &clients;

    while (*p)
    {
        if (*p == client)
        {
            *p = client->next;
            free(client);
            return;
        }
        p = &(*p)->next;
    }

    fprintf(stderr, "drop_client not found.\n");
    exit(1);
}

//Returns the client address.
const char *get_client_address(struct client_info *ci)
{
    static char address_buffer[100];
    getnameinfo((struct sockaddr *)&ci->address,
                ci->address_length,
                address_buffer, sizeof(address_buffer), 0, 0,
                NI_NUMERICHOST | NI_NUMERICSERV);
    return address_buffer;
}

//Waits for either a new connection on server or request from a client.
fd_set wait_on_clients(SOCKET server)
{
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(server, &reads);
    SOCKET max_socket = server;

    struct client_info *ci = clients;

    while (ci)
    {
        FD_SET(ci->socket, &reads);
        if (ci->socket > max_socket)
            max_socket = ci->socket;
        ci = ci->next;
    }

    if (select(max_socket + 1, &reads, 0, 0, 0) < 0)
    {
        fprintf(stderr, "select() failed. (%d)\n", GETSOCKETERRNO());
        exit(1);
    }

    return reads;
}

int main()
{
    char port[10];
    get_input("Enter the port number to host the SMTP server: ", port);
    SOCKET server_socket = create_listening_socket(port);
    while (1)
    {
        fd_set reads;
        reads = wait_on_clients(server_socket);
        if (FD_ISSET(server_socket, &reads))
        {
            struct client_info *client = get_client(-1);
            client->socket = accept(server_socket, (struct sockaddr *)&(client->address),
                                    &(client->address_length));

            if (!ISVALIDSOCKET(client->socket))
            {
                fprintf(stderr, "accept() failed. (%d)\n", GETSOCKETERRNO());
                return 1;
            }
            printf("New connection from %s.\n", get_client_address(client));
            char initial[3] = "220";
            send(client->socket, initial, strlen(initial), 0);
        }
        struct client_info *client = clients;
        while (client)
        {
            struct client_info *next = client->next;
            if (FD_ISSET(client->socket, &reads))
            {
                int r = recv(client->socket, client->request + client->received, MAX_REQUEST_SIZE - client->received, 0);
                if (r < 1)
                {
                    printf("Unexpected disconnect from %s.\n",
                           get_client_address(client));
                    drop_client(client);
                }
                else
                {
                    client->received += r;
                    client->request[client->received] = 0;
                    char *q = strstr(client->request, "\r\n");
                    if (q)
                    {
                        char *topic_colon = strstr(client->request, ":");
                        if (topic_colon)
                        {
                            char topic[MAXRESPONSE];
                            char *pointer = client->request;
                            int i = 0;
                            while (pointer != topic_colon)
                            {
                                topic[i++] = *pointer++;
                            }
                            topic[i] = '\0';
                            printf("topic is %s\n", topic);
                            if (strcmp(topic, "email") == 0)
                            {
                                char *email = ++pointer;
                                char *end = strstr(email, "\r");
                                *end = '\0';
                                strcpy(client->email, email);
                                printf("email:%s is connected\n",email);
                            }
                        }
                    }
                }
            }
            client = next;
        }
    }
}