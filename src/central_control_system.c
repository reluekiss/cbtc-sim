#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <json-c/json.h>

#define MAX_ZONES 10
#define BUFFER_SIZE 1024
#define CCS_PORT 8000
#define CONFIG_FILE "track_config.json"

typedef struct {
  int id;
  int connected;
  struct sockaddr_in address;
  int socket;
} ZoneController;

// Track configuration
typedef struct {
  int id;
  int zone;
  int nextSections[5];
  int nextCount;
  int hasStation;
  char stationName[32];
  int hasSwitch;
  int switchId;
} TrackSection;

typedef struct {
  int id;
  int section;
  int stopTime;
  int isTerminus;
  char name[32];
} Station;

typedef struct {
  int id;
  int section;
  int normalNext;
  int reverseNext;
} Switch;

// Global variables
ZoneController zoneControllers[MAX_ZONES];
int zoneCount = 0;
TrackSection trackSections[100];
int sectionCount = 0;
Station stations[20];
int stationCount = 0;
Switch switches[10];
int switchCount = 0;

// Function to load track configuration
void loadTrackConfig() {
  struct json_object *parsed_json;
  struct json_object *sections;
  struct json_object *section;
  struct json_object *stations_arr;
  struct json_object *station;
  struct json_object *switches_arr;
  struct json_object *switch_obj;
  
  parsed_json = json_object_from_file(CONFIG_FILE);
  if (!parsed_json) {
    printf("Error loading config file: %s\n", CONFIG_FILE);
    printf("Using default configuration\n");
    return;
  }
  
  // Parse track sections
  json_object_object_get_ex(parsed_json, "track_sections", &sections);
  sectionCount = json_object_array_length(sections);
  
  for (int i = 0; i < sectionCount; i++) {
    section = json_object_array_get_idx(sections, i);
    
    struct json_object *id, *zone, *next_sections, *station, *switch_id;
    
    json_object_object_get_ex(section, "id", &id);
    json_object_object_get_ex(section, "zone", &zone);
    json_object_object_get_ex(section, "next_sections", &next_sections);
    
    trackSections[i].id = json_object_get_int(id);
    trackSections[i].zone = json_object_get_int(zone);
    
    // Get next sections
    int nextCount = json_object_array_length(next_sections);
    trackSections[i].nextCount = nextCount;
    for (int j = 0; j < nextCount; j++) {
      struct json_object *next = json_object_array_get_idx(next_sections, j);
      trackSections[i].nextSections[j] = json_object_get_int(next);
    }
    
    // Check if section has a station
    if (json_object_object_get_ex(section, "station", &station)) {
      trackSections[i].hasStation = 1;
      strncpy(trackSections[i].stationName, 
              json_object_get_string(station), 
              sizeof(trackSections[i].stationName) - 1);
    } else {
      trackSections[i].hasStation = 0;
    }
    
    // Check if section has a switch
    if (json_object_object_get_ex(section, "switch", &switch_id)) {
      trackSections[i].hasSwitch = 1;
      trackSections[i].switchId = json_object_get_int(switch_id);
    } else {
      trackSections[i].hasSwitch = 0;
    }
  }
  
  // Parse stations
  json_object_object_get_ex(parsed_json, "stations", &stations_arr);
  stationCount = json_object_array_length(stations_arr);
  
  for (int i = 0; i < stationCount; i++) {
    station = json_object_array_get_idx(stations_arr, i);
    
    struct json_object *name, *section, *stop_time, *terminus;
    
    json_object_object_get_ex(station, "name", &name);
    json_object_object_get_ex(station, "section", &section);
    json_object_object_get_ex(station, "stop_time", &stop_time);
    json_object_object_get_ex(station, "terminus", &terminus);
    
    stations[i].id = i + 1;
    stations[i].section = json_object_get_int(section);
    stations[i].stopTime = json_object_get_int(stop_time);
    stations[i].isTerminus = json_object_get_boolean(terminus);
    strncpy(stations[i].name, json_object_get_string(name), sizeof(stations[i].name) - 1);
  }
  
  // Parse switches
  json_object_object_get_ex(parsed_json, "switches", &switches_arr);
  switchCount = json_object_array_length(switches_arr);
  
  for (int i = 0; i < switchCount; i++) {
    switch_obj = json_object_array_get_idx(switches_arr, i);
    
    struct json_object *id, *section, *normal_next, *reverse_next;
    
    json_object_object_get_ex(switch_obj, "id", &id);
    json_object_object_get_ex(switch_obj, "section", &section);
    json_object_object_get_ex(switch_obj, "normal_next", &normal_next);
    json_object_object_get_ex(switch_obj, "reverse_next", &reverse_next);
    
    switches[i].id = json_object_get_int(id);
    switches[i].section = json_object_get_int(section);
    switches[i].normalNext = json_object_get_int(normal_next);
    switches[i].reverseNext = json_object_get_int(reverse_next);
  }
  
  printf("Loaded track configuration: %d sections, %d stations, %d switches\n",
         sectionCount, stationCount, switchCount);
  
  json_object_put(parsed_json); // Free memory
}

