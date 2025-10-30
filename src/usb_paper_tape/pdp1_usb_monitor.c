#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <glob.h>
#include <mntent.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// PDP-1 USB Port Assignments (loaded from config file)
#define CONFIG_FILE "/opt/pidp1/bin/usb_paper_tape_ports.cfg"

static int READER_BUS = 1;
static int READER_PORT = 2;
static int PUNCH_BUS = 1;
static int PUNCH_PORT = 3;

#define MAX_PATH 512
#define MAX_LINE 1024
#define MAX_DEVICES 32

typedef struct {
    int bus;
    int port;
    char device_name[32];     // sda, sdb, etc.
    char vendor[64];
    char model[64];
    char mount_point[MAX_PATH];
    char full_path[MAX_PATH];
    int is_reader;
    int is_punch;
} usb_device_t;

static int running = 1;
static usb_device_t previous_devices[MAX_DEVICES];
static int prev_device_count = 0;

void signal_handler(int sig) {
    (void)sig;
    running = 0;
    printf("\nShutting down PDP-1 USB monitor...\n");
}

// Load configuration from file
int load_config() {
    FILE* file = fopen(CONFIG_FILE, "r");
    if (!file) {
        printf("Warning: Cannot open config file %s, using defaults\n", CONFIG_FILE);
        printf("  READER: Bus %d Port %d\n", READER_BUS, READER_PORT);
        printf("  PUNCH:  Bus %d Port %d\n", PUNCH_BUS, PUNCH_PORT);
        return 0;  // Use defaults
    }

    char line[MAX_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), file)) {
        line_num++;

        // Remove trailing whitespace
        char* end = line + strlen(line) - 1;
        while (end > line && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') continue;

        // Parse configuration lines
        char key[64], value[64];
        if (sscanf(line, "%63s %63s", key, value) == 2) {
            if (strcmp(key, "READER_BUS") == 0) {
                READER_BUS = atoi(value);
            } else if (strcmp(key, "READER_PORT") == 0) {
                READER_PORT = atoi(value);
            } else if (strcmp(key, "PUNCH_BUS") == 0) {
                PUNCH_BUS = atoi(value);
            } else if (strcmp(key, "PUNCH_PORT") == 0) {
                PUNCH_PORT = atoi(value);
            } else {
                printf("Warning: Unknown config option '%s' on line %d\n", key, line_num);
            }
        } else {
            printf("Warning: Invalid config line %d: %s\n", line_num, line);
        }
    }

    fclose(file);
    printf("Configuration loaded from %s:\n", CONFIG_FILE);
    printf("  READER: Bus %d Port %d\n", READER_BUS, READER_PORT);
    printf("  PUNCH:  Bus %d Port %d\n", PUNCH_BUS, PUNCH_PORT);
    return 1;
}

// Send mount command to PDP-1 simulator
int send_mount_command(const char* filepath) {
    int sockfd;
    struct sockaddr_in server_addr;
    char command[MAX_PATH + 10];

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("   ❌ Failed to create socket for mount command\n");
        return 0;
    }

    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(1050);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Set socket timeout (1 second like ncat -w 1)
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("   ⚠️  Could not connect to PDP-1 simulator (localhost:1050)\n");
        close(sockfd);
        return 0;
    }

    // Send mount command
    snprintf(command, sizeof(command), "r %s\n", filepath);
    if (send(sockfd, command, strlen(command), 0) < 0) {
        printf("   ❌ Failed to send mount command\n");
        close(sockfd);
        return 0;
    }

    printf("   📡 Sent mount command to PDP-1 simulator: r %s\n", filepath);

    close(sockfd);
    return 1;
}

