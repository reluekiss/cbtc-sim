#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define ZC_PORT 8100
#define MULTICAST_PORT 8200

typedef struct {
  int id;
  int currentSection;
  int currentSpeed;
  int targetSpeed;
  int zoneId;
} TrainState;

TrainState state;
int zoneControllerSocket;
int multicastSocket;

void initializeTrain(int trainId, int zoneId, int initialSection) {
  state.id = trainId;
  state.currentSection = initialSection;
  state.currentSpeed = 0;
  state.targetSpeed = 0;
  state.zoneId = zoneId;

  printf("Train %d initializing in Zone %d, Section %d\n", trainId, zoneId, initialSection);
}

int connectToZoneController(const char *zcIP) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("Socket creation failed");
    exit(EXIT_FAILURE);
  }

  int reuse = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in zcAddr;
  memset(&zcAddr, 0, sizeof(zcAddr));
  zcAddr.sin_family = AF_INET;
  zcAddr.sin_addr.s_addr = inet_addr(zcIP);
  zcAddr.sin_port = htons(ZC_PORT + state.zoneId);

  if (connect(sock, (struct sockaddr *)&zcAddr, sizeof(zcAddr)) < 0) {
    perror("Connection to Zone Controller failed");
    close(sock);
    exit(EXIT_FAILURE);
  }

  // Register with Zone Controller
  char registerMsg[BUFFER_SIZE];
  sprintf(registerMsg, "REGISTER_TRAIN %d %d", state.id, state.currentSection);
  send(sock, registerMsg, strlen(registerMsg), 0);

  // Wait for confirmation
  char buffer[BUFFER_SIZE];
  int bytesRead = recv(sock, buffer, BUFFER_SIZE, 0);
  if (bytesRead > 0) {
    buffer[bytesRead] = '\0';
    printf("Zone Controller response: %s\n", buffer);
  }

  return sock;
}

void setupMulticastListener() {
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

  struct sockaddr_in localAddr;
  memset(&localAddr, 0, sizeof(localAddr));
  localAddr.sin_family = AF_INET;
  localAddr.sin_addr.s_addr = INADDR_ANY;
  localAddr.sin_port = htons(MULTICAST_PORT);

  if (bind(multicastSocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
    perror("Multicast bind failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  // Join multicast group for current section
  struct ip_mreq mreq;
  char multicastGroup[20];
  sprintf(multicastGroup, "239.0.%d.%d", state.zoneId, state.currentSection);
  mreq.imr_multiaddr.s_addr = inet_addr(multicastGroup);
  mreq.imr_interface.s_addr = INADDR_ANY;

  if (setsockopt(multicastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    perror("Joining multicast group failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  printf("Joined multicast group: %s\n", multicastGroup);
}

void joinMulticastGroup(int section) {
  // Leave current multicast group
  struct ip_mreq mreq;
  char oldGroup[20];
  sprintf(oldGroup, "239.0.%d.%d", state.zoneId, state.currentSection);
  mreq.imr_multiaddr.s_addr = inet_addr(oldGroup);
  mreq.imr_interface.s_addr = INADDR_ANY;

  setsockopt(multicastSocket, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

  // Join new multicast group
  char newGroup[20];
  sprintf(newGroup, "239.0.%d.%d", state.zoneId, section);
  mreq.imr_multiaddr.s_addr = inet_addr(newGroup);

  if (setsockopt(multicastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
    perror("Joining new multicast group failed");
  } else {
    printf("Switched to multicast group: %s\n", newGroup);
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

    printf("Position updated: Section %d -> %d\n", state.currentSection, newSection);
    state.currentSection = newSection;
  }
}

void adjustSpeed() {
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

  printf("Current speed: %d km/h, Target: %d km/h\n", state.currentSpeed, state.targetSpeed);
}

void processMovementAuthority(char *message) {
  int maZoneId, section, speed;
  if (sscanf(message, "MA %d %d %d", &maZoneId, &section, &speed) == 3) {
    if (maZoneId == state.zoneId && section == state.currentSection) {
      printf("Received new movement authority: Speed %d km/h\n", speed);
      state.targetSpeed = speed;
    }
  }
}

int train_main(int argc, char *argv[]) {
  if (argc != 5) {
    printf("Usage: %s <train_id> <zone_id> <initial_section> <zc_ip>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  int trainId = atoi(argv[1]);
  int zoneId = atoi(argv[2]);
  int initialSection = atoi(argv[3]);

  initializeTrain(trainId, zoneId, initialSection);
  zoneControllerSocket = connectToZoneController(argv[4]);
  setupMulticastListener();

  fd_set readfds;
  struct timeval tv;
  int maxfd;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(zoneControllerSocket, &readfds);
    FD_SET(multicastSocket, &readfds);
    FD_SET(STDIN_FILENO, &readfds); // Add stdin for manual commands

    maxfd = zoneControllerSocket > multicastSocket ? zoneControllerSocket : multicastSocket;

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    if (activity < 0) {
      perror("Select error");
      continue;
    }

    // Process messages from Zone Controller
    if (FD_ISSET(zoneControllerSocket, &readfds)) {
      char buffer[BUFFER_SIZE];
      int bytesRead = recv(zoneControllerSocket, buffer, BUFFER_SIZE, 0);
      if (bytesRead <= 0) {
        printf("Zone Controller disconnected. Stopping train...\n");
        state.targetSpeed = 0;
        // In a real system, would trigger emergency braking
        break;
      } else {
        buffer[bytesRead] = '\0';
        printf("Message from Zone Controller: %s\n", buffer);
        
        // Process speed limit updates
        int speedLimit;
        if (sscanf(buffer, "SPEED_LIMIT %d", &speedLimit) == 1) {
          printf("Received speed limit: %d km/h\n", speedLimit);
          state.targetSpeed = speedLimit;
        }
      }
    }

    // Process multicast messages (Movement Authorities)
    if (FD_ISSET(multicastSocket, &readfds)) {
      char buffer[BUFFER_SIZE];
      struct sockaddr_in senderAddr;
      socklen_t addrLen = sizeof(senderAddr);
      int bytesRead =
          recvfrom(multicastSocket, buffer, BUFFER_SIZE, 0,
                   (struct sockaddr *)&senderAddr, &addrLen);
      if (bytesRead > 0) {
        buffer[bytesRead] = '\0';
        processMovementAuthority(buffer);
      }
    }

    // Check for user commands
    if (FD_ISSET(STDIN_FILENO, &readfds)) {
      char command[BUFFER_SIZE];
      if (fgets(command, BUFFER_SIZE, stdin) != NULL) {
        int section;
        if (sscanf(command, "move %d", &section) == 1) {
          updatePosition(section);
        } else if (strncmp(command, "status", 6) == 0) {
          printf("Train %d status:\n", state.id);
          printf("  Zone: %d\n", state.zoneId);
          printf("  Section: %d\n", state.currentSection);
          printf("  Current speed: %d km/h\n", state.currentSpeed);
          printf("  Target speed: %d km/h\n", state.targetSpeed);
        } else if (strncmp(command, "quit", 4) == 0) {
          break;
        }
      }
    }

    // Simulate train behavior
    adjustSpeed();
  }

  // Clean up
  close(zoneControllerSocket);
  close(multicastSocket);
  return 0;
}
