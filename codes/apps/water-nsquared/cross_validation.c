#include "cross_validation.h"
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

// Global variables
cross_validation_t *g_validation = NULL;
int g_instance_id = -1;
int g_validation_enabled = 0;

static const char *SHARED_MEM_NAME = "/water_nsquared_validation";

int init_cross_validation(int instance_id, int num_instances) {
    if (num_instances > MAX_INSTANCES) {
        fprintf(stderr, "❌ Too many instances: %d (max: %d)\n", num_instances, MAX_INSTANCES);
        return -1;
    }
    
    g_instance_id = instance_id;
    
    // Create or open shared memory
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        return -1;
    }
    
    // Set size of shared memory
    if (ftruncate(shm_fd, sizeof(cross_validation_t)) == -1) {
        perror("ftruncate failed");
        close(shm_fd);
        return -1;
    }
    
    // Map shared memory
    g_validation = (cross_validation_t *)mmap(NULL, sizeof(cross_validation_t),
                                              PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_validation == MAP_FAILED) {
        perror("mmap failed");
        close(shm_fd);
        return -1;
    }
    
    close(shm_fd);
    
    // Initialize shared memory (first instance only)
    if (instance_id == 0) {
        memset(g_validation, 0, sizeof(cross_validation_t));
        g_validation->num_instances = num_instances;
        g_validation->current_sync_point = -1;
        g_validation->instances_arrived = 0;
        g_validation->validation_failed = 0;
        
        // Initialize semaphores
        if (sem_init(&g_validation->coordinator_sem, 1, 1) == -1) {
            perror("sem_init coordinator_sem failed");
            return -1;
        }
        
        for (int i = 0; i < MAX_INSTANCES; i++) {
            if (sem_init(&g_validation->instance_sem[i], 1, 0) == -1) {
                perror("sem_init instance_sem failed");
                return -1;
            }
        }
        
        printf("🔍 Cross-validation initialized for %d instances\n", num_instances);
        fflush(stdout);
    } else {
        // Wait a bit for instance 0 to initialize
        usleep(100000); // 100ms
    }
    
    g_validation_enabled = 1;
    printf("✅ Instance %d connected to cross-validation system\n", instance_id);
    fflush(stdout);
    
    return 0;
}

void cleanup_cross_validation() {
    if (!g_validation_enabled) return;
    
    if (g_validation) {
        // Cleanup semaphores (instance 0 only)
        if (g_instance_id == 0) {
            sem_destroy(&g_validation->coordinator_sem);
            for (int i = 0; i < MAX_INSTANCES; i++) {
                sem_destroy(&g_validation->instance_sem[i]);
            }
        }
        
        munmap(g_validation, sizeof(cross_validation_t));
        g_validation = NULL;
    }
    
    // Remove shared memory (last instance)
    if (g_instance_id == 0) {
        shm_unlink(SHARED_MEM_NAME);
    }
    
    g_validation_enabled = 0;
    printf("🧹 Instance %d cleaned up cross-validation\n", g_instance_id);
    fflush(stdout);
}

void generate_fingerprint(char *buffer, size_t buffer_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, buffer_size, format, args);
    va_end(args);
}

