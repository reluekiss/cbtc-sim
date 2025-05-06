#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <math.h> // For fabs
#include <errno.h> // For errno and EINTR

#define BUFFER_SIZE 1024
#define ZC_PORT_ENV "ZC_BASE_PORT"
#define MULTICAST_PORT_ENV "MULTICAST_PORT"
#define POSITION_MULTICAST_PORT_ENV "POSITION_MULTICAST_PORT"
#define POSITION_MULTICAST_GROUP_ENV "POSITION_MULTICAST_GROUP"
#define POSITION_UPDATE_INTERVAL_MS 100 // Update 10 times per second

#define MAX_STATIONS_PER_TRAIN 10

// Simplified Station Info for the train, received from ZC
typedef struct {
    int id;
    int section;
    int stopTime;
    int isTerminus;
    char name[32];
} TrainStationInfo;

typedef struct {
    int id;
    int currentSection;     // Section ZC last told the train it's in, or train infers from MA
    int currentSpeed;
    int targetSpeed;        // Commanded by ZC
    int zoneId;
    float x;
    float y;
    int direction;          // 1 for positive X/negative Y (branch), -1 for negative X/positive Y (branch)
    struct timespec lastUpdateTime;
    int atStation;          // 0 = not at station, 1 = actively stopping/stopped
    int currentStationId;   // ID of the station it's currently at (if any, 0 otherwise)
    int stationTimer;       // Countdown timer for station stop (in update cycles)
    TrainStationInfo stations[MAX_STATIONS_PER_TRAIN]; // Info about stations relevant to this train
    int stationCount;
    int takingNorthRoute;   // Flag for train 102 special route, set by ZC
    char lastZcIP[16];      // Store ZC IP for potential reconnects/handoffs
} TrainState;

TrainState state;
int zoneControllerSocket = -1;
int movementAuthoritySocket = -1;
int positionBroadcastSocket = -1;

// Ports and group from environment
int zcPortBase;
int multicastPort;
int positionMulticastPort;
char positionMulticastGroup[20];


void initializeTrain(int trainId, int zoneId, int initialSection,
                     float initialX, float initialY, const char* zc_ip_arg) {
    state.id = trainId;
    state.currentSection = initialSection;
    state.currentSpeed = 0;
    state.targetSpeed = 0; // Wait for MA/Speed Limit
    state.zoneId = zoneId;
    state.x = initialX;
    state.y = initialY;
    state.direction = 1; // Default forward
    clock_gettime(CLOCK_MONOTONIC, &state.lastUpdateTime);
    state.atStation = 0;
    state.currentStationId = 0;
    state.stationTimer = 0;
    state.stationCount = 0;
    state.takingNorthRoute = 0;
    strncpy(state.lastZcIP, zc_ip_arg, sizeof(state.lastZcIP) - 1);
    state.lastZcIP[sizeof(state.lastZcIP) - 1] = '\0';


    char *zcPortBaseStr = getenv(ZC_PORT_ENV);
    char *multicastPortStr = getenv(MULTICAST_PORT_ENV);
    char *posMcPortStr = getenv(POSITION_MULTICAST_PORT_ENV);
    char *posMcGroupStr = getenv(POSITION_MULTICAST_GROUP_ENV);

    if (!zcPortBaseStr || !multicastPortStr || !posMcPortStr || !posMcGroupStr) {
        fprintf(stderr, "Train %d: Error: Missing env vars for ports/group.\n", state.id);
        exit(EXIT_FAILURE);
    }
    zcPortBase = atoi(zcPortBaseStr);
    multicastPort = atoi(multicastPortStr);
    positionMulticastPort = atoi(posMcPortStr);
    strncpy(positionMulticastGroup, posMcGroupStr, sizeof(positionMulticastGroup) - 1);
    positionMulticastGroup[sizeof(positionMulticastGroup)-1] = '\0';

    printf("Train %d initialized: Zone %d, Section %d, Pos (%.1f, %.1f), Dir %d, ZC IP %s\n",
           state.id, state.zoneId, state.currentSection, state.x, state.y, state.direction, state.lastZcIP);
}

