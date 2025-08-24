#ifndef CROSS_VALIDATION_H
#define CROSS_VALIDATION_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#include <errno.h>

#define MAX_INSTANCES 4
#define MAX_FINGERPRINT_LEN 256
#define MAX_SYNC_POINTS 20
#define FLOAT_TOLERANCE 1e-10  // Tolerance for floating-point comparisons

// Sync point identifiers
typedef enum {
    SYNC_WORKSTART_BEGIN = 0,
    SYNC_INTRAF_BARRIER_INIT,
    SYNC_INTERF_BARRIER_INIT,
    SYNC_INTRAF_BARRIER_STEP_1,
    SYNC_INTERF_FORCES_STEP_1,
    SYNC_KINETI_BARRIER_STEP_1,
    SYNC_TIMESTEP_END_BARRIER_1,
    SYNC_INTRAF_BARRIER_STEP_2,
    SYNC_INTERF_FORCES_STEP_2,
    SYNC_KINETI_BARRIER_STEP_2,
    SYNC_TIMESTEP_END_BARRIER_2,
    SYNC_INTRAF_BARRIER_STEP_3,
    SYNC_INTERF_FORCES_STEP_3,
    SYNC_KINETI_BARRIER_STEP_3,
    SYNC_POTENG_INTRAMOL_BARRIER,
    SYNC_POTENG_PRE_RACE,              // Before race condition in POTENG
    SYNC_POTENG_POST_RACE,             // After race condition in POTENG
    SYNC_POTENG_BARRIER_STEP_3,
    SYNC_TIMESTEP_END_BARRIER_3,
    SYNC_WORKSTART_END,
    SYNC_MAX
} sync_point_t;

// Shared memory structure for cross-instance validation
typedef struct {
    sem_t instance_sem[MAX_INSTANCES];  // One semaphore per instance
    sem_t coordinator_sem;              // Coordinator semaphore
    
    int num_instances;                  // Total number of instances
    int current_sync_point;             // Current sync point being validated
    int instances_arrived;              // Number of instances that reached current sync point
    
    char fingerprints[MAX_INSTANCES][MAX_FINGERPRINT_LEN];  // Fingerprints from each instance
    int instance_ids[MAX_INSTANCES];    // Instance IDs
    
    int validation_failed;              // Flag indicating validation failure
    char mismatch_details[512];         // Details of the mismatch
} cross_validation_t;

// Global validation context
extern cross_validation_t *g_validation;
extern int g_instance_id;
extern int g_validation_enabled;

// Function prototypes
int init_cross_validation(int instance_id, int num_instances);
void cleanup_cross_validation();
void cross_validate_sync_point(sync_point_t sync_point, const char *fingerprint);
void generate_fingerprint(char *buffer, size_t buffer_size, const char *format, ...);
int compare_fingerprints_with_tolerance(const char *fp1, const char *fp2);

// Macros for easy integration
#define CROSS_VALIDATE_SYNC(sync_point, ...) \
    do { \
        if (g_validation_enabled) { \
            char _fingerprint[MAX_FINGERPRINT_LEN]; \
            generate_fingerprint(_fingerprint, sizeof(_fingerprint), __VA_ARGS__); \
            cross_validate_sync_point(sync_point, _fingerprint); \
        } \
    } while(0)

#define CROSS_VALIDATE_ASSERT(sync_point, ...) \
    do { \
        if (g_validation_enabled) { \
            char _fingerprint[MAX_FINGERPRINT_LEN]; \
            generate_fingerprint(_fingerprint, sizeof(_fingerprint), __VA_ARGS__); \
            cross_validate_sync_point(sync_point, _fingerprint); \
            if (g_validation && g_validation->validation_failed) { \
                fprintf(stderr, "❌ ASSERTION FAILED at sync point %d: %s\n", \
                        sync_point, g_validation->mismatch_details); \
                assert(0 && "Cross-instance validation failed"); \
            } \
        } \
    } while(0)

#endif // CROSS_VALIDATION_H
