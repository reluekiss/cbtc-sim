#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ESP-IDF includes
#include "esp_log_color.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "esp_log.h"

#define RUNNING_IN_QEMU 1

#define BUFFER_SIZE 1024
#define ZC_PORT 8100
#define MULTICAST_PORT 8200

#define DEFAULT_TRAIN_ID 1
#define DEFAULT_ZONE_ID 1  
#define DEFAULT_SECTION 1
#define DEFAULT_ZC_IP "192.168.1.1"

typedef struct {
    int id;
    int currentSection;
    int currentSpeed;
    int targetSpeed;
    int zoneId;
} TrainState;

// Global variables
static const char *TAG = "train_control";
TrainState state;
int zoneControllerSocket = -1;
int multicastSocket = -1;
static bool console_active = false;

// WiFi connection parameters
#define WIFI_SSID "TrainNet"
#define WIFI_PASS "traincontrol123"

// Event group to signal WiFi connection
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// WiFi event handler
static void event_handler(void* arg, esp_event_base_t event_base,
                         int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Retry connecting to AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        sprintf(ip_str, IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void load_train_config(int *train_id, int *zone_id, int *section, char *zc_ip, size_t ip_size) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Set defaults
    *train_id = DEFAULT_TRAIN_ID;
    *zone_id = DEFAULT_ZONE_ID;
    *section = DEFAULT_SECTION;
    strncpy(zc_ip, DEFAULT_ZC_IP, ip_size);
    
    // Open NVS
    err = nvs_open("train_cfg", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS not initialized yet, using defaults");
        return;
    }
    
    // Read values from NVS, keep defaults if not found
    nvs_get_i32(nvs_handle, "train_id", (int32_t*)train_id);
    nvs_get_i32(nvs_handle, "zone_id", (int32_t*)zone_id);
    nvs_get_i32(nvs_handle, "section", (int32_t*)section);
    
    size_t required_size = 0;
    err = nvs_get_str(nvs_handle, "zc_ip", NULL, &required_size);
    if (err == ESP_OK && required_size <= ip_size) {
        nvs_get_str(nvs_handle, "zc_ip", zc_ip, &required_size);
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Loaded configuration: Train ID=%d, Zone ID=%d, Section=%d, ZC IP=%s", 
             *train_id, *zone_id, *section, zc_ip);
}

// Function to save configuration to NVS
esp_err_t save_train_config(int train_id, int zone_id, int section, const char *zc_ip) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // Open NVS
    err = nvs_open("train_cfg", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS!");
        return err;
    }
    
    // Write values to NVS
    err = nvs_set_i32(nvs_handle, "train_id", train_id);
    if (err != ESP_OK) return err;
    
    err = nvs_set_i32(nvs_handle, "zone_id", zone_id);
    if (err != ESP_OK) return err;
    
    err = nvs_set_i32(nvs_handle, "section", section);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs_handle, "zc_ip", zc_ip);
    if (err != ESP_OK) return err;
    
    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) return err;
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Configuration saved to NVS");
    return ESP_OK;
}
#if RUNNING_IN_QEMU
// Mock WiFi implementation to bypass hardware
#include "esp_netif.h"

// Mocked WiFi functions - these will be more direct now

// Modified WiFi init function for QEMU
void wifi_init_sta_qemu(void) {
    // Create the event group first
    wifi_event_group = xEventGroupCreate();
    
    ESP_LOGI(TAG, "[MOCK] Initializing WiFi in QEMU simulation");
    
    // Initialize standard hardware - keep only what's necessary
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create a default network interface (actually used)
    esp_netif_create_default_wifi_sta();
    
    ESP_LOGI(TAG, "[MOCK] Setting static IP: 192.168.1.100");
    
    // No need to actually call wifi functions or post events
    // Just simulate a successful connection by setting the event group bit directly
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    
    ESP_LOGI(TAG, "[MOCK] WiFi connection simulated successfully");
    
    // Log the IP address to match normal behavior
    ESP_LOGI(TAG, "Got IP: 192.168.1.100");
}

