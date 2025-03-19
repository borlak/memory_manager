#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>

#define MIN_CHUNK_SIZE 4       // Smallest chunk size (must be a power of 2)
#define MAX_CHUNK_SIZE 65536   // Largest chunk size (must be a power of 2)
#define CHUNK_CLASSES 14       // Number of chunk classes (4, 8, 16, ..., 65536)

#define TOTAL_MEMORY (10 * 1024 * 1024)  // 10MB

// Linked list for free memory blocks
typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

// Memory manager structure
typedef struct {
    FreeBlock *free_list[CHUNK_CLASSES];  // Free lists for each chunk size
    size_t preallocated_counts[CHUNK_CLASSES]; // Track preallocated blocks per chunk size
} MemoryManager;

MemoryManager mem_manager = { {NULL}, {0} };  // Initialize all lists to NULL

// Power-of-2 chunk sizes
size_t chunk_sizes[] = {4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};

// Get index for chunk size (smallest power of 2 greater than or equal to `size`)
size_t get_chunk_index(size_t size) {
    if (size == 0) return 0;  // Edge case

    // Round up to the next power of 2 (only if not already a power of 2)
    if (size & (size - 1)) {
        size = 1ULL << (64 - __builtin_clzll(size));  // Find next power of 2
    }

    return __builtin_ctzll(size / 4);  // Compute index (divide by 4 to match array)
}

// Evenly distribute preallocated memory across chunk sizes in a cyclic manner
void preallocate_memory(size_t total_memory) {
    size_t allocated_memory = 0;
    size_t i = 0; // Start at chunk size 4 bytes

    while (allocated_memory + chunk_sizes[i] <= total_memory*2) {
        // Allocate a single block at each chunk size in order
        FreeBlock *block = (FreeBlock *)malloc(chunk_sizes[i]);
        if (!block) {
            printf("Failed to allocate memory\n");
            exit(1);
        }
        block->next = mem_manager.free_list[i];
        mem_manager.free_list[i] = block;
        mem_manager.preallocated_counts[i]++;  // Track preallocated blocks

        allocated_memory += chunk_sizes[i];

        // Move to the next chunk size (loop back to 0 if needed)
        i = (i + 1) % CHUNK_CLASSES;
    }
    
    printf("Preallocated %zu bytes of memory in a cyclic manner across all chunk sizes.\n", allocated_memory);
}

// Custom malloc (allocates from free list or falls back to malloc)
void *mm_malloc(size_t size) {
    if (size == 0 || size > MAX_CHUNK_SIZE) return NULL;  // Invalid size

    size_t index = get_chunk_index(size);
    if (mem_manager.free_list[index]) {
        // Take from free list
        FreeBlock *block = mem_manager.free_list[index];
        mem_manager.free_list[index] = block->next;
        return (void *)block;
    }

    // Fallback: Allocate new memory if no preallocated blocks are available
    return malloc(chunk_sizes[index]);
}

// Custom free
void mm_free(void *ptr, size_t size) {
    if (!ptr || size == 0 || size > MAX_CHUNK_SIZE) return;

    size_t index = get_chunk_index(size);
    FreeBlock *block = (FreeBlock *)ptr;
    block->next = mem_manager.free_list[index];
    mem_manager.free_list[index] = block;
}

// Generate a list of random sizes summing up to approximately `total_size`
size_t generate_random_sizes(size_t total_size, size_t *sizes, size_t max_count, size_t *requested_counts) {
    size_t num_sizes = 0;
    size_t used_memory = 0;
    srand(time(NULL));  // Seed random generator

    while (used_memory < total_size && num_sizes < max_count) {
        size_t rand_index = rand() % CHUNK_CLASSES;
        size_t chunk_size = chunk_sizes[rand_index];

        if (used_memory + chunk_size > total_size) break;  // Stop if we exceed the limit

        sizes[num_sizes++] = chunk_size;
        used_memory += chunk_size;
        requested_counts[rand_index]++;  // Track requested sizes
    }

    return num_sizes;  // Return actual number of allocations
}

// Debugging: Print memory usage statistics
void print_memory_stats(size_t *preallocated_counts, size_t *requested_counts) {
    printf("\nMemory Statistics:\n");
    printf("%-10s %-15s %-15s\n", "Chunk Size", "Preallocated", "Requested");
    
    for (size_t i = 0; i < CHUNK_CLASSES; i++) {
        printf("%-10zu %-15zu %-15zu\n", chunk_sizes[i], preallocated_counts[i], requested_counts[i]);
    }
}

