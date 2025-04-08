#include <assert.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "harness/unity.h"
#include "../src/lab.h"


void setUp(void) {
  // set stuff up here
}

void tearDown(void) {
  // clean stuff up here
}



/**
 * Check the pool to ensure it is full.
 */
void check_buddy_pool_full(struct buddy_pool *pool)
{
  //A full pool should have all values 0-(kval-1) as empty
  for (size_t i = 0; i < pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }

  //The avail array at kval should have the base block
  assert(pool->avail[pool->kval_m].next->tag == BLOCK_AVAIL);
  assert(pool->avail[pool->kval_m].next->next == &pool->avail[pool->kval_m]);
  assert(pool->avail[pool->kval_m].prev->prev == &pool->avail[pool->kval_m]);

  //Check to make sure the base address points to the starting pool
  //If this fails either buddy_init is wrong or we have corrupted the
  //buddy_pool struct.
  assert(pool->avail[pool->kval_m].next == pool->base);
}

/**
 * Check the pool to ensure it is empty.
 */
void check_buddy_pool_empty(struct buddy_pool *pool)
{
  //An empty pool should have all values 0-(kval) as empty
  for (size_t i = 0; i <= pool->kval_m; i++)
    {
      assert(pool->avail[i].next == &pool->avail[i]);
      assert(pool->avail[i].prev == &pool->avail[i]);
      assert(pool->avail[i].tag == BLOCK_UNUSED);
      assert(pool->avail[i].kval == i);
    }
}

/**
 * Test allocating 1 byte to make sure we split the blocks all the way down
 * to MIN_K size. Then free the block and ensure we end up with a full
 * memory pool again
 */
