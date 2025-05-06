// esp32_hw.h (Header)
#ifndef ESP32_HW_H
#define ESP32_HW_H

#include <stdint.h>
#include <stddef.h>

// Basic hardware initialization
void esp_cpu_init(void);
void esp_intr_init(void);
void esp_clk_init(void);
void esp_periph_init(void);
void esp_cpu_intr_enable(void);
void esp_cpu_intr_disable(void);
void esp_cpu_wait_for_intr(void);

// IRQ management
#define ESP32_IRQ_COUNT 32
void esp_intr_controller_init(void);
void esp_intr_set_priority(int source, int priority);
void esp_intr_enable(int source);
void esp_intr_disable(int source);
void esp_cpu_set_exception_handlers(void);

// Timing functions
uint32_t esp_get_time_ms(void);
void esp_delay_ms(uint32_t ms);
void esp_yield(void);

// UART functions
void esp_uart_init(int uart_num, int baud_rate);
void esp_uart_putc(int uart_num, char c);
int esp_uart_getc(int uart_num);

// WiFi-related structs and functions
typedef enum {
    WIFI_MODE_NULL = 0,
    WIFI_MODE_STA,
    WIFI_MODE_AP,
    WIFI_MODE_APSTA
} wifi_mode_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
} wifi_sta_config_t;

typedef struct {
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
    int connected;
    uint8_t ip_addr[4];
} esp_wifi_state_t;

typedef enum {
    ESP_IF_WIFI_STA = 0,
    ESP_IF_WIFI_AP
} esp_interface_t;

void esp_wifi_init(void);
int esp_wifi_set_mode(wifi_mode_t mode);
int esp_wifi_set_config(esp_interface_t interface, wifi_config_t *conf);
int esp_wifi_start(void);
int esp_wifi_stop(void);

// TCP/IP stack functions
void esp_tcpip_init(void);
int esp_tcp_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int esp_tcp_connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int esp_tcp_send(int sockfd, const void *buf, size_t len, int flags);
int esp_tcp_recv(int sockfd, void *buf, size_t len, int flags);
int esp_tcp_available(int sockfd);

int esp_udp_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int esp_udp_sendto(int sockfd, const void *buf, size_t len, int flags,
                   const struct sockaddr *dest_addr, socklen_t addrlen);
int esp_udp_recvfrom(int sockfd, void *buf, size_t len, int flags,
                     struct sockaddr *src_addr, socklen_t *addrlen);
int esp_udp_available(int sockfd);
int esp_udp_join_multicast_group(int sockfd, const struct ip_mreq *mreq);
int esp_udp_leave_multicast_group(int sockfd, const struct ip_mreq *mreq);

int esp_socket_setsockopt(int sockfd, int level, int optname, 
                         const void *optval, socklen_t optlen);

#endif /* ESP32_HW_H */
