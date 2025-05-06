#include "raylib.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

#define MAX_LOGS 20
#define MAX_LOG_LENGTH 100
#define MAX_TRAINS 5
#define MAX_SIGNALS 10
#define MAX_SWITCHES 5
#define MAX_ZONES 3
#define MAX_SECTIONS 30
#define MAX_STATIONS 6
#define TRAIN_SIZE 10
#define STATION_WIDTH 40
#define STATION_HEIGHT 20
#define MAX_PROCESSES 20
#define BUFFER_SIZE 1024
#define POSITION_MULTICAST_PORT 8300

// Shared memory structure for system state
typedef struct {
    // Train state
    struct {
        int id;
        int zoneId;
        int section;
        float x;
        float y;
        int speed;
        int targetSpeed;
        int stationStopTime;
        int stationTimer;
        int atStation;
        int direction;  // 1 for forward, -1 for backward
        char color[20]; // Color name as string
    } trains[MAX_TRAINS];
    int trainCount;
    
    // Signal state
    struct {
        int id;
        int zoneId;
        int section;
        float x;
        float y;
        int state; // 0=RED, 1=YELLOW, 2=GREEN
    } signals[MAX_SIGNALS];
    int signalCount;
    
    // Switch state
    struct {
        int id;
        int zoneId;
        int section;
        float x;
        float y;
        int state; // 0=NORMAL, 1=REVERSE
    } switches[MAX_SWITCHES];
    int switchCount;
    
    // Log messages
    char logs[MAX_LOGS][MAX_LOG_LENGTH];
    int logCount;
    
    // Mutex
    pthread_mutex_t mutex;
} SharedState;

// Track segments for visualization
typedef struct {
    Vector2 start;
    Vector2 end;
    int zoneId;
    int section;
} TrackSegment;

// Station information
typedef struct {
    int id;
    Vector2 position;
    const char *name;
    int section;
    int stopTime; // in seconds
    Rectangle bounds;
} Station;

// Process management
typedef struct {
    char name[32];
    pid_t pid;
    int running;
} ProcessInfo;

// Global variables
TrackSegment trackSegments[MAX_SECTIONS];
Station stations[MAX_STATIONS];
int trackSegmentCount = 0;
int stationCount = 0;
ProcessInfo processes[MAX_PROCESSES];
int processCount = 0;
SharedState *sharedState;
int shmFd;
const char *shmName = "/cbtc_state";
int isCleanupDone = 0;  // Flag to prevent multiple cleanups
int positionMulticastSocket;
const char *positionMulticastGroup = "239.0.0.1";

// Function to initialize shared memory
void initSharedMemory() {
    // First try to remove any existing shared memory with this name
    shm_unlink(shmName);
    
    // Create or open shared memory
    shmFd = shm_open(shmName, O_CREAT | O_RDWR, 0666);
    if (shmFd == -1) {
        perror("shm_open failed");
        exit(EXIT_FAILURE);
    }
    
    // Set the size of the shared memory segment
    if (ftruncate(shmFd, sizeof(SharedState)) == -1) {
        perror("ftruncate failed");
        close(shmFd);
        shm_unlink(shmName);
        exit(EXIT_FAILURE);
    }
    
    // Map the shared memory segment
    sharedState = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, 
                       MAP_SHARED, shmFd, 0);
    if (sharedState == MAP_FAILED) {
        perror("mmap failed");
        close(shmFd);
        shm_unlink(shmName);
        exit(EXIT_FAILURE);
    }
    
    // Initialize shared memory
    memset(sharedState, 0, sizeof(SharedState));
    
    // Initialize mutex with shared attribute
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sharedState->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    
    printf("Shared memory initialized\n");
}

// Function to add a log to the shared state
void addLog(const char *message) {
    pthread_mutex_lock(&sharedState->mutex);
    
    if (sharedState->logCount >= MAX_LOGS) {
        // Shift logs up
        for (int i = 0; i < MAX_LOGS - 1; i++) {
            strcpy(sharedState->logs[i], sharedState->logs[i + 1]);
        }
        sharedState->logCount = MAX_LOGS - 1;
    }
    
    time_t now = time(NULL);
    struct tm *timeinfo = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
    
    snprintf(sharedState->logs[sharedState->logCount], MAX_LOG_LENGTH, 
             "[%s] %s", timestamp, message);
    sharedState->logCount++;
    
    pthread_mutex_unlock(&sharedState->mutex);
}

