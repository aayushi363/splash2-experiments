#include "cross_validation.h"
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

// Global variables
validation_context_t *g_validation_context = NULL;
int g_instance_id = -1;
int g_validation_enabled = 0;
int g_client_fds[MAX_INSTANCES] = {0}; // Global client file descriptors

// Coordinator state
typedef struct {
    int current_sync_point;
    int instances_arrived;
    char fingerprints[MAX_INSTANCES][MAX_FINGERPRINT_LEN];
    int instance_ids[MAX_INSTANCES];
    int validation_failed;
    char mismatch_details[512];
    int num_instances;
} coordinator_state_t;

static coordinator_state_t g_coordinator_state = {0};

int init_cross_validation(int instance_id, int num_instances) {
    if (num_instances > MAX_INSTANCES) {
        fprintf(stderr, "❌ Too many instances: %d (max: %d)\n", num_instances, MAX_INSTANCES);
        return -1;
    }
    
    g_instance_id = instance_id;
    
    // Allocate validation context
    g_validation_context = (validation_context_t*)calloc(1, sizeof(validation_context_t));
    if (!g_validation_context) {
        perror("Failed to allocate validation context");
        return -1;
    }
    
    g_validation_context->instance_id = instance_id;
    g_validation_context->num_instances = num_instances;
    g_validation_context->is_coordinator = (instance_id == 0);
    
    // If this is the coordinator (instance 0), set up Unix domain socket server
    if (g_validation_context->is_coordinator) {
        // Remove existing socket file
        unlink(SOCKET_PATH);
        
        // Create Unix domain socket
        g_validation_context->server_socket = socket(AF_UNIX, SOCK_STREAM, 0);
        if (g_validation_context->server_socket < 0) {
            perror("Failed to create Unix domain socket");
            free(g_validation_context);
            return -1;
        }
        
        // Bind socket
        struct sockaddr_un server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sun_family = AF_UNIX;
        strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);
        
        if (bind(g_validation_context->server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Failed to bind Unix domain socket");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        
        // Listen for connections
        if (listen(g_validation_context->server_socket, MAX_INSTANCES) < 0) {
            perror("Failed to listen on Unix domain socket");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        
        // Initialize coordinator state
        g_coordinator_state.current_sync_point = -1;
        g_coordinator_state.instances_arrived = 0;
        g_coordinator_state.validation_failed = 0;
        g_coordinator_state.num_instances = num_instances;
        
        // Start coordinator thread
        if (pthread_create(&g_validation_context->coordinator_thread, NULL, coordinator_thread_func, g_validation_context) != 0) {
            perror("Failed to create coordinator thread");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        
        printf("🔍 Cross-validation coordinator started with Unix socket %s for %d instances\n", SOCKET_PATH, num_instances);
        fflush(stdout);
    }
    
    // All instances (including coordinator) create client socket
    usleep(200000); // 200ms delay to ensure server is ready
    
    g_validation_context->client_socket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_validation_context->client_socket < 0) {
        perror("Failed to create client Unix socket");
        cleanup_cross_validation();
        return -1;
    }
    
    // Connect to coordinator
    struct sockaddr_un coord_addr;
    memset(&coord_addr, 0, sizeof(coord_addr));
    coord_addr.sun_family = AF_UNIX;
    strncpy(coord_addr.sun_path, SOCKET_PATH, sizeof(coord_addr.sun_path) - 1);
    
    if (connect(g_validation_context->client_socket, (struct sockaddr*)&coord_addr, sizeof(coord_addr)) < 0) {
        perror("Failed to connect to coordinator Unix socket");
        cleanup_cross_validation();
        return -1;
    }
    
    // Register this instance
    validation_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_REGISTER_INSTANCE;
    msg.instance_id = instance_id;
    
    if (send_validation_message(&msg) < 0) {
        fprintf(stderr, "Failed to register instance %d\n", instance_id);
        cleanup_cross_validation();
        return -1;
    }
    
    g_validation_enabled = 1;
    printf("✅ Instance %d connected to Unix socket-based cross-validation system\n", instance_id);
    fflush(stdout);
    
    return 0;
}

void cleanup_cross_validation() {
    if (!g_validation_enabled || !g_validation_context) return;
    
    // Send shutdown message
    validation_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = MSG_SHUTDOWN;
    msg.instance_id = g_instance_id;
    send_validation_message(&msg);
    
    // Close client socket
    if (g_validation_context->client_socket >= 0) {
        close(g_validation_context->client_socket);
    }
    
    // If coordinator, cleanup server resources
    if (g_validation_context->is_coordinator) {
        if (g_validation_context->server_socket >= 0) {
            close(g_validation_context->server_socket);
        }
        
        // Wait for coordinator thread to finish
        pthread_cancel(g_validation_context->coordinator_thread);
        pthread_join(g_validation_context->coordinator_thread, NULL);
        
        // Remove socket file
        unlink(SOCKET_PATH);
    }
    
    free(g_validation_context);
    g_validation_context = NULL;
    
    g_validation_enabled = 0;
    printf("🧹 Instance %d cleaned up Unix socket-based cross-validation\n", g_instance_id);
    fflush(stdout);
}

void generate_fingerprint(char *buffer, size_t buffer_size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, buffer_size, format, args);
    va_end(args);
}

void cross_validate_sync_point(sync_point_t sync_point, const char *fingerprint) {
    if (!g_validation_enabled || !g_validation_context) {
        return;
    }
    
    printf("🔧 Instance %d sending sync point %d: %s\n", g_instance_id, sync_point, fingerprint);
    fflush(stdout);
    
    // Send sync point message via socket
    validation_message_t msg;
    msg.type = MSG_SYNC_POINT;
    msg.instance_id = g_instance_id;
    msg.sync_point = sync_point;
    strncpy(msg.fingerprint, fingerprint, MAX_FINGERPRINT_LEN - 1);
    msg.fingerprint[MAX_FINGERPRINT_LEN - 1] = '\0';
    
    ssize_t sent = send(g_validation_context->client_socket, &msg, sizeof(msg), 0);
    if (sent != sizeof(msg)) {
        printf("❌ Instance %d failed to send sync point message: %s\n", g_instance_id, strerror(errno));
        return;
    }
    
    // Wait for validation result from coordinator
    validation_message_t response;
    ssize_t received = recv(g_validation_context->client_socket, &response, sizeof(response), 0);
    if (received != sizeof(response)) {
        printf("❌ Instance %d failed to receive validation result: %s\n", g_instance_id, strerror(errno));
        return;
    }
    
    // Check result and assert if mismatch
    if (response.type == MSG_VALIDATION_RESULT) {
        if (response.validation_passed) {
            printf("✅ MATCH at sync point %d: %s\n", sync_point, fingerprint);
            fflush(stdout);
        } else {
            printf("❌ MISMATCH at sync point %d: %s\n", sync_point, response.mismatch_details);
            fflush(stdout);
            
            // Assert failure on mismatch as requested
            fprintf(stderr, "ASSERTION FAILED: Fingerprint mismatch between processes at sync point %d\n", sync_point);
            fprintf(stderr, "Details: %s\n", response.mismatch_details);
            fflush(stderr);
            assert(0); // This will terminate the program
        }
    }
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

// Socket communication functions
int send_validation_message(validation_message_t *msg) {
    if (!g_validation_context || g_validation_context->client_socket < 0) {
        return -1;
    }
    
    ssize_t bytes_sent = send(g_validation_context->client_socket, msg, sizeof(validation_message_t), 0);
    if (bytes_sent != sizeof(validation_message_t)) {
        perror("Failed to send validation message");
        return -1;
    }
    
    return 0;
}

int receive_validation_message(int client_socket, validation_message_t *msg) {
    ssize_t bytes_received = recv(client_socket, msg, sizeof(validation_message_t), 0);
    if (bytes_received != sizeof(validation_message_t)) {
        if (bytes_received == 0) {
            return 0; // Connection closed
        }
        perror("Failed to receive validation message");
        return -1;
    }
    
    return 1; // Success
}

// Handle sync point messages from instances with assertion on mismatch
void handle_sync_point_message(validation_message_t *msg) {
    // Check if this is a new sync point
    if (g_coordinator_state.current_sync_point != (int)msg->sync_point) {
        // Reset for new sync point
        g_coordinator_state.current_sync_point = (int)msg->sync_point;
        g_coordinator_state.instances_arrived = 0;
        memset(g_coordinator_state.fingerprints, 0, sizeof(g_coordinator_state.fingerprints));
        memset(g_coordinator_state.instance_ids, -1, sizeof(g_coordinator_state.instance_ids));
    }
    
    // Store this instance's fingerprint
    int slot = g_coordinator_state.instances_arrived;
    strncpy(g_coordinator_state.fingerprints[slot], msg->fingerprint, MAX_FINGERPRINT_LEN - 1);
    g_coordinator_state.fingerprints[slot][MAX_FINGERPRINT_LEN - 1] = '\0';
    g_coordinator_state.instance_ids[slot] = msg->instance_id;
    g_coordinator_state.instances_arrived++;
    
    printf("📥 Coordinator received sync point %d from instance %d: %s (%d/%d)\n", 
           (int)msg->sync_point, msg->instance_id, msg->fingerprint, 
           g_coordinator_state.instances_arrived, g_coordinator_state.num_instances);
    fflush(stdout);
    
    // Check if all instances have arrived
    if (g_coordinator_state.instances_arrived == g_coordinator_state.num_instances) {
        // Compare all fingerprints
        int match = 1;
        char mismatch_details[512];
        
        for (int i = 1; i < g_coordinator_state.num_instances; i++) {
            if (!compare_fingerprints_with_tolerance(g_coordinator_state.fingerprints[0], g_coordinator_state.fingerprints[i])) {
                match = 0;
                snprintf(mismatch_details, sizeof(mismatch_details),
                         "Sync point %d: Instance %d='%s' vs Instance %d='%s'",
                         (int)msg->sync_point,
                         g_coordinator_state.instance_ids[0], g_coordinator_state.fingerprints[0],
                         g_coordinator_state.instance_ids[i], g_coordinator_state.fingerprints[i]);
                break;
            }
        }
        
        if (match) {
            printf("✅ UNIX SOCKET MATCH at sync point %d: %s\n", (int)msg->sync_point, g_coordinator_state.fingerprints[0]);
            fflush(stdout);
            g_coordinator_state.validation_failed = 0;
        } else {
            printf("❌ UNIX SOCKET MISMATCH at sync point %d: %s\n", (int)msg->sync_point, mismatch_details);
            fflush(stdout);
            g_coordinator_state.validation_failed = 1;
            
            // ASSERTION: Fail immediately on mismatch
            if (1) { // Always assert on mismatch as requested
                fprintf(stderr, "\n🚨 FATAL: Cross-validation assertion failed!\n");
                fprintf(stderr, "🔍 Details: %s\n", mismatch_details);
                fprintf(stderr, "💥 Terminating all instances due to validation mismatch.\n\n");
                fflush(stderr);
                
                // Assert will terminate the program
                assert(0 && "Cross-validation fingerprint mismatch detected between instances");
            }
        }
        
        // Send response to all registered instances
        validation_message_t response;
        response.type = MSG_VALIDATION_RESULT;
        response.instance_id = -1; // From coordinator
        response.sync_point = msg->sync_point;
        response.validation_passed = match;
        if (!match) {
            strncpy(response.mismatch_details, mismatch_details, sizeof(response.mismatch_details) - 1);
            response.mismatch_details[sizeof(response.mismatch_details) - 1] = '\0';
        } else {
            response.mismatch_details[0] = '\0';
        }
        
        // Send response to all instances that participated in this sync point
        for (int i = 0; i < g_coordinator_state.instances_arrived; i++) {
            int instance_id = g_coordinator_state.instance_ids[i];
            if (instance_id >= 0 && instance_id < MAX_INSTANCES) {
                int client_fd = g_client_fds[instance_id];
                if (client_fd > 0) {
                    ssize_t sent = send(client_fd, &response, sizeof(response), 0);
                    if (sent != sizeof(response)) {
                        printf("⚠️ Failed to send response to instance %d\n", instance_id);
                    } else {
                        printf("📤 Sent validation result to instance %d (fd=%d)\n", instance_id, client_fd);
                    }
                }
            }
        }
    }
}

// Coordinator thread function
void* coordinator_thread_func(void* arg) {
    validation_context_t *ctx = (validation_context_t*)arg;
    fd_set master_fds, read_fds;
    int max_fd = ctx->server_socket;
    int client_fds[MAX_INSTANCES];
    int registered_instances = 0;
    
    // Initialize client file descriptors
    for (int i = 0; i < MAX_INSTANCES; i++) {
        client_fds[i] = -1;
    }
    
    FD_ZERO(&master_fds);
    FD_SET(ctx->server_socket, &master_fds);
    
    printf("🎯 Unix socket coordinator thread started, waiting for %d instances\n", ctx->num_instances);
    fflush(stdout);
    
    while (registered_instances < ctx->num_instances) {
        read_fds = master_fds;
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select failed in coordinator");
            break;
        }
        
        // Check for new connections
        if (FD_ISSET(ctx->server_socket, &read_fds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(ctx->server_socket, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                perror("accept failed");
                continue;
            }
            
            FD_SET(client_fd, &master_fds);
            if (client_fd > max_fd) {
                max_fd = client_fd;
            }
            
            printf("🔗 New Unix socket client connected: fd=%d\n", client_fd);
            fflush(stdout);
        }
        
        // Check for messages from existing clients
        for (int fd = 0; fd <= max_fd; fd++) {
            if (fd != ctx->server_socket && FD_ISSET(fd, &read_fds)) {
                validation_message_t msg;
                int result = receive_validation_message(fd, &msg);
                
                if (result <= 0) {
                    // Client disconnected
                    close(fd);
                    FD_CLR(fd, &master_fds);
                    continue;
                }
                
                if (msg.type == MSG_REGISTER_INSTANCE) {
                    client_fds[msg.instance_id] = fd;
                    g_client_fds[msg.instance_id] = fd; // Also store in global array
                    registered_instances++;
                    printf("✅ Instance %d registered (fd=%d), total: %d/%d\n", 
                           msg.instance_id, fd, registered_instances, ctx->num_instances);
                    fflush(stdout);
                } else if (msg.type == MSG_SYNC_POINT) {
                    // Handle sync point validation
                    handle_sync_point_message(&msg);
                } else if (msg.type == MSG_SHUTDOWN) {
                    printf("🛑 Instance %d shutting down\n", msg.instance_id);
                    fflush(stdout);
                }
            }
        }
    }
    
    printf("🎯 All instances registered, Unix socket coordinator ready for validation\n");
    fflush(stdout);
    
    // Continue handling messages until shutdown
    while (1) {
        read_fds = master_fds;
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select failed in coordinator main loop");
            break;
        }
        
        for (int fd = 0; fd <= max_fd; fd++) {
            if (fd != ctx->server_socket && FD_ISSET(fd, &read_fds)) {
                validation_message_t msg;
                int result = receive_validation_message(fd, &msg);
                
                if (result <= 0) {
                    close(fd);
                    FD_CLR(fd, &master_fds);
                    continue;
                }
                
                if (msg.type == MSG_SYNC_POINT) {
                    handle_sync_point_message(&msg);
                } else if (msg.type == MSG_SHUTDOWN) {
                    printf("🛑 Instance %d shutting down\n", msg.instance_id);
                    fflush(stdout);
                }
            }
        }
    }
    
    return NULL;
}
