#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif

#include "lab.h"

#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes)
{
    //DO NOT use math.pow
    size_t k = 0;
    size_t size = 1;
    
    while (size < bytes) {
        size <<= 1;
        k++;
    }
    
    return k;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    // Calculate the buddy address by XORing the appropriate bit
    size_t kval = buddy->kval;
    uintptr_t block_addr = (uintptr_t)buddy;
    uintptr_t base_addr = (uintptr_t)pool->base;
    
    // Calculate the offset from the base
    uintptr_t offset = block_addr - base_addr;
    
    // XOR the appropriate bit to find the buddy
    offset ^= (UINT64_C(1) << kval);
    
    // Return the buddy address
    return (struct avail *)(base_addr + offset);

}

void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    if (pool == NULL || size == 0) {
        return NULL;
    }

    //get the kval for the requested size with enough room for the tag and kval fields
    size_t header_size = sizeof(struct avail);
    size_t total_size = size + header_size;
    size_t kval = btok(total_size);
    
    // Ensure we don't go below the minimum size
    if (kval < SMALLEST_K) {
        kval = SMALLEST_K;
    }

    //R1 Find a block
    size_t j;
    struct avail *block = NULL;
    for (j = kval; j <= pool->kval_m; j++) {
        if (pool->avail[j].next != &pool->avail[j]) {
            // Found an available block
            block = pool->avail[j].next;
            break;
        }
    }

    //There was not enough memory to satisfy the request thus we need to set error and return NULL
    if (j > pool->kval_m || block == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    //R2 Remove from list;
    block->next->prev = block->prev;
    block->prev->next = block->next;

    //R3 Split required?
    while (j > kval) {
        //R4 Split the block
        j--;
        
        // Calculate the buddy address (buddy will be at block + 2^j)
        struct avail *buddy = (struct avail *)((uintptr_t)block + (UINT64_C(1) << j));
        
        // Setup buddy metadata
        buddy->kval = j;
        buddy->tag = BLOCK_AVAIL;
        
        // Add buddy to the available list
        buddy->next = pool->avail[j].next;
        buddy->prev = &pool->avail[j];
        pool->avail[j].next->prev = buddy;
        pool->avail[j].next = buddy;
    }

    // Mark block as reserved
    block->tag = BLOCK_RESERVED;
    block->kval = kval;

    // Return the user memory area (after the header)
    return (void *)((uintptr_t)block + header_size);
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    if (pool == NULL || ptr == NULL) {
        return;
    }

    // Get the header address from user pointer
    struct avail *block = (struct avail *)((uintptr_t)ptr - sizeof(struct avail));
    
    // Verify this is a valid block
    if (block->tag != BLOCK_RESERVED) {
        // Invalid memory block
        return;
    }

    size_t kval = block->kval;
    
    // Mark as available
    block->tag = BLOCK_AVAIL;

    // Try to coalesce with buddy
    while (kval < pool->kval_m) {
        // Find buddy
        struct avail *buddy = buddy_calc(pool, block);
        
        // Check if buddy is available and of the same size
        if (buddy->tag != BLOCK_AVAIL || buddy->kval != kval) {
            break;
        }
        
        // Remove buddy from its avail list
        buddy->next->prev = buddy->prev;
        buddy->prev->next = buddy->next;
        
        // Determine which block (original or buddy) is lower in memory
        // The combined block starts at the lower address
        if ((uintptr_t)buddy < (uintptr_t)block) {
            block = buddy;
        }
        
        // Increase the size
        kval++;
        block->kval = kval;
    }

    // Add block to the appropriate avail list
    block->next = pool->avail[kval].next;
    block->prev = &pool->avail[kval];
    pool->avail[kval].next->prev = block;
    pool->avail[kval].next = block;
}

/**
 * @brief This is a simple version of realloc.
 *
 * @param poolThe memory pool
 * @param ptr  The user memory
 * @param size the new size requested
 * @return void* pointer to the new user memory
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    //Required for Grad Students
    //Optional for Undergrad Students
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool,0,sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
static void printb(unsigned long int b)
{
     size_t bits = sizeof(b) * 8;
     unsigned long int curr = UINT64_C(1) << (bits - 1);
     for (size_t i = 0; i < bits; i++)
     {
          if (b & curr)
          {
               printf("1");
          }
          else
          {
               printf("0");
          }
          curr >>= 1L;
     }
}