// Send punch command to PDP-1 simulator
int send_punch_command(const char* filepath) {
    int sockfd;
    struct sockaddr_in server_addr;
    char command[MAX_PATH + 10];

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("   ❌ Failed to create socket for punch command\n");
        return 0;
    }

    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(1050);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Set socket timeout (1 second like ncat -w 1)
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Connect to server
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("   ⚠️  Could not connect to PDP-1 simulator (localhost:1050)\n");
        close(sockfd);
        return 0;
    }

    // Send punch command
    snprintf(command, sizeof(command), "p %s\n", filepath);
    if (send(sockfd, command, strlen(command), 0) < 0) {
        printf("   ❌ Failed to send punch command\n");
        close(sockfd);
        return 0;
    }

    printf("   📡 Sent punch command to PDP-1 simulator: p %s\n", filepath);

    close(sockfd);
    return 1;
}

// Get mount point for a device (checks both device and first partition)
char* get_mount_point(const char* device_name, char* mount_buffer, size_t buffer_size) {
    FILE* mtab;
    struct mntent* mnt;
    char dev_path[64];
    char partition_path[64];

    // Try the main device first
    snprintf(dev_path, sizeof(dev_path), "/dev/%s", device_name);
    // Also try the first partition (e.g., sda1)
    snprintf(partition_path, sizeof(partition_path), "/dev/%s1", device_name);

    mtab = setmntent("/proc/mounts", "r");
    if (!mtab) return NULL;

    while ((mnt = getmntent(mtab)) != NULL) {
        if (strcmp(mnt->mnt_fsname, dev_path) == 0 || strcmp(mnt->mnt_fsname, partition_path) == 0) {
            strncpy(mount_buffer, mnt->mnt_dir, buffer_size - 1);
            mount_buffer[buffer_size - 1] = '\0';
            endmntent(mtab);
            return mount_buffer;
        }
    }

    endmntent(mtab);
    return NULL;
}

// Read file content, trimming whitespace
char* read_file_content(const char* filepath, char* buffer, size_t buffer_size) {
    FILE* file = fopen(filepath, "r");
    if (!file) return NULL;

    if (fgets(buffer, buffer_size, file)) {
        // Remove trailing whitespace
        char* end = buffer + strlen(buffer) - 1;
        while (end > buffer && (*end == '\n' || *end == '\r' || *end == ' ')) {
            *end = '\0';
            end--;
        }
        fclose(file);
        return buffer;
    }

    fclose(file);
    return NULL;
}

// Parse USB bus-port from sysfs path
int parse_usb_location(const char* device_name, int* bus, int* port) {
    char symlink_path[MAX_PATH];
    char real_path[MAX_PATH];
    ssize_t len;

    snprintf(symlink_path, sizeof(symlink_path), "/sys/block/%s", device_name);

    len = readlink(symlink_path, real_path, sizeof(real_path) - 1);
    if (len == -1) return 0;

    real_path[len] = '\0';

    // Look for USB pattern like ../devices/pci.../usb1/1-2/...
    char* usb_part = strstr(real_path, "/usb");
    if (!usb_part) return 0;

    // Find the bus-port pattern like "1-2"
    char* slash = strchr(usb_part + 4, '/');
    if (!slash) return 0;

    char bus_port[32];
    char* next_slash = strchr(slash + 1, '/');
    if (!next_slash) return 0;

    int len_to_copy = next_slash - slash - 1;
    if (len_to_copy >= (int)sizeof(bus_port)) return 0;

    strncpy(bus_port, slash + 1, len_to_copy);
    bus_port[len_to_copy] = '\0';

    // Parse "1-2" into bus=1, port=2
    char* dash = strchr(bus_port, '-');
    if (!dash) return 0;

    *dash = '\0';
    *bus = atoi(bus_port);
    *port = atoi(dash + 1);

    return 1;
}

