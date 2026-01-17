#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#include "alloc_track.h"

struct chunk {

  void *block;
  
  // Double direction is easier to implememt.
  struct chunk *prev;
  struct chunk *next;
};

// The head always contains the most-recently
// allocated chunk.
//
// Make this simple: just space for 100 threads tops.
// Increase if you need it bigger.
//
#define MAX_TRACKING 256
struct chunk *tracking_head[MAX_TRACKING];
int allocations[MAX_TRACKING];

// We do this function pointer song and dance
// so that we only have one allocation implementation
// to deal with.
void * malloc_wrapper(size_t size, size_t unused) {

  return malloc(size);
}

void * calloc_wrapper(size_t nmemb, size_t size) {

  return calloc(nmemb, size);
}

int alloc_track_insane(void) {

  int j;
  struct chunk *list_cur, *prev, *next;
  int count;
  // int headcount;

  // headcount = 0;

#ifdef _OPENMP
  j = omp_get_thread_num();
#else
  j = 0;
#endif

  count = 0;
    
  // Skip if its never been allocated
  if(!tracking_head[j])
    return 0;
  
  // Walk down the list
  list_cur = tracking_head[j];
  prev = NULL;
    
  while(list_cur) {

    // Use to verify allocations here
    ++count;

    // Make sure that our left is correct
    assert(list_cur->prev == prev);

    // Make sure that we don't hold a null pointer
    assert(list_cur->block);
    
    // Advance
    prev = list_cur;
    list_cur = list_cur->next;
  }

  // Verify that the number of allocations is equal to what we think it is
  assert(count == allocations[j]);    
  return 0;
}

// This method uses the requested allocator and tracks the
// returned pointer at the head of the list.
//
// We use the head of the list because the most frequent usage is to
// free memory in the reversed order from which you allocated it.
// So most of the time, we are landing right on the block we want
// to free.
void * tracked_alloc_core( void * (*allocation_wrapper)(size_t, size_t),
			   size_t arg1,
			   size_t arg2) { 

  struct chunk *achunk;
  void *ptr;
  int me = 0;
  
  // Get a chunk to keep track of
  achunk = (struct chunk *)malloc(sizeof(struct chunk));
  if(!achunk)
    return 0x0;

  // We use the desired call here
  ptr = allocation_wrapper(arg1, arg2);
  
  if(!ptr) {

    // It failed, so deallocate achunk and return 0x0
    free(achunk);
    return 0x0;
  }

  // Track it
  achunk->block = ptr;

#ifdef _OPENMP
  me = omp_get_thread_num();
#endif

  // Does the table exist yet?
  if(!tracking_head[me]) {

    // Nope.  This becomes the head
    tracking_head[me] = achunk;
    achunk->prev = NULL;
    achunk->next = NULL;

    // KC 7/21/25
    // And set the allocations to zero...
    allocations[me] = 0;
  }
  else {
      
    // Table exists already.
    // Attach new chunk ****AT THE HEAD****
    achunk->next = tracking_head[me];
    achunk->prev = NULL;
    tracking_head[me]->prev = achunk;
    tracking_head[me] = achunk;
  }
  ++allocations[me];
  
  // Return the newly allocated block
  return ptr;
}
			  
void * tracked_malloc(size_t size) {

  return tracked_alloc_core(malloc_wrapper, size, 0);
}

void * tracked_calloc(size_t nmemb, size_t size) {

  return tracked_alloc_core(calloc_wrapper, nmemb, size);
}

void tracked_free(void *ptr) {

  struct chunk *list_cur;
  int pos = 0;
  int me = 0;
  int *debug = 0x0;
  pid_t pid;
  
  // Mimic POSIX free() behaviour on null pointeres
  if(!ptr)
    return;

#ifdef _OPENMP
  me = omp_get_thread_num();
#endif

  list_cur = tracking_head[me];

  // Check for empty list!
  if(!list_cur) {

    // Force a segfault
    fprintf(stderr, "tracked_free(): attempting to free unknown %p from an empty list\n", ptr);
    // walk_chunks();
    *debug = 1;
  }

  // Search the list until we find it
  while(list_cur->block != ptr) {

    // Move to the right if we can
    if(list_cur->next != NULL) {

      ++pos;

      // Did we just run off the edge?
      if(pos == allocations[me]) {
	pid = getpid();
	fprintf(stderr, "THREAD %d] tracked_free(): table corruption!  we just ran off the edge of the list (position %d)\n", me, pos);
	// walk_chunks();
	*debug = 1;
      }

      list_cur = list_cur->next;
    }
    else {
      // Force a segfault
      fprintf(stderr, "tracked_free(): requested to free 0x%p, but this address is not tracked!\n", ptr);
      // walk_chunks();
      *debug = 1;
    }
  }

  // We have found the right chunk.
  // Unlink it
  // Three cases:
  
  if(list_cur == tracking_head[me]) {

    // We are at the head of the list.

    // SANITY CHECK
    if(list_cur->prev != NULL) {

      fprintf(stderr, "tracked_free(): the head of the list has a non-null record to the left!\n");
      // walk_chunks();
      printf("%d", *(int *)(0x0));
    }

    // Are we the only one?
    if(list_cur->next == NULL) {

      // Yes.  We only had one thing.  Better check.
      assert(allocations[me] == 1);
      
      // Free the memory
      free(list_cur->block);

      // Free the accounting chunk associated to it
      free(list_cur);

      // Set the tracking head to empty
      tracking_head[me] = NULL;

      // Record that we freed memory
      --allocations[me];

      // Sanity check that allocations are zero
      assert(allocations[me] == 0);
    }
    else {

      // There is a subsequent one

      // Free the memory
      free(list_cur->block);

      // Unlink it 
      list_cur->next->prev = NULL;

      // Set the head to its next
      tracking_head[me] = list_cur->next;

      // Free the memory associated with the accounting structure
      free(list_cur);

      // Record that we freed memory
      --allocations[me];
    }
  }
  else {

    // We are either in the middle of the list,
    // or at the end (and we are never in first position).
    
    // Are we at the end?
    if(list_cur->next == NULL) {

      // Free its memory
      free(list_cur->block);

      // We can just unlink it
      list_cur->prev->next = NULL;

      // Now destroy the accounting structure
      free(list_cur);

      // Record that we freed memory
      --allocations[me];
    }
    else {

      // We are in the middle.

      // Unlink me
      list_cur->prev->next = list_cur->next;
      list_cur->next->prev = list_cur->prev;

      // Free the allocated memory
      free(list_cur->block);
      
      // Now free the accounting structure
      free(list_cur);

      // Record that we freed memory
      --allocations[me];
    }
  }
  return;
}