void cross_validate_sync_point(sync_point_t sync_point, const char *fingerprint) {
    if (!g_validation_enabled || !g_validation) {
        return;
    }
    
    // DEBUG: Add logging to confirm non-blocking behavior
    printf("🔧 Instance %d attempting sync point %d (non-blocking)\n", g_instance_id, sync_point);
    fflush(stdout);
    
    // NON-BLOCKING: Try to acquire coordinator lock without waiting
    if (sem_trywait(&g_validation->coordinator_sem) != 0) {
        // If we can't get the lock immediately, just record locally and return
        printf("🔄 Instance %d reached sync point %d (non-blocking, skipped): %s\n", g_instance_id, sync_point, fingerprint);
        fflush(stdout);
        return;
    }
    
    printf("🔧 Instance %d acquired lock for sync point %d\n", g_instance_id, sync_point);
    fflush(stdout);
    
    // Check if this is a new sync point
    if (g_validation->current_sync_point != sync_point) {
        // Reset for new sync point
        g_validation->current_sync_point = sync_point;
        g_validation->instances_arrived = 0;
        memset(g_validation->fingerprints, 0, sizeof(g_validation->fingerprints));
        memset(g_validation->instance_ids, -1, sizeof(g_validation->instance_ids));
    }
    
    // Store this instance's fingerprint
    int slot = g_validation->instances_arrived;
    strncpy(g_validation->fingerprints[slot], fingerprint, MAX_FINGERPRINT_LEN - 1);
    g_validation->fingerprints[slot][MAX_FINGERPRINT_LEN - 1] = '\0';
    g_validation->instance_ids[slot] = g_instance_id;
    g_validation->instances_arrived++;
    
    printf("🔄 Instance %d reached sync point %d: %s\n", g_instance_id, sync_point, fingerprint);
    fflush(stdout);
    
    // Check if all instances have arrived
    if (g_validation->instances_arrived == g_validation->num_instances) {
        // Compare all fingerprints
        int match = 1;
        for (int i = 1; i < g_validation->num_instances; i++) {
            if (!compare_fingerprints_with_tolerance(g_validation->fingerprints[0], g_validation->fingerprints[i])) {
                match = 0;
                snprintf(g_validation->mismatch_details, sizeof(g_validation->mismatch_details),
                         "Sync point %d: Instance %d='%s' vs Instance %d='%s'",
                         sync_point,
                         g_validation->instance_ids[0], g_validation->fingerprints[0],
                         g_validation->instance_ids[i], g_validation->fingerprints[i]);
                break;
            }
        }
        
        if (match) {
            printf("✅ MATCH at sync point %d: %s\n", sync_point, g_validation->fingerprints[0]);
            fflush(stdout);
            g_validation->validation_failed = 0;
        } else {
            printf("❌ MISMATCH at sync point %d: %s\n", sync_point, g_validation->mismatch_details);
            fflush(stdout);
            g_validation->validation_failed = 1;
        }
    }
    
    // Release coordinator lock immediately - NO WAITING FOR OTHER THREADS
    sem_post(&g_validation->coordinator_sem);
    printf("🔧 Instance %d released lock for sync point %d\n", g_instance_id, sync_point);
    fflush(stdout);
    
    // DO NOT WAIT - this was the serialization point!
    // Threads continue execution immediately
}

// Function to compare fingerprints with floating-point tolerance
int compare_fingerprints_with_tolerance(const char *fp1, const char *fp2) {
    char *fp1_copy = strdup(fp1);
    char *fp2_copy = strdup(fp2);
    
    if (!fp1_copy || !fp2_copy) {
        free(fp1_copy);
        free(fp2_copy);
        return strcmp(fp1, fp2) == 0;  // Fallback to exact comparison
    }
    
    char *token1, *token2;
    char *saveptr1, *saveptr2;
    int match = 1;
    
    // Tokenize both strings by spaces and = signs
    token1 = strtok_r(fp1_copy, " =", &saveptr1);
    token2 = strtok_r(fp2_copy, " =", &saveptr2);
    
    while (token1 && token2 && match) {
        // Check if both tokens are numeric (floating-point)
        char *endptr1, *endptr2;
        double val1 = strtod(token1, &endptr1);
        double val2 = strtod(token2, &endptr2);
        
        // If both are valid numbers, compare with tolerance
        if (*endptr1 == '\0' && *endptr2 == '\0') {
            if (fabs(val1 - val2) > FLOAT_TOLERANCE) {
                match = 0;
                break;
            }
        } else {
            // For non-numeric tokens, use exact string comparison
            if (strcmp(token1, token2) != 0) {
                match = 0;
                break;
            }
        }
        
        token1 = strtok_r(NULL, " =", &saveptr1);
        token2 = strtok_r(NULL, " =", &saveptr2);
    }
    
    // Check if both reached end simultaneously
    if (token1 || token2) {
        match = 0;
    }
    
    free(fp1_copy);
    free(fp2_copy);
    return match;
}