// Get current USB storage devices
int get_current_devices(usb_device_t* devices, int max_devices) {
    glob_t glob_result;
    int device_count = 0;

    // Find all sd* block devices
    if (glob("/sys/block/sd*", GLOB_NOSORT, NULL, &glob_result) != 0) {
        return 0;
    }

    for (size_t i = 0; i < glob_result.gl_pathc && device_count < max_devices; i++) {
        char* path = glob_result.gl_pathv[i];
        char* device_name = strrchr(path, '/') + 1;  // Extract "sda" from "/sys/block/sda"

        usb_device_t* dev = &devices[device_count];
        memset(dev, 0, sizeof(usb_device_t));

        // Parse USB location
        if (!parse_usb_location(device_name, &dev->bus, &dev->port)) {
            continue;  // Not a USB device
        }

        strcpy(dev->device_name, device_name);

        // Get vendor and model
        char vendor_path[MAX_PATH], model_path[MAX_PATH];
        snprintf(vendor_path, sizeof(vendor_path), "/sys/block/%s/device/vendor", device_name);
        snprintf(model_path, sizeof(model_path), "/sys/block/%s/device/model", device_name);

        if (!read_file_content(vendor_path, dev->vendor, sizeof(dev->vendor))) {
            strcpy(dev->vendor, "Unknown");
        }
        if (!read_file_content(model_path, dev->model, sizeof(dev->model))) {
            strcpy(dev->model, "Device");
        }

        // Get mount point
        if (get_mount_point(device_name, dev->mount_point, sizeof(dev->mount_point))) {
            snprintf(dev->full_path, sizeof(dev->full_path), "/dev/%s", device_name);
        }

        // Check if it's READER or PUNCH
        dev->is_reader = (dev->bus == READER_BUS && dev->port == READER_PORT);
        dev->is_punch = (dev->bus == PUNCH_BUS && dev->port == PUNCH_PORT);

        device_count++;
    }

    globfree(&glob_result);
    return device_count;
}

// Find most recent .pt file in directory
char* find_most_recent_pt_file(const char* mount_point, char* filename_buffer, size_t filename_size, char* fullpath_buffer, size_t fullpath_size) {
    glob_t glob_result;
    char pattern[MAX_PATH];
    time_t newest_time = 0;
    char* newest_file = NULL;

    snprintf(pattern, sizeof(pattern), "%s/*.pt", mount_point);

    if (glob(pattern, GLOB_NOSORT, NULL, &glob_result) != 0) {
        return NULL;  // No .pt files found
    }

    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        struct stat st;
        if (stat(glob_result.gl_pathv[i], &st) == 0) {
            if (st.st_mtime > newest_time) {
                newest_time = st.st_mtime;
                newest_file = glob_result.gl_pathv[i];
            }
        }
    }

    if (newest_file) {
        strncpy(fullpath_buffer, newest_file, fullpath_size - 1);
        fullpath_buffer[fullpath_size - 1] = '\0';

        char* basename_start = strrchr(newest_file, '/');
        if (basename_start) {
            strncpy(filename_buffer, basename_start + 1, filename_size - 1);
            filename_buffer[filename_size - 1] = '\0';
        }

        globfree(&glob_result);
        return fullpath_buffer;
    }

    globfree(&glob_result);
    return NULL;
}

// List all .pt files in directory
void list_pt_files(const char* mount_point) {
    glob_t glob_result;
    char pattern[MAX_PATH];

    snprintf(pattern, sizeof(pattern), "%s/*.pt", mount_point);

    if (glob(pattern, GLOB_NOSORT, NULL, &glob_result) == 0 && glob_result.gl_pathc > 0) {
        // Sort filenames
        for (size_t i = 0; i < glob_result.gl_pathc - 1; i++) {
            for (size_t j = i + 1; j < glob_result.gl_pathc; j++) {
                if (strcmp(glob_result.gl_pathv[i], glob_result.gl_pathv[j]) > 0) {
                    char* temp = glob_result.gl_pathv[i];
                    glob_result.gl_pathv[i] = glob_result.gl_pathv[j];
                    glob_result.gl_pathv[j] = temp;
                }
            }
        }

        for (size_t i = 0; i < glob_result.gl_pathc; i++) {
            char* filename = strrchr(glob_result.gl_pathv[i], '/') + 1;
            printf("      %s\n", filename);
        }
    } else {
        printf("      (No .pt files found)\n");
    }

    if (glob_result.gl_pathc > 0) {
        globfree(&glob_result);
    }
}

