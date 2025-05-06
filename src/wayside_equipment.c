#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h> // For errno
#include <pthread.h> // For pthread_mutex_t if used directly (though orchestrator owns it)

#define BUFFER_SIZE 1024
#define ZC_PORT_ENV "ZC_BASE_PORT"
#define SHM_NAME_ENV "CBTC_SHM_NAME"

// --- Copied from orchestrator for SharedState access ---
#define MAX_TRAINS_SHM 5
#define MAX_SIGNALS_SHM 10
#define MAX_SWITCHES_SHM 5
#define MAX_LOGS_SHM 20
#define MAX_LOG_LENGTH_SHM 100

typedef struct {
    struct {
        int id; int zoneId; int section; float x; float y; int speed;
        int targetSpeed; int stationStopTime; int stationTimer;
        int atStation; int direction; char color[20];
    } trains[MAX_TRAINS_SHM];
    int trainCount;
    struct {
        int id; int zoneId; int section; float x; float y; int state;
    } signals[MAX_SIGNALS_SHM];
    int signalCount;
    struct {
        int id; int zoneId; int section; float x; float y; int state;
    } switches[MAX_SWITCHES_SHM];
    int switchCount;
    char logs[MAX_LOGS_SHM][MAX_LOG_LENGTH_SHM];
    int logCount;
    pthread_mutex_t mutex;
} SharedState;
// --- End of copied SharedState ---

typedef enum { SIGNAL_TYPE, SWITCH_TYPE } EquipmentType; // Renamed to avoid conflict

typedef struct {
    int id;
    EquipmentType type;
    int zoneId;
    int trackSection;
    int currentState; // 0,1,2 for signal; 0,1 for switch
} WaysideEquipment;

WaysideEquipment equipment;
int zoneControllerSocket = -1;
char zcIP[16];
int zcPortBase;

SharedState *sharedState_ptr = MAP_FAILED;
int shmFd = -1;

void initSharedMemoryAccess() {
    const char *shmName = getenv(SHM_NAME_ENV);
    if (!shmName) {
        fprintf(stderr, "Wayside %d: Error: CBTC_SHM_NAME env var not set.\n", equipment.id);
        return; // Operate without shared memory if name not found
    }

    shmFd = shm_open(shmName, O_RDWR, 0666);
    if (shmFd == -1) {
        //perror("Wayside: shm_open failed"); // Can be noisy if orchestrator hasn't created it yet
        return;
    }

    sharedState_ptr = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (sharedState_ptr == MAP_FAILED) {
        perror("Wayside: mmap failed");
        close(shmFd); shmFd = -1;
    } else {
        printf("Wayside %d: Shared memory mapped.\n", equipment.id);
    }
}

void cleanupSharedMemoryAccess() {
    if (sharedState_ptr != MAP_FAILED) {
        munmap(sharedState_ptr, sizeof(SharedState));
        sharedState_ptr = MAP_FAILED;
    }
    if (shmFd != -1) {
        close(shmFd); shmFd = -1;
    }
}

void updateSharedMemoryState() {
    if (sharedState_ptr == MAP_FAILED) {
        // Attempt to re-map if not available, could be orchestrator started later
        initSharedMemoryAccess(); 
        if (sharedState_ptr == MAP_FAILED) return;
    }

    // It's generally safer for only one process (orchestrator) to *write* to shared memory
    // after initialization, or to use the mutex strictly.
    // Here, wayside updates its own state in shared memory.
    pthread_mutex_lock(&sharedState_ptr->mutex);
    if (equipment.type == SIGNAL_TYPE) {
        for (int i = 0; i < sharedState_ptr->signalCount; ++i) {
            if (sharedState_ptr->signals[i].id == equipment.id) {
                sharedState_ptr->signals[i].state = equipment.currentState;
                break;
            }
        }
    } else if (equipment.type == SWITCH_TYPE) {
        for (int i = 0; i < sharedState_ptr->switchCount; ++i) {
            if (sharedState_ptr->switches[i].id == equipment.id) {
                sharedState_ptr->switches[i].state = equipment.currentState;
                break;
            }
        }
    }
    pthread_mutex_unlock(&sharedState_ptr->mutex);
}

void initializeEquipmentState(int id, EquipmentType type, int zoneId, int section) {
    equipment.id = id;
    equipment.type = type;
    equipment.zoneId = zoneId;
    equipment.trackSection = section;

    if (type == SIGNAL_TYPE) {
        equipment.currentState = 0; // Default RED
        printf("Wayside Signal %d init: Zone %d, Sec %d, State RED\n", id, zoneId, section);
    } else { // SWITCH_TYPE
        equipment.currentState = 0; // Default NORMAL
        printf("Wayside Switch %d init: Zone %d, Sec %d, State NORMAL\n", id, zoneId, section);
    }

    char* zcPortBaseStr = getenv(ZC_PORT_ENV);
    if (!zcPortBaseStr) {
        fprintf(stderr, "Wayside %d: Error: ZC_BASE_PORT env var not set.\n", equipment.id);
        exit(EXIT_FAILURE);
    }
    zcPortBase = atoi(zcPortBaseStr);
}