void initializeSystem() {
  printf("Central Control System initializing...\n");
  loadTrackConfig();
}

void handleZoneConnection(int serverSocket) {
  struct sockaddr_in clientAddr;
  socklen_t addrLen = sizeof(clientAddr);
  int clientSocket =
      accept(serverSocket, (struct sockaddr *)&clientAddr, &addrLen);

  if (clientSocket < 0) {
    perror("Accept failed");
    return;
  }

  char buffer[BUFFER_SIZE];
  int bytesRead = recv(clientSocket, buffer, BUFFER_SIZE, 0);
  if (bytesRead <= 0) {
    close(clientSocket);
    return;
  }

  // Parse zone controller registration
  int zoneId;
  if (sscanf(buffer, "REGISTER_ZONE %d", &zoneId) == 1) {
    if (zoneCount < MAX_ZONES) {
      zoneControllers[zoneCount].id = zoneId;
      zoneControllers[zoneCount].connected = 1;
      zoneControllers[zoneCount].address = clientAddr;
      zoneControllers[zoneCount].socket = clientSocket;
      zoneCount++;

      char response[BUFFER_SIZE];
      sprintf(response, "ZONE_REGISTERED %d", zoneId);
      send(clientSocket, response, strlen(response), 0);

      printf("Zone Controller %d registered\n", zoneId);
    }
  }
}

void issueMovementAuthority(int zoneId, int trackSection, int speed) {
  // Find the zone controller
  for (int i = 0; i < zoneCount; i++) {
    if (zoneControllers[i].id == zoneId && zoneControllers[i].connected) {
      char command[BUFFER_SIZE];
      sprintf(command, "MOVEMENT_AUTHORITY %d %d", trackSection, speed);
      send(zoneControllers[i].socket, command, strlen(command), 0);
      printf("Issued movement authority to zone %d, track %d, speed %d\n",
             zoneId, trackSection, speed);
      return;
    }
  }
  printf("Zone controller %d not found or not connected\n", zoneId);
}

void setRoute(int trainId, int destinationSection) {
  printf("Setting route for Train %d to destination section %d\n", trainId, destinationSection);
  
  // Find the destination section in our track configuration
  int destinationZone = 0;
  for (int i = 0; i < sectionCount; i++) {
    if (trackSections[i].id == destinationSection) {
      destinationZone = trackSections[i].zone;
      break;
    }
  }
  
  if (destinationZone == 0) {
    printf("Destination section %d not found\n", destinationSection);
    return;
  }
  
  // Configure route through each zone
  for (int i = 0; i < zoneCount; i++) {
    if (zoneControllers[i].connected) {
      char command[BUFFER_SIZE];
      sprintf(command, "ROUTE_TRAIN %d %d", trainId, destinationSection);
      send(zoneControllers[i].socket, command, strlen(command), 0);
      printf("Sent route command to Zone %d\n", zoneControllers[i].id);
    }
  }
}