// Socket mocking functions
// Create a simple tracking system for sockets instead of trying to override system functions

#define MAX_MOCK_SOCKETS 10

typedef struct {
    int fd;
    bool is_multicast;
    int multicast_group;
    int port;
    bool in_use;
} mock_socket_t;

static mock_socket_t mock_sockets[MAX_MOCK_SOCKETS];
static int next_mock_fd = 10;

// Initialize the socket mocking system
void mock_sockets_init(void) {
    memset(mock_sockets, 0, sizeof(mock_sockets));
}

// Create a mock socket
int mock_socket_create(void) {
    int fd = next_mock_fd++;
    
    for (int i = 0; i < MAX_MOCK_SOCKETS; i++) {
        if (!mock_sockets[i].in_use) {
            mock_sockets[i].fd = fd;
            mock_sockets[i].in_use = true;
            mock_sockets[i].is_multicast = false;
            ESP_LOGI(TAG, "[MOCK] Created socket fd=%d", fd);
            return fd;
        }
    }
    
    ESP_LOGE(TAG, "[MOCK] No free mock sockets available");
    return -1;
}

// Get a mock socket by fd
mock_socket_t* mock_get_socket(int fd) {
    for (int i = 0; i < MAX_MOCK_SOCKETS; i++) {
        if (mock_sockets[i].in_use && mock_sockets[i].fd == fd) {
            return &mock_sockets[i];
        }
    }
    return NULL;
}

// Mark a socket as multicast
void mock_set_multicast(int fd, int group, int port) {
    mock_socket_t* socket = mock_get_socket(fd);
    if (socket) {
        socket->is_multicast = true;
        socket->multicast_group = group;
        socket->port = port;
        ESP_LOGI(TAG, "[MOCK] Socket %d marked as multicast for group %d", fd, group);
    }
}

// Generate mock responses
ssize_t mock_generate_response(int fd, void* buffer, size_t len) {
    static int call_count = 0;
    call_count++;
    
    mock_socket_t* socket = mock_get_socket(fd);
    if (!socket) {
        return -1; // Invalid socket
    }
    
    // For multicast sockets, periodically generate events
    if (socket->is_multicast && call_count % 10 == 0) {
        char response[64];
        snprintf(response, sizeof(response), "MA %d %d %d", 
                 state.zoneId, state.currentSection, 60);
        
        size_t resp_len = strlen(response);
        memcpy(buffer, response, resp_len < len ? resp_len : len);
        
        ESP_LOGI(TAG, "[MOCK] Generated multicast message: %s", response);
        return resp_len < len ? resp_len : len;
    }
    
    // For ZC connections, generate different responses
    if (!socket->is_multicast) {
        const char* response;
        
        if (call_count <= 1) {
            response = "REGISTER_OK Train registered successfully";
        } else if (call_count % 5 == 0) {
            response = "SPEED_LIMIT 80";
        } else if (call_count % 7 == 0) {
            response = "SPEED_LIMIT 30";
        } else {
            return -1; // No data available most of the time
        }
        
        size_t resp_len = strlen(response);
        memcpy(buffer, response, resp_len < len ? resp_len : len);
        
        ESP_LOGI(TAG, "[MOCK] Generated ZC message: %s", response);
        return resp_len < len ? resp_len : len;
    }
    
    return -1; // No data available
}

#endif  // RUNNING_IN_QEMU

// Initialize WiFi as station
void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // Wait for WiFi connection
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

// Train control functions adapted to use ESP-IDF
void initializeTrain(int trainId, int zoneId, int initialSection) {
    state.id = trainId;
    state.currentSection = initialSection;
    state.currentSpeed = 0;
    state.targetSpeed = 0;
    state.zoneId = zoneId;

    ESP_LOGI(TAG, "Train %d initializing in Zone %d, Section %d", 
             trainId, zoneId, initialSection);
}

