#include "netstack.h"
#include "libc.h"
#include "esp32_hw.h"

#define MAX_SOCKETS 16

typedef struct {
    int in_use;
    int type;          // SOCK_STREAM or SOCK_DGRAM
    int protocol;
    struct sockaddr local_addr;
    struct sockaddr remote_addr;
    int connected;
    uint8_t rx_buffer[2048];
    int rx_data_len;
} socket_t;

static socket_t sockets[MAX_SOCKETS];
static esp_wifi_state_t wifi_state;

int net_init(const char* ssid, const char* password) {
    // Initialize ESP32 WiFi hardware
    esp_wifi_init();
    
    // Configure WiFi in station mode
    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);
    
    // Start WiFi
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
    
    // Wait for connection
    while (wifi_state.connected == 0) {
        esp_delay_ms(100);
    }
    
    // Initialize TCP/IP stack
    esp_tcpip_init();
    
    // Initialize socket structures
    for (int i = 0; i < MAX_SOCKETS; i++) {
        sockets[i].in_use = 0;
        sockets[i].rx_data_len = 0;
    }
    
    return 0;
}

int socket(int domain, int type, int protocol) {
    if (domain != AF_INET) {
        return -1; // Only support IPv4
    }
    
    // Find a free socket slot
    int sock_fd = -1;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            sock_fd = i;
            break;
        }
    }
    
    if (sock_fd == -1) {
        return -1; // No free sockets
    }
    
    // Initialize socket structure
    sockets[sock_fd].in_use = 1;
    sockets[sock_fd].type = type;
    sockets[sock_fd].protocol = protocol;
    sockets[sock_fd].connected = 0;
    sockets[sock_fd].rx_data_len = 0;
    
    return sock_fd;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS || !sockets[sockfd].in_use) {
        return -1;
    }
    
    // Copy address
    if (addrlen > sizeof(struct sockaddr)) {
        addrlen = sizeof(struct sockaddr);
    }
    memcpy(&sockets[sockfd].local_addr, addr, addrlen);
    
    // Configure the socket in lwIP
    if (sockets[sockfd].type == SOCK_DGRAM) {
        return esp_udp_bind(sockfd, addr, addrlen);
    } else {
        return esp_tcp_bind(sockfd, addr, addrlen);
    }
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS || !sockets[sockfd].in_use) {
        return -1;
    }
    
    // Store remote address
    if (addrlen > sizeof(struct sockaddr)) {
        addrlen = sizeof(struct sockaddr);
    }
    memcpy(&sockets[sockfd].remote_addr, addr, addrlen);
    
    // Establish connection
    if (sockets[sockfd].type == SOCK_STREAM) {
        int ret = esp_tcp_connect(sockfd, addr, addrlen);
        if (ret == 0) {
            sockets[sockfd].connected = 1;
        }
        return ret;
    } else {
        // For UDP, just store the remote address
        return 0;
    }
}

int send(int sockfd, const void *buf, size_t len, int flags) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS || !sockets[sockfd].in_use) {
        return -1;
    }
    
    if (sockets[sockfd].type == SOCK_STREAM && !sockets[sockfd].connected) {
        return -1;
    }
    
    // For TCP
    if (sockets[sockfd].type == SOCK_STREAM) {
        return esp_tcp_send(sockfd, buf, len, flags);
    } else {
        // For UDP, this should use the stored remote address
        return esp_udp_sendto(sockfd, buf, len, flags, 
                            &sockets[sockfd].remote_addr, 
                            sizeof(struct sockaddr));
    }
}

int recv(int sockfd, void *buf, size_t len, int flags) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS || !sockets[sockfd].in_use) {
        return -1;
    }
    
    if (sockets[sockfd].type == SOCK_STREAM && !sockets[sockfd].connected) {
        return -1;
    }
    
    // Check if there's cached data
    if (sockets[sockfd].rx_data_len > 0) {
        int copy_len = len < sockets[sockfd].rx_data_len ? len : sockets[sockfd].rx_data_len;
        memcpy(buf, sockets[sockfd].rx_buffer, copy_len);
        
        // Shift remaining data
        if (copy_len < sockets[sockfd].rx_data_len) {
            memmove(sockets[sockfd].rx_buffer, 
                   sockets[sockfd].rx_buffer + copy_len,
                   sockets[sockfd].rx_data_len - copy_len);
            sockets[sockfd].rx_data_len -= copy_len;
        } else {
            sockets[sockfd].rx_data_len = 0;
        }
        
        return copy_len;
    }
    
    // No cached data, perform actual receive
    if (sockets[sockfd].type == SOCK_STREAM) {
        return esp_tcp_recv(sockfd, buf, len, flags);
    } else {
        struct sockaddr src_addr;
        socklen_t addrlen = sizeof(src_addr);
        return esp_udp_recvfrom(sockfd, buf, len, flags, &src_addr, &addrlen);
    }
}

// Implement other socket functions similarly
// close(), setsockopt(), etc.

// Function to join a multicast group
int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    if (sockfd < 0 || sockfd >= MAX_SOCKETS || !sockets[sockfd].in_use) {
        return -1;
    }
    
    // Handle IP_ADD_MEMBERSHIP
    if (level == IPPROTO_IP && optname == IP_ADD_MEMBERSHIP) {
        if (optlen < sizeof(struct ip_mreq)) {
            return -1;
        }
        
        const struct ip_mreq *mreq = optval;
        return esp_udp_join_multicast_group(sockfd, mreq);
    }
    
    // Handle IP_DROP_MEMBERSHIP
    if (level == IPPROTO_IP && optname == IP_DROP_MEMBERSHIP) {
        if (optlen < sizeof(struct ip_mreq)) {
            return -1;
        }
        
        const struct ip_mreq *mreq = optval;
        return esp_udp_leave_multicast_group(sockfd, mreq);
    }
    
    // Handle other socket options
    return esp_socket_setsockopt(sockfd, level, optname, optval, optlen);
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    int result = 0;
    uint32_t timeout_ms = 0;
    
    if (timeout) {
        timeout_ms = (timeout->tv_sec * 1000) + (timeout->tv_usec / 1000);
    }
    
    // Start time
    uint32_t start_time = esp_get_time_ms();
    
    while (1) {
        // Check each socket if it's in the readfds set
        if (readfds) {
            for (int i = 0; i < nfds && i < MAX_SOCKETS; i++) {
                if (FD_ISSET(i, readfds) && sockets[i].in_use) {
                    // Check if there's data available
                    int available = sockets[i].rx_data_len;
                    if (available == 0) {
                        // Otherwise check with the TCP/IP stack
                        if (sockets[i].type == SOCK_STREAM) {
                            available = esp_tcp_available(i);
                        } else {
                            available = esp_udp_available(i);
                        }
                    }
                    
                    if (available > 0) {
                        // Keep it set in the readfds
                        result++;
                    } else {
                        // Clear it from readfds
                        FD_CLR(i, readfds);
                    }
                }
            }
        }
        
        // Similar checks for writefds and exceptfds...
        
        // Check if we have any results or if we've timed out
        if (result > 0 || (timeout && (esp_get_time_ms() - start_time >= timeout_ms))) {
            break;
        }
        
        // Small yield to give other tasks a chance
        esp_yield();
    }
    
    return result;
}