// Function to initialize track layout for visualization
void initializeTrackLayout() {
    // Define track segments (each segment is a section)
    // Main line
    for (int i = 0; i < 20; i++) {
        trackSegments[trackSegmentCount].start = (Vector2){100 + i * 40, 300};
        trackSegments[trackSegmentCount].end = (Vector2){140 + i * 40, 300};
        trackSegments[trackSegmentCount].zoneId = (i < 7) ? 1 : ((i < 14) ? 2 : 3);
        trackSegments[trackSegmentCount].section = i + 1;
        trackSegmentCount++;
    }
    
    // Branch line
    trackSegments[trackSegmentCount].start = (Vector2){420, 300};
    trackSegments[trackSegmentCount].end = (Vector2){420, 260};
    trackSegments[trackSegmentCount].zoneId = 2;
    trackSegments[trackSegmentCount].section = 21;
    trackSegmentCount++;
    
    trackSegments[trackSegmentCount].start = (Vector2){420, 260};
    trackSegments[trackSegmentCount].end = (Vector2){500, 260};
    trackSegments[trackSegmentCount].zoneId = 2;
    trackSegments[trackSegmentCount].section = 22;
    trackSegmentCount++;
    
    trackSegments[trackSegmentCount].start = (Vector2){500, 260};
    trackSegments[trackSegmentCount].end = (Vector2){580, 260};
    trackSegments[trackSegmentCount].zoneId = 2;
    trackSegments[trackSegmentCount].section = 23;
    trackSegmentCount++;
    
    trackSegments[trackSegmentCount].start = (Vector2){580, 260};
    trackSegments[trackSegmentCount].end = (Vector2){580, 300};
    trackSegments[trackSegmentCount].zoneId = 2;
    trackSegments[trackSegmentCount].section = 24;
    trackSegmentCount++;

    // Define stations
    stations[stationCount].id = 1;
    stations[stationCount].position = (Vector2){170, 280};
    stations[stationCount].name = "Westgate";
    stations[stationCount].section = 2;
    stations[stationCount].stopTime = 5;
    stations[stationCount].bounds = (Rectangle){
        stations[stationCount].position.x,
        stations[stationCount].position.y,
        STATION_WIDTH,
        STATION_HEIGHT
    };
    stationCount++;
    
    stations[stationCount].id = 2;
    stations[stationCount].position = (Vector2){370, 280};
    stations[stationCount].name = "Central";
    stations[stationCount].section = 7;
    stations[stationCount].stopTime = 10;
    stations[stationCount].bounds = (Rectangle){
        stations[stationCount].position.x,
        stations[stationCount].position.y,
        STATION_WIDTH,
        STATION_HEIGHT
    };
    stationCount++;
    
    stations[stationCount].id = 3;
    stations[stationCount].position = (Vector2){520, 240};
    stations[stationCount].name = "North";
    stations[stationCount].section = 23;
    stations[stationCount].stopTime = 7;
    stations[stationCount].bounds = (Rectangle){
        stations[stationCount].position.x,
        stations[stationCount].position.y,
        STATION_WIDTH,
        STATION_HEIGHT
    };
    stationCount++;
    
    stations[stationCount].id = 4;
    stations[stationCount].position = (Vector2){610, 280};
    stations[stationCount].name = "Eastgate";
    stations[stationCount].section = 13;
    stations[stationCount].stopTime = 5;
    stations[stationCount].bounds = (Rectangle){
        stations[stationCount].position.x,
        stations[stationCount].position.y,
        STATION_WIDTH,
        STATION_HEIGHT
    };
    stationCount++;
    
    stations[stationCount].id = 5;
    stations[stationCount].position = (Vector2){810, 280};
    stations[stationCount].name = "Terminal";
    stations[stationCount].section = 18;
    stations[stationCount].stopTime = 15;
    stations[stationCount].bounds = (Rectangle){
        stations[stationCount].position.x,
        stations[stationCount].position.y,
        STATION_WIDTH,
        STATION_HEIGHT
    };
    stationCount++;
}

