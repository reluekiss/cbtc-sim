#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <json-c/json.h>

#define BUFFER_SIZE 1024
#define CCS_PORT 8000
#define ZC_PORT 8100
#define MULTICAST_PORT 8200
#define MAX_TRAINS 20
#define MAX_TRACK_SECTIONS 30
#define CONFIG_FILE "track_config.json"

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
Train trains[MAX_TRAINS];
int trainCount = 0;
TrackSection trackSections[MAX_TRACK_SECTIONS];
int trackSectionCount = 0;
Station stations[10];
int stationCount = 0;
Switch switches[5];
int switchCount = 0;
int zoneId;
char multicastGroups[MAX_TRACK_SECTIONS][20];
int multicastSocket;

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
  
  // Parse track sections for this zone
  json_object_object_get_ex(parsed_json, "track_sections", &sections);
  int totalSections = json_object_array_length(sections);
  
  for (int i = 0; i < totalSections; i++) {
    section = json_object_array_get_idx(sections, i);
    
    struct json_object *id, *zone;
    
    json_object_object_get_ex(section, "id", &id);
    json_object_object_get_ex(section, "zone", &zone);
    
    int sectionId = json_object_get_int(id);
    int sectionZone = json_object_get_int(zone);
    
    // Only add sections for this zone
    if (sectionZone == zoneId) {
      trackSections[trackSectionCount].id = sectionId;
      trackSections[trackSectionCount].speed = 50; // Default speed
      trackSections[trackSectionCount].occupied = 0;
      trackSectionCount++;
    }
  }
  
  // Parse stations for this zone
  json_object_object_get_ex(parsed_json, "stations", &stations_arr);
  int totalStations = json_object_array_length(stations_arr);
  
  for (int i = 0; i < totalStations; i++) {
    station = json_object_array_get_idx(stations_arr, i);
    
    struct json_object *name, *section, *stop_time, *terminus;
    
    json_object_object_get_ex(station, "name", &name);
    json_object_object_get_ex(station, "section", &section);
    json_object_object_get_ex(station, "stop_time", &stop_time);
    json_object_object_get_ex(station, "terminus", &terminus);
    
    int stationSection = json_object_get_int(section);
    
    // Check if station is in a section managed by this zone
    for (int j = 0; j < trackSectionCount; j++) {
      if (trackSections[j].id == stationSection) {
        stations[stationCount].id = i + 1;
        stations[stationCount].section = stationSection;
        stations[stationCount].stopTime = json_object_get_int(stop_time);
        stations[stationCount].isTerminus = json_object_get_boolean(terminus);
        strncpy(stations[stationCount].name, json_object_get_string(name), 
                sizeof(stations[stationCount].name) - 1);
        stationCount++;
        break;
      }
    }
  }
  
  // Parse switches for this zone
  json_object_object_get_ex(parsed_json, "switches", &switches_arr);
  int totalSwitches = json_object_array_length(switches_arr);
  
  for (int i = 0; i < totalSwitches; i++) {
    switch_obj = json_object_array_get_idx(switches_arr, i);
    
    struct json_object *id, *section, *normal_next, *reverse_next;
    
    json_object_object_get_ex(switch_obj, "id", &id);
    json_object_object_get_ex(switch_obj, "section", &section);
    json_object_object_get_ex(switch_obj, "normal_next", &normal_next);
    json_object_object_get_ex(switch_obj, "reverse_next", &reverse_next);
    
    int switchSection = json_object_get_int(section);
    
    // Check if switch is in a section managed by this zone
    for (int j = 0; j < trackSectionCount; j++) {
      if (trackSections[j].id == switchSection) {
        switches[switchCount].id = json_object_get_int(id);
        switches[switchCount].section = switchSection;
        switches[switchCount].normalNext = json_object_get_int(normal_next);
        switches[switchCount].reverseNext = json_object_get_int(reverse_next);
        switchCount++;
        break;
      }
    }
  }
  
  printf("Zone %d loaded configuration: %d sections, %d stations, %d switches\n",
         zoneId, trackSectionCount, stationCount, switchCount);
  
  json_object_put(parsed_json); // Free memory
}