// Benchmark function
void benchmark(size_t total_memory) {
    const size_t max_allocations = 100000;
    size_t *sizes = malloc(max_allocations * sizeof(size_t));
    void **ptrs = malloc(max_allocations * sizeof(void *));
    size_t requested_counts[CHUNK_CLASSES] = {0};  // Track actual requested sizes
    
    if (!sizes || !ptrs) {
        printf("Memory allocation failed for benchmark setup.\n");
        return;
    }

    size_t num_allocations = generate_random_sizes(total_memory, sizes, max_allocations, requested_counts);

    printf("Benchmarking with %zu allocations totaling ~%zu bytes...\n", num_allocations, total_memory);

    // **Standard malloc/free benchmark**
    clock_t start = clock();
    for (size_t i = 0; i < num_allocations; i++) {
        ptrs[i] = malloc(sizes[i]);
    }
    for (size_t i = 0; i < num_allocations; i++) {
        free(ptrs[i]);
    }
    clock_t end = clock();
    printf("Standard malloc/free: %lf sec\n", (double)(end - start) / CLOCKS_PER_SEC);

    // **Custom mm_malloc/mm_free benchmark (Preallocated)**
    preallocate_memory(total_memory);  // Allocate memory for our allocator before benchmarking

    // Some options here to sleep before we do the memory manager test, in an attempt to play with caching
    //sleep(1);
    for (volatile int i = 0; i < 1000000000; i++);  // Waste CPU cycles instead of sleeping

    start = clock();
    for (size_t i = 0; i < num_allocations; i++) {
        ptrs[i] = mm_malloc(sizes[i]);
    }
    for (size_t i = 0; i < num_allocations; i++) {
        mm_free(ptrs[i], sizes[i]);
    }
    end = clock();
    printf("Custom mm_malloc/mm_free: %lf sec\n", (double)(end - start) / CLOCKS_PER_SEC);

    // Print memory allocation statistics
    print_memory_stats(mem_manager.preallocated_counts, requested_counts);

    free(sizes);
    free(ptrs);
}

////////////// Testing Functions
// When benchmarking, memory can be cached in L1/L2/L3, which speeds up access. 
// This function is an attempt to clera the cache for more realistic benchmarking.
void clear_cpu_cache() {
    size_t cache_size = 32 * 1024 * 1024; // 32MB, larger than most L3 caches
    char *buffer = malloc(cache_size);
    for (size_t i = 0; i < cache_size; i += 64) {
        buffer[i] = 0;  // Touch every cache line
    }
    free(buffer);
}

// This tells the OS to discard memory pages, forcing a reload.
void flush_memory(void *ptr, size_t size) {
    madvise(ptr, size, MADV_DONTNEED);  // Tell OS to discard pages
}

// This will randomly deallocate half of the memory, creating fragmentation.
void random_free(void **ptrs, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (rand() % 2) {  // 50% chance to free
            free(ptrs[i]);
            ptrs[i] = NULL;
        }
    }
}

// This exceeds RAM capacity, causing page swapping.
size_t force_page_faults() {
    size_t size = 2L * 1024 * 1024 * 1024; // 2GB
    char *data = malloc(size);
    for (size_t i = 0; i < size; i += 4096) {
        data[i] = 0;  // Touch every page
    }
    return size;
}

// This will force the OS to allocate all available RAM, pushing other memory pages to disk (swap space).
void consume_memory() {
    size_t size = 1024 * 1024 * 1024; // 1GB
    char *data = malloc(size);
    while (data) {
        for (size_t i = 0; i < size; i += 4096) {
            data[i] = 1;  // Touch memory to force OS to commit pages
        }
        size += 512 * 1024 * 1024; // Increase allocation
        data = malloc(size);
    }
}

// Multi-thread testing
void *thread_alloc(void *arg) {
    for (int i = 0; i < 100000; i++) {
        void *ptr = mm_malloc(128);
        mm_free(ptr, 128);
    }
    return NULL;
}

void test_multithreading() {
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) {
        pthread_create(&threads[i], NULL, thread_alloc, NULL);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }
}
////////////// End testing functions

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s [-c] [-f] [-p] [-m] [-t] [-b]\n", argv[0]);
        printf("Options:\n");
        printf("  -c  Clear CPU Cache\n");
        printf("  -f  Fragment Memory\n");
        printf("  -p  Force Page Faults\n");
        printf("  -m  Simulate Memory Pressure\n");
        printf("  -t  Multi-Threaded Test\n");
        printf("  -b  Run Benchmark (Default if no options)\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            clear_cpu_cache();
        } else if (strcmp(argv[i], "-f") == 0) {
            void **ptrs = malloc(10000 * sizeof(void *));
            for (size_t j = 0; j < 10000; j++) {
                ptrs[j] = malloc(128);
            }
            random_free(ptrs, 10000);
            free(ptrs);
        } else if (strcmp(argv[i], "-p") == 0) {
            force_page_faults();
        } else if (strcmp(argv[i], "-m") == 0) {
            consume_memory();
        } else if (strcmp(argv[i], "-t") == 0) {
            test_multithreading();
        } else if (strcmp(argv[i], "-b") == 0) {
            ;
        } else {
            printf("Unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    benchmark(TOTAL_MEMORY);  // Benchmark with 10MB worth of allocations
    return 0;
}
