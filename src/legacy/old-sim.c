#include "raylib.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

typedef struct {
  int id;
  int zoneId;
  int section;
  Vector2 position;
  int speed;
  int targetSpeed;
  int stationStopTime;
  int stationTimer;
  Color color;
  bool atStation;
  int direction;  // 1 for forward, -1 for backward
} Train;

typedef struct {
  int id;
  int zoneId;
  int section;
  Vector2 position;
  int state; // 0=RED, 1=YELLOW, 2=GREEN
} Signal;

typedef struct {
  int id;
  int zoneId;
  int section;
  Vector2 position;
  int state; // 0=NORMAL, 1=REVERSE
  Rectangle switchNormal;
  Rectangle switchReverse;
} Switch;

typedef struct {
  int id;
  Vector2 position;
  const char *name;
  int section;
  int stopTime; // in seconds
  Rectangle bounds;
} Station;

typedef struct {
  Vector2 start;
  Vector2 end;
  int zoneId;
  int section;
} TrackSegment;

// Global state
char logs[MAX_LOGS][MAX_LOG_LENGTH];
Train trains[MAX_TRAINS];
Signal signals[MAX_SIGNALS];
Switch switches[MAX_SWITCHES];
Station stations[MAX_STATIONS];
TrackSegment trackSegments[MAX_SECTIONS];
int logCount = 0;
int trainCount = 0;
int signalCount = 0;
int switchCount = 0;
int stationCount = 0;
int trackSegmentCount = 0;

pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;

void addLog(const char *message) {
  pthread_mutex_lock(&stateMutex);
  if (logCount >= MAX_LOGS) {
    // Shift logs up
    for (int i = 0; i < MAX_LOGS - 1; i++) {
      strcpy(logs[i], logs[i + 1]);
    }
    logCount = MAX_LOGS - 1;
  }
  
  time_t now = time(NULL);
  struct tm *timeinfo = localtime(&now);
  char timestamp[20];
  strftime(timestamp, sizeof(timestamp), "%H:%M:%S", timeinfo);
  
  snprintf(logs[logCount], MAX_LOG_LENGTH, "[%s] %s", timestamp, message);
  logCount++;
  pthread_mutex_unlock(&stateMutex);
}

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

  // Define signals
  signals[signalCount].id = 1;
  signals[signalCount].zoneId = 1;
  signals[signalCount].section = 1;
  signals[signalCount].position = (Vector2){130, 280};
  signals[signalCount].state = 2; // GREEN
  signalCount++;
  
  signals[signalCount].id = 2;
  signals[signalCount].zoneId = 1;
  signals[signalCount].section = 5;
  signals[signalCount].position = (Vector2){290, 280};
  signals[signalCount].state = 2; // GREEN
  signalCount++;
  
  signals[signalCount].id = 3;
  signals[signalCount].zoneId = 2;
  signals[signalCount].section = 9;
  signals[signalCount].position = (Vector2){450, 280};
  signals[signalCount].state = 2; // GREEN
  signalCount++;
  
  signals[signalCount].id = 4;
  signals[signalCount].zoneId = 2;
  signals[signalCount].section = 21;
  signals[signalCount].position = (Vector2){400, 260};
  signals[signalCount].state = 1; // YELLOW
  signalCount++;
  
  signals[signalCount].id = 5;
  signals[signalCount].zoneId = 3;
  signals[signalCount].section = 15;
  signals[signalCount].position = (Vector2){690, 280};
  signals[signalCount].state = 2; // GREEN
  signalCount++;

  // Define switches
  switches[switchCount].id = 1;
  switches[switchCount].zoneId = 2;
  switches[switchCount].section = 8;
  switches[switchCount].position = (Vector2){420, 300};
  switches[switchCount].state = 0; // NORMAL
  switches[switchCount].switchNormal = (Rectangle){400, 290, 40, 20};
  switches[switchCount].switchReverse = (Rectangle){410, 280, 20, 40};
  switchCount++;
  
  switches[switchCount].id = 2;
  switches[switchCount].zoneId = 2;
  switches[switchCount].section = 12;
  switches[switchCount].position = (Vector2){580, 300};
  switches[switchCount].state = 0; // NORMAL
  switches[switchCount].switchNormal = (Rectangle){560, 290, 40, 20};
  switches[switchCount].switchReverse = (Rectangle){570, 280, 20, 40};
  switchCount++;

  // Initialize trains
  trains[trainCount].id = 101;
  trains[trainCount].zoneId = 1;
  trains[trainCount].section = 1;
  trains[trainCount].position = (Vector2){110, 300};
  trains[trainCount].speed = 0;
  trains[trainCount].targetSpeed = 40;
  trains[trainCount].stationStopTime = 0;
  trains[trainCount].stationTimer = 0;
  trains[trainCount].color = RED;
  trains[trainCount].atStation = false;
  trains[trainCount].direction = 1;
  trainCount++;
  
  trains[trainCount].id = 102;
  trains[trainCount].zoneId = 2;
  trains[trainCount].section = 10;
  trains[trainCount].position = (Vector2){490, 300};
  trains[trainCount].speed = 0;
  trains[trainCount].targetSpeed = 40;
  trains[trainCount].stationStopTime = 0;
  trains[trainCount].stationTimer = 0;
  trains[trainCount].color = BLUE;
  trains[trainCount].atStation = false;
  trainCount++;
  
  trains[trainCount].id = 103;
  trains[trainCount].zoneId = 3;
  trains[trainCount].section = 17;
  trains[trainCount].position = (Vector2){770, 300};
  trains[trainCount].speed = 0;
  trains[trainCount].targetSpeed = 40;
  trains[trainCount].stationStopTime = 0;
  trains[trainCount].stationTimer = 0;
  trains[trainCount].color = GREEN;
  trains[trainCount].atStation = false;
  trainCount++;
}

