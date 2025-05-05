#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define CCS_PORT 8000
#define ZC_PORT 8100
#define MULTICAST_PORT 8200
#define MAX_TRAINS 20
#define MAX_TRACK_SECTIONS 10

typedef struct {
  int id;
  int connected;
  struct sockaddr_in address;
  int socket;
  int currentSection;
} Train;

typedef struct {
  int id;
  int speed;
  int occupied;
} TrackSection;

Train trains[MAX_TRAINS];
int trainCount = 0;
TrackSection trackSections[MAX_TRACK_SECTIONS];
int zoneId;
char multicastGroups[MAX_TRACK_SECTIONS][20];
int multicastSocket;

void initializeZoneController(int id) {
  zoneId = id;
  printf("Zone Controller %d initializing...\n", zoneId);

  // Initialize track sections
  for (int i = 0; i < MAX_TRACK_SECTIONS; i++) {
    trackSections[i].id = i + 1;
    trackSections[i].speed = 100; // Default speed in km/h
    trackSections[i].occupied = 0;

    // Create multicast group addresses for each track section
    sprintf(multicastGroups[i], "239.0.%d.%d", zoneId, i + 1);
  }
}

void setupMulticastSocket() {
  multicastSocket = socket(AF_INET, SOCK_DGRAM, 0);
  if (multicastSocket < 0) {
    perror("Multicast socket creation failed");
    exit(EXIT_FAILURE);
  }

  int reuse = 1;
  if (setsockopt(multicastSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) < 0) {
    perror("Setting SO_REUSEADDR failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  // Enable broadcasting movement authorities
  struct sockaddr_in multicastAddr;
  memset(&multicastAddr, 0, sizeof(multicastAddr));
  multicastAddr.sin_family = AF_INET;
  multicastAddr.sin_addr.s_addr = INADDR_ANY;
  multicastAddr.sin_port = htons(MULTICAST_PORT);

  if (bind(multicastSocket, (struct sockaddr *)&multicastAddr,
           sizeof(multicastAddr)) < 0) {
    perror("Multicast bind failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  printf("Multicast socket setup complete\n");
}

void broadcastMovementAuthority(int trackSection, int speed) {
  if (trackSection < 1 || trackSection > MAX_TRACK_SECTIONS) {
    printf("Invalid track section\n");
    return;
  }

  trackSections[trackSection - 1].speed = speed;

  char message[BUFFER_SIZE];
  sprintf(message, "MA %d %d %d", zoneId, trackSection, speed);

  struct sockaddr_in groupAddr;
  memset(&groupAddr, 0, sizeof(groupAddr));
  groupAddr.sin_family = AF_INET;
  groupAddr.sin_addr.s_addr =
      inet_addr(multicastGroups[trackSection - 1]);
  groupAddr.sin_port = htons(MULTICAST_PORT);

  if (sendto(multicastSocket, message, strlen(message), 0,
             (struct sockaddr *)&groupAddr, sizeof(groupAddr)) < 0) {
    perror("Movement authority broadcast failed");
  } else {
    printf("Broadcasted MA to track section %d: speed %d\n", trackSection,
           speed);
  }
}

void handleTrainConnection(int serverSocket) {
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

  // Parse train registration
  int trainId, section;
  if (sscanf(buffer, "REGISTER_TRAIN %d %d", &trainId, &section) == 2) {
    if (trainCount < MAX_TRAINS) {
      trains[trainCount].id = trainId;
      trains[trainCount].connected = 1;
      trains[trainCount].address = clientAddr;
      trains[trainCount].socket = clientSocket;
      trains[trainCount].currentSection = section;
      trainCount++;

      // Mark section as occupied
      if (section >= 1 && section <= MAX_TRACK_SECTIONS) {
        trackSections[section - 1].occupied = 1;
      }

      char response[BUFFER_SIZE];
      sprintf(response, "TRAIN_REGISTERED %d", trainId);
      send(clientSocket, response, strlen(response), 0);

      printf("Train %d registered in section %d\n", trainId, section);
    }
  }
}

void processTrainUpdate(int trainIndex, char *message) {
  int trainId, newSection;
  if (sscanf(message, "POSITION_UPDATE %d %d", &trainId, &newSection) == 2) {
    if (trains[trainIndex].id == trainId) {
      int oldSection = trains[trainIndex].currentSection;
      
      // Update occupancy
      if (oldSection >= 1 && oldSection <= MAX_TRACK_SECTIONS) {
        trackSections[oldSection - 1].occupied = 0;
      }
      if (newSection >= 1 && newSection <= MAX_TRACK_SECTIONS) {
        trackSections[newSection - 1].occupied = 1;
      }
      
      trains[trainIndex].currentSection = newSection;
      printf("Train %d moved from section %d to %d\n", trainId, oldSection,
             newSection);
      
      // Send current speed limit to the train
      char response[BUFFER_SIZE];
      sprintf(response, "SPEED_LIMIT %d", 
              trackSections[newSection - 1].speed);
      send(trains[trainIndex].socket, response, strlen(response), 0);
    }
  }
}

int connectToCCS(const char *ccsIP) {
  int ccsSocket = socket(AF_INET, SOCK_STREAM, 0);
  if (ccsSocket < 0) {
    perror("CCS socket creation failed");
    exit(EXIT_FAILURE);
  }

  int reuse = 1;
  if (setsockopt(ccsSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in ccsAddr;
  memset(&ccsAddr, 0, sizeof(ccsAddr));
  ccsAddr.sin_family = AF_INET;
  ccsAddr.sin_addr.s_addr = inet_addr(ccsIP);
  ccsAddr.sin_port = htons(CCS_PORT);

  if (connect(ccsSocket, (struct sockaddr *)&ccsAddr, sizeof(ccsAddr)) < 0) {
    perror("Connection to CCS failed");
    close(ccsSocket);
    exit(EXIT_FAILURE);
  }

  // Register with CCS
  char registerMsg[BUFFER_SIZE];
  sprintf(registerMsg, "REGISTER_ZONE %d", zoneId);
  send(ccsSocket, registerMsg, strlen(registerMsg), 0);

  // Wait for confirmation
  char buffer[BUFFER_SIZE];
  int bytesRead = recv(ccsSocket, buffer, BUFFER_SIZE, 0);
  if (bytesRead > 0) {
    buffer[bytesRead] = '\0';
    printf("CCS response: %s\n", buffer);
  }

  return ccsSocket;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    printf("Usage: %s <zone_id> <ccs_ip>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int id = atoi(argv[1]);
  initializeZoneController(id);
  setupMulticastSocket();

  // Connect to Central Control System
  int ccsSocket = connectToCCS(argv[2]);

  // Create TCP server socket for train connections
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
  serverAddr.sin_port = htons(ZC_PORT + zoneId);

  if (bind(serverSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) <
      0) {
    perror("Bind failed");
    exit(EXIT_FAILURE);
  }

  if (listen(serverSocket, 10) < 0) {
    perror("Listen failed");
    exit(EXIT_FAILURE);
  }

  printf("Zone Controller %d online. Listening on port %d\n", zoneId,
         ZC_PORT + zoneId);

  fd_set readfds;
  struct timeval tv;
  int maxfd;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(serverSocket, &readfds);
    FD_SET(ccsSocket, &readfds);
    FD_SET(STDIN_FILENO, &readfds); // Add stdin for manual commands

    maxfd = serverSocket > ccsSocket ? serverSocket : ccsSocket;

    // Add all train sockets
    for (int i = 0; i < trainCount; i++) {
      if (trains[i].connected) {
        FD_SET(trains[i].socket, &readfds);
        if (trains[i].socket > maxfd)
          maxfd = trains[i].socket;
      }
    }

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    if (activity < 0) {
      perror("Select error");
      continue;
    }

    // New train connection
    if (FD_ISSET(serverSocket, &readfds)) {
      handleTrainConnection(serverSocket);
    }

    // Message from CCS
    if (FD_ISSET(ccsSocket, &readfds)) {
      char buffer[BUFFER_SIZE];
      int bytesRead = recv(ccsSocket, buffer, BUFFER_SIZE, 0);
      if (bytesRead <= 0) {
        printf("CCS disconnected. Exiting...\n");
        break;
      } else {
        buffer[bytesRead] = '\0';
        printf("Message from CCS: %s\n", buffer);

        // Process CCS commands
        int trackSection, speed;
        if (sscanf(buffer, "MOVEMENT_AUTHORITY %d %d", &trackSection, &speed) ==
            2) {
          broadcastMovementAuthority(trackSection, speed);
        }
      }
    }

    // Check for user commands
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char command[BUFFER_SIZE];
      if (fgets(command, BUFFER_SIZE, stdin) != NULL) {
        int trackSection, speed;
        if (sscanf(command, "ma %d %d", &trackSection, &speed) == 2) {
          broadcastMovementAuthority(trackSection, speed);
        } else if (strncmp(command, "status", 6) == 0) {
          printf("Track Sections Status:\n");
          for (int i = 0; i < MAX_TRACK_SECTIONS; i++) {
            printf("Section %d: Speed %d, %s\n", trackSections[i].id,
                   trackSections[i].speed,
                   trackSections[i].occupied ? "Occupied" : "Clear");
          }
        } else if (strncmp(command, "trains", 6) == 0) {
          printf("Connected Trains:\n");
          for (int i = 0; i < trainCount; i++) {
            if (trains[i].connected) {
              printf("Train %d in section %d\n", trains[i].id,
                     trains[i].currentSection);
            }
          }
        } else if (strncmp(command, "quit", 4) == 0) {
          break;
        }
      }
    }

    // Check for messages from trains
    for (int i = 0; i < trainCount; i++) {
      if (trains[i].connected && FD_ISSET(trains[i].socket, &readfds)) {
        char buffer[BUFFER_SIZE];
        int bytesRead = recv(trains[i].socket, buffer, BUFFER_SIZE, 0);
        if (bytesRead <= 0) {
          close(trains[i].socket);
          trains[i].connected = 0;
          printf("Train %d disconnected\n", trains[i].id);
          
          // Clear the track section
          if (trains[i].currentSection >= 1 && 
              trains[i].currentSection <= MAX_TRACK_SECTIONS) {
            trackSections[trains[i].currentSection - 1].occupied = 0;
          }
        } else {
          buffer[bytesRead] = '\0';
          processTrainUpdate(i, buffer);
        }
      }
    }
  }

  // Clean up
  for (int i = 0; i < trainCount; i++) {
    if (trains[i].connected) {
      close(trains[i].socket);
    }
  }
  close(serverSocket);
  close(ccsSocket);
  close(multicastSocket);
  return 0;
}
