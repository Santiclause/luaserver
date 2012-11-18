/*
 * =============================================================================
 *
 *       Filename:  server.c
 *
 *    Description:  A simple socket server, this time with select().
 *
 *        Version:  1.0
 *        Created:  12-10-03 04:40:17 PM
 *       Revision:  none
 *       Compiler:  gcc
 *
 * =============================================================================
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#define SERVER_PORT 5000

#define READ_OVERFLOW_SIZE_LIMIT 4098 /* maximum socket line length */
#define READ_BUFFER_SIZE 4098
#define BUFFER_SIZE 1024
#define SHORT_BUFFER_SIZE 64


#define STATE_LUA_FILE "lua/state.lua"
/* L_ prefix for Lua registry table keys */
#define L_CONNFD "server_connfd"
/* G_ prefix for Lua global table keys */
#define G_ONREAD_HANDLER "OnRead"

typedef struct node_t {
    struct node_t *next;
    int value;
    lua_State *state;
} ll_conn_t;


/*-----------------------------------------------------------------------------
 *  Appends a connection tuple to a linked list. Returns 0 on success, or a -1
 *  on error.
 *  If the connfd value is already present in the linked list, it will error.
 *---------------------------------------------------------------------------*/
int ll_conn_append(ll_conn_t *list, int value, lua_State *state)
{
    ll_conn_t *temp;
    for (temp = list; temp->next != NULL; temp = temp->next)
    {
        if (temp->next->value == value)
            return -1;
    }
    temp->next = (ll_conn_t *)malloc(sizeof(ll_conn_t));
    if (temp->next == NULL)
    {
        /* TODO error handling */
    }
    temp->next->next = NULL;
    temp->next->value = value;
    temp->next->state = state;
    list->value++;
    return 0;
}	/*-----  end of function ll_conn_append  -----*/


/*-----------------------------------------------------------------------------
 *  Creates a new linked list and returns a pointer to the new linked list.
 *---------------------------------------------------------------------------*/
ll_conn_t *ll_conn_create()
{
    ll_conn_t *list = (ll_conn_t *)malloc(sizeof(ll_conn_t));
    if (list == NULL)
    {
        /* TODO error handling */
    }
    list->next = NULL;
    list->value = 0;
    list->state = NULL;
    return list;
}	/*-----  end of function ll_conn_create  -----*/


/*-----------------------------------------------------------------------------
 *  Iterates over a linked list and completely frees up its memory.
 *---------------------------------------------------------------------------*/
void ll_conn_delete(ll_conn_t *list)
{
    ll_conn_t *temp;
    for (temp = list->next; temp != NULL; list = temp, temp = temp->next)
    {
        lua_close(list->state);
        free(list);
    }
}	/*-----  end of function ll_conn_delete  -----*/


/*-----------------------------------------------------------------------------
 *  Searches for and removes the first occurrence of a specific connfd from a
 *  linked list. Returns a boolean which indicates whether the value was found.
 *---------------------------------------------------------------------------*/
int ll_conn_remove(ll_conn_t *list, int value)
{
    ll_conn_t *iter, *temp;
    for (iter = list; iter->next != NULL; iter = iter->next)
    {
        temp = iter->next;
        if (temp->value == value)
        {
            iter->next = temp->next;
            lua_close(temp->state);
            free(temp);
            list->value--;
            return 1;
        }
    }
    return 0;
}	/*-----  end of function ll_conn_remove  -----*/


/*-----------------------------------------------------------------------------
 * lua_CFunction
 * Takes a string argument, and writes that string to this Lua state's socket
 * connection.
 *---------------------------------------------------------------------------*/
int luasend(lua_State *L)
{
    int connfd, success, length;
    lua_pushstring(L, L_CONNFD);
    lua_rawget(L, LUA_REGISTRYINDEX);
    connfd = (int)lua_tointegerx(L, -1, &success);
    lua_pop(L, 1);
    if (!success)
    {
        /* error! */
        lua_pushstring(L, "Socket connfd could not be converted to integer!");
        lua_error(L);
    }
    length = luaL_len(L, -1);
    write(connfd, lua_tostring(L, -1), length);
    lua_pop(L, 1);
    return 0;
}	/*-----  end of function luasend  -----*/


/*-----------------------------------------------------------------------------
 *  Takes as input a connected socket file descriptor and builds a new Lua
 *  state for that socket.
 *---------------------------------------------------------------------------*/