void test_buddy_malloc_one_byte(void)
{
  fprintf(stderr, "->Test allocating and freeing 1 byte\n");
  struct buddy_pool pool;
  int kval = MIN_K;
  size_t size = UINT64_C(1) << kval;
  buddy_init(&pool, size);
  void *mem = buddy_malloc(&pool, 1);
  //Make sure correct kval was allocated
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/**
 * Tests the allocation of one massive block that should consume the entire memory
 * pool and makes sure that after the pool is empty we correctly fail subsequent calls.
 */
void test_buddy_malloc_one_large(void)
{
  fprintf(stderr, "->Testing size that will consume entire memory pool\n");
  struct buddy_pool pool;
  size_t bytes = UINT64_C(1) << MIN_K;
  buddy_init(&pool, bytes);

  //Ask for an exact K value to be allocated. This test makes assumptions on
  //the internal details of buddy_init.
  size_t ask = bytes - sizeof(struct avail);
  void *mem = buddy_malloc(&pool, ask);
  assert(mem != NULL);

  //Move the pointer back and make sure we got what we expected
  struct avail *tmp = (struct avail *)mem - 1;
  assert(tmp->kval == MIN_K);
  assert(tmp->tag == BLOCK_RESERVED);
  check_buddy_pool_empty(&pool);

  //Verify that a call on an empty tool fails as expected and errno is set to ENOMEM.
  void *fail = buddy_malloc(&pool, 5);
  assert(fail == NULL);
  assert(errno = ENOMEM);

  //Free the memory and then check to make sure everything is OK
  buddy_free(&pool, mem);
  check_buddy_pool_full(&pool);
  buddy_destroy(&pool);
}

/**
 * Tests to make sure that the struct buddy_pool is correct and all fields
 * have been properly set kval_m, avail[kval_m], and base pointer after a
 * call to init
 */
void test_buddy_init(void)
{
  fprintf(stderr, "->Testing buddy init\n");
  //Loop through all kval MIN_k-DEFAULT_K and make sure we get the correct amount allocated.
  //We will check all the pointer offsets to ensure the pool is all configured correctly
  for (size_t i = MIN_K; i <= DEFAULT_K; i++)
    {
      size_t size = UINT64_C(1) << i;
      struct buddy_pool pool;
      buddy_init(&pool, size);
      check_buddy_pool_full(&pool);
      buddy_destroy(&pool);
    }
}

/**
 * Tests the buddy calculation function to ensure it correctly identifies
 * the buddy block at different k-values
 */
 void test_buddy_calc(void)
 {
   fprintf(stderr, "->Testing buddy_calc function\n");
   struct buddy_pool pool;
   buddy_init(&pool, 0); // Use default size
   
   // First, allocate two blocks of the same size
   size_t test_sizes[] = {64, 128, 256, 512};
   
   for (int i = 0; i < 4; i++) {
     size_t size = test_sizes[i];
     void *ptr1 = buddy_malloc(&pool, size);
     void *ptr2 = buddy_malloc(&pool, size);
     
     assert(ptr1 != NULL);
     assert(ptr2 != NULL);
     
     // Calculate headers
     struct avail *block1 = (struct avail *)((uintptr_t)ptr1 - sizeof(struct avail));
     struct avail *block2 = (struct avail *)((uintptr_t)ptr2 - sizeof(struct avail));
     
     // Verify buddy calculation
     struct avail *buddy1 = buddy_calc(&pool, block1);
     struct avail *buddy2 = buddy_calc(&pool, block2);
     
     // Due to how allocations work, these may not be each other's buddies
     // but we can verify that calculating buddy of buddy gives original block
     struct avail *orig1 = buddy_calc(&pool, buddy1);
     struct avail *orig2 = buddy_calc(&pool, buddy2);
     
     assert(orig1 == block1);
     assert(orig2 == block2);
     
     buddy_free(&pool, ptr1);
     buddy_free(&pool, ptr2);
   }
   
   buddy_destroy(&pool);
 }
 
 /**
  * Tests allocating multiple blocks of various sizes and ensures they don't overlap
  */
 void test_buddy_malloc_multiple(void)
 {
   fprintf(stderr, "->Testing multiple allocations of different sizes\n");
   struct buddy_pool pool;
   buddy_init(&pool, 0);
   
   // Allocate blocks of different sizes
   void *ptrs[5];
   size_t sizes[] = {32, 64, 128, 256, 512};
   
   for (int i = 0; i < 5; i++) {
     ptrs[i] = buddy_malloc(&pool, sizes[i]);
     assert(ptrs[i] != NULL);
     
     // Write to memory to ensure we can use it
     memset(ptrs[i], i+1, sizes[i]);
   }
   
   // Verify contents to ensure no overlapping occurred
   for (int i = 0; i < 5; i++) {
     unsigned char *bytes = (unsigned char *)ptrs[i];
     for (size_t j = 0; j < sizes[i]; j++) {
       assert(bytes[j] == (unsigned char)(i+1));
     }
   }
   
   // Free the blocks
   for (int i = 0; i < 5; i++) {
     buddy_free(&pool, ptrs[i]);
   }
   
   // Pool should be full again after freeing all blocks
   check_buddy_pool_full(&pool);
   buddy_destroy(&pool);
 }
 
 /**
  * Tests the merging of buddies when blocks are freed
  */
 void test_buddy_free_coalesce(void)
 {
   fprintf(stderr, "->Testing buddy coalescing when freeing blocks\n");
   struct buddy_pool pool;
   size_t pool_size = UINT64_C(1) << 24; // 16 MiB
   buddy_init(&pool, pool_size);
   
   // Allocate multiple small blocks that should come from the same large block
   // We'll allocate 8 blocks of 1KB each
   void* blocks[8];
   for (int i = 0; i < 8; i++) {
     blocks[i] = buddy_malloc(&pool, 1024);
     assert(blocks[i] != NULL);
     
     // Mark the memory so we can identify it
     memset(blocks[i], 0xAA, 1024);
   }
   
   // Free the blocks in an order that should trigger coalescing
   // Free blocks 0,1 (should coalesce)
   // Then 2,3 (should coalesce)
   // Then the coalesced 0+1 and 2+3 (should coalesce again)
   // And so on
   buddy_free(&pool, blocks[0]);
   buddy_free(&pool, blocks[1]);
   buddy_free(&pool, blocks[2]);
   buddy_free(&pool, blocks[3]);
   buddy_free(&pool, blocks[4]);
   buddy_free(&pool, blocks[5]);
   buddy_free(&pool, blocks[6]);
   buddy_free(&pool, blocks[7]);
   
   // After freeing all blocks in the right order, we should be able to allocate
   // one large block that spans the entire space those 8 blocks used
   void *large_block = buddy_malloc(&pool, 8192);
   assert(large_block != NULL);
   
   buddy_free(&pool, large_block);
   check_buddy_pool_full(&pool);
   buddy_destroy(&pool);
 }
 
 /**
  * Test that NULL and invalid pointers are handled properly in buddy_free
  */
 void test_buddy_free_edge_cases(void)
 {
   fprintf(stderr, "->Testing edge cases for buddy_free\n");
   struct buddy_pool pool;
   buddy_init(&pool, 0);
   
   // Free NULL pointer should not crash
   buddy_free(&pool, NULL);
   
   // Free with NULL pool should not crash
   void *ptr = buddy_malloc(&pool, 64);
   assert(ptr != NULL);
   buddy_free(NULL, ptr);
   
   // Free the pointer properly now
   buddy_free(&pool, ptr);
   
   // Attempt double-free (should be handled gracefully)
   buddy_free(&pool, ptr);
   
   check_buddy_pool_full(&pool);
   buddy_destroy(&pool);
 }
 
 /**
  * Tests allocating blocks at the boundary of k values
  */
 void test_buddy_malloc_boundaries(void)
 {
   fprintf(stderr, "->Testing allocations at k-value boundaries\n");
   struct buddy_pool pool;
   buddy_init(&pool, 0);
   
   // Test allocating exact powers of 2 minus header size
   size_t header_size = sizeof(struct avail);
   
   // These should all fit exactly in their respective k values
   size_t sizes[] = {
     (1 << 6) - header_size,  // k=6
     (1 << 7) - header_size,  // k=7
     (1 << 8) - header_size,  // k=8
     (1 << 9) - header_size   // k=9
   };
   
   void *ptrs[4];
   
   for (int i = 0; i < 4; i++) {
     ptrs[i] = buddy_malloc(&pool, sizes[i]);
     assert(ptrs[i] != NULL);
     
     // Check that the block has the expected k value
     struct avail *block = (struct avail *)((uintptr_t)ptrs[i] - header_size);
     assert(block->kval == (i + 6));
     
     // Write to the memory to ensure it's usable
     memset(ptrs[i], 0xBB, sizes[i]);
   }
   
   // Free everything
   for (int i = 0; i < 4; i++) {
     buddy_free(&pool, ptrs[i]);
   }
   
   check_buddy_pool_full(&pool);
   buddy_destroy(&pool);
 }


int main(void) {
  time_t t;
  unsigned seed = (unsigned)time(&t);
  fprintf(stderr, "Random seed:%d\n", seed);
  srand(seed);
  printf("Running memory tests.\n");

  UNITY_BEGIN();
  RUN_TEST(test_buddy_init);
  RUN_TEST(test_buddy_malloc_one_byte);
  RUN_TEST(test_buddy_malloc_one_large);
  RUN_TEST(test_buddy_calc);
  RUN_TEST(test_buddy_malloc_multiple);
  RUN_TEST(test_buddy_free_coalesce);
  RUN_TEST(test_buddy_free_edge_cases);
  RUN_TEST(test_buddy_malloc_boundaries);

return UNITY_END();
}