int main() {
  initializeSystem();

  // Create TCP server socket for zone controller connections
  int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (serverSocket < 0) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }
  
  int reuse = 1;
  if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in serverAddr;
  memset(&serverAddr, 0, sizeof(serverAddr));
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_addr.s_addr = INADDR_ANY;
  serverAddr.sin_port = htons(CCS_PORT);

  if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) <
      0) {
    perror("Bind failed");
    exit(EXIT_FAILURE);
  }

  if (listen(serverSocket, 10) < 0) {
    perror("Listen failed");
    exit(EXIT_FAILURE);
  }

  printf("Central Control System online. Listening on port %d\n", CCS_PORT);

  fd_set readfds;
  struct timeval tv;
  int maxfd;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(serverSocket, &readfds);
    FD_SET(STDIN_FILENO, &readfds); // Add stdin for user commands
    maxfd = serverSocket;

    // Add all zone controller sockets
    for (int i = 0; i < zoneCount; i++) {
      if (zoneControllers[i].connected) {
        FD_SET(zoneControllers[i].socket, &readfds);
        if (zoneControllers[i].socket > maxfd)
          maxfd = zoneControllers[i].socket;
      }
    }

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    if (activity < 0) {
      perror("Select error");
      continue;
    }

    // New zone controller connection
    if (FD_ISSET(serverSocket, &readfds)) {
      handleZoneConnection(serverSocket);
    }

    // Check for user commands
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char command[BUFFER_SIZE];
      if (fgets(command, BUFFER_SIZE, stdin) != NULL) {
        int zoneId, trackSection, speed, trainId, destination;
        
        if (sscanf(command, "auth %d %d %d", &zoneId, &trackSection, &speed) == 3) {
          issueMovementAuthority(zoneId, trackSection, speed);
        } 
        else if (sscanf(command, "route %d %d", &trainId, &destination) == 2) {
          setRoute(trainId, destination);
        }
        else if (strncmp(command, "list", 4) == 0) {
          printf("Connected Zone Controllers:\n");
          for (int i = 0; i < zoneCount; i++) {
            if (zoneControllers[i].connected) {
              printf("Zone %d\n", zoneControllers[i].id);
            }
          }
        } 
        else if (strncmp(command, "stations", 8) == 0) {
          printf("Stations:\n");
          for (int i = 0; i < stationCount; i++) {
            printf("%d: %s (Section %d, %s)\n", 
                   stations[i].id, stations[i].name, stations[i].section,
                   stations[i].isTerminus ? "Terminus" : "Regular");
          }
        }
        else if (strncmp(command, "quit", 4) == 0) {
          break;
        }
      }
    }

    // Check for messages from zone controllers
    for (int i = 0; i < zoneCount; i++) {
      if (zoneControllers[i].connected &&
          FD_ISSET(zoneControllers[i].socket, &readfds)) {
        char buffer[BUFFER_SIZE];
        int bytesRead =
            recv(zoneControllers[i].socket, buffer, BUFFER_SIZE, 0);
        if (bytesRead <= 0) {
          close(zoneControllers[i].socket);
          zoneControllers[i].connected = 0;
          printf("Zone Controller %d disconnected\n", zoneControllers[i].id);
        } else {
          buffer[bytesRead] = '\0';
          printf("Message from Zone %d: %s\n", zoneControllers[i].id, buffer);
        }
      }
    }
  }

  // Clean up
  for (int i = 0; i < zoneCount; i++) {
    if (zoneControllers[i].connected) {
      close(zoneControllers[i].socket);
    }
  }
  close(serverSocket);
  return 0;
}