lua_State *l_create_state(int connfd)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushstring(L, L_CONNFD);
    lua_pushinteger(L, connfd);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_register(L, "send", *luasend);
    luaL_dofile(L, STATE_LUA_FILE);
    return L;
}	/*-----  end of function l_create_state  -----*/


/*-----------------------------------------------------------------------------
 *  Reads from the Lua state's connection, and calls the Lua handler for each
 *  line it encounters in the read buffer.
 *---------------------------------------------------------------------------*/
void l_read_conn(lua_State *L, int connfd)
{
    static char buffer[READ_BUFFER_SIZE];
    int count = 1, i, start, overflow_count = 0, overflow_size = 0, len;
    while (1)
    {
        ioctl(connfd, FIONREAD, &len);
        if (len == 0)
            break;
        count = read(connfd, buffer, READ_BUFFER_SIZE);
        if (count == -1)
        {
            /* TODO error handling */
            break;
        }
        if (count == 0)
        {
            /* EOF */
            break;
        }
        start = 0;
        for (i = 0; i < count; i++)
        {
            if (buffer[i] != '\n')
                continue;
            if (!overflow_count)
                lua_getglobal(L, G_ONREAD_HANDLER);
            /* We won't be passing newlines to the handler. Otherwise, it would
             * be 1 + i - start. */
            lua_pushlstring(L, buffer + start, i - start);
            lua_call(L, 1, 0);
            overflow_count = overflow_size = 0;
            start = i + 1;
        }
        /* If there's bits in the buffer that aren't terminated with a newline,
         * add it as overflow to the stack.
         * NOTE: THIS IS A REALLY BAD IDEA !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!*/
        if (start < count)
        {
            if (!overflow_count)
                lua_getglobal(L, G_ONREAD_HANDLER);
            lua_pushlstring(L, buffer + start, count - start);
            overflow_size += count - start;
            overflow_count++;
            /* let's include this check to make this a marginally less bad idea
             * in order to soothe my OCD worries */
            if (overflow_size >= READ_OVERFLOW_SIZE_LIMIT)
                break;
        }
    }
    if (overflow_count)
        lua_call(L, overflow_count, 0);
}	/*-----  end of function l_read_conn  -----*/


int main(int argc, char *argv[])
{
    int listenfd = 0, connfd = 0, maxfd = 0;
    struct sockaddr_in serv_addr, conn_addr;
    char sendBuff[BUFFER_SIZE];
    lua_State *L;
    fd_set fds;
    ll_conn_t *connections = ll_conn_create();
    /* iterators */
    ll_conn_t *temp;
    int n;
    unsigned int i, j;

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        /* TODO error handling */
    }
    memset(&serv_addr, '0', sizeof(serv_addr));
    memset(sendBuff, '0', BUFFER_SIZE);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(SERVER_PORT);

    if (bind(listenfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == 0)
    {
        /* TODO error handling */
    }

    if (listen(listenfd, 10) == -1)
    {
        /* TODO error handling */
    }

    while (1)
    {
        FD_ZERO(&fds);
        FD_SET(listenfd, &fds);
        if (listenfd >= maxfd)
            maxfd = listenfd + 1;
        for (temp = connections->next; temp != NULL; temp = temp->next)
            FD_SET(temp->value, &fds);
        if ((n = select(maxfd, &fds, NULL, NULL, NULL)) > 0)
        {
            if (FD_ISSET(listenfd, &fds))
            {
                i = sizeof(conn_addr);
                connfd = accept(listenfd, (struct sockaddr *)&conn_addr, &i);
                if (connfd < 0)
                {
                    /* TODO error handling */
                }
                L = l_create_state(connfd); /* create new state */
                if (ll_conn_append(connections, connfd, L) < 0)
                {
                    /* TODO error handling */
                }
                if (connfd >= maxfd)
                    maxfd = connfd + 1;
                printf("New connection from %s.\n",
                        inet_ntoa(conn_addr.sin_addr));
            }
            /* we do iteration this way so that we don't segfault!!! */
            for (temp = connections->next; temp != NULL;)
            {
                connfd = temp->value;
                L = temp->state;
                temp = temp->next;
                if (FD_ISSET(connfd, &fds))
                {
                    /* check connection status */
                    ioctl(connfd, FIONREAD, &j);
                    if (j == 0)
                    {
                        /* the socket should now be removed! */
                        ll_conn_remove(connections, connfd);
                    }
                    else
                    {
                        /* read from connection */
                        l_read_conn(L, connfd);
                    }
                }
            }
        }
        if (n < 0)
        {
            /* TODO error handling */
        }
    }
    return EXIT_SUCCESS;
}       /*----------  end of function main  ----------*/
