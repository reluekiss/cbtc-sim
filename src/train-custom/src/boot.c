// boot.c
#include "esp32_hw.h"
#include "libc.h"

extern void kernel_main(void);

// This gets called by ROM bootloader
void call_start_cpu0(void) {
    // Initialize CPU
    esp_cpu_init();
    
    // Set up the interrupt controller
    esp_intr_init();
    
    // Initialize clock and peripheral systems
    esp_clk_init();
    esp_periph_init();
    
    // Initialize heap memory
    heap_init();
    
    // Call the main application
    kernel_main();
    
    while(1) {
        esp_cpu_wait_for_intr();
    }
}

void call_start_cpu1(void) {
    while(1) {
        esp_cpu_wait_for_intr();
    }
}
