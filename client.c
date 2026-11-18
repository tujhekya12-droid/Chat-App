#include "common.h"

static SOCKET client_sock;
static volatile int running = 1;

/* Reads a line from the keyboard without the newline. Returns 0 on end of input. */
static int read_input(const char *prompt, char *buffer, int size)
{
    size_t length;

    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, size, stdin) == NULL) {
        return 0;
    }
    length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == '\n') {
        buffer[--length] = '\0';
    } else {
        int ch;   /* line was too long: throw away the rest */
        while ((ch = getchar()) != '\n' && ch != EOF) { }
    }
    if (length > 0 && buffer[length - 1] == '\r') {
        buffer[--length] = '\0';
    }
    return 1;
}

/* Runs in a separate thread: prints everything the server sends */
static DWORD WINAPI receive_thread(LPVOID arg)
{
    char line[BUFFER_SIZE];
    (void)arg;

    while (running) {
        if (recv_line(client_sock, line, sizeof(line)) < 0) {
            if (running) {
                printf("\nConnection to server lost. Press Enter to exit.\n");
                running = 0;
            }
            break;
        }
        printf("\r%s\n> ", line);
        fflush(stdout);
    }
    return 0;
}

/* Register / login menu. Returns 1 when logged in, 0 to exit. */
static int authenticate(void)
{
    char choice[16], username[64], password[64];
    char request[BUFFER_SIZE], reply[BUFFER_SIZE];

    while (1) {
        int is_login;

        printf("\n1. Register\n2. Login\n3. Exit\n");
        if (!read_input("Choose: ", choice, sizeof(choice)) || choice[0] == '3') {
            return 0;
        }
        if (choice[0] != '1' && choice[0] != '2') {
            printf("Invalid choice.\n");
            continue;
        }
        is_login = (choice[0] == '2');

        if (!read_input("Username: ", username, sizeof(username)) ||
            !read_input("Password: ", password, sizeof(password))) {
            return 0;
        }
        if (username[0] == '\0' || password[0] == '\0' ||
            strchr(username, ' ') != NULL || strchr(password, ' ') != NULL ||
            strlen(username) >= NAME_LEN || strlen(password) >= PASS_LEN) {
            printf("Username and password must be 1-31 characters with no spaces.\n");
            continue;
        }

        snprintf(request, sizeof(request), "%s %s %s",
                 is_login ? "LOGIN" : "REGISTER", username, password);

        if (send_line(client_sock, request) < 0 ||
            recv_line(client_sock, reply, sizeof(reply)) < 0) {
            printf("Connection to server lost.\n");
            return 0;
        }

        if (strncmp(reply, "OK ", 3) == 0) {
            printf("%s\n", reply + 3);
            if (is_login) {
                return 1;
            }
        } else if (strncmp(reply, "ERR ", 4) == 0) {
            printf("Error: %s\n", reply + 4);
        } else {
            printf("%s\n", reply);
        }
    }
}

int main(void)
{
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    HANDLE receiver;
    char input[MAX_MESSAGE_LEN + 1];
    char request[BUFFER_SIZE];

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }

    client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        printf("socket() failed. Error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);

    if (connect(client_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Could not connect to server. Error: %d. Is the server running?\n", WSAGetLastError());
        closesocket(client_sock);
        WSACleanup();
        return 1;
    }
    printf("Connected to chat server.\n");

    if (!authenticate()) {
        send_line(client_sock, "QUIT");
        closesocket(client_sock);
        WSACleanup();
        return 0;
    }

    printf("\nCommands: /users  /history  /quit   (max %d characters per message)\n", MAX_MESSAGE_LEN);

    receiver = CreateThread(NULL, 0, receive_thread, NULL, 0, NULL);
    if (receiver == NULL) {
        printf("CreateThread() failed. Error: %lu\n", GetLastError());
        closesocket(client_sock);
        WSACleanup();
        return 1;
    }

    while (running) {
        if (!read_input("> ", input, sizeof(input))) {
            break;
        }
        if (!running) {
            break;   /* connection was lost while waiting for input */
        }
        if (input[0] == '\0') {
            continue;
        }

        if (strcmp(input, "/quit") == 0) {
            break;
        } else if (strcmp(input, "/users") == 0) {
            snprintf(request, sizeof(request), "USERS");
        } else if (strcmp(input, "/history") == 0) {
            snprintf(request, sizeof(request), "HISTORY");
        } else if (input[0] == '/') {
            printf("Unknown command. Use /users, /history or /quit.\n");
            continue;
        } else {
            snprintf(request, sizeof(request), "MSG %s", input);
        }

        if (send_line(client_sock, request) < 0) {
            printf("Failed to send. Connection lost.\n");
            break;
        }
    }

    /* Clean shutdown */
    send_line(client_sock, "QUIT");
    running = 0;
    shutdown(client_sock, SD_BOTH);          /* wakes up the receiver thread */
    WaitForSingleObject(receiver, 2000);
    CloseHandle(receiver);
    closesocket(client_sock);
    WSACleanup();
    printf("Disconnected.\n");
    return 0;
}