void adjustSpeed() {
    static int log_counter = 0;
    
    // Simple simulation of train speed adjustment
    if (state.currentSpeed < state.targetSpeed) {
        state.currentSpeed += 5;
        if (state.currentSpeed > state.targetSpeed) {
            state.currentSpeed = state.targetSpeed;
        }
    } else if (state.currentSpeed > state.targetSpeed) {
        state.currentSpeed -= 10;
        if (state.currentSpeed < state.targetSpeed) {
            state.currentSpeed = state.targetSpeed;
        }
    }

    if (++log_counter >= 10) {
        ESP_LOGI(TAG, "Current speed: %d km/h, Target: %d km/h", 
                state.currentSpeed, state.targetSpeed);
        log_counter = 0;
    }
}

// Add other missing functions from your original code:
void joinMulticastGroup(int section) {
    // Leave current multicast group
    struct ip_mreq mreq;
    char oldGroup[20];
    sprintf(oldGroup, "239.0.%d.%d", state.zoneId, state.currentSection);
    mreq.imr_multiaddr.s_addr = inet_addr(oldGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;

    setsockopt(multicastSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    ESP_LOGI(TAG, "Left multicast group: %s", oldGroup);

    // Join new multicast group
    char newGroup[20];
    sprintf(newGroup, "239.0.%d.%d", state.zoneId, section);
    mreq.imr_multiaddr.s_addr = inet_addr(newGroup);

    if (setsockopt(multicastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "Joining new multicast group failed: errno %d", errno);
    } else {
        ESP_LOGI(TAG, "Switched to multicast group: %s", newGroup);
    }
}

void updatePosition(int newSection) {
    if (newSection != state.currentSection) {
        // Send position update to zone controller
        char updateMsg[BUFFER_SIZE];
        sprintf(updateMsg, "POSITION_UPDATE %d %d", state.id, newSection);
        send(zoneControllerSocket, updateMsg, strlen(updateMsg), 0);

        // Join new multicast group
        joinMulticastGroup(newSection);

        ESP_LOGI(TAG, "Position updated: Section %d -> %d", state.currentSection, newSection);
        state.currentSection = newSection;
    }
}

void processMovementAuthority(char *message) {
    int maZoneId, section, speed;
    if (sscanf(message, "MA %d %d %d", &maZoneId, &section, &speed) == 3) {
        if (maZoneId == state.zoneId && section == state.currentSection) {
            ESP_LOGI(TAG, "Received new movement authority: Speed %d km/h", speed);
            state.targetSpeed = speed;
        }
    }
}

int connectToZoneController(const char *zcIP) {
#if RUNNING_IN_QEMU
    // In QEMU mode, just create a mock socket
    int sock = mock_socket_create();
    if (sock < 0) {
        ESP_LOGE(TAG, "[MOCK] Failed to create socket");
        return -1;
    }
    
    ESP_LOGI(TAG, "[MOCK] Connected to Zone Controller at %s:%d", zcIP, ZC_PORT + state.zoneId);
    
    // Generate a successful registration response
    char buffer[BUFFER_SIZE];
    strcpy(buffer, "REGISTER_OK Train registered successfully");
    ESP_LOGI(TAG, "[MOCK] Zone Controller response: %s", buffer);
    
    return sock;
#else
    // Original implementation for real hardware
    struct sockaddr_in zcAddr;
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    if (sock < 0) {
        ESP_LOGE(TAG, "Socket creation failed: errno %d", errno);
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ESP_LOGE(TAG, "setsockopt failed: errno %d", errno);
        close(sock);
        return -1;
    }

    memset(&zcAddr, 0, sizeof(zcAddr));
    zcAddr.sin_family = AF_INET;
    zcAddr.sin_addr.s_addr = inet_addr(zcIP);
    zcAddr.sin_port = htons(ZC_PORT + state.zoneId);

    ESP_LOGI(TAG, "Connecting to Zone Controller at %s:%d", 
             zcIP, ZC_PORT + state.zoneId);
             
    if (connect(sock, (struct sockaddr *)&zcAddr, sizeof(zcAddr)) < 0) {
        ESP_LOGE(TAG, "Connection failed: errno %d", errno);
        close(sock);
        return -1;
    }

    // Register with Zone Controller
    char registerMsg[BUFFER_SIZE];
    sprintf(registerMsg, "REGISTER_TRAIN %d %d", state.id, state.currentSection);
    
    if (send(sock, registerMsg, strlen(registerMsg), 0) < 0) {
        ESP_LOGE(TAG, "Send failed: errno %d", errno);
        close(sock);
        return -1;
    }

    // Wait for confirmation
    char buffer[BUFFER_SIZE];
    int bytesRead = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    
    if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        ESP_LOGI(TAG, "Zone Controller response: %s", buffer);
    } else {
        ESP_LOGE(TAG, "Receive failed: errno %d", errno);
        close(sock);
        return -1;
    }

    return sock;
#endif
}

void setupMulticastListener() {
#if RUNNING_IN_QEMU
    // In QEMU mode, just create a mock multicast socket
    multicastSocket = mock_socket_create();
    if (multicastSocket < 0) {
        ESP_LOGE(TAG, "[MOCK] Failed to create multicast socket");
        return;
    }
    
    // Mark this socket as multicast
    mock_set_multicast(multicastSocket, state.currentSection, MULTICAST_PORT);
    
    ESP_LOGI(TAG, "[MOCK] Multicast listener set up for section %d", state.currentSection);
#else
    // Original implementation for real hardware
    struct sockaddr_in localAddr;
    multicastSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    
    if (multicastSocket < 0) {
        ESP_LOGE(TAG, "Multicast socket creation failed: errno %d", errno);
        return;
    }

    int reuse = 1;
    if (setsockopt(multicastSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        ESP_LOGE(TAG, "Multicast setsockopt failed: errno %d", errno);
        close(multicastSocket);
        multicastSocket = -1;
        return;
    }

    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(MULTICAST_PORT);

    if (bind(multicastSocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        ESP_LOGE(TAG, "Multicast bind failed: errno %d", errno);
        close(multicastSocket);
        multicastSocket = -1;
        return;
    }

    // Join multicast group for current section
    struct ip_mreq mreq;
    char multicastGroup[20];
    sprintf(multicastGroup, "239.0.%d.%d", state.zoneId, state.currentSection);
    mreq.imr_multiaddr.s_addr = inet_addr(multicastGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(multicastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        ESP_LOGE(TAG, "Joining multicast group failed: errno %d", errno);
        close(multicastSocket);
        multicastSocket = -1;
        return;
    }

    ESP_LOGI(TAG, "Joined multicast group: %s", multicastGroup);
#endif
}

// Implement other train control functions (joinMulticastGroup, updatePosition, adjustSpeed, etc.)
// similar to the original but using ESP_LOG functions instead of printf

// Train control task
#include "esp_task_wdt.h"
void train_control_task(void *pvParameters) {
    ESP_LOGI(TAG, "Train control task started");
    
    if (pvParameters == NULL) {
        ESP_LOGE(TAG, "No parameters provided to train_control_task");
        vTaskDelete(NULL);
        return;
    }
    
    // Make a copy of the parameters to ensure we're not accessing stack memory
    char params_copy[64];
    strncpy(params_copy, (char *)pvParameters, sizeof(params_copy) - 1);
    params_copy[sizeof(params_copy) - 1] = '\0';
    
    ESP_LOGI(TAG, "Parsing parameters: %s", params_copy);
    
    char *argv[5] = {NULL};
    int argc = 0;
    
    char *token = strtok(params_copy, " ");
    while (token != NULL && argc < 5) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    if (argc < 4) {
        ESP_LOGE(TAG, "Not enough parameters. Need: <train_id> <zone_id> <initial_section> <zc_ip>");
        vTaskDelete(NULL);
        return;
    }

    int trainId = 0, zoneId = 0, initialSection = 0;
    char *zcIP = NULL;
    
    trainId = atoi(argv[0]);
    if (trainId <= 0) {
        ESP_LOGE(TAG, "Invalid train ID: %s", argv[0]);
        vTaskDelete(NULL);
        return;
    }
    
    zoneId = atoi(argv[1]);
    if (zoneId <= 0) {
        ESP_LOGE(TAG, "Invalid zone ID: %s", argv[1]);
        vTaskDelete(NULL);
        return;
    }
    
    initialSection = atoi(argv[2]);
    if (initialSection <= 0) {
        ESP_LOGE(TAG, "Invalid initial section: %s", argv[2]);
        vTaskDelete(NULL);
        return;
    }
    
    zcIP = argv[3];
    if (zcIP == NULL || strlen(zcIP) < 7) {
        ESP_LOGE(TAG, "Invalid ZC IP address");
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Parameters parsed successfully. Train: %d, Zone: %d, Section: %d, ZC: %s",
             trainId, zoneId, initialSection, zcIP);

    // Initialize train system
    initializeTrain(trainId, zoneId, initialSection);
    zoneControllerSocket = connectToZoneController(zcIP);
    
    if (zoneControllerSocket < 0) {
        ESP_LOGE(TAG, "Failed to connect to Zone Controller");
        vTaskDelete(NULL);
        return;
    }
    
    setupMulticastListener();
    
    if (multicastSocket < 0) {
        ESP_LOGE(TAG, "Failed to set up multicast listener");
        close(zoneControllerSocket);
        vTaskDelete(NULL);
        return;
    }

    // Main control loop
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    fd_set readfds;
    
#if !RUNNING_IN_QEMU
    struct timeval tv;
    int maxfd = (zoneControllerSocket > multicastSocket) ? zoneControllerSocket : multicastSocket;
#endif

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(zoneControllerSocket, &readfds);
        FD_SET(multicastSocket, &readfds);

        esp_task_wdt_reset();
#if !RUNNING_IN_QEMU
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
#else
        // Simplified select for QEMU - don't actually use select
        static int call_count = 0;
        call_count++;
        
        // Every 7th iteration, simulate data on ZC socket
        bool has_zc_data = (call_count % 7 == 0);
        
        // Every 13th iteration, simulate data on multicast socket
        bool has_multicast_data = (call_count % 13 == 0);
        
        // Clear and set appropriate bits
        FD_ZERO(&readfds);
        if (has_zc_data) FD_SET(zoneControllerSocket, &readfds);
        if (has_multicast_data) FD_SET(multicastSocket, &readfds);
        
        // Simulated activity
        int activity = has_zc_data || has_multicast_data ? 1 : 0;
        
        // Don't block for long
        vTaskDelay(10 / portTICK_PERIOD_MS);
#endif

        if (activity < 0) {
            ESP_LOGE(TAG, "Select error: errno %d", errno);
            continue;
        }

        // Handle Zone Controller messages
        if (FD_ISSET(zoneControllerSocket, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytesRead;
            
#if RUNNING_IN_QEMU
            bytesRead = mock_generate_response(zoneControllerSocket, buffer, BUFFER_SIZE - 1);
#else
            bytesRead = recv(zoneControllerSocket, buffer, BUFFER_SIZE - 1, 0);
#endif

            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                ESP_LOGI(TAG, "Message from Zone Controller: %s", buffer);
                
                // Process speed limit updates
                int speedLimit;
                if (sscanf(buffer, "SPEED_LIMIT %d", &speedLimit) == 1) {
                    ESP_LOGI(TAG, "Received speed limit: %d km/h", speedLimit);
                    state.targetSpeed = speedLimit;
                }
            } else if (bytesRead < 0) {
                // In mock mode, negative return just means no data available
#if RUNNING_IN_QEMU
                // Do nothing, just no data available
#else
                ESP_LOGI(TAG, "Zone Controller disconnected. Stopping train...");
                state.targetSpeed = 0;
                // In a real system, would trigger emergency braking
                break;
#endif
            }
        }

        // Handle multicast messages
        if (FD_ISSET(multicastSocket, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytesRead;
            
#if RUNNING_IN_QEMU
            bytesRead = mock_generate_response(multicastSocket, buffer, BUFFER_SIZE - 1);
#else
            struct sockaddr_in senderAddr;
            socklen_t addrLen = sizeof(senderAddr);
            bytesRead = recvfrom(multicastSocket, buffer, BUFFER_SIZE - 1, 0,
                             (struct sockaddr *)&senderAddr, &addrLen);
#endif

            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                processMovementAuthority(buffer);
            }
        }

        // Simulate train behavior
        adjustSpeed();
        
        // Small delay to prevent CPU hogging
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }

    // Cleanup (never reached in this implementation)
    close(zoneControllerSocket);
    close(multicastSocket);
    vTaskDelete(NULL);
}

// Initial console log level (can be changed by user)
static esp_log_level_t console_log_level = ESP_LOG_INFO;

// Custom log function to filter logs when console is active
static int console_log_filter(const char *fmt, va_list args) {
    static bool in_logging = false;
    
    // Prevent recusion
    if (in_logging) return 0;
    
    in_logging = true;
    if (!console_active) {
        vprintf(fmt, args);
    }
    in_logging = false;
    
    return 0;
}

// Help command handler
static int cmd_help(int argc, char **argv) {
    printf("Available commands:\n");
    
    printf("  help          - Display this help message\n");
    printf("  show          - Show current configuration\n");
    printf("  set           - Set configuration parameter (train_id, zone_id, section, zc_ip)\n");
    printf("  save          - Save current configuration to NVS\n");
    printf("  reset         - Reset configuration to defaults\n");
    printf("  log_level     - Set logging level (0-5: none, error, warn, info, debug, verbose)\n");
    
    return 0;
}

// Show command handler
static int cmd_show(int argc, char **argv) {
    int train_id, zone_id, section;
    char zc_ip[32];
    load_train_config(&train_id, &zone_id, &section, zc_ip, sizeof(zc_ip));
    
    printf("Current configuration:\n");
    printf("  Train ID: %d\n", train_id);
    printf("  Zone ID: %d\n", zone_id);
    printf("  Section: %d\n", section);
    printf("  ZC IP: %s\n", zc_ip);
    
    printf("  Log level: %d (%s)\n", console_log_level, 
           console_log_level == ESP_LOG_NONE ? "none" :
           console_log_level == ESP_LOG_ERROR ? "error" :
           console_log_level == ESP_LOG_WARN ? "warning" :
           console_log_level == ESP_LOG_INFO ? "info" :
           console_log_level == ESP_LOG_DEBUG ? "debug" :
           console_log_level == ESP_LOG_VERBOSE ? "verbose" : "unknown");
    
    return 0;
}

// Set command handler
static int cmd_set(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: set <param> <value>\n");
        printf("Parameters: train_id, zone_id, section, zc_ip\n");
        return 1;
    }
    
    const char *param = argv[1];
    const char *value = argv[2];
    
    // Get current configuration
    int train_id, zone_id, section;
    char zc_ip[32];
    load_train_config(&train_id, &zone_id, &section, zc_ip, sizeof(zc_ip));
    
    // Update the requested parameter
    if (strcmp(param, "train_id") == 0) {
        int new_id = atoi(value);
        if (new_id <= 0) {
            printf("Error: train_id must be positive\n");
            return 1;
        }
        train_id = new_id;
        printf("Train ID set to %d\n", train_id);
    } else if (strcmp(param, "zone_id") == 0) {
        int new_id = atoi(value);
        if (new_id <= 0) {
            printf("Error: zone_id must be positive\n");
            return 1;
        }
        zone_id = new_id;
        printf("Zone ID set to %d\n", zone_id);
    } else if (strcmp(param, "section") == 0) {
        int new_section = atoi(value);
        if (new_section <= 0) {
            printf("Error: section must be positive\n");
            return 1;
        }
        section = new_section;
        printf("Section set to %d\n", section);
    } else if (strcmp(param, "zc_ip") == 0) {
        // Validate IP format (basic check)
        if (strlen(value) < 7 || strlen(value) > 15) {
            printf("Error: Invalid IP format\n");
            return 1;
        }
        strncpy(zc_ip, value, sizeof(zc_ip) - 1);
        zc_ip[sizeof(zc_ip) - 1] = '\0';
        printf("Zone Controller IP set to %s\n", zc_ip);
    } else {
        printf("Unknown parameter: %s\n", param);
        printf("Valid parameters: train_id, zone_id, section, zc_ip\n");
        return 1;
    }
    
    // Update in-memory configuration
    // Note: We don't save to NVS here, user must use 'save' command
    return 0;
}

// Save command handler
static int cmd_save(int argc, char **argv) {
    int train_id, zone_id, section;
    char zc_ip[32];
    load_train_config(&train_id, &zone_id, &section, zc_ip, sizeof(zc_ip));
    
    esp_err_t err = save_train_config(train_id, zone_id, section, zc_ip);
    if (err == ESP_OK) {
        printf("Configuration saved successfully\n");
        return 0;
    } else {
        printf("Error saving configuration: %s\n", esp_err_to_name(err));
        return 1;
    }
}

// Reset command handler
static int cmd_reset(int argc, char **argv) {
    esp_err_t err = save_train_config(DEFAULT_TRAIN_ID, DEFAULT_ZONE_ID, DEFAULT_SECTION, DEFAULT_ZC_IP);
    if (err == ESP_OK) {
        printf("Configuration reset to defaults:\n");
        printf("  Train ID: %d\n", DEFAULT_TRAIN_ID);
        printf("  Zone ID: %d\n", DEFAULT_ZONE_ID);
        printf("  Section: %d\n", DEFAULT_SECTION);
        printf("  ZC IP: %s\n", DEFAULT_ZC_IP);
        return 0;
    } else {
        printf("Error resetting configuration: %s\n", esp_err_to_name(err));
        return 1;
    }
}

// Log level command handler
static int cmd_log_level(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: log_level <level>\n");
        printf("Levels: 0=none, 1=error, 2=warn, 3=info, 4=debug, 5=verbose\n");
        return 1;
    }
    
    int level = atoi(argv[1]);
    if (level < 0 || level > 5) {
        printf("Error: Level must be between 0 and 5\n");
        return 1;
    }
    
    console_log_level = level;
    esp_log_level_set("*", level);
    
    printf("Log level set to %d (%s)\n", level,
           level == 0 ? "none" :
           level == 1 ? "error" :
           level == 2 ? "warning" :
           level == 3 ? "info" :
           level == 4 ? "debug" :
           "verbose");
    
    return 0;
}

// Register all console commands
void register_console_commands(void) {
    // Command: help
    const esp_console_cmd_t help_cmd = {
        .command = "help",
        .help = "Display list of available commands",
        .hint = NULL,
        .func = &cmd_help,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&help_cmd));

    // Command: show
    const esp_console_cmd_t show_cmd = {
        .command = "show",
        .help = "Show current configuration",
        .hint = NULL,
        .func = &cmd_show,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&show_cmd));

    // Command: set
    const esp_console_cmd_t set_cmd = {
        .command = "set",
        .help = "Set configuration parameter (train_id, zone_id, section, zc_ip)",
        .hint = "<param> <value>",
        .func = &cmd_set,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&set_cmd));

    // Command: save
    const esp_console_cmd_t save_cmd = {
        .command = "save",
        .help = "Save current configuration to NVS",
        .hint = NULL,
        .func = &cmd_save,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&save_cmd));

    // Command: reset
    const esp_console_cmd_t reset_cmd = {
        .command = "reset",
        .help = "Reset configuration to defaults",
        .hint = NULL,
        .func = &cmd_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_cmd));

    // Command: log_level
    const esp_console_cmd_t log_cmd = {
        .command = "log_level",
        .help = "Set logging level (0-5: none, error, warn, info, debug, verbose)",
        .hint = "<level>",
        .func = &cmd_log_level,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&log_cmd));
}