int connectToZoneController() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("Wayside: ZC Socket creation failed"); return -1; }
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Wayside: ZC setsockopt(SO_REUSEADDR) failed"); close(sock); return -1;
    }
    struct sockaddr_in zcAddr;
    memset(&zcAddr, 0, sizeof(zcAddr));
    zcAddr.sin_family = AF_INET;
    zcAddr.sin_addr.s_addr = inet_addr(zcIP);
    zcAddr.sin_port = htons(zcPortBase + equipment.zoneId);

    if (connect(sock, (struct sockaddr *)&zcAddr, sizeof(zcAddr)) < 0) {
        char errorMsg[128];
        sprintf(errorMsg, "Wayside %d: Connection to ZC (Zone %d, IP %s, Port %d) failed", equipment.id, equipment.zoneId, zcIP, zcPortBase + equipment.zoneId);
        perror(errorMsg);
        close(sock); return -1;
    }

    char registerMsg[BUFFER_SIZE];
    if (equipment.type == SIGNAL_TYPE) {
        sprintf(registerMsg, "REGISTER_SIGNAL %d %d", equipment.id, equipment.trackSection);
    } else {
        sprintf(registerMsg, "REGISTER_SWITCH %d %d", equipment.id, equipment.trackSection);
    }
    if(send(sock, registerMsg, strlen(registerMsg), 0) < 0) {
        perror("Wayside: Failed to send registration to ZC"); close(sock); return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytesRead = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        printf("Wayside %d: ZC Response: %s\n", equipment.id, buffer);
    } else {
        printf("Wayside %d: No ZC response on registration or conn closed.\n", equipment.id);
        close(sock); return -1;
    }
    return sock;
}

void processCommandFromZC(const char *command) {
    int recv_id, new_state_val;
    char statusMsg[BUFFER_SIZE];

    if (equipment.type == SIGNAL_TYPE) {
        if (sscanf(command, "SET_SIGNAL %d %d", &recv_id, &new_state_val) == 2) {
            if (recv_id == equipment.id) {
                if (new_state_val >= 0 && new_state_val <= 2) {
                    if (equipment.currentState != new_state_val) {
                        equipment.currentState = new_state_val;
                        printf("Wayside Signal %d: State changed to %s by ZC.\n", equipment.id,
                               new_state_val == 0 ? "RED" : (new_state_val == 1 ? "YELLOW" : "GREEN"));
                        updateSharedMemoryState();
                        sprintf(statusMsg, "SIGNAL_STATUS %d %d", equipment.id, equipment.currentState);
                        if(zoneControllerSocket != -1) send(zoneControllerSocket, statusMsg, strlen(statusMsg), 0);
                    }
                } else {
                    printf("Wayside Signal %d: Invalid state value %d from ZC.\n", equipment.id, new_state_val);
                }
            }
        }
    } else { // SWITCH_TYPE
        if (sscanf(command, "SET_SWITCH %d %d", &recv_id, &new_state_val) == 2) {
            if (recv_id == equipment.id) {
                if (new_state_val >= 0 && new_state_val <= 1) {
                     if (equipment.currentState != new_state_val) {
                        equipment.currentState = new_state_val;
                        printf("Wayside Switch %d: State changed to %s by ZC.\n", equipment.id,
                               new_state_val == 0 ? "NORMAL" : "REVERSE");
                        updateSharedMemoryState();
                        sprintf(statusMsg, "SWITCH_STATUS %d %d", equipment.id, equipment.currentState);
                        if(zoneControllerSocket != -1) send(zoneControllerSocket, statusMsg, strlen(statusMsg), 0);
                     }
                } else {
                    printf("Wayside Switch %d: Invalid state value %d from ZC.\n", equipment.id, new_state_val);
                }
            }
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <id> <type:0=signal,1=switch> <zone_id> <section> <zc_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    initializeEquipmentState(atoi(argv[1]), (atoi(argv[2]) == 0) ? SIGNAL_TYPE : SWITCH_TYPE, atoi(argv[3]), atoi(argv[4]));
    strncpy(zcIP, argv[5], sizeof(zcIP) - 1); zcIP[sizeof(zcIP)-1] = '\0';

    initSharedMemoryAccess();
    updateSharedMemoryState(); // Update SHM with initial state

    zoneControllerSocket = connectToZoneController();
    if (zoneControllerSocket == -1) {
        fprintf(stderr, "Wayside %d: Critical - Failed to connect to ZC. Exiting.\n", equipment.id);
        cleanupSharedMemoryAccess();
        exit(EXIT_FAILURE);
    }
    
    fd_set readfds;
    struct timeval tv;
    printf("Wayside %d: Entering main loop.\n", equipment.id);

    while (1) {
        FD_ZERO(&readfds);
        if (zoneControllerSocket != -1) FD_SET(zoneControllerSocket, &readfds);
        
        int maxfd = zoneControllerSocket;
        if (maxfd < 0) { // Socket closed, try to reconnect
            sleep(5);
            zoneControllerSocket = connectToZoneController();
            if (zoneControllerSocket == -1) {
                printf("Wayside %d: Reconnect failed. Will retry...\n", equipment.id);
                continue; // Skip select if no valid socket
            } else {
                 printf("Wayside %d: Reconnected to ZC.\n", equipment.id);
            }
        }


        tv.tv_sec = 5; tv.tv_usec = 0;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0 && errno != EINTR) { perror("Wayside: select error"); break; }

        if (zoneControllerSocket != -1 && FD_ISSET(zoneControllerSocket, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytesRead = recv(zoneControllerSocket, buffer, BUFFER_SIZE - 1, 0);
            if (bytesRead <= 0) {
                printf("Wayside %d: ZC disconnected or error. Closing socket.\n", equipment.id);
                close(zoneControllerSocket); zoneControllerSocket = -1;
            } else {
                buffer[bytesRead] = '\0';
                processCommandFromZC(buffer);
            }
        }
    }

    printf("Wayside %d: Exiting.\n", equipment.id);
    if (zoneControllerSocket != -1) close(zoneControllerSocket);
    cleanupSharedMemoryAccess();
    return 0;
}
