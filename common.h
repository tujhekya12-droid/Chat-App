#ifndef COMMON_H
#define COMMON_H

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SERVER_IP        "127.0.0.1"
#define SERVER_PORT      5000
#define MAX_CLIENTS      10
#define BUFFER_SIZE      1024
#define NAME_LEN         32      /* username/password: max 31 characters */
#define PASS_LEN         32
#define MAX_MESSAGE_LEN  800
#define USERS_FILE       "users.txt"
#define HISTORY_FILE     "group_chat.txt"

/*
 * Protocol: every message is one text line ending with '\n'.
 *
 * Client -> Server:
 *   REGISTER <user> <pass>    LOGIN <user> <pass>
 *   MSG <text>   USERS   HISTORY   QUIT
 *
 * Server -> Client:
 *   Before login: "OK <text>" or "ERR <text>"
 *   After login : plain text lines to display
 */

/* Send one line (adds '\n'). Returns 0 on success, -1 on error. */
static int send_line(SOCKET sock, const char *text)
{
    char line[BUFFER_SIZE + 1];
    int length = (int)strlen(text);
    int sent = 0;

    if (length > BUFFER_SIZE - 1) {
        length = BUFFER_SIZE - 1;
    }
    memcpy(line, text, length);
    line[length++] = '\n';

    while (sent < length) {
        int result = send(sock, line + sent, length - sent, 0);
        if (result == SOCKET_ERROR) {
            return -1;
        }
        sent += result;
    }
    return 0;
}

/* Receive one line (without '\n'). Returns length, or -1 if connection closed/failed. */
static int recv_line(SOCKET sock, char *buffer, int size)
{
    int length = 0;
    char ch;

    while (1) {
        int result = recv(sock, &ch, 1, 0);
        if (result <= 0) {
            return -1;
        }
        if (ch == '\n') {
            break;
        }
        if (ch == '\r') {
            continue;
        }
        if (length < size - 1) {   /* extra characters of a too-long line are dropped */
            buffer[length++] = ch;
        }
    }
    buffer[length] = '\0';
    return length;
}

#endif