void initializeZoneController(int id) {
  zoneId = id;
  printf("Zone Controller %d initializing...\n", zoneId);
  
  // Load configuration
  loadTrackConfig();

  // Create multicast group addresses for each track section
  for (int i = 0; i < trackSectionCount; i++) {
    sprintf(multicastGroups[i], "239.0.%d.%d", zoneId, trackSections[i].id);
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
  // Find multicast group for the section
  char *multicastGroup = NULL;
  
  for (int i = 0; i < trackSectionCount; i++) {
    if (trackSections[i].id == trackSection) {
      trackSections[i].speed = speed;
      multicastGroup = multicastGroups[i];
      break;
    }
  }
  
  if (multicastGroup == NULL) {
    printf("Invalid track section %d\n", trackSection);
    return;
  }

  char message[BUFFER_SIZE];
  sprintf(message, "MA %d %d %d", zoneId, trackSection, speed);

  struct sockaddr_in groupAddr;
  memset(&groupAddr, 0, sizeof(groupAddr));
  groupAddr.sin_family = AF_INET;
  groupAddr.sin_addr.s_addr = inet_addr(multicastGroup);
  groupAddr.sin_port = htons(MULTICAST_PORT);

  if (sendto(multicastSocket, message, strlen(message), 0,
             (struct sockaddr *)&groupAddr, sizeof(groupAddr)) < 0) {
    perror("Movement authority broadcast failed");
  } else {
    printf("Broadcasted MA to track section %d: speed %d\n", trackSection,
           speed);
  }
}

void setSwitch(int switchId, int position) {
  char command[BUFFER_SIZE];
  sprintf(command, "SET_SWITCH %d %d", switchId, position);
  
  // Send to all connected train components (in a real system, would send to specific wayside equipment)
  for (int i = 0; i < trainCount; i++) {
    if (trains[i].connected) {
      send(trains[i].socket, command, strlen(command), 0);
    }
  }
  
  printf("Set switch %d to position %d\n", switchId, position);
}

// Route train to destination
void routeTrain(int trainId, int destinationSection) {
  printf("Routing train %d to section %d\n", trainId, destinationSection);
  
  // See if destination is in this zone
  int inThisZone = 0;
  for (int i = 0; i < trackSectionCount; i++) {
    if (trackSections[i].id == destinationSection) {
      inThisZone = 1;
      break;
    }
  }
  
  if (!inThisZone) {
    printf("Destination section %d not in zone %d\n", destinationSection, zoneId);
    return;
  }
  
  // Check if destination is North station (section 23)
  if (destinationSection == 23) {
    // Set switch 1 to REVERSE
    setSwitch(1, 1);
    
    // Find train
    for (int i = 0; i < trainCount; i++) {
      if (trains[i].id == trainId && trains[i].connected) {
        char routeMsg[BUFFER_SIZE];
        sprintf(routeMsg, "ROUTE_TO %d", destinationSection);
        send(trains[i].socket, routeMsg, strlen(routeMsg), 0);
        break;
      }
    }
    
    printf("Set northbound route for train %d\n", trainId);
  }
  
  // Ensure routes have proper authority
  for (int i = 0; i < trackSectionCount; i++) {
    broadcastMovementAuthority(trackSections[i].id, 50);
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
      for (int i = 0; i < trackSectionCount; i++) {
        if (trackSections[i].id == section) {
          trackSections[i].occupied = 1;
          break;
        }
      }

      char response[BUFFER_SIZE];
      sprintf(response, "TRAIN_REGISTERED %d", trainId);
      send(clientSocket, response, strlen(response), 0);

      printf("Train %d registered in section %d\n", trainId, section);
      
      // Send station information for this zone
      for (int i = 0; i < stationCount; i++) {
        char stationMsg[BUFFER_SIZE];
        sprintf(stationMsg, "STATION_INFO %d %d %d %d %s", 
                stations[i].id, stations[i].section, stations[i].stopTime,
                stations[i].isTerminus, stations[i].name);
        send(clientSocket, stationMsg, strlen(stationMsg), 0);
      }
      
      // Send initial speed limit
      int speedLimit = 50; // Default
      for (int i = 0; i < trackSectionCount; i++) {
        if (trackSections[i].id == section) {
          speedLimit = trackSections[i].speed;
          break;
        }
      }
      
      char speedMsg[BUFFER_SIZE];
      sprintf(speedMsg, "SPEED_LIMIT %d", speedLimit);
      send(clientSocket, speedMsg, strlen(speedMsg), 0);
      
      // Broadcast movement authority for this section
      broadcastMovementAuthority(section, speedLimit);
    }
  } else if (sscanf(buffer, "REGISTER_SIGNAL %d %d", &trainId, &section) == 2) {
    // Handle signal registration
    char response[BUFFER_SIZE];
    sprintf(response, "SIGNAL_REGISTERED %d", trainId);
    send(clientSocket, response, strlen(response), 0);
    printf("Signal %d registered in section %d\n", trainId, section);
  } else if (sscanf(buffer, "REGISTER_SWITCH %d %d", &trainId, &section) == 2) {
    // Handle switch registration
    char response[BUFFER_SIZE];
    sprintf(response, "SWITCH_REGISTERED %d", trainId);
    send(clientSocket, response, strlen(response), 0);
    printf("Switch %d registered in section %d\n", trainId, section);
  }
}

