#ifndef CROSS_VALIDATION_H
#define CROSS_VALIDATION_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <pthread.h>
#include <time.h>

#define MAX_INSTANCES 4
#define MAX_FINGERPRINT_LEN 256
#define MAX_SYNC_POINTS 20
#define FLOAT_TOLERANCE 1e-10  // Tolerance for floating-point comparisons
#define SOCKET_PATH "/tmp/water_validation_socket"
#define MAX_MESSAGE_SIZE 512

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
    MSG_SYNC_POINT,
    MSG_VALIDATION_RESULT,
    MSG_SHUTDOWN
} message_type_t;

// Socket message structure
typedef struct {
    message_type_t type;
    int instance_id;
    sync_point_t sync_point;
    char fingerprint[MAX_FINGERPRINT_LEN];
    int validation_passed;      // 1 = match, 0 = mismatch
    char mismatch_details[256]; // Details when validation fails
} validation_message_t;

// Validation context for socket-based communication
typedef struct {
    int client_socket;          // Client socket for instances
    int server_socket;          // Server socket (coordinator only)
    int instance_id;
    int num_instances;
    int is_coordinator;         // 1 if this instance is the coordinator
    pthread_t coordinator_thread;
    int validation_enabled;
    int assert_on_mismatch;     // Flag to enable assertion on mismatch
} validation_context_t;

// Global validation context
extern validation_context_t *g_validation_context;
extern int g_instance_id;
extern int g_validation_enabled;

// Function prototypes
int init_cross_validation(int instance_id, int num_instances);
void cleanup_cross_validation();
void cross_validate_sync_point(sync_point_t sync_point, const char *fingerprint);
void generate_fingerprint(char *buffer, size_t buffer_size, const char *format, ...);
int compare_fingerprints_with_tolerance(const char *fp1, const char *fp2);
void* coordinator_thread_func(void* arg);
int send_validation_message(validation_message_t *msg);
int receive_validation_message(int socket_fd, validation_message_t *msg);
void handle_sync_point_message(validation_message_t *msg);
void enable_assertion_on_mismatch();

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
        } \
    } while(0)

#endif // CROSS_VALIDATION_H
