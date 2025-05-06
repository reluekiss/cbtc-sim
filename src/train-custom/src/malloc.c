#include "libc.h"
#include "esp32_hw.h"

#define HEAP_SIZE (256 * 1024) // 256 KB heap

// Simple block structure
typedef struct block {
    size_t size;
    int free;
    struct block* next;
} block_t;

static uint8_t heap[HEAP_SIZE] __attribute__((aligned(8)));
static block_t* head = NULL;

void heap_init(void) {
    head = (block_t*)heap;
    head->size = HEAP_SIZE - sizeof(block_t);
    head->free = 1;
    head->next = NULL;
}

void* malloc(size_t size) {
    // Align size to 8 bytes
    size = (size + 7) & ~7;
    
    block_t* current = head;
    block_t* prev = NULL;
    
    // First-fit allocation strategy
    while (current) {
        if (current->free && current->size >= size) {
            // Split if block is significantly larger
            if (current->size > size + sizeof(block_t) + 8) {
                block_t* new_block = (block_t*)((uint8_t*)current + sizeof(block_t) + size);
                new_block->size = current->size - size - sizeof(block_t);
                new_block->free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->free = 0;
            return (void*)((uint8_t*)current + sizeof(block_t));
        }
        
        prev = current;
        current = current->next;
    }
    
    // No suitable block found
    return NULL;
}

void free(void* ptr) {
    if (!ptr) return;
    
    // Get block header
    block_t* block = (block_t*)((uint8_t*)ptr - sizeof(block_t));
    block->free = 1;
    
    // Coalesce with next block if free
    if (block->next && block->next->free) {
        block->size += sizeof(block_t) + block->next->size;
        block->next = block->next->next;
    }
    
    // Coalesce with previous block if free
    // This is more complex and would require a doubly-linked list
    // or a full scan from the head, which I'm omitting for brevity
}