// Mock functions to simulate thread behavior of CBTC components
void *runCentralControlSystem(void *arg) {
  addLog("Central Control System started");
  
  while (1) {
    pthread_mutex_lock(&stateMutex);
    
    // Issue movement authorities based on train positions
    for (int i = 0; i < signalCount; i++) {
      bool trainNearby = false;
      
      // Check if any train is in the next few sections
      for (int j = 0; j < trainCount; j++) {
        if (trains[j].zoneId == signals[i].zoneId && 
            abs(trains[j].section - signals[i].section) <= 2) {
          trainNearby = true;
          break;
        }
      }
      
      // Set signal states based on train positions
      if (trainNearby) {
        signals[i].state = 0; // RED
      } else {
        signals[i].state = 2; // GREEN
      }
    }
    
    pthread_mutex_unlock(&stateMutex);
    
    // Simulate CCS commands and decision making
    char logMsg[100];
    int randomTrain = rand() % trainCount;
    int randomSpeed = (rand() % 4) * 20; // 0, 20, 40, 60
    
    snprintf(logMsg, sizeof(logMsg), "CCS: Setting train %d target speed to %d", 
             trains[randomTrain].id, randomSpeed);
    addLog(logMsg);
    
    pthread_mutex_lock(&stateMutex);
    trains[randomTrain].targetSpeed = randomSpeed;
    pthread_mutex_unlock(&stateMutex);
    
    sleep(5); // Issue commands every 5 seconds
  }
  return NULL;
}

void *runZoneController(void *arg) {
  int zoneId = *((int*)arg);
  char logMsg[100];
  snprintf(logMsg, sizeof(logMsg), "Zone Controller %d started", zoneId);
  addLog(logMsg);
  
  while (1) {
    pthread_mutex_lock(&stateMutex);
    
    // Update MA for trains in this zone
    for (int i = 0; i < trainCount; i++) {
      if (trains[i].zoneId == zoneId) {
        // Check signals affecting this train
        for (int j = 0; j < signalCount; j++) {
          if (signals[j].zoneId == zoneId && 
              trains[i].section == signals[j].section) {
            
            // Adjust train speed based on signal state
            switch(signals[j].state) {
              case 0: // RED
                trains[i].targetSpeed = 0;
                break;
              case 1: // YELLOW
                if (trains[i].targetSpeed > 30)
                  trains[i].targetSpeed = 30;
                break;
              case 2: // GREEN
                // Keep current target speed
                break;
            }
            
            snprintf(logMsg, sizeof(logMsg), 
                     "ZC %d: Signal %d is %s, Train %d target speed %d", 
                     zoneId, signals[j].id, 
                     signals[j].state == 0 ? "RED" : 
                     (signals[j].state == 1 ? "YELLOW" : "GREEN"),
                     trains[i].id, trains[i].targetSpeed);
            addLog(logMsg);
          }
        }
      }
    }
    
    pthread_mutex_unlock(&stateMutex);
    sleep(2);
  }
  return NULL;
}

