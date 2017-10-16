#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>

#define SYSTEM_MALLOC 0
#define MIN_SIZE sizeof(Node *) + sizeof(int)

/* Node: structure that represents a node in the linked list,
 contains the reference to the next node and the size of the
 free space.
 */

typedef struct Node {
    unsigned int size;
    struct Node *next;
} Node;

// Global variables to control the free list

void *heap_start;
void *heap_end;
Node *list_start;
int initialized;

// mutex to control the threads access

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* initialize: function to start the heap space, as well as the
 free spaces list and the semaphores to control the access.
 */

void initialize() {
    initialized = 1;
    heap_start = sbrk(0);
    heap_end = heap_start;
    list_start = NULL;
}

// wordAlignment: transform the given size to a word aligned size
unsigned int wordAlignment(unsigned int bytes) {
    if (bytes < MIN_SIZE)
        bytes = MIN_SIZE;
    return ((bytes +7)/8) * 8;
}

//mergeBlocks: Merge two consecutive free blocks into a larger one
void mergeBlocks(Node *block1, Node *block2) {
    unsigned int total_size = block1->size + block2->size;
    block1->size = total_size;
    block1->next = block2->next;
}

// insertList: insert a new Node in the free blocks list
void insertList(unsigned int size, void *offset, Node *prev) {
    Node newNode;
    Node *current = (Node *) offset;
    newNode.size = size;
    if (prev == list_start) {
        if(prev == NULL || current < prev) {
            newNode.next = prev;
            *current = newNode;
            list_start = current;
            if (prev != NULL && (unsigned int *)current + current->size/sizeof(unsigned int) == (unsigned int *)prev)
                mergeBlocks(current, prev);
            return;
        }
    }
    newNode.next = prev->next;
    *current = newNode;
    prev->next = current;
    if ((unsigned int *)prev + prev->size/sizeof(unsigned int) == (unsigned int *)current) {
        mergeBlocks(prev, current);
        current = prev;
    }
    if ((unsigned int *)current + current->size/sizeof(unsigned int) == (unsigned int *)(current->next))
        mergeBlocks(current, current->next);
}

// removeList: remove a block from the free blocks list
void removeList(Node *ref, Node *prev) {
    if (list_start == ref)
        list_start = ref->next;
    else
        prev->next = ref->next;
}

/* allocate: helper function that given the right address, allocate memory of specified size
 and write the metadata in this space
 */

unsigned int *allocate(unsigned int size, void *ptr) {
    unsigned int *uiptr = (unsigned int *) ptr;
    *uiptr = size;
    *(uiptr + size/sizeof(unsigned int) - 1) = size;
    return uiptr;
}

/* mymalloc: allocates memory on the heap of the requested size. The block
 of memory returned should always be padded so that it begins
 and ends on a word boundary.
 unsigned int size: the number of bytes to allocate.
 retval: a pointer to the block of memory allocated or NULL if the
 memory could not be allocated.
 (NOTE: the system also sets errno, but we are not the system,
 so you are not required to do so.)
 */

void *mymalloc(unsigned int size) {
    pthread_mutex_lock(&mutex);
    if (!initialized)
        initialize();
    
    if (size == 0) {  // when the size is 0, it doesn't need to be allocated
        pthread_mutex_unlock(&mutex);
        return NULL;
    }
    
    unsigned int total_size = size + 2 * sizeof(unsigned int);
    total_size = wordAlignment(total_size);
    
    //look for a free space that best fits on the list
    Node *current = list_start;
    Node *aux = current;
    Node *bestfit = NULL;
    Node *prevbest = NULL;
    if (list_start != NULL) {
        
        // find a free block or the reference to the last block of the list
        while (current != NULL && current->size != total_size) {
            if (current->size >= total_size) {
                if(bestfit == NULL || bestfit->size > current->size) {
                    bestfit = current;
                    prevbest = aux;
                }
            }
            aux = current;
            current = current->next;
        }
        
        if (bestfit != NULL) { // found somewhere that fits
            
            // case A: perfect fit
            if (bestfit->size == total_size || (bestfit->size - total_size) < MIN_SIZE) {
                //remove node from list and return the pointer
                removeList(bestfit, prevbest);
                pthread_mutex_unlock(&mutex);
                return (void *)(allocate(bestfit->size, (void *)bestfit) + 1);
            }
            
            // case B: not a perfect fit
            // remove this node and create a new one with the remaining free space
            unsigned int newnodesize = bestfit->size - total_size;
            removeList(bestfit, prevbest);
            insertList(newnodesize, (void *) (((unsigned int *)bestfit) + total_size/sizeof(unsigned int)), prevbest);
            pthread_mutex_unlock(&mutex);
            return (void *)(allocate(total_size, (void *)bestfit) + 1);
        }
    }
    
    // spaces available don't fit or there's no free space
    void *newAllocation;
    newAllocation = sbrk(total_size); // ask sbrk for more memory
    if (newAllocation != (void *) -1) {
        unsigned int *allocationStart = allocate(total_size, newAllocation);
        heap_end = (void *)(allocationStart + total_size/ sizeof(unsigned int));
        pthread_mutex_unlock(&mutex);
        return (void *) (allocationStart + 1); // return the pointer to allocated area
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

/* myfree: unallocates memory that has been allocated with mymalloc.
 void *ptr: pointer to the first byte of a block of memory allocated by
 mymalloc.
 retval: 0 if the memory was successfully freed and 1 otherwise.
 (NOTE: the system version of free returns no error.)
 */

unsigned int myfree(void *ptr) {
    pthread_mutex_lock(&mutex);
    
    /* if the function initialize hasn't been called before, there's no possible allocations
     and if the given pointer is NULL, it is not a valid allocation
     */
    if (!initialized || ptr == NULL) {
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    
    unsigned int *uiptr = (unsigned int *) ptr;
    unsigned int size = *(uiptr-1);
    
     // compare if size before the data is equal to size after
    if (size !=  *(uiptr - 2 + size/sizeof(unsigned int)) || ptr <= heap_start || ptr >= heap_end ) {
        pthread_mutex_unlock(&mutex);
        return 1;
    }
    
    if (list_start == NULL || list_start > ((Node *) ptr)) {// list is empty or new node comes after the first
        insertList(size, uiptr - 1, list_start); // insert the free block on the list
    }
    
    else {
        Node *current = list_start;
        // find the node that comes before the allocated area
        while (current->next != NULL && current->next < (Node *) ptr)
            current = current->next;
        insertList(size, uiptr - 1, current); // insert the free block on the list
    }
    
    pthread_mutex_unlock(&mutex);
    return 0;
}