// Handle READER connection
void handle_reader_connection(const usb_device_t* dev) {
    time_t now;
    struct tm* timeinfo;
    char timestr[32];

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestr, sizeof(timestr), "%H:%M:%S", timeinfo);

    printf("📖 [%s] TAPE READER CONNECTED: Bus %d Port %d: %s %s (/dev/%s)",
           timestr, dev->bus, dev->port, dev->vendor, dev->model, dev->device_name);

    if (strlen(dev->mount_point) > 0) {
        printf(" -> %s [READER]\n", dev->mount_point);

        printf("   📄 Paper tape files (.pt) on READER:\n");
        list_pt_files(dev->mount_point);

        char filename[256], fullpath[MAX_PATH];
        if (find_most_recent_pt_file(dev->mount_point, filename, sizeof(filename), fullpath, sizeof(fullpath))) {
            printf("--->Mount Reader: %s %s\n", filename, fullpath);
            send_mount_command(fullpath);
        } else {
            printf("--->No file to mount\n");
        }
    } else {
        printf(" [READER]\n");
        // Wait a moment for mounting and try again
        sleep(2);
        char mount_point[MAX_PATH];
        if (get_mount_point(dev->device_name, mount_point, sizeof(mount_point))) {
            printf("   📄 Paper tape files (.pt) on READER:\n");
            list_pt_files(mount_point);

            char filename[256], fullpath[MAX_PATH];
            if (find_most_recent_pt_file(mount_point, filename, sizeof(filename), fullpath, sizeof(fullpath))) {
                printf("--->Mount Reader: %s %s\n", filename, fullpath);
                send_mount_command(fullpath);
            } else {
                printf("--->No file to mount\n");
            }
        } else {
            printf("--->No file to mount\n");
        }
    }
}

// Handle PUNCH connection
void handle_punch_connection(const usb_device_t* dev) {
    time_t now;
    struct tm* timeinfo;
    char timestr[32], timestamp[64];

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestr, sizeof(timestr), "%H:%M:%S", timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);

    printf("🥊 [%s] TAPE PUNCH CONNECTED: Bus %d Port %d: %s %s (/dev/%s)",
           timestr, dev->bus, dev->port, dev->vendor, dev->model, dev->device_name);

    char mount_point[MAX_PATH];
    if (strlen(dev->mount_point) > 0) {
        strcpy(mount_point, dev->mount_point);
        printf(" -> %s [PUNCH]\n", mount_point);
    } else {
        printf(" [PUNCH]\n");
        // Wait a moment for mounting
        sleep(2);
        if (!get_mount_point(dev->device_name, mount_point, sizeof(mount_point))) {
            printf("   ❌ Device not mounted - cannot create file\n");
            return;
        }
        printf("   Device mounted at: %s\n", mount_point);
    }

    char punch_file[MAX_PATH + 128];  // Generous space for mount point + timestamp + extension
    snprintf(punch_file, sizeof(punch_file), "%s/%s.pt", mount_point, timestamp);

    printf("   📝 Prepared paper tape file: %s.pt\n", timestamp);
    printf("Punch: %s %s\n", timestamp, punch_file);

    // Send punch command to PDP-1 simulator
    send_punch_command(punch_file);
}

// Handle device disconnection
void handle_device_disconnection(const usb_device_t* dev) {
    time_t now;
    struct tm* timeinfo;
    char timestr[32];

    time(&now);
    timeinfo = localtime(&now);
    strftime(timestr, sizeof(timestr), "%H:%M:%S", timeinfo);

    if (dev->is_reader) {
        printf("📖 [%s] TAPE READER DISCONNECTED: Bus %d Port %d (was: %s %s)\n",
               timestr, dev->bus, dev->port, dev->vendor, dev->model);
    } else if (dev->is_punch) {
        printf("🥊 [%s] TAPE PUNCH DISCONNECTED: Bus %d Port %d (was: %s %s)\n",
               timestr, dev->bus, dev->port, dev->vendor, dev->model);
    } else {
        printf("🔴 [%s] USB STICK REMOVED: Bus %d Port %d (was: %s %s)\n",
               timestr, dev->bus, dev->port, dev->vendor, dev->model);
    }
}