void * tracked_realloc(void *ptr, size_t size) {
  
  void *newblock;
  int me = 0;
  struct chunk *list_cur;

  // Mimic POSIX semantics.
  // realloc() is equivalent to malloc() if ptr is null
  if(!ptr)
    return tracked_malloc(size);

  // Trap on Linux semantics that we don't yet implement...
  if(!size && ptr) {
    abort();
  }
#ifdef _OPENMP
  me = omp_get_thread_num();
#endif

  list_cur = tracking_head[me];

  // Accounting structures don't need to change, just
  // what is being tracked is changing.

  // Search the list until we find it
  while(list_cur->block != ptr) {

    // Move to the right if we can
    if(list_cur->next != NULL)
      list_cur = list_cur->next;
    else {
      fprintf(stderr, "tracked_realloc(): requested to reallocate 0x%p, but this address is not tracked!\n", ptr);

      // Walk the chunks
      // walk_chunks();

      // Force a segfault
      printf("%d", *(int *)(0x0));
    }
  }

  // Call the underlying realloc()
  newblock = realloc(ptr, size);

  // KC 7/21/25
  // XXX
  // If realloc fails for some reason and gives me a null
  // pointer, then I need to unlink myself, otherwise
  // I will have an entry in the list that cannot be
  // removed (because its value will be 0x0)
  
  // WTF was I doing here before? LOL
  if(newblock != ptr)
    list_cur->block = newblock;

  return newblock;
}
		     
void tracked_free_all(void) {

  struct chunk *list_next;
  int i;
  int counter;
  
  // Clear out all allocations in parallel
  // Should probably just walk the arrays here
  for(i = 0; i < MAX_TRACKING; ++i) {

    if(!tracking_head[i])
      continue;

    counter = 0;
    
    while(tracking_head[i]) {

      ++counter;
      list_next = tracking_head[i]->next;
      free(tracking_head[i]->block);
      free(tracking_head[i]);
      tracking_head[i] = list_next;
      --allocations[i];
    }
    //printf("[Thread %d] alloc_track: Cleaned up %d allocations\n", i, counter);
  }
}

/* void walk_chunks(void) { */

/*   int i = 0; */
/*   struct chunk *achunk; */
/*   int me = 0; */
  
/*   printf("Total allocations open: %d\n", allocations); */

/* #ifdef _OPENMP */
/*   me = omp_get_thread_num(); */
/* #endif */

/*   // KC 6/24/24 */
/*   // This is an unused old debug function, but the way its currently */
/*   // written is broken. */
/*   achunk = tracking_head[me]; */

/*   // Walk forward */
/*   while(achunk) {  */
/*     printf("(forward) Position %d: memory at %p, contains %d\n", i++, achunk->block, *(int *)(achunk->block)); */

/*     if(achunk->next != NULL) */
/*       achunk = achunk->next; */
/*     else */
/*       break; */
/*   } */

/*   // Walk backward */
/*   while(achunk) { */

/*     printf("(backward) Position %d: memory at %p, contains %d\n", --i, achunk->block, *(int *)(achunk->block)); */

/*     if(achunk->prev != NULL) */
/*       achunk = achunk->prev; */
/*     else */
/*       break; */
/*   } */
/* } */

#ifdef DEBUG_ALLOC_TRACK
#define TEST_LEN 5
int main(int argc, char **argv) {

  int i;
  int *ptr;
  void *addrs[TEST_LEN];
  
  for(i=0; i < TEST_LEN; ++i) {
    ptr = tracked_malloc(sizeof(int));
    addrs[i] = ptr;
    *ptr = i;
  }

  // walk_chunks();

  // Looks good.

  printf("Removing item in the (zero-indexed) 2nd position\n");
  tracked_free(addrs[2]);
  // walk_chunks();

  // Looks good.

  printf("Removing item at the head of the list (most recent addition)\n");
  tracked_free(addrs[TEST_LEN-1]);
  // walk_chunks();

  // Looks good.

  printf("Removing item at the tail of the list (oldest addition)\n");
  tracked_free(addrs[0]);
  // walk_chunks();

  // Looks good.

  printf("Removing 2nd added item\n");
  tracked_free(addrs[1]);
  // walk_chunks();

  printf("Removing 4th added item\n");
  tracked_free(addrs[3]);
  // walk_chunks();
  
  tracked_free_all();
  
  return 0;
}
#endif
