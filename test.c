#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NUM_REQUESTS 10
#define DOMAIN "facebook.com"
#define DOMAIN1 "google.com"
#define PROXY_SERVER_IP "127.0.0.1"

int main() {
    for (int i = 0; i < NUM_REQUESTS; i++) {
        char command[256];
        if (i % 2 == 1) {
            snprintf(command, sizeof(command), "dig @%s -p %d %s", PROXY_SERVER_IP, 12121, DOMAIN);
        } else {
            snprintf(command, sizeof(command), "dig @%s -p %d %s", PROXY_SERVER_IP, 12121, DOMAIN1);
        }
        system(command);
        sleep(1);  // Затримка між запитами (у секундах)
    }
    
    return 0;
}
