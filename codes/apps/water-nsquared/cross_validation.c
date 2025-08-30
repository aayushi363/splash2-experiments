#define _GNU_SOURCE
#include "cross_validation.h"
#include <stdarg.h>
#include <time.h>
#include <math.h>

#include <ctype.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

// DMTCP support
#ifdef DMTCP
#include "dmtcp.h"
#endif

// Global validation state
validation_context_t *g_validation_context = NULL;
cross_validation_t *g_validation = NULL;
int g_validation_enabled = 0;
int g_instance_id = 0;
char g_socket_path[256];
int g_client_fds[16];
int g_sync_point_counter = 0;  // Sequential counter for unique sync points

// DMTCP checkpoint state
static int saved_instance_id = -1;
static int saved_num_instances = -1;
static volatile int checkpoint_in_progress = 0;

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

// Forward declaration for DMTCP functions
static void close_validation_sockets(void);

int init_cross_validation(int instance_id, int num_instances) {
    if (num_instances > MAX_INSTANCES) {
        fprintf(stderr, "❌ Too many instances: %d (max: %d)\n", num_instances, MAX_INSTANCES);
        return -1;
    }
    
    g_instance_id = instance_id;

    // Get server address and port from environment or use defaults
    const char *server_addr_env = getenv("CROSS_VALIDATION_SERVER_ADDR");
    const char *server_port_env = getenv("CROSS_VALIDATION_SERVER_PORT");
    char server_addr_str[64];
    int server_port = 5000;
    if (server_addr_env && server_addr_env[0]) {
        strncpy(server_addr_str, server_addr_env, sizeof(server_addr_str)-1);
        server_addr_str[sizeof(server_addr_str)-1] = '\0';
    } else {
        strcpy(server_addr_str, "0.0.0.0");
    }
    if (server_port_env && server_port_env[0]) {
        server_port = atoi(server_port_env);
        if (server_port <= 0) server_port = 5000;
    }
    printf("🔧 Instance %d using TCP validation at %s:%d\n", instance_id, server_addr_str, server_port);
    fflush(stdout);
    
    // Allocate validation context
    g_validation_context = (validation_context_t*)calloc(1, sizeof(validation_context_t));
    if (!g_validation_context) {
        perror("Failed to allocate validation context");
        return -1;
    }
    
    g_validation_context->instance_id = instance_id;
    g_validation_context->num_instances = num_instances;
    g_validation_context->is_coordinator = (instance_id == 0);

    // If this is the coordinator (instance 0), set up TCP server
    if (g_validation_context->is_coordinator) {
        g_validation_context->server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (g_validation_context->server_socket < 0) {
            perror("Failed to create TCP server socket");
            free(g_validation_context);
            return -1;
        }
        int optval = 1;
        setsockopt(g_validation_context->server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_addr_str, &server_addr.sin_addr) <= 0) {
            perror("Invalid server address");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        if (bind(g_validation_context->server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            perror("Failed to bind TCP server socket");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        if (listen(g_validation_context->server_socket, MAX_INSTANCES) < 0) {
            perror("Failed to listen on TCP server socket");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        printf("🎯 TCP coordinator ready, bound and listening on %s:%d\n", server_addr_str, server_port);
        fflush(stdout);
        g_coordinator_state.current_sync_point = -1;
        g_coordinator_state.instances_arrived = 0;
        g_coordinator_state.validation_failed = 0;
        g_coordinator_state.num_instances = num_instances;
        if (pthread_create(&g_validation_context->coordinator_thread, NULL, coordinator_thread_func, g_validation_context) != 0) {
            perror("Failed to create coordinator thread");
            close(g_validation_context->server_socket);
            free(g_validation_context);
            return -1;
        }
        printf("🔍 Cross-validation coordinator started with TCP %s:%d for %d instances\n", server_addr_str, server_port, num_instances);
        fflush(stdout);
    }
    usleep(200000);
    g_validation_context->client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_validation_context->client_socket < 0) {
        perror("Failed to create TCP client socket");
        cleanup_cross_validation();
        return -1;
    }
    struct sockaddr_in coord_addr;
    memset(&coord_addr, 0, sizeof(coord_addr));
    coord_addr.sin_family = AF_INET;
    coord_addr.sin_port = htons(server_port);
    if (inet_pton(AF_INET, server_addr_str, &coord_addr.sin_addr) <= 0) {
        perror("Invalid coordinator address");
        cleanup_cross_validation();
        return -1;
    }
    int connect_attempts = 0;
    while (connect(g_validation_context->client_socket, (struct sockaddr*)&coord_addr, sizeof(coord_addr)) < 0) {
        if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR || errno == EAGAIN) {
            usleep(100000);
            if (++connect_attempts > 50) {
                perror("Failed to connect to coordinator TCP socket (timeout)");
                cleanup_cross_validation();
                return -1;
            }
            continue;
        }
        perror("Failed to connect to coordinator TCP socket");
        cleanup_cross_validation();
        return -1;
    }
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
    printf("✅ Instance %d connected to TCP-based cross-validation system\n", instance_id);
    fflush(stdout);
    return 0;
}

static void close_validation_sockets(void) {
    if (!g_validation_context) return;
    
    checkpoint_in_progress = 1;
    
    // Stop coordinator thread if running
    if (g_validation_context->is_coordinator && g_validation_context->coordinator_thread) {
        pthread_cancel(g_validation_context->coordinator_thread);
        pthread_join(g_validation_context->coordinator_thread, NULL);
        g_validation_context->coordinator_thread = 0;
    }
    
    // Close all sockets
    if (g_validation_context->client_socket >= 0) {
        close(g_validation_context->client_socket);
        g_validation_context->client_socket = -1;
    }
    
    if (g_validation_context->server_socket >= 0) {
        close(g_validation_context->server_socket);
        g_validation_context->server_socket = -1;
    }
    
    // Close all client fds in coordinator
    for (int i = 0; i < MAX_INSTANCES; i++) {
        if (g_client_fds[i] >= 0) {
            close(g_client_fds[i]);
            g_client_fds[i] = -1;
        }
    }
    
    // Remove socket file
    if (g_validation_context->is_coordinator) {
        unlink(g_socket_path);
    }
}

void cleanup_cross_validation() {
    if (!g_validation_enabled || !g_validation_context) return;

    // Send shutdown message if socket is still open
    if (g_validation_context->client_socket >= 0) {
        validation_message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.type = MSG_SHUTDOWN;
        msg.instance_id = g_instance_id;
        send_validation_message(&msg);
    }

    close_validation_sockets();

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
    
    // Skip validation if checkpoint is in progress
    if (checkpoint_in_progress) {
        printf("[CV-DEBUG] Skipping validation during checkpoint\n");
        return;
    }
    
    int unique_sync_point = ++g_sync_point_counter;
    printf("🔧 Instance %d sending sync point %d: %s\n", g_instance_id, unique_sync_point, fingerprint);
    fflush(stdout);
    
    validation_message_t msg;
    msg.type = MSG_SYNC_POINT;
    msg.instance_id = g_instance_id;
    msg.sync_point = unique_sync_point;
    strncpy(msg.fingerprint, fingerprint, MAX_FINGERPRINT_LEN - 1);
    msg.fingerprint[MAX_FINGERPRINT_LEN - 1] = '\0';
    
    // Send with proper EINTR handling
    size_t total_sent = 0;
    char *buffer = (char*)&msg;
    while (total_sent < sizeof(msg)) {
        ssize_t sent = send(g_validation_context->client_socket, 
                           buffer + total_sent, 
                           sizeof(msg) - total_sent, 
                           MSG_NOSIGNAL);
        
        if (sent > 0) {
            total_sent += sent;
        } else if (sent < 0) {
            if (errno == EINTR) {
                continue;  // Retry immediately on interrupt
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer full, wait a bit
                usleep(10000);
                continue;
            } else {
                printf("❌ Instance %d failed to send sync point message: %s\n", 
                       g_instance_id, strerror(errno));
                return;
            }
        }
    }
    
    // Receive response with timeout using select
    validation_message_t response;
    fd_set readfds;
    struct timeval tv;
    int max_wait_ms = 5000;  // 5 second total timeout
    int elapsed_ms = 0;
    
    size_t total_received = 0;
    char *recv_buffer = (char*)&response;
    
    while (total_received < sizeof(response) && elapsed_ms < max_wait_ms) {
        FD_ZERO(&readfds);
        FD_SET(g_validation_context->client_socket, &readfds);
        
        tv.tv_sec = 0;
        tv.tv_usec = 100000;  // 100ms per select
        
        int ret = select(g_validation_context->client_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;  // Retry on interrupt
            }
            printf("❌ Instance %d select failed: %s\n", g_instance_id, strerror(errno));
            return;
        }
        
        if (ret == 0) {
            // Timeout - no data yet
            elapsed_ms += 100;
            continue;
        }
        
        // Data available, try to receive
        ssize_t received = recv(g_validation_context->client_socket,
                               recv_buffer + total_received,
                               sizeof(response) - total_received,
                               MSG_DONTWAIT);
        
        if (received > 0) {
            total_received += received;
        } else if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                elapsed_ms += 100;
                continue;
            }
            printf("❌ Instance %d failed to receive validation result: %s\n", 
                   g_instance_id, strerror(errno));
            return;
        } else {
            // Connection closed
            printf("❌ Instance %d connection closed while waiting for validation\n", g_instance_id);
            return;
        }
    }
    
    if (total_received < sizeof(response)) {
        printf("⚠️ Instance %d timeout waiting for validation response\n", g_instance_id);
        return;
    }
    
    // Process response as before
    if (response.type == MSG_VALIDATION_RESULT) {
        if (response.validation_passed) {
            printf("✅ SYNCHRONIZED MATCH at sync point %d: %s\n", unique_sync_point, fingerprint);
            fflush(stdout);
        } else {
            const char* other_fingerprint = response.mismatch_details;
            printf("❌ CLIENT MISMATCH DETECTED at sync point %d\n", unique_sync_point);
            printf("🔍 Local fingerprint: %s\n", fingerprint);
            printf("🔍 Other fingerprint: %s\n", other_fingerprint);
            fflush(stdout);
            fprintf(stderr, "🚨 CLIENT ASSERTION FAILED: Synchronized cross-validation failed!\n");
            fprintf(stderr, "Instance %d at sync point %d:\n", g_instance_id, unique_sync_point);
            fprintf(stderr, "  Local:  %s\n", fingerprint);
            fprintf(stderr, "  Other:  %s\n", other_fingerprint);
            fprintf(stderr, "💥 Client terminating due to validation mismatch.\n");
            fflush(stderr);
            assert(0 && "Client: Synchronized cross-validation fingerprint mismatch detected");
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
    
    size_t total_sent = 0;
    char *buffer = (char*)msg;
    size_t remaining = sizeof(*msg);
    
    while (remaining > 0) {
        ssize_t bytes_sent = send(g_validation_context->client_socket, 
                                  buffer + total_sent, 
                                  remaining, 
                                  MSG_NOSIGNAL);
        if (bytes_sent > 0) {
            total_sent += bytes_sent;
            remaining -= bytes_sent;
        } else if (bytes_sent < 0) {
            if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            perror("Failed to send validation message");
            return -1;
        }
    }
    return 0;
}

int receive_validation_message(int client_socket, validation_message_t *msg) {
    size_t total_received = 0;
    char *buffer = (char*)msg;
    size_t remaining = sizeof(*msg);
    
    // Try to receive with non-blocking and handle partial messages
    while (remaining > 0) {
        ssize_t received = recv(client_socket, buffer + total_received, remaining, MSG_DONTWAIT);
        
        if (received > 0) {
            total_received += received;
            remaining -= received;
        } else if (received == 0) {
            // Connection closed
            if (total_received > 0) {
                // Partial message received before close
                return -1;
            }
            return 0;  // Clean close
        } else {
            // Error occurred
            if (errno == EINTR) {
                // Interrupted by signal, retry
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available right now
                if (total_received == 0) {
                    // Nothing received yet, return special code
                    return -2;
                }
                // Partial message, wait a bit and retry
                usleep(10000);
                continue;
            } else {
                // Real error
                perror("recv failed in receive_validation_message");
                return -1;
            }
        }
    }
    
    return 1;  // Complete message received
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

    // Don't send immediate ACK - wait for all instances to arrive for synchronized comparison
    // This ensures both coordinator and client wait at sync point as required
    
    // Check if all instances have arrived for SYNCHRONIZED comparison
    if (g_coordinator_state.instances_arrived == g_coordinator_state.num_instances) {
        printf("🔄 All %d instances arrived for sync point %d - performing SYNCHRONIZED comparison...\n", 
               g_coordinator_state.num_instances, (int)msg->sync_point);
        
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
            printf("✅ SYNCHRONIZED MATCH at sync point %d: %s\n", (int)msg->sync_point, g_coordinator_state.fingerprints[0]);
            fflush(stdout);
        } else {
            printf("❌ SYNCHRONIZED MISMATCH at sync point %d: %s\n", (int)msg->sync_point, mismatch_details);
            printf("🔍 COMPARISON DETAILS: %s\n", mismatch_details);
            fflush(stdout);
            
            // Coordinator assertion on mismatch
            fprintf(stderr, "\n🚨 COORDINATOR ASSERTION FAILED: Synchronized cross-validation failed!\n");
            fprintf(stderr, "🔍 Details: %s\n", mismatch_details);
            fprintf(stderr, "💥 Coordinator terminating due to validation mismatch.\n\n");
            fflush(stderr);
            assert(0 && "Coordinator: Synchronized cross-validation fingerprint mismatch detected");
        }
        
        // Send comparison results to ALL instances so they can also compare and assert
        for (int i = 0; i < g_coordinator_state.instances_arrived; i++) {
            int instance_id = g_coordinator_state.instance_ids[i];
            if (instance_id >= 0 && instance_id < MAX_INSTANCES) {
                int client_fd = g_client_fds[instance_id];
                if (client_fd > 0) {
                    validation_message_t response;
                    response.type = MSG_VALIDATION_RESULT;
                    response.instance_id = -1; // From coordinator
                    response.sync_point = msg->sync_point;
                    response.validation_passed = match;
                    
                    // Send OTHER instance's fingerprint so client can compare locally
                    if (g_coordinator_state.num_instances == 2) {
                        int other_idx = (i == 0) ? 1 : 0;  // Get the other instance's index
                        strncpy(response.mismatch_details, g_coordinator_state.fingerprints[other_idx], 
                               sizeof(response.mismatch_details) - 1);
                        response.mismatch_details[sizeof(response.mismatch_details) - 1] = '\0';
                    } else {
                        response.mismatch_details[0] = '\0';
                    }
                    
                    ssize_t sent = send(client_fd, &response, sizeof(response), MSG_NOSIGNAL);
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
    
    // Set timeout for select to avoid indefinite blocking
    struct timeval tv;
    
    while (registered_instances < ctx->num_instances) {
        read_fds = master_fds;
        
        // Use 100ms timeout for select
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal (likely checkpoint), continue
                continue;
            }
            perror("select failed in coordinator");
            break;
        }
        
        if (ret == 0) {
            // Timeout - no events, continue loop
            continue;
        }
        
        // Check for new connections
        if (FD_ISSET(ctx->server_socket, &read_fds)) {
            struct sockaddr_un client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(ctx->server_socket, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("accept failed");
                continue;
            }
            
            // Set new client socket to non-blocking
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            
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
                
                if (result == -2) {
                    // Timeout or would block - continue
                    continue;
                }
                
                if (result <= 0) {
                    // Client disconnected or error
                    close(fd);
                    FD_CLR(fd, &master_fds);
                    continue;
                }
                
                if (msg.type == MSG_REGISTER_INSTANCE) {
                    client_fds[msg.instance_id] = fd;
                    g_client_fds[msg.instance_id] = fd;
                    registered_instances++;
                    printf("✅ Instance %d registered (fd=%d), total: %d/%d\n", 
                           msg.instance_id, fd, registered_instances, ctx->num_instances);
                    fflush(stdout);
                } else if (msg.type == MSG_SYNC_POINT) {
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
        
        // Use 100ms timeout
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                // Interrupted by signal, continue
                continue;
            }
            perror("select failed in coordinator main loop");
            break;
        }
        
        if (ret == 0) {
            // Timeout - no events, continue
            continue;
        }
        
        for (int fd = 0; fd <= max_fd; fd++) {
            if (fd != ctx->server_socket && FD_ISSET(fd, &read_fds)) {
                validation_message_t msg;
                int result = receive_validation_message(fd, &msg);
                
                if (result == -2) {
                    // Timeout or would block - continue
                    continue;
                }
                
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

#ifdef DMTCP
// DMTCP Event Hook function
static void cross_validation_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data) {
    switch (event) {
        case DMTCP_EVENT_INIT:
            // Plugin initialization - nothing needed here
            printf("[CV-DEBUG] DMTCP plugin initialized\n");
            fflush(stdout);
            break;
            
        case DMTCP_EVENT_PRECHECKPOINT:
            printf("[CV-DEBUG] DMTCP Pre-checkpoint: Closing validation sockets\n");
            fflush(stdout);
            
            if (g_validation_context) {
                // Save configuration
                saved_instance_id = g_validation_context->instance_id;
                saved_num_instances = g_validation_context->num_instances;
                
                // Close all sockets to prevent DMTCP from trying to checkpoint them
                close_validation_sockets();
                
                // Free the context
                free(g_validation_context);
                g_validation_context = NULL;
                g_validation_enabled = 0;
            }
            break;
            
        case DMTCP_EVENT_RESUME:
            printf("[CV-DEBUG] DMTCP Resume: Reinitializing validation system\n");
            fflush(stdout);
            
            checkpoint_in_progress = 0;
            
            if (saved_instance_id >= 0 && saved_num_instances > 0) {
                // Reset globals
                g_sync_point_counter = 0;
                memset(g_client_fds, -1, sizeof(g_client_fds));
                
                // Small delay to ensure both processes are ready
                usleep(500000); // 500ms
                
                // Reinitialize the validation system
                printf("[CV-DEBUG] Reinitializing validation for instance %d of %d\n", 
                       saved_instance_id, saved_num_instances);
                fflush(stdout);
                
                init_cross_validation(saved_instance_id, saved_num_instances);
            }
            break;
            
        case DMTCP_EVENT_RESTART:
            // This is for restart from checkpoint file, not resume
            // We don't handle this case for now
            printf("[CV-DEBUG] DMTCP Restart from checkpoint - not handled\n");
            fflush(stdout);
            break;
            
        default:
            break;
    }
}

// Plugin descriptor
static DmtcpPluginDescriptor_t cross_validation_plugin = {
    DMTCP_PLUGIN_API_VERSION,
    DMTCP_PACKAGE_VERSION,
    "cross_validation",
    "DMTCP",
    "dmtcp@ccs.neu.edu",
    "Cross-validation plugin for synchronized validation",
    cross_validation_EventHook
};

// Register the plugin (this macro expands to the constructor function)
DMTCP_DECL_PLUGIN(cross_validation_plugin);
#endif // DMTCP