#include "libc.h"
#include "netstack.h"
#include "esp32_hw.h"

// Configuration constants
#define WIFI_SSID "TrainNet"
#define WIFI_PASSWORD "traincontrol123"

// Function to adapt main() function from your train program
extern int train_main(int argc, char **argv);

void kernel_main(void) {
    printf("Train Control System starting on ESP32...\n");
    
    // Initialize network
    printf("Connecting to WiFi network: %s\n", WIFI_SSID);
    net_init(WIFI_SSID, WIFI_PASSWORD);
    printf("WiFi connected!\n");
    
    // Set up arguments for the train program
    char *args[] = {
        "train",       // Program name
        "1",           // Train ID
        "1",           // Zone ID
        "1",           // Initial section
        "192.168.1.1", // Zone controller IP
        NULL
    };
    
    // Call the adapted train program
    train_main(5, args);
    
    // Should never reach here
    printf("Train application exited!\n");
    while(1) {
        esp_delay_ms(1000);
    }
}
