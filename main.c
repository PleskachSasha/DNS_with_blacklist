#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#define BUFFER_SIZE 256
#define MAX_BLACK_DOMAINS 100

typedef struct {
    char** domains;
    char* response;
    char upstream_server[16];
    int count;
} Blacklist;

Blacklist* parse_config_file(const char* file_path) {
    FILE* file = fopen(file_path, "r");
    if (file == NULL) {
        printf("Помилка відкриття конфігураційного файлу.\n");
        return NULL;
    }

    Blacklist* blacklist = (Blacklist*)malloc(sizeof(Blacklist));
    if (blacklist == NULL) {
        printf("Помилка виділення пам'яті для чорного списку.\n");
        fclose(file);
        return NULL;
    }

    blacklist->domains = (char**)malloc(MAX_BLACK_DOMAINS * sizeof(char*));
    blacklist->count = 0;
    blacklist->response = NULL;

    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), file)){
        line[strcspn(line, "\n")] = '\0';

        if (strncmp(line, "upstream_server=", 16) == 0) {
            strncpy(blacklist->upstream_server, line + 16, sizeof(blacklist->upstream_server) - 1);
            blacklist->upstream_server[sizeof(blacklist->upstream_server) - 1] = '\0';

        } else if (strncmp(line, "response=", 9) == 0) {
            int length = strlen(line + 9);
            blacklist->response = (char*)malloc((length + 1) * sizeof(char));
            strncpy(blacklist->response, line + 9, length);
            blacklist->response[length] = '\0';

        } else if (strncmp(line, "domain=", 7) == 0) {
            int length = strlen(line + 7);
            blacklist->domains[blacklist->count] = (char*)malloc((length + 1) * sizeof(char));
            strncpy(blacklist->domains[blacklist->count], line + 7, length);
            blacklist->domains[blacklist->count][length] = '\0';
            blacklist->count++;
        }
    }

    fclose(file);
    return blacklist;
}

int is_blacklisted(const char* domain, Blacklist* blacklist) {
    for (int i = 0; i < blacklist->count; ++i) {
        if (strcmp(domain, blacklist->domains[i]) == 0)
            return 1;
    }
    return 0;
}


void start_dns_proxy_server(const char* config_file) {
    Blacklist* blacklist = parse_config_file(config_file);
    if (blacklist == NULL) {
        printf("Помилка отримання параметрів з конфігураційного файлу.\n");
        return;
    }

    int clientSocket, dnsSocket;
    struct sockaddr_in clientAddr, dnsAddr;
    socklen_t clientAddrLen;
    unsigned char buffer[BUFFER_SIZE];
    unsigned char queryBuffer[BUFFER_SIZE];

    clientSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (clientSocket < 0) {
        perror("Помилка при створенні сокету");
        exit(1);
    }

    memset(&clientAddr, 0, sizeof(clientAddr));
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_addr.s_addr = INADDR_ANY;
    clientAddr.sin_port = htons(12345);

    if (bind(clientSocket, (struct sockaddr*)&clientAddr, sizeof(clientAddr)) < 0) {
        perror("Помилка при прив'язці сокету");
        exit(1);
    }

    printf("Сервер DNS запущено. Очікування запитів...\n");

    while (1) {
        clientAddrLen = sizeof(clientAddr);
        ssize_t receivedBytes = recvfrom(clientSocket, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (receivedBytes < 0) {
            perror("Помилка при отриманні запиту");
            continue;
        }

        printf("Отримано DNS запит від клієнта\n");

        char domain_name[BUFFER_SIZE];
        int position = 12;
        int domain_length = 0;

        while (buffer[position] != 0) {
            int label_length = buffer[position];
            ++position;

            for (int i = 0; i < label_length; ++i) {
                if (buffer[position] != '@') {
                    domain_name[domain_length] = buffer[position];
                    ++domain_length;
                }
                ++position;
            }

            domain_name[domain_length] = '.';
            ++domain_length;
        }

        if (domain_length > 0 && domain_name[domain_length - 1] == '.') {
            domain_name[domain_length - 1] = '\0';
        } else {
            domain_name[domain_length] = '\0';
        }

        if (is_blacklisted(domain_name, blacklist)) {
            unsigned short requestID = (buffer[0] << 8) | buffer[1];
                queryBuffer[0] = (requestID >> 8) & 0xFF;
                queryBuffer[1] = requestID & 0xFF;

                size_t responseLength = strlen(blacklist->response);
                memcpy(queryBuffer + 2, blacklist->response, responseLength);

                ssize_t sentResponseBytes = sendto(clientSocket, queryBuffer, responseLength + 2, 0, (struct sockaddr*)&clientAddr, clientAddrLen);

                if (sentResponseBytes < 0) {
                    perror("Помилка при відправці відповіді клієнту");
                } else {
                    printf("Відправлено відповідь клієнту: %s\n", blacklist->response);
                }

        }else {
            printf("Перенаправление DNS-запроса для домена: %s\n", domain_name);


            dnsSocket = socket(AF_INET, SOCK_DGRAM, 0);
            if (dnsSocket < 0) {
                perror("Помилка при створенні сокету для сервера DNS");
                continue;
            }

            memset(&dnsAddr, 0, sizeof(dnsAddr));
            dnsAddr.sin_family = AF_INET;
            dnsAddr.sin_addr.s_addr = inet_addr(blacklist->upstream_server);
            dnsAddr.sin_port = htons(53);

            ssize_t sentBytes = sendto(dnsSocket, buffer, receivedBytes, 0, (struct sockaddr*)&dnsAddr, sizeof(dnsAddr));
            if (sentBytes < 0) {
                perror("Помилка при відправці запиту на сервер DNS");
                close(dnsSocket);
                continue;
            }

            printf("Запит перенаправлено на сервер DNS\n");
            memset(buffer, 0, BUFFER_SIZE);
            ssize_t dnsResponseBytes = recvfrom(dnsSocket, buffer, BUFFER_SIZE, 0, NULL, NULL);
            if (dnsResponseBytes < 0) {
                perror("Помилка при отриманні відповіді від сервера DNS");
                close(dnsSocket);
                continue;
            }

            printf("Отримано відповідь від сервера DNS\n");

            ssize_t sentResponseBytes = sendto(clientSocket, buffer, dnsResponseBytes, 0, (struct sockaddr*)&clientAddr, clientAddrLen);
            if (sentResponseBytes < 0) {
                perror("Помилка при відправці відповіді клієнту");
            }

            printf("Відповідь відправлено клієнту\n");

            close(dnsSocket);
        }
    }
    close(clientSocket);

    for (int i = 0; i < blacklist->count; ++i) {
            free(blacklist->domains[i]);
    }
    free(blacklist->domains);
    free(blacklist->response);
    free(blacklist);
}
int main() {
    const char* config_file = "/home/oleksandr/DNSproxyserver/config.txt";
    start_dns_proxy_server(config_file);
}