void *runTrainSim(void *arg) {
  int trainIdx = *((int*)arg);
  char logMsg[100];
  snprintf(logMsg, sizeof(logMsg), "Train %d simulation started", 
           trains[trainIdx].id);
  addLog(logMsg);
  
  while (1) {
    pthread_mutex_lock(&stateMutex);
    Train *train = &trains[trainIdx];
    
    // Check if at station
    if (train->atStation) {
      // Update station timer
      if (train->stationTimer > 0) {
        train->stationTimer--;
        snprintf(logMsg, sizeof(logMsg), "Train %d waiting at station: %d seconds left", 
                 train->id, train->stationTimer);
        addLog(logMsg);
      } else {
        // Departure
        train->atStation = false;
        snprintf(logMsg, sizeof(logMsg), "Train %d departing station", train->id);
        addLog(logMsg);
      }
      pthread_mutex_unlock(&stateMutex);
      sleep(1);
      continue;
    }
    
    // Adjust speed toward target
    if (train->speed < train->targetSpeed) {
      train->speed += 5;
      if (train->speed > train->targetSpeed)
        train->speed = train->targetSpeed;
    } else if (train->speed > train->targetSpeed) {
      train->speed -= 10;
      if (train->speed < train->targetSpeed)
        train->speed = train->targetSpeed;
      if (train->speed < 0)
        train->speed = 0;
    }
    
    // Move train based on speed
    if (train->speed > 0) {
      float moveDistance = train->speed * 0.05; // Scale to visualization
      float dx = 0, dy = 0;
      
      // Find track segment the train is on
      for (int i = 0; i < trackSegmentCount; i++) {
        if (trackSegments[i].section == train->section) {
          Vector2 dir;
          Vector2 targetPos;
          
          if (train->direction == 1) {
            dir.x = trackSegments[i].end.x - trackSegments[i].start.x;
            dir.y = trackSegments[i].end.y - trackSegments[i].start.y;
            targetPos = trackSegments[i].end;
          } else {
            dir.x = trackSegments[i].start.x - trackSegments[i].end.x;
            dir.y = trackSegments[i].start.y - trackSegments[i].end.y;
            targetPos = trackSegments[i].start;
          }
          
          // Normalize direction
          float length = sqrt(dir.x * dir.x + dir.y * dir.y);
          if (length > 0) {
            dir.x /= length;
            dir.y /= length;
          }
          
          dx = dir.x * moveDistance;
          dy = dir.y * moveDistance;
          
          train->position.x += dx;
          train->position.y += dy;
          
          // Check if train reached end of segment
          Vector2 toEnd = {
            trackSegments[i].end.x - train->position.x,
            trackSegments[i].end.y - train->position.y
          };
          
          // If dot product of direction and toEnd is <= 0, we've reached or passed the end
          if (dir.x * toEnd.x + dir.y * toEnd.y <= 0) {
            train->position = trackSegments[i].end;
            
            // Find next section
            int nextSection = -1;
            
            // Check for switch points
            for (int j = 0; j < switchCount; j++) {
              if (switches[j].section == train->section) {
                if (train->direction == 1) {
                  if (switches[j].state == 0) { // NORMAL
                    // Follow normal path (usually straight)
                    for (int k = 0; k < trackSegmentCount; k++) {
                      if (trackSegments[k].start.x == trackSegments[i].end.x &&
                          trackSegments[k].start.y == trackSegments[i].end.y &&
                          trackSegments[k].section != train->section) {
                        nextSection = trackSegments[k].section;
                        break;
                      }
                    }
                  } else { // REVERSE
                    // Follow diverging path
                    for (int k = 0; k < trackSegmentCount; k++) {
                      if (trackSegments[k].start.x == trackSegments[i].end.x &&
                          trackSegments[k].start.y == trackSegments[i].end.y &&
                          trackSegments[k].zoneId == train->zoneId &&
                          trackSegments[k].section != train->section) {
                        nextSection = trackSegments[k].section;
                        // Use different logic to select diverging route
                        if (nextSection > 20) { // Branch line sections
                          break;
                        }
                      }
                    }
                  }
                } else {
                  if (switches[j].state == 0) { // NORMAL
                    // Follow normal path (usually straight)
                    for (int k = 0; k < trackSegmentCount; k++) {
                      if (trackSegments[i].start.x == trackSegments[k].end.x &&
                          trackSegments[i].start.y == trackSegments[k].end.y &&
                          trackSegments[k].section != train->section) {
                        nextSection = trackSegments[k].section;
                        break;
                      }
                    }
                  } else { // REVERSE
                    // Follow diverging path
                    for (int k = 0; k < trackSegmentCount; k++) {
                      if (trackSegments[i].start.x == trackSegments[k].end.x &&
                          trackSegments[i].start.y == trackSegments[k].end.y &&
                          trackSegments[k].zoneId == train->zoneId &&
                          trackSegments[k].section != train->section) {
                        nextSection = trackSegments[k].section;
                        // Use different logic to select diverging route
                        if (nextSection > 20) { // Branch line sections
                          break;
                        }
                      }
                    }
                  }
                }
                break;
              }
            }
            
            // If not at a switch point, find the next connecting segment
            if (nextSection == -1) {
              for (int k = 0; k < trackSegmentCount; k++) {
                if (train->direction == 1) {
                  // Forward: look for segment whose start connects to current segment's end
                  if (trackSegments[k].start.x == trackSegments[i].end.x &&
                      trackSegments[k].start.y == trackSegments[i].end.y &&
                      trackSegments[k].section != train->section) {
                    nextSection = trackSegments[k].section;
                    break;
                  }
                } else {
                  // Backward: look for segment whose end connects to current segment's start
                  if (trackSegments[k].end.x == trackSegments[i].start.x &&
                      trackSegments[k].end.y == trackSegments[i].start.y &&
                      trackSegments[k].section != train->section) {
                    nextSection = trackSegments[k].section;
                    break;
                  }
                }
              }
            }

            // If no next section is found, reverse direction
            if (nextSection == -1) {
              train->direction *= -1; // Reverse direction
              snprintf(logMsg, sizeof(logMsg), "Train %d reached %s of line, reversing direction",
                       train->id, train->direction == 1 ? "beginning" : "end");
              addLog(logMsg);
              
              // The current section is still valid, just going the other way now
              break;
            }
            
            // Update train section if we found a next section
            if (nextSection != -1) {
              snprintf(logMsg, sizeof(logMsg), "Train %d moved: Section %d -> %d",
                       train->id, train->section, nextSection);
              addLog(logMsg);
              
              train->section = nextSection;
              
              // Update zone if necessary
              for (int k = 0; k < trackSegmentCount; k++) {
                if (trackSegments[k].section == nextSection) {
                  train->zoneId = trackSegments[k].zoneId;
                  break;
                }
              }
              
              // Check if arrived at station
              for (int k = 0; k < stationCount; k++) {
                if (stations[k].section == train->section) {
                  snprintf(logMsg, sizeof(logMsg), 
                           "Train %d arriving at %s station",
                           train->id, stations[k].name);
                  addLog(logMsg);
                  
                  train->atStation = true;
                  train->stationStopTime = stations[k].stopTime;
                  train->stationTimer = train->stationStopTime;
                  train->speed = 0;
                  break;
                }
              }
            }
            break;
          }
          
          break;
        }
      }
    }
    
    pthread_mutex_unlock(&stateMutex);
    usleep(50000); // 50ms update
  }
  return NULL;
}

