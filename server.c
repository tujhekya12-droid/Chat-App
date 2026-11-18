#include "common.h"
#include <time.h>

typedef struct {
    SOCKET sock;
    char username[NAME_LEN];
    int in_use;      /* slot is taken by a connection */
    int logged_in;   /* user has logged in */
} Client;

static Client clients[MAX_CLIENTS];
static CRITICAL_SECTION clients_lock;   /* protects clients[] and all sends to clients */
static CRITICAL_SECTION file_lock;      /* protects users.txt and group_chat.txt */
/* Lock order when both are needed: clients_lock first, then file_lock. */

/* ---------- Helpers ---------- */

static void get_time_string(char *buffer, int size)
{
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    strftime(buffer, size, "%H:%M:%S", local);
}

/* Send a line to every logged-in client */
static void broadcast(const char *text)
{
    int i;
    printf("%s\n", text);
    EnterCriticalSection(&clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in) {
            send_line(clients[i].sock, text);
        }
    }
    LeaveCriticalSection(&clients_lock);
}

/* ---------- File handling ---------- */

/* Caller must hold file_lock. Returns 1 if found and copies the password. */
static int find_user(const char *name, char *password_out)
{
    FILE *file = fopen(USERS_FILE, "r");
    char stored_name[NAME_LEN], stored_pass[PASS_LEN];

    if (file == NULL) {
        return 0;
    }
    while (fscanf(file, "%31s %31s", stored_name, stored_pass) == 2) {
        if (strcmp(stored_name, name) == 0) {
            if (password_out != NULL) {
                strcpy(password_out, stored_pass);
            }
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

/* Returns 1 = registered, 0 = username taken, -1 = file error */
static int register_user(const char *name, const char *password)
{
    int result = 1;
    FILE *file;

    EnterCriticalSection(&file_lock);
    if (find_user(name, NULL)) {
        result = 0;
    } else {
        file = fopen(USERS_FILE, "a");
        if (file == NULL) {
            result = -1;
        } else {
            fprintf(file, "%s %s\n", name, password);
            fclose(file);
        }
    }
    LeaveCriticalSection(&file_lock);
    return result;
}

static int check_login(const char *name, const char *password)
{
    char stored_pass[PASS_LEN];
    int found;

    EnterCriticalSection(&file_lock);
    found = find_user(name, stored_pass);
    LeaveCriticalSection(&file_lock);

    return found && strcmp(stored_pass, password) == 0;
}

static void save_history(const char *text)
{
    FILE *file;

    EnterCriticalSection(&file_lock);
    file = fopen(HISTORY_FILE, "a");
    if (file != NULL) {
        fprintf(file, "%s\n", text);
        fclose(file);
    } else {
        printf("Warning: could not write %s\n", HISTORY_FILE);
    }
    LeaveCriticalSection(&file_lock);
}

/* ---------- Sending info to one client ---------- */

static void send_history(int index)
{
    FILE *file;
    char line[BUFFER_SIZE];

    EnterCriticalSection(&clients_lock);
    EnterCriticalSection(&file_lock);

    send_line(clients[index].sock, "--- Chat history ---");
    file = fopen(HISTORY_FILE, "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            send_line(clients[index].sock, line);
        }
        fclose(file);
    }
    send_line(clients[index].sock, "--- End of history ---");

    LeaveCriticalSection(&file_lock);
    LeaveCriticalSection(&clients_lock);
}

static void send_user_list(int index)
{
    char list[BUFFER_SIZE] = "";
    char message[BUFFER_SIZE];
    int count = 0;
    int i;

    EnterCriticalSection(&clients_lock);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].in_use && clients[i].logged_in) {
            if (count > 0) {
                strcat(list, ", ");
            }
            strcat(list, clients[i].username);
            count++;
        }
    }
    snprintf(message, sizeof(message), "[Server] Online users (%d): %s", count, list);
    send_line(clients[index].sock, message);
    LeaveCriticalSection(&clients_lock);
}

/* ---------- Per-client logic ---------- */

/* Handles REGISTER / LOGIN requests. Returns 1 when logged in, 0 if client left. */
static int authenticate_client(int index)
{
    SOCKET sock = clients[index].sock;
    char line[BUFFER_SIZE];
    char command[16], user[64], pass[64], extra[64];

    while (1) {
        int fields, i, already_online;

        if (recv_line(sock, line, sizeof(line)) < 0) {
            return 0;
        }

        command[0] = '\0';
        fields = sscanf(line, "%15s %63s %63s %63s", command, user, pass, extra);

        if (strcmp(command, "QUIT") == 0) {
            return 0;
        }
        if (fields != 3 || strlen(user) >= NAME_LEN || strlen(pass) >= PASS_LEN) {
            send_line(sock, "ERR Username and password must be 1-31 characters with no spaces");
            continue;
        }

        if (strcmp(command, "REGISTER") == 0) {
            int result = register_user(user, pass);
            if (result == 1) {
                send_line(sock, "OK Registration successful. You can now login.");
            } else if (result == 0) {
                send_line(sock, "ERR Username already exists");
            } else {
                send_line(sock, "ERR Server could not save the user");
            }
        } else if (strcmp(command, "LOGIN") == 0) {
            if (!check_login(user, pass)) {
                send_line(sock, "ERR Invalid username or password");
                continue;
            }

            /* Check and mark as logged in in one locked step */
            already_online = 0;
            EnterCriticalSection(&clients_lock);
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].in_use && clients[i].logged_in &&
                    strcmp(clients[i].username, user) == 0) {
                    already_online = 1;
                }
            }
            if (!already_online) {
                strcpy(clients[index].username, user);
                send_line(sock, "OK Login successful");   /* sent before logged_in so it arrives first */
                clients[index].logged_in = 1;
            }
            LeaveCriticalSection(&clients_lock);

            if (already_online) {
                send_line(sock, "ERR This user is already logged in");
            } else {
                return 1;
            }
        } else {
            send_line(sock, "ERR Unknown command");
        }
    }
}

