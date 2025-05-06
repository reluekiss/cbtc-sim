#include "esp32_hw.h"

// Define IRQ handlers
static void (*irq_handlers[ESP32_IRQ_COUNT])(void* arg);
static void* irq_args[ESP32_IRQ_COUNT];

void esp_intr_init(void) {
    // Initialize interrupt controller hardware
    esp_intr_controller_init();
    
    // Clear all handler registrations
    for (int i = 0; i < ESP32_IRQ_COUNT; i++) {
        irq_handlers[i] = NULL;
        irq_args[i] = NULL;
    }
    
    // Set up CPU exceptions
    esp_cpu_set_exception_handlers();
    
    // Enable interrupts
    esp_cpu_intr_enable();
}

// Register an interrupt handler
int esp_intr_alloc(int source, int flags, void (*handler)(void* arg), void* arg) {
    if (source >= ESP32_IRQ_COUNT || !handler) {
        return -1;
    }
    
    // Disable interrupts while modifying handler table
    esp_cpu_intr_disable();
    
    irq_handlers[source] = handler;
    irq_args[source] = arg;
    
    // Configure the interrupt in hardware
    esp_intr_set_priority(source, (flags >> 16) & 0xF);
    esp_intr_enable(source);
    
    // Enable interrupts
    esp_cpu_intr_enable();
    
    return 0;
}

// This is called from assembly when an interrupt occurs
void esp_dispatch_irq(int irq_num) {
    if (irq_num < ESP32_IRQ_COUNT && irq_handlers[irq_num]) {
        irq_handlers[irq_num](irq_args[irq_num]);
    }
}