// Initialize signal positions in shared memory
void initializeSignals() {
    pthread_mutex_lock(&sharedState->mutex);
    
    // Signal 1
    sharedState->signals[0].id = 1;
    sharedState->signals[0].zoneId = 1;
    sharedState->signals[0].section = 1;
    sharedState->signals[0].x = 130;
    sharedState->signals[0].y = 280;
    sharedState->signals[0].state = 2; // GREEN
    
    // Signal 2
    sharedState->signals[1].id = 2;
    sharedState->signals[1].zoneId = 1;
    sharedState->signals[1].section = 5;
    sharedState->signals[1].x = 290;
    sharedState->signals[1].y = 280;
    sharedState->signals[1].state = 2; // GREEN
    
    // Signal 3
    sharedState->signals[2].id = 3;
    sharedState->signals[2].zoneId = 2;
    sharedState->signals[2].section = 9;
    sharedState->signals[2].x = 450;
    sharedState->signals[2].y = 280;
    sharedState->signals[2].state = 2; // GREEN
    
    // Signal 4
    sharedState->signals[3].id = 4;
    sharedState->signals[3].zoneId = 2;
    sharedState->signals[3].section = 21;
    sharedState->signals[3].x = 400;
    sharedState->signals[3].y = 260;
    sharedState->signals[3].state = 1; // YELLOW
    
    // Signal 5
    sharedState->signals[4].id = 5;
    sharedState->signals[4].zoneId = 3;
    sharedState->signals[4].section = 15;
    sharedState->signals[4].x = 690;
    sharedState->signals[4].y = 280;
    sharedState->signals[4].state = 2; // GREEN
    
    sharedState->signalCount = 5;
    
    pthread_mutex_unlock(&sharedState->mutex);
}

// Initialize switch positions in shared memory
void initializeSwitches() {
    pthread_mutex_lock(&sharedState->mutex);
    
    // Switch 1
    sharedState->switches[0].id = 1;
    sharedState->switches[0].zoneId = 2;
    sharedState->switches[0].section = 8;
    sharedState->switches[0].x = 420;
    sharedState->switches[0].y = 300;
    sharedState->switches[0].state = 0; // NORMAL
    
    // Switch 2
    sharedState->switches[1].id = 2;
    sharedState->switches[1].zoneId = 2;
    sharedState->switches[1].section = 12;
    sharedState->switches[1].x = 580;
    sharedState->switches[1].y = 300;
    sharedState->switches[1].state = 0; // NORMAL
    
    sharedState->switchCount = 2;
    
    pthread_mutex_unlock(&sharedState->mutex);
}

// Initialize train positions in shared memory
void initializeTrains() {
    pthread_mutex_lock(&sharedState->mutex);
    
    // Train 1
    sharedState->trains[0].id = 101;
    sharedState->trains[0].zoneId = 1;
    sharedState->trains[0].section = 1;
    sharedState->trains[0].x = 110;
    sharedState->trains[0].y = 300;
    sharedState->trains[0].speed = 0;
    sharedState->trains[0].targetSpeed = 40;
    sharedState->trains[0].stationStopTime = 0;
    sharedState->trains[0].stationTimer = 0;
    sharedState->trains[0].atStation = 0;
    sharedState->trains[0].direction = 1;
    strcpy(sharedState->trains[0].color, "RED");
    
    // Train 2
    sharedState->trains[1].id = 102;
    sharedState->trains[1].zoneId = 2;
    sharedState->trains[1].section = 10;
    sharedState->trains[1].x = 490;
    sharedState->trains[1].y = 300;
    sharedState->trains[1].speed = 0;
    sharedState->trains[1].targetSpeed = 40;
    sharedState->trains[1].stationStopTime = 0;
    sharedState->trains[1].stationTimer = 0;
    sharedState->trains[1].atStation = 0;
    sharedState->trains[1].direction = 1;
    strcpy(sharedState->trains[1].color, "BLUE");
    
    // Train 3
    sharedState->trains[2].id = 103;
    sharedState->trains[2].zoneId = 3;
    sharedState->trains[2].section = 17;
    sharedState->trains[2].x = 770;
    sharedState->trains[2].y = 300;
    sharedState->trains[2].speed = 0;
    sharedState->trains[2].targetSpeed = 40;
    sharedState->trains[2].stationStopTime = 0;
    sharedState->trains[2].stationTimer = 0;
    sharedState->trains[2].atStation = 0;
    sharedState->trains[2].direction = 1;
    strcpy(sharedState->trains[2].color, "GREEN");
    
    sharedState->trainCount = 3;
    
    pthread_mutex_unlock(&sharedState->mutex);
}