// Check for device changes
void check_device_changes(usb_device_t* current_devices, int current_count) {
    // Find newly added devices
    for (int i = 0; i < current_count; i++) {
        int found = 0;
        for (int j = 0; j < prev_device_count; j++) {
            if (strcmp(current_devices[i].device_name, previous_devices[j].device_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            // New device added
            if (current_devices[i].is_reader) {
                handle_reader_connection(&current_devices[i]);
            } else if (current_devices[i].is_punch) {
                handle_punch_connection(&current_devices[i]);
            } else {
                time_t now;
                struct tm* timeinfo;
                char timestr[32];

                time(&now);
                timeinfo = localtime(&now);
                strftime(timestr, sizeof(timestr), "%H:%M:%S", timeinfo);

                printf("🟢 [%s] USB STICK INSERTED: Bus %d Port %d: %s %s (/dev/%s)\n",
                       timestr, current_devices[i].bus, current_devices[i].port,
                       current_devices[i].vendor, current_devices[i].model, current_devices[i].device_name);
            }
        }
    }

    // Find removed devices
    for (int i = 0; i < prev_device_count; i++) {
        int found = 0;
        for (int j = 0; j < current_count; j++) {
            if (strcmp(previous_devices[i].device_name, current_devices[j].device_name) == 0) {
                found = 1;
                break;
            }
        }

        if (!found) {
            // Device removed
            handle_device_disconnection(&previous_devices[i]);
        }
    }

    // Update previous devices
    memcpy(previous_devices, current_devices, sizeof(usb_device_t) * current_count);
    prev_device_count = current_count;
}

// USB Port Configuration Mode
void usb_port_scan_mode() {
    printf("PDP-1 USB Port Configuration Mode\n");
    printf("=================================\n");
    printf("Please insert USB sticks into the ports you want to use.\n");
    printf("The program will ask you to assign them as READER or PUNCH.\n");
    printf("Press Ctrl+C to exit at any time.\n\n");

    int reader_bus = -1, reader_port = -1;
    int punch_bus = -1, punch_port = -1;
    int assignments_made = 0;

    usb_device_t previous_scan[MAX_DEVICES];
    int prev_scan_count = 0;

    while (assignments_made < 2) {
        usb_device_t current_scan[MAX_DEVICES];
        int current_scan_count = get_current_devices(current_scan, MAX_DEVICES);

        // Find newly inserted devices
        for (int i = 0; i < current_scan_count; i++) {
            int found = 0;
            for (int j = 0; j < prev_scan_count; j++) {
                if (strcmp(current_scan[i].device_name, previous_scan[j].device_name) == 0) {
                    found = 1;
                    break;
                }
            }

            if (!found) {
                // New USB stick detected
                printf("📀 USB stick detected: Bus %d Port %d (%s %s)\n",
                       current_scan[i].bus, current_scan[i].port,
                       current_scan[i].vendor, current_scan[i].model);

                char response;
                printf("Assign this port as (r)eader or (p)unch? (r/p): ");
                fflush(stdout);

                if (scanf(" %c", &response) == 1) {
                    if (response == 'r' || response == 'R') {
                        reader_bus = current_scan[i].bus;
                        reader_port = current_scan[i].port;
                        printf("✅ READER assigned to Bus %d Port %d\n\n", reader_bus, reader_port);
                        assignments_made++;
                    } else if (response == 'p' || response == 'P') {
                        punch_bus = current_scan[i].bus;
                        punch_port = current_scan[i].port;
                        printf("✅ PUNCH assigned to Bus %d Port %d\n\n", punch_bus, punch_port);
                        assignments_made++;
                    } else {
                        printf("Invalid choice. Please enter 'r' for reader or 'p' for punch.\n\n");
                    }
                } else {
                    printf("Invalid input. Please try again.\n\n");
                }

                if (assignments_made == 1) {
                    printf("Please insert a second USB stick for the remaining assignment.\n\n");
                }
            }
        }

        // Update previous state
        memcpy(previous_scan, current_scan, sizeof(usb_device_t) * current_scan_count);
        prev_scan_count = current_scan_count;

        sleep(1);  // Check every second
    }

    // Both ports assigned
    printf("Configuration complete:\n");
    printf("  READER: Bus %d Port %d\n", reader_bus, reader_port);
    printf("  PUNCH:  Bus %d Port %d\n", punch_bus, punch_port);
    printf("\n");

    // Ask to save configuration
    char save_response;
    printf("Save this configuration to %s? (y/n): ", CONFIG_FILE);
    fflush(stdout);

    if (scanf(" %c", &save_response) == 1 && (save_response == 'y' || save_response == 'Y')) {
        FILE* config_file = fopen(CONFIG_FILE, "w");
        if (config_file) {
            fprintf(config_file, "# PDP-1 USB Paper Tape Port Configuration\n");
            fprintf(config_file, "# Generated by USB port scan mode\n");
            fprintf(config_file, "#\n");
            fprintf(config_file, "# READER - Paper tape reader (input device)\n");
            fprintf(config_file, "# PUNCH  - Paper tape punch (output device)\n\n");
            fprintf(config_file, "READER_BUS %d\n", reader_bus);
            fprintf(config_file, "READER_PORT %d\n", reader_port);
            fprintf(config_file, "PUNCH_BUS %d\n", punch_bus);
            fprintf(config_file, "PUNCH_PORT %d\n", punch_port);
            fclose(config_file);
            printf("✅ Configuration saved to %s\n", CONFIG_FILE);
        } else {
            printf("❌ Error: Could not write to %s\n", CONFIG_FILE);
            printf("You may need to run with sudo or check file permissions.\n");
        }
    } else {
        printf("Configuration not saved.\n");
    }

    printf("\nYou can now run the monitor in normal mode without -scan.\n");
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Check for -scan command line argument
    if (argc > 1 && strcmp(argv[1], "-scan") == 0) {
        usb_port_scan_mode();
        return 0;
    }

    printf("PDP-1 USB Storage Monitor (C version)\n");
    printf("=====================================\n");

    // Load configuration
    load_config();
    printf("\n");

    // Get initial device state
    usb_device_t current_devices[MAX_DEVICES];
    int current_count = get_current_devices(current_devices, MAX_DEVICES);

    printf("Current USB Storage Devices:\n");
    printf("----------------------------\n");

    for (int i = 0; i < current_count; i++) {
        printf("Bus %d Port %d: %s %s (/dev/%s)",
               current_devices[i].bus, current_devices[i].port,
               current_devices[i].vendor, current_devices[i].model,
               current_devices[i].device_name);

        if (strlen(current_devices[i].mount_point) > 0) {
            printf(" -> %s", current_devices[i].mount_point);
        }

        if (current_devices[i].is_reader) {
            printf(" [READER]");
        } else if (current_devices[i].is_punch) {
            printf(" [PUNCH]");
        }
        printf("\n");
    }

    if (current_count == 0) {
        printf("No USB storage devices currently detected.\n");
    }

    printf("\nFound %d USB storage device(s). Monitoring for changes...\n", current_count);
    printf("========================\n");

    // Copy to previous state
    memcpy(previous_devices, current_devices, sizeof(usb_device_t) * current_count);
    prev_device_count = current_count;

    // Monitoring loop
    while (running) {
        sleep(2);  // Poll every 2 seconds

        current_count = get_current_devices(current_devices, MAX_DEVICES);
        check_device_changes(current_devices, current_count);
    }

    return 0;
}