void processTrainUpdate(int trainIndex, char *message) {
  int trainId, newSection;
  if (sscanf(message, "POSITION_UPDATE %d %d", &trainId, &newSection) == 2) {
    if (trains[trainIndex].id == trainId) {
      int oldSection = trains[trainIndex].currentSection;
      
      // Update occupancy
      for (int i = 0; i < trackSectionCount; i++) {
        if (trackSections[i].id == oldSection) {
          trackSections[i].occupied = 0;
          break;
        }
      }
      
      for (int i = 0; i < trackSectionCount; i++) {
        if (trackSections[i].id == newSection) {
          trackSections[i].occupied = 1;
          break;
        }
      }
      
      trains[trainIndex].currentSection = newSection;
      printf("Train %d moved from section %d to %d\n", trainId, oldSection,
             newSection);
      
      // Send current speed limit to the train
      int speedLimit = 50; // Default
      for (int i = 0; i < trackSectionCount; i++) {
        if (trackSections[i].id == newSection) {
          speedLimit = trackSections[i].speed;
          break;
        }
      }
      
      char response[BUFFER_SIZE];
      sprintf(response, "SPEED_LIMIT %d", speedLimit);
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
  int ccsSocket;

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
				int trackSection, speed, trainId;
				if (sscanf(buffer, "MOVEMENT_AUTHORITY %d %d", &trackSection, &speed) == 2) {
					broadcastMovementAuthority(trackSection, speed);
				}

				if (sscanf(buffer, "TRAIN_SPEED %d %d", &trainId, &speed) == 2) {
						// Find the train and send speed command
						for (int i = 0; i < trainCount; i++) {
								if (trains[i].connected && trains[i].id == trainId) {
										char speedCmd[BUFFER_SIZE];
										sprintf(speedCmd, "SPEED_LIMIT %d", speed);
										send(trains[i].socket, speedCmd, strlen(speedCmd), 0);
										printf("Sent speed %d to Train %d\n", speed, trainId);
										break;
								}
						}
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
                } else if (strncmp(command, "route_north", 11) == 0) {
                    // Set switches for northbound route
                    setSwitch(1, 1); // Set switch 1 to REVERSE
                    printf("Setting northbound route\n");
                    
                    // Find Train 102 and direct it
                    for (int i = 0; i < trainCount; i++) {
                        if (trains[i].connected && trains[i].id == 102) {
                            char routeMsg[BUFFER_SIZE];
                            sprintf(routeMsg, "TAKE_NORTH_ROUTE");
                            send(trains[i].socket, routeMsg, strlen(routeMsg), 0);
                            break;
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
