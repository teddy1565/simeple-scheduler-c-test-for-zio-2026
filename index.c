#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <stdint.h>

#define NUM_WORKERS 4
#define QUEUE_SIZE 65536     
#define QUEUE_MASK (QUEUE_SIZE - 1)
#define CACHE_LINE_SIZE 128  

typedef struct {
    uint64_t id;
    int complexity; 
} Task;

// --- memory struct ---
typedef struct {
    // [Worker WriteZone]
    volatile unsigned long head; 
    volatile uint64_t total_processed; 
    
    // [Allocator WriteZone]
    volatile unsigned long tail;
    
    // [Content]
    Task ring_buffer[QUEUE_SIZE];
} __attribute__((aligned(CACHE_LINE_SIZE))) WorkerSection;

WorkerSection *workers_mem; 
volatile int system_running = 1; 
volatile int start_signal = 0;   
volatile int workers_ready_count = 0; 

// --- Cross Platform CPU Pause ---
static inline void cpu_relax() {
#if defined(__aarch64__) || defined(__arm__)
    // Apple Silicon
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    // Intel / AMD
    __asm__ __volatile__("rep; nop" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

void cpu_busy_work(int loops) {
    volatile int k = 0;
    for (int i = 0; i < loops; i++) k++;
}

// --- Worker ---
void* worker_thread(void* arg) {
    long id = (long)arg;
    WorkerSection *my_mem = &workers_mem[id];
    
    __sync_fetch_and_add(&workers_ready_count, 1);
    while (start_signal == 0) { usleep(1000); }

    while (system_running) {
        unsigned long current_tail = my_mem->tail;
        unsigned long current_head = my_mem->head;

        if (current_head < current_tail) {
            Task t = my_mem->ring_buffer[current_head & QUEUE_MASK];
            
            // update head
            my_mem->head++; 
            
            cpu_busy_work(t.complexity);
            my_mem->total_processed++;     
        } else {
            cpu_relax();
        }
    }
    return NULL;
}

// --- Monitor ---
void* monitor_thread(void* arg) {
    while (start_signal == 0) { usleep(10000); } 

    uint64_t last_total = 0;
    while (system_running) {
        sleep(1);
        
        uint64_t current_total = 0;
        printf("\n=== System Status ===\n");
        for (int i = 0; i < NUM_WORKERS; i++) {
            unsigned long t = workers_mem[i].tail;
            unsigned long h = workers_mem[i].head;
            unsigned long load = t - h; 

            uint64_t processed = workers_mem[i].total_processed;
            current_total += processed;
            
            printf("Worker %d: [Load: %5lu] processed: %llu ", i, load, processed);
            int bar = load / 100; 
            if (bar > 20) bar = 20;
            printf("|");
            for(int k=0; k<bar; k++) printf("#");
            printf("\n");
        }
        
        printf(">>> TPS: %llu ops/sec <<<\n", (current_total - last_total));
        last_total = current_total;
    }
    return NULL;
}

// --- Allocator ---
void* allocator_thread(void* arg) {
    while (workers_ready_count < NUM_WORKERS) { usleep(1000); }
    sleep(1);
    start_signal = 1; 
    
    uint64_t task_id = 0;
    int start_offset = 0; 

    while (system_running) {
        Task t = { .id = task_id++, .complexity = 1000 + (rand() % 4000) };

        int best_worker = -1;
        unsigned long min_load = 99999999;

        start_offset = (start_offset + 1) % NUM_WORKERS;

        for (int i = 0; i < NUM_WORKERS; i++) {
            int w = (start_offset + i) % NUM_WORKERS;

            unsigned long head = workers_mem[w].head;
            unsigned long tail = workers_mem[w].tail;
            unsigned long load = tail - head;

            if (load >= QUEUE_SIZE) continue; 

            if (load < min_load) {
                min_load = load;
                best_worker = w;
            }
        }

        if (best_worker != -1) {
            WorkerSection *target = &workers_mem[best_worker];
            target->ring_buffer[target->tail & QUEUE_MASK] = t;
            
            __sync_synchronize(); 
            
            target->tail++;
        } else {
            cpu_relax();
        }
    }
    return NULL;
}

int main() {
    size_t mem_size = sizeof(WorkerSection) * NUM_WORKERS;
    workers_mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    memset(workers_mem, 0, mem_size);

    pthread_t p_thread, m_thread;
    pthread_t w_threads[NUM_WORKERS];

    for(long i=0; i<NUM_WORKERS; i++) 
        pthread_create(&w_threads[i], NULL, worker_thread, (void*)i);
    pthread_create(&m_thread, NULL, monitor_thread, NULL);
    allocator_thread(NULL);

    return 0;
}