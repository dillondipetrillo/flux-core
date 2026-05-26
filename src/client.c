#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "protocol.h"
#include "utils.h"

#define MAX_NAME 32

void send_packet(int socket_fd, enum packet_type type, uint32_t scope,
    uint64_t expires, const char *payload, size_t payload_len)
{
    struct packet_header header;
    memset(&header, 0, sizeof(struct packet_header));
    header.type = (uint8_t)type;
    header.payload_len = htonl(payload_len);
    header.sender_id = htonl(socket_fd);
    header.scope_id = htonl(scope);
    header.expires_at = htobe64(expires);

    handle_send(socket_fd, (const char *)&header,
        sizeof(struct packet_header));

    if (payload != NULL && payload_len > 0)
        handle_send(socket_fd, payload, payload_len);
}

int main(int argc, char **argv)
{
    struct engine_config config = config_load();
    int socketfd = socket(PF_INET, SOCK_STREAM, 0);
    if (socketfd == -1) {
        perror("socket");
        return(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);

    if (connect(socketfd, (struct sockaddr *)&address,
            sizeof(address)) == -1
    ) {
        perror("connect");
        exit(EXIT_FAILURE);
    }
    printf("Connected to server...\n");

    char username[MAX_NAME] = "Guest";
    if (argc > 1) {
        strncpy(username, argv[1], MAX_NAME - 1);
        username[MAX_NAME - 1] = '\0';
    }
    uint32_t my_scopes[MAX_SCOPES];
    int my_scope_count = 0;
    uint32_t active_scope = 1;
    if (argc > 2 && my_scope_count < MAX_SCOPES) {
        active_scope = (uint32_t)strtoul(argv[2], NULL, 10);
        my_scopes[my_scope_count++] = active_scope;
    }
    char pending_name[MAX_NAME] = {0};
    uint32_t pending_scope = 0;
    uint32_t pending_leave_scope = 0;

    send_packet(socketfd, TYPE_SYS_IDENTIFY, active_scope, 0, username,
        strlen(username));

    uint32_t scope_payload = htonl(active_scope);
    send_packet(socketfd, TYPE_SYS_JOIN, active_scope, 0,
        (const char *)&scope_payload, sizeof(uint32_t));

    printf("Handshake complete.\n");

    /**
     * // TEMP TEST - join 17 scopes
    for (int i = 1; i <= 17; i++) {
        char scope_str[16];
        snprintf(scope_str, sizeof(scope_str), "%d", i);
        uint32_t s = (uint32_t)i;
        send_packet(socketfd, TYPE_SYS_JOIN, s, 0, NULL, 0);
        // small delay so server processes each one
        usleep(10000);
    }
     */

    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(socketfd, &readfds);

        int maxfd = socketfd;

        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) == -1) {
            perror("select");
            exit(EXIT_FAILURE);
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            /****************************************************************
             * BURST TEST START                                             *
             ****************************************************************/
            // char dummy[10];
            // fgets(dummy, 10, stdin);

            // for (int i = 0; i <= 500; i++) {
            //     char burst_message[MAX_PAYLOAD];
            //     snprintf(burst_message, MAX_PAYLOAD,
            //         "Packet #%d from Dillon", i);

            //     struct packet_header burst_h;
            //     memset(&burst_h, 0, sizeof(burst_h));
            //     burst_h.type = (uint8_t)TYPE_APP_REALTIME;
            //     burst_h. payload_len = htonl(strlen(burst_message));
            //     burst_h.scope_id = htonl(active_scope);
            //     burst_h.sender_id = htonl(socketfd);
            //     burst_h.expires_at = htobe64(0);

            //     handle_send(socketfd, (const char *)&burst_h,
            //         sizeof(struct packet_header));
            //     handle_send(socketfd, burst_message, strlen(burst_message));
            // }
            // printf("Sent 500 packets in a burst.\n");
            /****************************************************************
             * BURST TEST END                                               *
             ****************************************************************/

            char input[MAX_PAYLOAD];
            if (fgets(input, MAX_PAYLOAD, stdin) == NULL)
                break;
            input[strcspn(input, "\n\r")] = '\0';

            if (input[0] == '/') {
                char cmd[32] = {0};
                char arg[MAX_PAYLOAD] = {0};
                sscanf(input, "%31s %1023s", cmd, arg);

                if (strcmp(cmd, "/nick") == 0) {
                    if (arg[0] == '\0')
                        printf("Usage: /nick <name>\n");
                    else {
                        strncpy(pending_name, arg, MAX_NAME - 1);
                        pending_name[MAX_NAME - 1] = '\0';
                        send_packet(socketfd, TYPE_SYS_IDENTIFY, 0, 0, arg,
                            strlen(arg));
                    }
                } else if (strcmp(cmd, "/join") == 0) {
                    if (arg[0] == '\0')
                        printf("Usage: /join <scope_id>\n");
                    else {
                        pending_scope = (uint32_t)strtoul(arg, NULL,
                            10);
                        send_packet(socketfd, TYPE_SYS_JOIN, pending_scope, 0,
                            NULL, 0);
                    }
                } else if (strcmp(cmd, "/leave") == 0) {
                    if (arg[0] == '\0')
                        printf("Usage: /leave <scope_id>\n");
                    else {
                        uint32_t leave_scope = (uint32_t)strtoul(arg, NULL,
                            10);
                        pending_leave_scope = leave_scope;
                        send_packet(socketfd, TYPE_SYS_LEAVE, leave_scope, 0,
                            NULL, 0);
                    }
                } else if (strcmp(cmd, "/status") == 0) {
                    printf("[Status] Name %s | Active scope: %u | "
                        "In %d scope(s)\n", username, active_scope,
                        my_scope_count);
                } else {
                    printf("Unknown command: %s\n", cmd);
                    printf("Commands: /nick <name>, /join <id>, "
                        "/leave <id>, /status\n");
                }
            } else {
                if (active_scope == 0) {
                    printf("You must /join a scope before sending "
                        "messsages.\n");
                } else {
                    send_packet(socketfd, TYPE_APP_REALTIME, active_scope,
                        (uint64_t)time(NULL) - 1, input, strlen(input));
                }
            }
        }

        if (FD_ISSET(socketfd, &readfds)) {
            char r_buffer[sizeof(struct packet_header)];
            ssize_t recv_header = handle_recv(socketfd, r_buffer,
                sizeof(struct packet_header));

            if (recv_header == 0) {
                printf("Server disconnected.\n");
                exit(EXIT_SUCCESS);
            }
            
            if (recv_header == -1) {
                perror("recv");
                exit(EXIT_FAILURE);
            }

            struct packet_header r_header;
            memcpy(&r_header, r_buffer, sizeof(struct packet_header));
            r_header.payload_len = ntohl(r_header.payload_len);
            r_header.scope_id    = ntohl(r_header.scope_id);
            r_header.sender_id   = ntohl(r_header.sender_id);
            r_header.expires_at  = be64toh(r_header.expires_at);

            if (r_header.payload_len > MAX_PAYLOAD) {
                printf("Received oversized payload, disconnecting.\n");
                exit(EXIT_FAILURE);
            }

            char r_payload[MAX_PAYLOAD + 1];
            memset(r_payload, 0, sizeof(r_payload));

            if (r_header.payload_len > 0) {
                ssize_t recv_payload = handle_recv(socketfd, r_payload,
                    r_header.payload_len);

                if (recv_payload == 0) {
                    printf("Server disconnected.\n");
                    exit(EXIT_SUCCESS);
                }
                if (recv_payload == -1) {
                    perror("recv");
                    exit(EXIT_FAILURE);
                }
            }

            switch ((enum packet_type)r_header.type) {
                case TYPE_SYS_ACK: {
                    // Read and discard the 4-byte response payload
                    struct response_payload rp;
                    memcpy(&rp, r_payload, sizeof(rp));
                    uint32_t code = ntohl(rp.status_code);
                    printf("[ACK] status=%u\n", code);
                    break;

                    if (pending_scope != 0) {
                        active_scope = pending_scope;
                        if (my_scope_count < MAX_SCOPES)
                            my_scopes[my_scope_count++] = pending_scope;
                        printf("[OK] Joined scope %u\n", active_scope);
                        pending_scope = 0;
                    } else if (pending_leave_scope != 0) {
                        for (int j = 0; j < my_scope_count; j++) {
                            if (my_scopes[j] == pending_leave_scope) {
                                my_scopes[j] = my_scopes[my_scope_count - 1];
                                my_scope_count--;
                                break;
                            }
                        }
                        if (active_scope == pending_leave_scope)
                            active_scope = 0;
                        printf("[OK] Left scope %u\n", pending_leave_scope);
                        pending_leave_scope = 0;
                    } else if (pending_name[0] != '\0') {
                        strncpy(username, pending_name, MAX_NAME - 1);
                        username[MAX_NAME - 1] = '\0';
                        printf("[OK] Name is set to %s\n", username);
                        pending_name[0] = '\0';
                    } else {
                        printf("[OK] Operation succeeded.\n");
                    }
                    break;
                }
                case TYPE_SYS_ERROR: {
                    struct response_payload rp;
                    memcpy(&rp, r_payload, sizeof(rp));
                    uint32_t code = ntohl(rp.status_code);
                    printf("[ERROR] status=%u\n", code);
                    break;

                    pending_scope = 0;
                    pending_leave_scope = 0;
                    pending_name[0] = '\0';

                    switch (code) {
                        case STATUS_ERR_UNIDENTIFIED:
                            printf("[ERROR] You must /nick before doing that.\n");
                            break;
                        case STATUS_ERR_ALREADY_ID:
                            printf("[ERROR] Already identified.\n");
                            break;
                        case STATUS_ERR_ROOM_FULL:
                            printf("[ERROR] You are in the maximum number "
                                "of rooms.\n");
                            break;
                        case STATUS_ERR_ALREADY_IN_ROOM:
                            printf("[ERROR] You are already in that room.\n");
                            break;
                        case STATUS_ERR_NOT_IN_ROOM:
                            printf("[ERROR] You are not in that room.\n");
                            break;
                        case STATUS_ERR_EXPIRED:
                            printf("[ERROR] Packet expired before delivery.\n");
                            break;
                        case STATUS_ERR_SCOPES_FULL:
                            printf("[ERROR] Maximum scopes reached.\n");
                            break;
                        default:
                            printf("[ERROR] Unknown error code %u\n", code);
                            break; 
                    }
                    break;
                }
                case TYPE_SYS_PING:
                    // Silently consume ping responses
                    break;
                default:
                    r_payload[r_header.payload_len] = '\0';
                    printf("Message: %s\n", r_payload);
                    break;
            }
        }
    }

    exit(EXIT_SUCCESS);
}