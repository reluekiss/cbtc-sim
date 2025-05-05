#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_ZONES 10
#define BUFFER_SIZE 1024
#define CCS_PORT 8000

typedef struct {
  int id;
  int connected;
  struct sockaddr_in address;
  int socket;
} ZoneController;

ZoneController zoneControllers[MAX_ZONES];
int zoneCount = 0;

void initializeSystem() {
  printf("Central Control System initializing...\n");
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
        int zoneId, trackSection, speed;
        if (sscanf(command, "auth %d %d %d", &zoneId, &trackSection, &speed) ==
            3) {
          issueMovementAuthority(zoneId, trackSection, speed);
        } else if (strncmp(command, "list", 4) == 0) {
          printf("Connected Zone Controllers:\n");
          for (int i = 0; i < zoneCount; i++) {
            if (zoneControllers[i].connected) {
              printf("Zone %d\n", zoneControllers[i].id);
            }
          }
        } else if (strncmp(command, "quit", 4) == 0) {
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