int main() {
  // Initialize window
  InitWindow(1000, 600, "CBTC Network Simulation");
  SetTargetFPS(60);
  
  // Initialize track layout
  initializeTrackLayout();
  
  // Start threads for CBTC components
  pthread_t ccsThread, zoneThreads[MAX_ZONES], trainThreads[MAX_TRAINS];
  int zoneIds[MAX_ZONES] = {1, 2, 3};
  int trainIndices[MAX_TRAINS] = {0, 1, 2};
  
  pthread_create(&ccsThread, NULL, runCentralControlSystem, NULL);
  
  for (int i = 0; i < MAX_ZONES; i++) {
    pthread_create(&zoneThreads[i], NULL, runZoneController, &zoneIds[i]);
  }
  
  for (int i = 0; i < trainCount; i++) {
    pthread_create(&trainThreads[i], NULL, runTrainSim, &trainIndices[i]);
  }
  
  // Main render loop
  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    
    // pthread_mutex_lock(&stateMutex);
    
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
    for (int i = 0; i < signalCount; i++) {
      Color signalColor;
      switch(signals[i].state) {
        case 0: signalColor = RED; break;
        case 1: signalColor = YELLOW; break;
        case 2: signalColor = GREEN; break;
        default: signalColor = GRAY;
      }
      DrawCircle(signals[i].position.x, signals[i].position.y, 6, signalColor);
      DrawCircleLines(signals[i].position.x, signals[i].position.y, 6, BLACK);
    }
    
    // Draw switches
    for (int i = 0; i < switchCount; i++) {
      if (switches[i].state == 0) { // NORMAL
        DrawRectangleRec(switches[i].switchNormal, DARKGREEN);
        DrawRectangleLinesEx(switches[i].switchNormal, 1, BLACK);
        DrawRectangleRec(switches[i].switchReverse, GRAY);
        DrawRectangleLinesEx(switches[i].switchReverse, 1, DARKGRAY);
      } else { // REVERSE
        DrawRectangleRec(switches[i].switchNormal, GRAY);
        DrawRectangleLinesEx(switches[i].switchNormal, 1, DARKGRAY);
        DrawRectangleRec(switches[i].switchReverse, DARKGREEN);
        DrawRectangleLinesEx(switches[i].switchReverse, 1, BLACK);
      }
    }
    
    // Draw trains
    for (int i = 0; i < trainCount; i++) {
      DrawCircle(trains[i].position.x, trains[i].position.y, TRAIN_SIZE, 
                 trains[i].color);
                 
      // Draw a small direction indicator
      float dirX = trains[i].direction * 8;
      DrawTriangle(
        (Vector2){trains[i].position.x + dirX, trains[i].position.y},
        (Vector2){trains[i].position.x - dirX/2, trains[i].position.y - 5},
        (Vector2){trains[i].position.x - dirX/2, trains[i].position.y + 5},
        trains[i].color
      );
      
      DrawCircleLines(trains[i].position.x, trains[i].position.y, TRAIN_SIZE, BLACK);
      
      char trainInfo[40];
      sprintf(trainInfo, "%d (%d km/h) %s", trains[i].id, trains[i].speed, 
              trains[i].direction == 1 ? "→" : "←");
      DrawText(trainInfo, trains[i].position.x - 20, 
               trains[i].position.y - 25, 10, BLACK);
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
    
    for (int i = 0; i < logCount; i++) {
      DrawText(logs[i], 30, 450 + i * 20, 10, BLACK);
    }
    
    pthread_mutex_unlock(&stateMutex);
    
    // Draw help text
    DrawText("Railway CBTC Simulation", 30, 30, 24, BLACK);
    DrawText("Trains move automatically based on movement authorities", 
             30, 60, 16, DARKGRAY);
    DrawText("Trains stop at stations for the designated time", 
             30, 80, 16, DARKGRAY);
    
    EndDrawing();
  }
  
  // Cleanup
  CloseWindow();
  return 0;
}

