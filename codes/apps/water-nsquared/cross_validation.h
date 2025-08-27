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
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <sys/select.h>

#define MAX_INSTANCES 4
#define MAX_FINGERPRINT_LEN 256
#define MAX_SYNC_POINTS 20
#define FLOAT_TOLERANCE 1e-10  // Tolerance for floating-point comparisons
#define SOCKET_PATH_BASE "/tmp/water_validation_socket"

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

// Message types for socket communication
typedef enum {
    MSG_REGISTER_INSTANCE = 1,
    MSG_SYNC_POINT = 2,
    MSG_VALIDATION_RESULT = 3,
    MSG_SHUTDOWN = 4
} message_type_t;

// Socket message structure
typedef struct {
    message_type_t type;
    int instance_id;
    sync_point_t sync_point;
    char fingerprint[MAX_FINGERPRINT_LEN];
    int validation_passed;
    char mismatch_details[512];
} validation_message_t;

// Validation context for socket-based cross-validation
typedef struct {
    int server_socket;      // Server socket (coordinator)
    int client_socket;      // Client socket (instance)
    int is_coordinator;     // Whether this instance is the coordinator
    int instance_id;        // Instance ID
    int num_instances;      // Total number of instances
    pthread_t coordinator_thread; // Coordinator thread handle
} validation_context_t;

// Shared memory structure for cross-instance validation (legacy)
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
extern validation_context_t *g_validation_context;
extern cross_validation_t *g_validation;
extern int g_instance_id;
extern int g_validation_enabled;

// Function prototypes
int init_cross_validation(int instance_id, int num_instances);
void cleanup_cross_validation();
void cross_validate_sync_point(sync_point_t sync_point, const char *fingerprint);
void generate_fingerprint(char *buffer, size_t buffer_size, const char *format, ...);
int compare_fingerprints_with_tolerance(const char *fp1, const char *fp2);
int send_validation_message(validation_message_t *msg);
int receive_validation_message(int client_socket, validation_message_t *msg);
void handle_sync_point_message(validation_message_t *msg);
void* coordinator_thread_func(void* arg);

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