// Add environment variables for component communication
void setupEnvironmentVars() {
    // Set environment variables for shared memory and ports
    setenv("CBTC_SHM_NAME", shmName, 1);
    setenv("CCS_PORT", "8000", 1);
    setenv("ZC_BASE_PORT", "8100", 1);
    setenv("MULTICAST_PORT", "8200", 1);
    setenv("POSITION_MULTICAST_PORT", "8300", 1);
    setenv("POSITION_MULTICAST_GROUP", "239.0.0.1", 1);
    setenv("SO_REUSEADDR", "1", 1);  // Signal to components to set SO_REUSEADDR
    setenv("CONFIG_FILE", "track_config.json", 1);
}

// Set up position multicast listener
void setupPositionMulticastListener() {
    positionMulticastSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (positionMulticastSocket < 0) {
        perror("Position multicast socket creation failed");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    if (setsockopt(positionMulticastSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
        perror("Setting SO_REUSEADDR failed");
        close(positionMulticastSocket);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in localAddr;
    memset(&localAddr, 0, sizeof(localAddr));
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = INADDR_ANY;
    localAddr.sin_port = htons(POSITION_MULTICAST_PORT);

    if (bind(positionMulticastSocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
        perror("Position multicast bind failed");
        close(positionMulticastSocket);
        exit(EXIT_FAILURE);
    }

    // Join multicast group for train positions
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(positionMulticastGroup);
    mreq.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(positionMulticastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("Joining position multicast group failed");
        close(positionMulticastSocket);
        exit(EXIT_FAILURE);
    }

    printf("Joined train position multicast group: %s\n", positionMulticastGroup);
}

// Process position updates from trains
void processPositionUpdate(char *message) {
    int trainId, direction, speed, section;
    float x, y;
    int atStation = 0;
    
    if (sscanf(message, "TRAIN_POSITION %d %f %f %d %d %d %d", 
               &trainId, &x, &y, &direction, &speed, &section, &atStation) >= 6) {
        
        pthread_mutex_lock(&sharedState->mutex);
        
        // Find the train in our shared state
        for (int i = 0; i < sharedState->trainCount; i++) {
            if (sharedState->trains[i].id == trainId) {
                // Update train position and movement data
                sharedState->trains[i].x = x;
                sharedState->trains[i].y = y;
                sharedState->trains[i].direction = direction;
                sharedState->trains[i].speed = speed;
                sharedState->trains[i].section = section;
                sharedState->trains[i].atStation = atStation;
                break;
            }
        }
        
        pthread_mutex_unlock(&sharedState->mutex);
    }
}

// Launch a CBTC component as a separate process
void launchProcess(const char *name, const char *executable, char *argv[]) {
    if (processCount >= MAX_PROCESSES) {
        fprintf(stderr, "Maximum number of processes reached\n");
        return;
    }
    
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("Fork failed");
        return;
    } else if (pid == 0) {
        // Child process
        execvp(executable, argv);
        perror("Exec failed");
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        processes[processCount].pid = pid;
        strncpy(processes[processCount].name, name, sizeof(processes[processCount].name) - 1);
        processes[processCount].running = 1;
        processCount++;
        
        printf("Launched %s (PID: %d)\n", name, pid);
        char logMsg[100];
        snprintf(logMsg, sizeof(logMsg), "Launched %s component (PID: %d)", name, pid);
        addLog(logMsg);
    }
}

// Launch system components in the correct order with delays
void launchComponents() {
    // First launch the Central Control System and wait for it to initialize
    char *ccsArgs[] = {"./central_control_system", NULL};
    launchProcess("Central Control System", "./central_control_system", ccsArgs);
    sleep(1);  // Give CCS time to start up
    
    // Launch Zone Controllers with delays
    for (int i = 1; i <= 3; i++) {
        char zoneId[8];
        sprintf(zoneId, "%d", i);
        char *zcArgs[] = {"./zone_controller", zoneId, "127.0.0.1", NULL};
        char name[32];
        sprintf(name, "Zone Controller %d", i);
        launchProcess(name, "./zone_controller", zcArgs);
        usleep(500000);  // 500ms delay between zone controller launches
    }
    
    sleep(1);  // Wait for zone controllers to connect to CCS
    
    // Launch Wayside Equipment with delays
    for (int i = 0; i < sharedState->signalCount; i++) {
        char id[8], type[8], zoneId[8], section[8];
        sprintf(id, "%d", sharedState->signals[i].id);
        sprintf(type, "0");  // 0 = signal
        sprintf(zoneId, "%d", sharedState->signals[i].zoneId);
        sprintf(section, "%d", sharedState->signals[i].section);
        
        char *signalArgs[] = {"./wayside_equipment", id, type, zoneId, section, "127.0.0.1", NULL};
        char name[32];
        sprintf(name, "Signal %d", sharedState->signals[i].id);
        launchProcess(name, "./wayside_equipment", signalArgs);
        usleep(200000);  // 200ms delay
    }
    
    for (int i = 0; i < sharedState->switchCount; i++) {
        char id[8], type[8], zoneId[8], section[8];
        sprintf(id, "%d", sharedState->switches[i].id);
        sprintf(type, "1");  // 1 = switch
        sprintf(zoneId, "%d", sharedState->switches[i].zoneId);
        sprintf(section, "%d", sharedState->switches[i].section);
        
        char *switchArgs[] = {"./wayside_equipment", id, type, zoneId, section, "127.0.0.1", NULL};
        char name[32];
        sprintf(name, "Switch %d", sharedState->switches[i].id);
        launchProcess(name, "./wayside_equipment", switchArgs);
        usleep(200000);  // 200ms delay
    }
    
    sleep(1);  // Wait for wayside equipment to connect
    
    // Finally, launch Trains
    for (int i = 0; i < sharedState->trainCount; i++) {
        char id[8], zoneId[8], section[8], initX[16], initY[16];
        sprintf(id, "%d", sharedState->trains[i].id);
        sprintf(zoneId, "%d", sharedState->trains[i].zoneId);
        sprintf(section, "%d", sharedState->trains[i].section);
        sprintf(initX, "%.1f", sharedState->trains[i].x);
        sprintf(initY, "%.1f", sharedState->trains[i].y);
        
        char *trainArgs[] = {"./train", id, zoneId, section, "127.0.0.1", initX, initY, NULL};
        char name[32];
        sprintf(name, "Train %d", sharedState->trains[i].id);
        launchProcess(name, "./train", trainArgs);
        usleep(300000);  // 300ms delay
    }
    
    // Add a final log message
    addLog("All CBTC components launched successfully");
    
    // // Wait for system to stabilize
    // sleep(2);
    // 
    // // Route Train 102 to North station
    // addLog("Routing Train 102 to North station");
    // system("echo 'route 102 23' | nc localhost 8000");
}

// Terminate all child processes with proper signal handling
void terminateProcesses() {
    // First ask nicely with SIGTERM
    for (int i = 0; i < processCount; i++) {
        if (processes[i].running) {
            printf("Sending SIGTERM to %s (PID: %d)\n", processes[i].name, processes[i].pid);
            kill(processes[i].pid, SIGTERM);
        }
    }
    
    // Give processes a chance to terminate gracefully
    sleep(1);
    
    // Now check which ones are still running and force quit with SIGKILL if needed
    for (int i = 0; i < processCount; i++) {
        if (processes[i].running) {
            int status;
            pid_t result = waitpid(processes[i].pid, &status, WNOHANG);
            
            if (result == 0) {
                // Process still running, force quit
                printf("Sending SIGKILL to %s (PID: %d)\n", processes[i].name, processes[i].pid);
                kill(processes[i].pid, SIGKILL);
                waitpid(processes[i].pid, &status, 0);
            }
            
            processes[i].running = 0;
        }
    }
    
    printf("All processes terminated\n");
}

// Clean up shared memory and ensure it's properly unlinked
void cleanupSharedMemory() {
    if (!isCleanupDone) {
        if (sharedState != MAP_FAILED) {
            pthread_mutex_destroy(&sharedState->mutex);
            munmap(sharedState, sizeof(SharedState));
        }
        
        if (shmFd >= 0) {
            close(shmFd);
        }
        
        shm_unlink(shmName);
        printf("Shared memory cleaned up\n");
        isCleanupDone = 1;
    }
}

// Signal handler for clean termination
void signalHandler(int sig) {
    printf("\nCaught signal %d. Cleaning up...\n", sig);
    terminateProcesses();
    cleanupSharedMemory();
    
    // Close raylib window if it's open
    if (IsWindowReady()) {
        CloseWindow();
    }
    
    exit(0);
}

// Main function
int main() {
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Initialize shared memory for component communication
    initSharedMemory();
    
    // Set up environment variables
    setupEnvironmentVars();
    
    // Set up position multicast listener
    setupPositionMulticastListener();
    
    // Initialize system state
    initializeSignals();
    initializeSwitches();
    initializeTrains();
    
    // Initialize track layout for visualization
    initializeTrackLayout();
    
    // Launch CBTC components in the correct order
    launchComponents();
    
    // Initialize window
    SetTraceLogLevel(LOG_ERROR);
    InitWindow(1000, 600, "CBTC Network Simulation");
    SetTargetFPS(60);
    
    // Add initial log
    addLog("CBTC System Orchestrator started");
    
    // Main render loop
    while (!WindowShouldClose()) {
        // Check for train position updates
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(positionMulticastSocket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 1000; // 1ms timeout for non-blocking check

        if (select(positionMulticastSocket + 1, &readfds, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(positionMulticastSocket, &readfds)) {
                char buffer[BUFFER_SIZE];
                struct sockaddr_in senderAddr;
                socklen_t addrLen = sizeof(senderAddr);
                int bytesRead = recvfrom(positionMulticastSocket, buffer, BUFFER_SIZE, 0,
                                  (struct sockaddr *)&senderAddr, &addrLen);
                if (bytesRead > 0) {
                    buffer[bytesRead] = '\0';
                    processPositionUpdate(buffer);
                }
            }
        }
        
        BeginDrawing();
        ClearBackground(RAYWHITE);
        
        // Lock shared state for reading
        pthread_mutex_lock(&sharedState->mutex);
        
        // Draw track segments
        for (int i = 0; i < trackSegmentCount; i++) {
            Color trackColor;
            switch(trackSegments[i].zoneId) {
                case 1: trackColor = (Color){200, 220, 255, 255}; break; // Light blue
                case 2: trackColor = (Color){220, 255, 220, 255}; break; // Light green
                case 3: trackColor = (Color){255, 220, 220, 255}; break; // Light red
                default: trackColor = LIGHTGRAY;
            }
            
            DrawLineEx(trackSegments[i].start, trackSegments[i].end, 6, trackColor);
            DrawLineEx(trackSegments[i].start, trackSegments[i].end, 2, BLACK);
            
            // Draw section number
            Vector2 midpoint = {
                (trackSegments[i].start.x + trackSegments[i].end.x) / 2,
                (trackSegments[i].start.y + trackSegments[i].end.y) / 2 + 15
            };
            char sectionText[10];
            sprintf(sectionText, "%d", trackSegments[i].section);
            DrawText(sectionText, midpoint.x - 5, midpoint.y, 16, DARKGRAY);
        }
        
        // Draw stations
        for (int i = 0; i < stationCount; i++) {
            DrawRectangleRec(stations[i].bounds, LIGHTGRAY);
            DrawRectangleLinesEx(stations[i].bounds, 2, BLACK);
            DrawText(stations[i].name, stations[i].position.x + 5, 
                   stations[i].position.y + 5, 10, BLACK);
        }
        
        // Draw signals
        for (int i = 0; i < sharedState->signalCount; i++) {
            Color signalColor;
            switch(sharedState->signals[i].state) {
                case 0: signalColor = RED; break;
                case 1: signalColor = YELLOW; break;
                case 2: signalColor = GREEN; break;
                default: signalColor = GRAY;
            }
            DrawCircle(sharedState->signals[i].x, sharedState->signals[i].y, 6, signalColor);
            DrawCircleLines(sharedState->signals[i].x, sharedState->signals[i].y, 6, BLACK);
        }
        
        // Draw switches
        for (int i = 0; i < sharedState->switchCount; i++) {
            float x = sharedState->switches[i].x;
            float y = sharedState->switches[i].y;
            Rectangle switchNormal = {x - 20, y - 10, 40, 20};
            Rectangle switchReverse = {x - 10, y - 20, 20, 40};
            
            if (sharedState->switches[i].state == 0) { // NORMAL
                DrawRectangleRec(switchNormal, DARKGREEN);
                DrawRectangleLinesEx(switchNormal, 1, BLACK);
                DrawRectangleRec(switchReverse, GRAY);
                DrawRectangleLinesEx(switchReverse, 1, DARKGRAY);
            } else { // REVERSE
                DrawRectangleRec(switchNormal, GRAY);
                DrawRectangleLinesEx(switchNormal, 1, DARKGRAY);
                DrawRectangleRec(switchReverse, DARKGREEN);
                DrawRectangleLinesEx(switchReverse, 1, BLACK);
            }
        }
        
        // Draw trains
        for (int i = 0; i < sharedState->trainCount; i++) {
            Color trainColor;
            if (strcmp(sharedState->trains[i].color, "RED") == 0)
                trainColor = RED;
            else if (strcmp(sharedState->trains[i].color, "BLUE") == 0)
                trainColor = BLUE;
            else if (strcmp(sharedState->trains[i].color, "GREEN") == 0)
                trainColor = GREEN;
            else
                trainColor = YELLOW;
                     
            DrawCircle(sharedState->trains[i].x, sharedState->trains[i].y, TRAIN_SIZE, trainColor);
                     
            // Draw a small direction indicator
            float dirX = sharedState->trains[i].direction * 8;
            DrawTriangle(
                (Vector2){sharedState->trains[i].x + dirX, sharedState->trains[i].y},
                (Vector2){sharedState->trains[i].x - dirX/2, sharedState->trains[i].y - 5},
                (Vector2){sharedState->trains[i].x - dirX/2, sharedState->trains[i].y + 5},
                trainColor
            );
            
            DrawCircleLines(sharedState->trains[i].x, sharedState->trains[i].y, TRAIN_SIZE, BLACK);
            
            char trainInfo[60];
            sprintf(trainInfo, "%d (%d km/h) %s %s", 
                    sharedState->trains[i].id, 
                    sharedState->trains[i].speed, 
                    sharedState->trains[i].direction == 1 ? "→" : "←",
                    sharedState->trains[i].atStation ? "STOPPED" : "");
            
            DrawText(trainInfo, sharedState->trains[i].x - 30, 
                   sharedState->trains[i].y - 25, 10, BLACK);
        }
        
        // Draw zone boundaries
        DrawLine(380, 200, 380, 400, GRAY);
        DrawLine(660, 200, 660, 400, GRAY);
        DrawText("ZONE 1", 200, 380, 20, DARKBLUE);
        DrawText("ZONE 2", 500, 380, 20, DARKGREEN);
        DrawText("ZONE 3", 780, 380, 20, MAROON);
        
        // Draw logs
        DrawRectangle(20, 420, 960, 160, LIGHTGRAY);
        DrawRectangleLines(20, 420, 960, 160, BLACK);
        DrawText("CBTC System Logs", 30, 425, 20, BLACK);
        
        for (int i = 0; i < sharedState->logCount; i++) {
            DrawText(sharedState->logs[i], 30, 450 + i * 20, 10, BLACK);
        }
        
        // Unlock shared state
        pthread_mutex_unlock(&sharedState->mutex);
        
        // Draw help text
        DrawText("Railway CBTC Simulation Orchestrator", 30, 30, 24, BLACK);
        DrawText("Running distributed CBTC components", 30, 60, 16, DARKGRAY);
        DrawText("Press ESC to exit and terminate all components", 30, 80, 16, DARKGRAY);
        
        // Process count display
        char procInfo[50];
        sprintf(procInfo, "Running components: %d", processCount);
        DrawText(procInfo, 780, 60, 16, DARKGRAY);
        
        EndDrawing();
    }
    
    // Clean up
    terminateProcesses();
    cleanupSharedMemory();
    CloseWindow();
    
    return 0;
}