static DWORD WINAPI client_thread(LPVOID arg)
{
    int index = (int)(INT_PTR)arg;
    SOCKET sock = clients[index].sock;
    char name[NAME_LEN] = "";
    char line[BUFFER_SIZE];
    char message[BUFFER_SIZE];
    char time_text[16];
    int was_logged_in;

    if (authenticate_client(index)) {
        strcpy(name, clients[index].username);

        send_history(index);
        snprintf(message, sizeof(message), "[Server] %s joined the chat", name);
        broadcast(message);

        while (recv_line(sock, line, sizeof(line)) >= 0) {
            if (strcmp(line, "QUIT") == 0) {
                break;
            } else if (strcmp(line, "USERS") == 0) {
                send_user_list(index);
            } else if (strcmp(line, "HISTORY") == 0) {
                send_history(index);
            } else if (strncmp(line, "MSG ", 4) == 0 && line[4] != '\0') {
                get_time_string(time_text, sizeof(time_text));
                snprintf(message, sizeof(message), "[%s] %s: %s", time_text, name, line + 4);
                save_history(message);
                broadcast(message);
            }
        }
    }

    /* Disconnect: free the slot first, then notify the others */
    EnterCriticalSection(&clients_lock);
    was_logged_in = clients[index].logged_in;
    clients[index].logged_in = 0;
    closesocket(clients[index].sock);
    clients[index].sock = INVALID_SOCKET;
    clients[index].in_use = 0;
    LeaveCriticalSection(&clients_lock);

    if (was_logged_in) {
        snprintf(message, sizeof(message), "[Server] %s left the chat", name);
        broadcast(message);
    }
    return 0;
}

/* ---------- Main ---------- */

int main(void)
{
    WSADATA wsa_data;
    SOCKET listen_sock;
    struct sockaddr_in server_addr;
    FILE *file;
    int i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    InitializeCriticalSection(&clients_lock);
    InitializeCriticalSection(&file_lock);

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = INVALID_SOCKET;
        clients[i].in_use = 0;
        clients[i].logged_in = 0;
        clients[i].username[0] = '\0';
    }

    /* Create data files if they do not exist */
    file = fopen(USERS_FILE, "a");
    if (file != NULL) fclose(file);
    file = fopen(HISTORY_FILE, "a");
    if (file != NULL) fclose(file);

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        printf("socket() failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);   /* localhost only */
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("bind() failed. Error: %d (is the port already in use?)\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, 5) == SOCKET_ERROR) {
        printf("listen() failed. Error: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    printf("Chat server running on %s:%d (max %d clients). Press Ctrl+C to stop.\n",
           SERVER_IP, SERVER_PORT, MAX_CLIENTS);

    /* Server runs until stopped with Ctrl+C */
    while (1) {
        SOCKET new_sock = accept(listen_sock, NULL, NULL);
        int slot = -1;
        HANDLE thread;

        if (new_sock == INVALID_SOCKET) {
            printf("accept() failed. Error: %d\n", WSAGetLastError());
            continue;
        }

        EnterCriticalSection(&clients_lock);
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].in_use) {
                slot = i;
                clients[i].in_use = 1;
                clients[i].logged_in = 0;
                clients[i].sock = new_sock;
                clients[i].username[0] = '\0';
                break;
            }
        }
        LeaveCriticalSection(&clients_lock);

        if (slot < 0) {
            send_line(new_sock, "ERR Server is full. Try again later.");
            closesocket(new_sock);
            continue;
        }

        thread = CreateThread(NULL, 0, client_thread, (LPVOID)(INT_PTR)slot, 0, NULL);
        if (thread == NULL) {
            printf("CreateThread() failed. Error: %lu\n", GetLastError());
            EnterCriticalSection(&clients_lock);
            closesocket(clients[slot].sock);
            clients[slot].sock = INVALID_SOCKET;
            clients[slot].in_use = 0;
            LeaveCriticalSection(&clients_lock);
        } else {
            CloseHandle(thread);
        }
    }

    return 0;
}