int connectToZoneController() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Train: ZC Socket creation failed");
        return -1;
    }
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Train: ZC setsockopt(SO_REUSEADDR) failed"); close(sock); return -1;
    }

    struct sockaddr_in zcAddr;
    memset(&zcAddr, 0, sizeof(zcAddr));
    zcAddr.sin_family = AF_INET;
    zcAddr.sin_addr.s_addr = inet_addr(state.lastZcIP);
    zcAddr.sin_port = htons(zcPortBase + state.zoneId);

    if (connect(sock, (struct sockaddr *)&zcAddr, sizeof(zcAddr)) < 0) {
        char errBuf[100];
        sprintf(errBuf, "Train %d: Connection to ZC (Zone %d, IP %s, Port %d) failed", state.id, state.zoneId, state.lastZcIP, zcPortBase + state.zoneId);
        perror(errBuf);
        close(sock); return -1;
    }

    char registerMsg[BUFFER_SIZE];
    sprintf(registerMsg, "REGISTER_TRAIN %d %d", state.id, state.currentSection);
    if (send(sock, registerMsg, strlen(registerMsg), 0) < 0) {
        perror("Train: Failed to send registration to ZC"); close(sock); return -1;
    }

    char buffer[BUFFER_SIZE];
    int bytesRead = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        printf("Train %d: ZC Response: %s\n", state.id, buffer);
    } else {
        printf("Train %d: No ZC response on registration or conn closed.\n", state.id);
        close(sock); return -1;
    }
    return sock;
}

