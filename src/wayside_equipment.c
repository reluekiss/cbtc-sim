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

typedef enum { SIGNAL, SWITCH } EquipmentType;
typedef enum { GREEN, YELLOW, RED } SignalState;
typedef enum { NORMAL, REVERSE } SwitchPosition;

typedef struct {
  int id;
  EquipmentType type;
  int zoneId;
  int trackSection;
  union {
    SignalState signalState;
    SwitchPosition switchPosition;
  } u;
} WaysideEquipment;

WaysideEquipment equipment;
int zoneControllerSocket;
int multicastSocket;

void initializeEquipment(int id, EquipmentType type, int zoneId,
                         int trackSection) {
  equipment.id = id;
  equipment.type = type;
  equipment.zoneId = zoneId;
  equipment.trackSection = trackSection;

  // Set default states
  if (type == SIGNAL) {
    equipment.u.signalState = RED; // Default to most restrictive
    printf("Signal %d initializing in Zone %d, Section %d (RED)\n", id, zoneId,
           trackSection);
  } else {
    equipment.u.switchPosition = NORMAL;
    printf("Switch %d initializing in Zone %d, Section %d (NORMAL)\n", id,
           zoneId, trackSection);
  }
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
  zcAddr.sin_port = htons(ZC_PORT + equipment.zoneId);

  if (connect(sock, (struct sockaddr *)&zcAddr, sizeof(zcAddr)) < 0) {
    perror("Connection to Zone Controller failed");
    close(sock);
    exit(EXIT_FAILURE);
  }

  // Register with Zone Controller
  char registerMsg[BUFFER_SIZE];
  if (equipment.type == SIGNAL) {
    sprintf(registerMsg, "REGISTER_SIGNAL %d %d", equipment.id,
            equipment.trackSection);
  } else {
    sprintf(registerMsg, "REGISTER_SWITCH %d %d", equipment.id,
            equipment.trackSection);
  }
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

  if (bind(multicastSocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) <
      0) {
    perror("Multicast bind failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  // Join multicast group for equipment's section
  struct ip_mreq mreq;
  char multicastGroup[20];
  sprintf(multicastGroup, "239.0.%d.%d", equipment.zoneId,
          equipment.trackSection);
  mreq.imr_multiaddr.s_addr = inet_addr(multicastGroup);
  mreq.imr_interface.s_addr = INADDR_ANY;

  if (setsockopt(multicastSocket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                 sizeof(mreq)) < 0) {
    perror("Joining multicast group failed");
    close(multicastSocket);
    exit(EXIT_FAILURE);
  }

  printf("Joined multicast group: %s\n", multicastGroup);
}

void processMovementAuthority(char *message) {
  int maZoneId, section, speed;
  if (sscanf(message, "MA %d %d %d", &maZoneId, &section, &speed) == 3) {
    if (maZoneId == equipment.zoneId && section == equipment.trackSection) {
      printf("Received movement authority for section %d: Speed %d km/h\n",
             section, speed);

      // For signals, set appropriate aspect based on speed
      if (equipment.type == SIGNAL) {
        if (speed == 0) {
          equipment.u.signalState = RED;
        } else if (speed < 50) {
          equipment.u.signalState = YELLOW;
        } else {
          equipment.u.signalState = GREEN;
        }
        
        printf("Signal %d changed to %s\n", equipment.id,
               equipment.u.signalState == RED
                   ? "RED"
                   : (equipment.u.signalState == YELLOW ? "YELLOW" : "GREEN"));
      }
      
      // Send status update to zone controller
      char statusMsg[BUFFER_SIZE];
      if (equipment.type == SIGNAL) {
        sprintf(statusMsg, "SIGNAL_STATUS %d %d", equipment.id,
                equipment.u.signalState);
      } else {
        sprintf(statusMsg, "SWITCH_STATUS %d %d", equipment.id,
                equipment.u.switchPosition);
      }
      send(zoneControllerSocket, statusMsg, strlen(statusMsg), 0);
    }
  }
}

void changeState(char *command) {
  if (equipment.type == SIGNAL) {
    if (strcmp(command, "red") == 0) {
      equipment.u.signalState = RED;
    } else if (strcmp(command, "yellow") == 0) {
      equipment.u.signalState = YELLOW;
    } else if (strcmp(command, "green") == 0) {
      equipment.u.signalState = GREEN;
    } else {
      printf("Invalid signal state\n");
      return;
    }
    printf("Signal %d changed to %s\n", equipment.id,
           equipment.u.signalState == RED
               ? "RED"
               : (equipment.u.signalState == YELLOW ? "YELLOW" : "GREEN"));
  } else {
    if (strcmp(command, "normal") == 0) {
      equipment.u.switchPosition = NORMAL;
    } else if (strcmp(command, "reverse") == 0) {
      equipment.u.switchPosition = REVERSE;
    } else {
      printf("Invalid switch position\n");
      return;
    }
    printf("Switch %d changed to %s\n", equipment.id,
           equipment.u.switchPosition == NORMAL ? "NORMAL" : "REVERSE");
  }

  // Send status update to zone controller
  char statusMsg[BUFFER_SIZE];
  if (equipment.type == SIGNAL) {
    sprintf(statusMsg, "SIGNAL_STATUS %d %d", equipment.id,
            equipment.u.signalState);
  } else {
    sprintf(statusMsg, "SWITCH_STATUS %d %d", equipment.id,
            equipment.u.switchPosition);
  }
  send(zoneControllerSocket, statusMsg, strlen(statusMsg), 0);
}

int main(int argc, char *argv[]) {
  if (argc != 6) {
    printf(
        "Usage: %s <id> <type:0=signal,1=switch> <zone_id> <section> <zc_ip>\n",
        argv[0]);
    exit(EXIT_FAILURE);
  }

  int id = atoi(argv[1]);
  EquipmentType type = atoi(argv[2]) == 0 ? SIGNAL : SWITCH;
  int zoneId = atoi(argv[3]);
  int section = atoi(argv[4]);

  initializeEquipment(id, type, zoneId, section);
  zoneControllerSocket = connectToZoneController(argv[5]);
  setupMulticastListener();

  fd_set readfds;
  struct timeval tv;
  int maxfd;

  while (1) {
    FD_ZERO(&readfds);
    FD_SET(zoneControllerSocket, &readfds);
    FD_SET(multicastSocket, &readfds);
    FD_SET(STDIN_FILENO, &readfds); // Add stdin for manual commands

    maxfd = zoneControllerSocket > multicastSocket ? zoneControllerSocket
                                                   : multicastSocket;

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
        printf("Zone Controller disconnected. Setting to safe state...\n");
        if (equipment.type == SIGNAL) {
          equipment.u.signalState = RED;
        } else {
          // In real systems, switches would typically stay in their current position
          printf("Switch remains in current position\n");
        }
        break;
      } else {
        buffer[bytesRead] = '\0';
        printf("Command from Zone Controller: %s\n", buffer);
        
        // Process direct commands
        if (equipment.type == SIGNAL) {
          int state;
          if (sscanf(buffer, "SET_SIGNAL %d %d", &id, &state) == 2 &&
              id == equipment.id) {
            equipment.u.signalState = state;
            printf("Signal set to %s\n",
                   state == RED ? "RED" : (state == YELLOW ? "YELLOW" : "GREEN"));
          }
        } else {
          int position;
          if (sscanf(buffer, "SET_SWITCH %d %d", &id, &position) == 2 &&
              id == equipment.id) {
            equipment.u.switchPosition = position;
            printf("Switch set to %s\n",
                   position == NORMAL ? "NORMAL" : "REVERSE");
          }
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
        // Remove newline
        command[strcspn(command, "\n")] = 0;
        
        if (strncmp(command, "status", 6) == 0) {
          if (equipment.type == SIGNAL) {
            printf("Signal %d status: %s\n", equipment.id,
                   equipment.u.signalState == RED
                       ? "RED"
                       : (equipment.u.signalState == YELLOW ? "YELLOW" : "GREEN"));
          } else {
            printf("Switch %d status: %s\n", equipment.id,
                   equipment.u.switchPosition == NORMAL ? "NORMAL" : "REVERSE");
          }
        } else if (strncmp(command, "quit", 4) == 0) {
          break;
        } else {
          changeState(command);
        }
      }
    }
  }

  // Clean up
  close(zoneControllerSocket);
  close(multicastSocket);
  return 0;
}