// Initialize the console system
void init_console(void) {
    // Drain stdout before reconfiguring it
    fflush(stdout);
    fsync(fileno(stdout));

    // Disable buffering on stdin and stdout
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    // Initialize VFS console driver - use the deprecated but available functions
    esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    esp_vfs_dev_uart_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    esp_vfs_dev_uart_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);

    // Initialize the console
    esp_console_config_t console_config = {
        .max_cmdline_length = 256,
        .max_cmdline_args = 8,
        .hint_color = 36  // Cyan color code directly
    };
    ESP_ERROR_CHECK(esp_console_init(&console_config));

    // Configure linenoise
    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(20);
    linenoiseAllowEmpty(false);

    // Set up a custom logging hook
    esp_log_set_vprintf(console_log_filter);

    // Register commands
    register_console_commands();

    // Print welcome message
    printf("\n"
           "==================================================\n"
           "               Train Control Console               \n"
           "==================================================\n"
           "Type 'help' to view available commands\n\n");
}

// Console task
void console_task(void *pvParameters) {
    // Initialize console
    init_console();
    
    // Main loop
    const char* prompt = LOG_COLOR_I "> " LOG_RESET_COLOR;
    while (true) {
        // Indicate console is active (suppress logs)
        console_active = true;
        
        // Get a line with linenoise
        char* line = linenoise(prompt);
        
        // Logs can show again
        console_active = false;
        
        // Handle empty line or EOF
        if (line == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        // If line is not empty, add to history and process
        if (strlen(line) > 0) {
            linenoiseHistoryAdd(line);
            
            // Process command
            int ret;
            esp_err_t err = esp_console_run(line, &ret);
            if (err == ESP_ERR_NOT_FOUND) {
                printf("Unknown command. Type 'help' for a list of commands.\n");
            } else if (err == ESP_ERR_INVALID_ARG) {
                printf("Invalid arguments\n");
            } else if (err == ESP_OK && ret != ESP_OK) {
                printf("Command returned error code: %d\n", ret);
            }
        }
        
        // Free the line buffer
        linenoiseFree(line);
        
        // Small delay
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void console_task_qemu(void *pvParameters) {
    printf("\n=== QEMU Simple Console - Train Control ===\n");
    printf("(Note: Full console functionality not available in QEMU)\n");
    
    // Just print status periodically
    while (true) {
        printf("\nTrain Status: ID=%d, Zone=%d, Section=%d, Speed=%d/%d km/h\n",
               state.id, state.zoneId, state.currentSection, 
               state.currentSpeed, state.targetSpeed);
        
        // Long delay to avoid flooding output
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Load configuration
    int train_id, zone_id, section;
    char zc_ip[32];
    load_train_config(&train_id, &zone_id, &section, zc_ip, sizeof(zc_ip));

#if RUNNING_IN_QEMU
    // Use mock WiFi implementation for QEMU
    ESP_LOGI(TAG, "Running in QEMU - using simulated WiFi");
    wifi_init_sta_qemu();
    
    // Initialize mock sockets
    mock_sockets_init();
#else
    // Real hardware - initialize WiFi normally
    ESP_LOGI(TAG, "Connecting to WiFi...");
    wifi_init_sta();
#endif

    ESP_LOGI(TAG, "WiFi connected");

    // Create a static buffer for task parameters
    static char params[64];
    snprintf(params, sizeof(params), "%d %d %d %s", train_id, zone_id, section, zc_ip);
    
    // Start the train control task
    ESP_LOGI(TAG, "Starting train control task with parameters: %s", params);
    xTaskCreate(train_control_task, "train_control", 8192, params, 5, NULL);
    
    // Brief delay to ensure train task is running
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Create appropriate console task based on environment
#if RUNNING_IN_QEMU
    xTaskCreate(console_task_qemu, "console", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Started simplified QEMU console");
#else
    xTaskCreate(console_task, "console", 16384, NULL, 4, NULL);
    ESP_LOGI(TAG, "Started interactive console");
#endif
}