void setupMovementAuthorityListener() {
    movementAuthoritySocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (movementAuthoritySocket < 0) { perror("Train: MA socket creation failed"); exit(EXIT_FAILURE); }
    int reuse = 1;
    if (setsockopt(movementAuthoritySocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("Train: MA Setting SO_REUSEADDR failed"); close(movementAuthoritySocket); exit(EXIT_FAILURE);
    }
    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(multicastPort);
    if (bind(movementAuthoritySocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        perror("Train: MA bind failed"); close(movementAuthoritySocket); exit(EXIT_FAILURE);
    }
    struct ip_mreq mreq;
    char maMulticastGroup[32];
    sprintf(maMulticastGroup, "239.0.%d.0", state.zoneId); // Zone-wide MA, train filters by section in MA msg
    mreq.imr_multiaddr.s_addr = inet_addr(maMulticastGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(movementAuthoritySocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        printf("Train %d: Could not join MA multicast group %s. Relying on TCP for speed commands.\n", state.id, maMulticastGroup);
        // movementAuthoritySocket = -1; // Don't disable, just means no multicast MAs
    } else {
        printf("Train %d: Joined MA multicast group: %s\n", state.id, maMulticastGroup);
    }
}

void setupPositionBroadcastSocket() {
    positionBroadcastSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (positionBroadcastSocket < 0) { perror("Train: Position broadcast socket creation failed"); exit(EXIT_FAILURE); }
}

void broadcastPosition() {
    char message[BUFFER_SIZE];
    sprintf(message, "TRAIN_POSITION %d %.1f %.1f %d %d %d %d", state.id,
            state.x, state.y, state.direction, state.currentSpeed,
            state.currentSection, state.atStation);
    struct sockaddr_in groupAddr;
    memset(&groupAddr, 0, sizeof(groupAddr));
    groupAddr.sin_family = AF_INET;
    groupAddr.sin_addr.s_addr = inet_addr(positionMulticastGroup);
    groupAddr.sin_port = htons(positionMulticastPort);
    sendto(positionBroadcastSocket, message, strlen(message), 0, (struct sockaddr *)&groupAddr, sizeof(groupAddr));
}

void processStationInfo(const char *message) {
    int id, section, stopTime, isTerminus;
    char name[32];
    if (sscanf(message, "STATION_INFO %d %d %d %d %31s", &id, &section, &stopTime, &isTerminus, name) == 5) {
        if (state.stationCount < MAX_STATIONS_PER_TRAIN) {
            state.stations[state.stationCount].id = id;
            state.stations[state.stationCount].section = section;
            state.stations[state.stationCount].stopTime = stopTime;
            state.stations[state.stationCount].isTerminus = isTerminus;
            strncpy(state.stations[state.stationCount].name, name, sizeof(state.stations[state.stationCount].name) - 1);
            state.stations[state.stationCount].name[sizeof(state.stations[state.stationCount].name) - 1] = '\0';
            state.stationCount++;
            printf("Train %d: Received info for station %s (Section %d, Stop %ds, Terminus: %d)\n",
                   state.id, name, section, stopTime, isTerminus);
        }
    }
}

void updatePositionAndState() {
    struct timespec currentTime;
    clock_gettime(CLOCK_MONOTONIC, &currentTime);
    double elapsedSeconds = (currentTime.tv_sec - state.lastUpdateTime.tv_sec) +
                            (currentTime.tv_nsec - state.lastUpdateTime.tv_nsec) / 1e9;

    if (elapsedSeconds < (POSITION_UPDATE_INTERVAL_MS / 2000.0)) { // Avoid overly small dt
        return;
    }
    state.lastUpdateTime = currentTime; // Update time early

    // Station stop logic
    if (state.atStation && state.stationTimer > 0) {
        state.stationTimer--;
        if (state.stationTimer == 0) {
            printf("Train %d: Departing station %s (Section %d)\n", state.id,
                   state.stations[state.currentStationId -1].name, state.currentSection);
            state.atStation = 0;
            for (int i = 0; i < state.stationCount; ++i) {
                if (state.stations[i].id == state.currentStationId && state.stations[i].isTerminus) {
                    state.direction *= -1;
                    printf("Train %d: Reversed direction at terminus %s. New dir: %d\n",
                           state.id, state.stations[i].name, state.direction);
                    break;
                }
            }
            state.currentStationId = 0; // Clear current station
            state.targetSpeed = 20; // Default departure speed, ZC can override
        }
        broadcastPosition(); // Keep broadcasting even when stopped
        return; 
    }

    // Movement
    float distanceMoved = state.currentSpeed * 0.15 * elapsedSeconds;

    // Special routing for Train 102 to North Station (Section 23)
    // This is a simplified behavior triggered by ZC.
    if (state.id == 102 && state.takingNorthRoute) {
        // ZC should have set switch 1 to REVERSE (section 8 -> 21)
        // Train approaches section 8, ZC confirms switch, train proceeds to 21
        if (state.currentSection == 8 && state.x >= 418.0 && state.x <= 422.0 && state.direction == 1) { // Near switch point
            state.y -= distanceMoved; // Move "up" (decrease Y)
            if (state.y <= 260.0) state.y = 260.0;
            printf("Train 102 (North Route): Moving Y at switch 1. Y=%.1f\n", state.y);
        } else if (state.currentSection == 21) { // On vertical part of branch, moving "up"
             state.y -= distanceMoved;
             if (state.y <= 260.0) state.y = 260.0;
        } else if (state.currentSection == 22 || state.currentSection == 23) { // On horizontal part of branch
            state.x += distanceMoved * state.direction; // Assumes branch direction is positive X
        } else if (state.currentSection == 24) { // On vertical part of branch, moving "down"
            state.y += distanceMoved;
            if (state.y >= 300.0) {
                state.y = 300.0; // Back to main line Y
                state.takingNorthRoute = 0; // End of special route
                printf("Train 102: North route completed, back on main line Y.\n");
            }
        } else { // Default movement if not in a special branch segment but still on "north route" mode
            state.x += distanceMoved * state.direction;
        }
    } else { // Standard movement
        state.x += distanceMoved * state.direction;
    }

    // Simple boundary checks (train doesn't know "track end" without ZC info)
    if (state.x > 900.0 && state.direction == 1) state.x = 900.0;
    if (state.x < 100.0 && state.direction == -1) state.x = 100.0;


    // Check if arrived at a station (if ZC commanded a stop and train is in a station section)
    if (!state.atStation && state.targetSpeed == 0) {
        for (int i = 0; i < state.stationCount; ++i) {
            if (state.stations[i].section == state.currentSection) {
                float stationCenterX = 100.0f + (state.stations[i].section - 1) * 40.0f + 20.0f;
                float stationCenterY = 300.0f; // Default main line
                if (state.stations[i].section == 23) { // North Station
                    stationCenterX = 540.0f; // Approx X center of North station
                    stationCenterY = 260.0f; // Y of North station branch
                }

                if (fabs(state.x - stationCenterX) < 10.0 && fabs(state.y - stationCenterY) < 10.0) {
                    state.atStation = 1;
                    state.currentStationId = state.stations[i].id;
                    state.stationTimer = state.stations[i].stopTime * (int)(1000.0 / POSITION_UPDATE_INTERVAL_MS);
                    printf("Train %d: Arrived and stopping at station %s (Sec %d) for %d cycles.\n",
                           state.id, state.stations[i].name, state.currentSection, state.stationTimer);
                    state.currentSpeed = 0; // Ensure fully stopped
                    break;
                }
            }
        }
    }
    broadcastPosition();

    // Periodically send TCP update to ZC with current section (as train perceives it)
    static struct timespec lastTcpReportTime = {0, 0};
    if (currentTime.tv_sec - lastTcpReportTime.tv_sec >= 1) { // Report every 1 second
        char updateMsg[BUFFER_SIZE];
        // The train reports its *believed* section. ZC verifies/corrects.
        // For this simplified model, the orchestrator's shared memory is the source of truth for section.
        // The train sends its X,Y, and the ZC can use that.
        // Let's assume the train sends what it *thinks* its section is, based on ZC's last MA.
        sprintf(updateMsg, "CURRENT_POS_SECTION %d %d %.1f %.1f", state.id, state.currentSection, state.x, state.y);
        if (zoneControllerSocket != -1) {
            send(zoneControllerSocket, updateMsg, strlen(updateMsg), 0);
        }
        lastTcpReportTime = currentTime;
    }
}

void adjustSpeed() {
    if (state.atStation) {
        state.currentSpeed = 0;
        return;
    }
    if (state.currentSpeed < state.targetSpeed) {
        state.currentSpeed += 2; // Simplified acceleration
        if (state.currentSpeed > state.targetSpeed) state.currentSpeed = state.targetSpeed;
    } else if (state.currentSpeed > state.targetSpeed) {
        state.currentSpeed -= 5; // Simplified deceleration
        if (state.currentSpeed < 0) state.currentSpeed = 0;
        if (state.currentSpeed < state.targetSpeed) state.currentSpeed = state.targetSpeed;
    }
    if (state.targetSpeed == 0 && state.currentSpeed > 0 && state.currentSpeed <= 5) {
        state.currentSpeed = 0; // Ensure full stop if target is 0
    }
}

void processMovementAuthority(const char *message) {
    int maZoneId, maSection, maSpeed;
    if (sscanf(message, "MA %d %d %d", &maZoneId, &maSection, &maSpeed) == 3) {
        if (maZoneId == state.zoneId) { // Check if MA is for this train's zone
            // Train should only act on MA for its *current* section or *next intended* section
            // For simplicity, if ZC sends MA for a section, train assumes it's relevant if it's close
            // A more robust system: ZC sends MA for train_id, section_id
            if (state.targetSpeed != maSpeed) {
                 printf("Train %d: Received MA for Z%d S%d. New target speed: %d km/h (was %d). My section: S%d\n",
                   state.id, maZoneId, maSection, maSpeed, state.targetSpeed, state.currentSection);
            }
            state.targetSpeed = maSpeed;
            state.currentSection = maSection; // Assume MA implies current section
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 7) { // id, zone, section, zc_ip, x, y
        fprintf(stderr, "Usage: %s <train_id> <zone_id> <initial_section> <zc_ip> <initial_x> <initial_y>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    initializeTrain(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]), atof(argv[5]), atof(argv[6]), argv[4]);

    zoneControllerSocket = connectToZoneController();
    if (zoneControllerSocket == -1) {
        fprintf(stderr, "Train %d: Failed to connect to ZC. Exiting.\n", state.id);
        exit(EXIT_FAILURE);
    }
    setupMovementAuthorityListener();
    setupPositionBroadcastSocket();
    broadcastPosition(); // Initial broadcast

    fd_set readfds;
    struct timeval tv;
    printf("Train %d: Entering main loop.\n", state.id);

    while (1) {
        FD_ZERO(&readfds);
        if (zoneControllerSocket != -1) FD_SET(zoneControllerSocket, &readfds);
        if (movementAuthoritySocket != -1) FD_SET(movementAuthoritySocket, &readfds);
        
        int maxfd = zoneControllerSocket > movementAuthoritySocket ? zoneControllerSocket : movementAuthoritySocket;
        if (maxfd < 0 && movementAuthoritySocket > 0) maxfd = movementAuthoritySocket;
        else if (maxfd < 0 && zoneControllerSocket > 0) maxfd = zoneControllerSocket;
        else if (maxfd < 0) { /* Both closed, maybe sleep and retry? */ usleep(100000); continue; }


        tv.tv_sec = 0;
        tv.tv_usec = POSITION_UPDATE_INTERVAL_MS * 1000;

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

        if (activity < 0 && errno != EINTR) { perror("Train: select error"); break; }

        if (zoneControllerSocket != -1 && FD_ISSET(zoneControllerSocket, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytesRead = recv(zoneControllerSocket, buffer, BUFFER_SIZE - 1, 0);
            if (bytesRead <= 0) {
                printf("Train %d: ZC disconnected. Stopping. Attempting reconnect...\n", state.id);
                state.targetSpeed = 0; close(zoneControllerSocket); zoneControllerSocket = -1;
                sleep(2); // Wait before reconnect attempt
                zoneControllerSocket = connectToZoneController();
                if(zoneControllerSocket == -1) {printf("Train %d: Reconnect failed. Exiting loop.\n", state.id); break;}
            } else {
                buffer[bytesRead] = '\0';
                // printf("Train %d: TCP Msg from ZC: %s\n", state.id, buffer);
                if (strncmp(buffer, "STATION_INFO", 12) == 0) processStationInfo(buffer);
                else if (strncmp(buffer, "SPEED_LIMIT", 11) == 0) {
                    int speed, section_for_limit; // ZC might specify section for speed limit
                    if (sscanf(buffer, "SPEED_LIMIT %d %d", &section_for_limit, &speed) == 2) {
                        if (section_for_limit == state.currentSection) { // Apply if for current section
                           if(state.targetSpeed != speed) printf("Train %d: ZC SPEED_LIMIT %d for S%d (was %d).\n", state.id, speed, section_for_limit, state.targetSpeed);
                           state.targetSpeed = speed;
                        }
                    } else if (sscanf(buffer, "SPEED_LIMIT %d", &speed) == 1) { // Generic speed limit
                        if(state.targetSpeed != speed) printf("Train %d: ZC SPEED_LIMIT %d (was %d).\n", state.id, speed, state.targetSpeed);
                        state.targetSpeed = speed;
                    }
                } else if (strncmp(buffer, "REVERSE_DIRECTION", 17) == 0) {
                    state.direction *= -1;
                    printf("Train %d: ZC REVERSE_DIRECTION. New dir: %d\n", state.id, state.direction);
                } else if (strncmp(buffer, "ROUTE_TO_NORTH", 14) == 0 && state.id == 102) {
                    printf("Train 102: ZC Commanded North route.\n");
                    state.takingNorthRoute = 1;
                } else if (strncmp(buffer, "UPDATE_SECTION", 14) == 0) {
                    int new_sec;
                    if (sscanf(buffer, "UPDATE_SECTION %d", &new_sec) == 1) {
                        if (state.currentSection != new_sec) {
                            printf("Train %d: ZC updated section to %d (was %d)\n", state.id, new_sec, state.currentSection);
                            state.currentSection = new_sec;
                        }
                    }
                }
            }
        }

        if (movementAuthoritySocket != -1 && FD_ISSET(movementAuthoritySocket, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytesRead = recvfrom(movementAuthoritySocket, buffer, BUFFER_SIZE - 1, 0, NULL, NULL);
            if (bytesRead > 0) {
                buffer[bytesRead] = '\0';
                processMovementAuthority(buffer);
            }
        }
        
        adjustSpeed();
        updatePositionAndState();
    }

    printf("Train %d: Exiting.\n", state.id);
    if (zoneControllerSocket != -1) close(zoneControllerSocket);
    if (movementAuthoritySocket != -1) close(movementAuthoritySocket);
    if (positionBroadcastSocket != -1) close(positionBroadcastSocket);
    return 0;
}
