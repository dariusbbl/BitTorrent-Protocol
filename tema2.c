#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

#define UPLOAD_TAG 0
#define DOWNLOAD_TAG 1
#define INIT_TAG 2
#define UPDATE_TAG 3

#define MAX_PEERS 10

// peer variables
typedef struct peer_segment {
    char hash[HASH_SIZE];
    int is_downloaded;
    int peers[MAX_FILES];
    int num_peers;
}peer_segment;
typedef struct peer_file {
    char name[MAX_FILENAME];
    int num_segments;
    struct peer_segment segments[MAX_CHUNKS];
    int is_finished;
}peer_file;

struct peer_file peer_files[MAX_FILES];
int num_peer_files = 0;
struct peer_file wanted_files[MAX_FILES];
int num_wanted_files = 0;


// tracker variables
typedef struct tracker_segment {
    char hash[HASH_SIZE];
    int peers[MAX_FILES];
    int num_peers;
}tracker_segment;

typedef struct tracker_file {
    char name[MAX_FILENAME];
    int num_segments;
    struct tracker_segment segments[MAX_CHUNKS];
}tracker_file;


// Write downloaded file
int write_file(int rank, const char file_name[], struct peer_file f) {
    if (!file_name || !f.segments || f.num_segments <= 0) {
        return -1;
    }

    // Create client-specific filename
    char full_filename[MAX_FILENAME];
    int result = snprintf(full_filename, sizeof(full_filename), "client%d_%s", rank, file_name);
    
    if (result < 0 || result >= sizeof(full_filename)) {
        return -1;  // Filename too long or encoding error
    }

    // Open file
    FILE* file = fopen(full_filename, "w");
    if (!file) {
        perror("Failed to open file");
        return -1;
    }

    // Write hash segments
    const char newline = '\n';
    for (int i = 0; i < f.num_segments; i++) {
        if (fwrite(f.segments[i].hash, sizeof(char), HASH_SIZE, file) != HASH_SIZE) {
            fclose(file);
            return -1;
        }

        // Add newline after all segments except the last one
        if (i < f.num_segments - 1) {
            if (fwrite(&newline, sizeof(char), 1, file) != 1) {
                fclose(file);
                return -1;
            }
        }
    }

    fclose(file);
    return 0;
}


// Init peer
int init_client(int numtasks, int rank) {
    // Generate and validate input filename
    char file_name[MAX_FILENAME];
    if (snprintf(file_name, sizeof(file_name), "in%d.txt", rank) >= sizeof(file_name)) {
        fprintf(stderr, "Error: Filename too long\n");
        return -1;
    }

    // Open and validate input file
    FILE* file = fopen(file_name, "r");
    if (!file) {
        fprintf(stderr, "Error: Cannot open file %s\n", file_name);
        return -1;
    }

    // Read owned files
    if (fscanf(file, "%d", &num_peer_files) != 1) {
        fprintf(stderr, "Error: Failed to read number of peer files\n");
        fclose(file);
        return -1;
    }

    for (int i = 0; i < num_peer_files; i++) {
        if (fscanf(file, "%s %d", peer_files[i].name, 
                  &peer_files[i].num_segments) != 2) {
            fprintf(stderr, "Error: Failed to read peer file info\n");
            fclose(file);
            return -1;
        }

        for (int j = 0; j < peer_files[i].num_segments; j++) {
            if (fscanf(file, "%s", peer_files[i].segments[j].hash) != 1) {
                fprintf(stderr, "Error: Failed to read segment hash\n");
                fclose(file);
                return -1;
            }
            peer_files[i].segments[j].is_downloaded = 1;
        }
    }

    // Read wanted files
    if (fscanf(file, "%d", &num_wanted_files) != 1) {
        fprintf(stderr, "Error: Failed to read number of wanted files\n");
        fclose(file);
        return -1;
    }

    for (int i = 0; i < num_wanted_files; i++) {
        if (fscanf(file, "%s", wanted_files[i].name) != 1) {
            fprintf(stderr, "Error: Failed to read wanted file name\n");
            fclose(file);
            return -1;
        }
        wanted_files[i].is_finished = 0;
    }

    fclose(file);

    // Send data to tracker
    if (MPI_Send(&num_peer_files, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
        fprintf(stderr, "Error: Failed to send number of peer files\n");
        return -1;
    }

    for (int i = 0; i < num_peer_files; i++) {
        // Send file name
        if (MPI_Send(peer_files[i].name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
            fprintf(stderr, "Error: Failed to send peer file name\n");
            return -1;
        }

        // Send number of segments
        if (MPI_Send(&peer_files[i].num_segments, 1, MPI_INT, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
            fprintf(stderr, "Error: Failed to send number of segments\n");
            return -1;
        }

        // Send segment hashes
        int j = 0;
        while (j < peer_files[i].num_segments) {
            if (MPI_Send(peer_files[i].segments[j].hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
                fprintf(stderr, "Error: Failed to send segment hash\n");
                return -1;
            }
            j++;
        }
    }

    // Wait for tracker acknowledgment
    char ack[4];
    if (MPI_Recv(ack, 4, MPI_CHAR, TRACKER_RANK, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
        fprintf(stderr, "Error: Failed to receive tracker acknowledgment\n");
        return -1;
    }

    return 0;
}


// Init tracker => returns number of tracker files
int init_tracker(int numtasks, struct tracker_file tracker_files[]) {
    int num_tracker_files = 0, peer_rank = 1;

    // Receive files from peers at init
    while (peer_rank < numtasks) {

        char cur_file_name[MAX_FILENAME];
        int cur_num_segments, num_files, j = 0;
        
        // Receive number of files from peer
        MPI_Recv(&num_files, 1, MPI_INT, peer_rank, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Process each file from current peer
        while (j < num_files) {
            // Receive file name and number of segments
            MPI_Recv(cur_file_name, 15, MPI_CHAR, peer_rank, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&cur_num_segments, 1, MPI_INT, peer_rank, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // Find if file already exists in tracker
            int file_index = -1, k = 0;
            while (k < num_tracker_files) {
                if (strcmp(tracker_files[k].name, cur_file_name) == 0) {
                    file_index = k;
                    break;
                }
                k++;
            }

            if (file_index == -1) {
                // New file
                file_index = num_tracker_files++;
                strcpy(tracker_files[file_index].name, cur_file_name);
                tracker_files[file_index].num_segments = cur_num_segments;

                // Receive segments for new file
                k = 0;
                while (k < cur_num_segments) {
                    char segment_hash[HASH_SIZE];
                    MPI_Recv(segment_hash, HASH_SIZE, MPI_CHAR, peer_rank, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    struct tracker_segment *seg = &tracker_files[file_index].segments[k];
                    memcpy(seg->hash, segment_hash, HASH_SIZE);
                    seg->num_peers = 1;
                    seg->peers[0] = peer_rank;
                    k += 1;
                }
            } else {
                // File exists, update peers for each segment
                k = 0;
                while (k < cur_num_segments) {
                    char tmp_hash[HASH_SIZE];
                    MPI_Recv(tmp_hash, HASH_SIZE, MPI_CHAR, peer_rank, INIT_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    
                    struct tracker_segment segment = tracker_files[file_index].segments[k];
                    segment.peers[segment.num_peers] = peer_rank;
                    segment.num_peers++;
                    k++;
                }
            }
            j++;
        }
        peer_rank++;
    }

    // Send ACK to all peers
    char ack[] = "ACK";
    for (int peer_rank = 1; peer_rank < numtasks; peer_rank++) {
        MPI_Send(ack, 4, MPI_CHAR, peer_rank, INIT_TAG, MPI_COMM_WORLD);
    }

    return num_tracker_files;
}


// Update tracker
int update_tracker(int rank) {
    // Send update notification
    char update_msg[10];
    if (snprintf(update_msg, sizeof(update_msg), "%dUPDATE", rank) >= sizeof(update_msg)) {
        fprintf(stderr, "Error: Update message buffer too small\n");
        return -1;
    }

    if (MPI_Send(update_msg, 10, MPI_CHAR, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
        fprintf(stderr, "Error: Failed to send update notification\n");
        return -1;
    }

    // Send total number of wanted files
    if (MPI_Send(&num_wanted_files, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
        fprintf(stderr, "Error: Failed to send number of wanted files\n");
        return -1;
    }

    // Process each wanted file
    for (int file_idx = 0; file_idx < num_wanted_files; file_idx++) {
        struct peer_file* current_file = &wanted_files[file_idx];

        // Send file name
        if (MPI_Send(current_file->name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
            fprintf(stderr, "Error: Failed to send file name\n");
            return -1;
        }

        // Count downloaded segments
        int downloaded_count = 0;
        for (int seg_idx = 0; seg_idx < current_file->num_segments; seg_idx++) {
            if (current_file->segments[seg_idx].is_downloaded) {
                downloaded_count++;
            }
        }

        // Send count of downloaded segments
        if (MPI_Send(&downloaded_count, 1, MPI_INT, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
            fprintf(stderr, "Error: Failed to send segment count\n");
            return -1;
        }

        // Send hashes of downloaded segments
        for (int seg_idx = 0; seg_idx < current_file->num_segments; seg_idx++) {
            if (current_file->segments[seg_idx].is_downloaded) {
                if (MPI_Send(current_file->segments[seg_idx].hash, HASH_SIZE, MPI_CHAR, TRACKER_RANK, UPDATE_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
                    fprintf(stderr, "Error: Failed to send segment hash\n");
                    return -1;
                }
            }
        }
    }

    return 0;
}


// Request file from tracker
int request_file(int wanted_file_index) {
   // Request file by sending name to tracker
   if (MPI_Send(wanted_files[wanted_file_index].name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) {
       return -1;
   }

   char file_name[MAX_FILENAME];
   int num_segments, i = 0;

   // Receive basic file info from tracker
   if (MPI_Recv(file_name, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE) != MPI_SUCCESS ||
       MPI_Recv(&num_segments, 1, MPI_INT, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
       return -1;
    }

   // Update file metadata
   strcpy(wanted_files[wanted_file_index].name, file_name);
   wanted_files[wanted_file_index].num_segments = num_segments;

   // Receive segment information
   while (i < num_segments) {
       // Get and verify peer count
       int num_peers;
       if (MPI_Recv(&num_peers, 1, MPI_INT, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
           return -1;
       }

       wanted_files[wanted_file_index].segments[i].num_peers = num_peers;

       // Get peers list
       int j = 0;
       while (j < num_peers) {
           if (MPI_Recv(&wanted_files[wanted_file_index].segments[i].peers[j], sizeof(int), MPI_INT, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
               return -1;
           }
           j++;
       }
       i++;
   }

   return 0;
}

struct segment_info {
    int index;
    int peers_count;
};

void notify_tracker_completion(int process_id) {
    char msg[10] = {0};
    snprintf(msg, 10, "%dFIN_ALL", process_id);
    MPI_Send(msg, 10, MPI_CHAR, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD);
}

void handle_periodic_updates(int process_id, long iter, int *completed_files) {
    // Update tracker only if completed_files exceeds threshold
    const int update_threshold = 10;

    if (*completed_files >= update_threshold) {
        update_tracker(process_id);
        *completed_files = 0;  // Reset counter
    }

    // Request updates periodically
    if (iter % 10 == 0) {
        char msg[10] = {0};
        snprintf(msg, sizeof(msg), "%dREQ_FILE", process_id);
        MPI_Send(msg, sizeof(msg), MPI_CHAR, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD);
        MPI_Send(&num_wanted_files, 1, MPI_INT, TRACKER_RANK, DOWNLOAD_TAG, MPI_COMM_WORLD);

        int i = 0;
        while (i < num_wanted_files) {
            request_file(i);
            i++;
        }
    }
}

struct segment_info find_missing_segment(int file_idx) {
    struct segment_info result = {-1, -1};
    
    for (int i = 0; i < wanted_files[file_idx].num_segments; i++) {
        if (!wanted_files[file_idx].segments[i].is_downloaded) {
            result.index = i;
            result.peers_count = wanted_files[file_idx].segments[i].num_peers;
            break;
        }
    }
    
    return result;
}

int try_download_segment(int process_id, int file_idx, struct segment_info *seg, long iter) {
    // Select peer using round-robin
    int peer = wanted_files[file_idx].segments[seg->index].peers[iter % seg->peers_count];
    
    // Send request
    char request[10] = {0};
    snprintf(request, sizeof(request), "%dREQ_SEG", process_id);

    // Trimiterea cererii către peer
    MPI_Send(request, sizeof(request), MPI_CHAR, peer, UPLOAD_TAG, MPI_COMM_WORLD);

    // Trimiterea informațiilor despre fișier și segment
    MPI_Send(wanted_files[file_idx].name, MAX_FILENAME, MPI_CHAR, peer, UPLOAD_TAG, MPI_COMM_WORLD);
    int segment_idx = seg->index;
    MPI_Send(&segment_idx, 1, MPI_INT, peer, UPLOAD_TAG, MPI_COMM_WORLD);

    // Get response 
    char resp[10] = {0};
    MPI_Recv(resp, 10, MPI_CHAR, peer, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    
    if (!strncmp(resp, "ACK", 3)) {
        wanted_files[file_idx].segments[seg->index].is_downloaded = 1;
        MPI_Recv(wanted_files[file_idx].segments[seg->index].hash, HASH_SIZE, MPI_CHAR, peer, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        return 1;
    }
    return 0;
}

int handle_download_complete(int process_id, int file_idx, int *completed_files) {
    // Check if file is complete
    for (int i = 0; i < wanted_files[file_idx].num_segments; i++) {
        if (!wanted_files[file_idx].segments[i].is_downloaded) return 0;
    }

    // Mark file as finished and write to disk
    wanted_files[file_idx].is_finished = 1;
    write_file(process_id, wanted_files[file_idx].name, wanted_files[file_idx]);

    // Update local state
    memcpy(&peer_files[num_peer_files++], &wanted_files[file_idx], sizeof(struct peer_file));
    for (int i = file_idx; i < num_wanted_files - 1; i++) {
        wanted_files[i] = wanted_files[i + 1];
    }
    num_wanted_files--;

    // Increment completed files count
    (*completed_files)++;

    // Check if all files are done 
    if (num_wanted_files == 0) {
        notify_tracker_completion(process_id);
        return 1;
    }
    return 0;
}


void *download_thread_func(void *arg) {
    int process_id = *(int *)arg;
    long iteration = 0;
    int completed_files = 0;

    // Exit early if no files to download 
    if (!num_wanted_files) {
        notify_tracker_completion(process_id);
        return NULL;
    }

    while (1) {
        handle_periodic_updates(process_id, iteration, &completed_files);

        int active_file = 0;
        struct segment_info next_segment = find_missing_segment(active_file);

        if (next_segment.index == -1) {
            iteration++;
            continue;
        }

        int success = try_download_segment(process_id, active_file, &next_segment, iteration);
        if (success) {
            int is_done = handle_download_complete(process_id, active_file, &completed_files);
            if (is_done) break;
        }

        iteration++;
    }

    return NULL;
}


struct segment_search_result {
    int found;
    int file_index;
    int segment_index;
};

struct segment_search_result locate_segment_in_files(int target_hash, const char file_name[], int check_downloaded) {
    struct segment_search_result result = {0, -1, -1};
    
    for (int file_idx = 0; file_idx < num_peer_files; file_idx++) {
        if (strncmp(peer_files[file_idx].name, file_name, MAX_FILENAME) != 0) {
            continue;
        }

        if (target_hash >= peer_files[file_idx].num_segments) {
            continue;
        }

        if (check_downloaded && !peer_files[file_idx].segments[target_hash].is_downloaded) {
            continue;
        }
        
        result.found = 1;
        result.file_index = file_idx;
        result.segment_index = target_hash;
        return result;

    }
    return result;
}

void *upload_thread_func(void *arg) {
    int node_id = *(int*)arg;
    char incoming_request[20];
    
    while (1) {
        // Wait for incoming request
        MPI_Recv(incoming_request, 20, MPI_CHAR, MPI_ANY_SOURCE, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Check termination condition
        if (!strcmp(incoming_request, "FIN")) {
            break;
        }

        // Validate request format
        if (strcmp(incoming_request + 1, "REQ_SEG") != 0) {
            continue;
        }

        // Extract requesting peer info
        char first_char[2] = {incoming_request[0], '\0'};
        int requester = atoi(first_char);
        
        // Receive segment hash
        char file_name[MAX_FILENAME] = {0};
        MPI_Recv(file_name, sizeof(file_name), MPI_CHAR, requester, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        // Primire index segment de la peer-ul care solicită
        int segment_index = -1;
        MPI_Recv(&segment_index, sizeof(segment_index), MPI_INT, requester, UPLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        // Search for requested segment 
        struct segment_search_result search_result;
        
        // Check completed files first
        search_result = locate_segment_in_files(segment_index,file_name,0);
                                              
        // If not found, check partially downloaded files
        if (!search_result.found) {
            search_result = locate_segment_in_files(segment_index,file_name,1);
        }

        // Send response to peer 
        const char* status = search_result.found ? "ACK" : "NACK";
        int response_size = search_result.found ? 4 : 5;
        
        if (!search_result.found) {
            printf("UPLOAD_PEER%d => I don't have the segment\n", node_id);
        }
        
        MPI_Send(status, response_size, MPI_CHAR, requester, DOWNLOAD_TAG, MPI_COMM_WORLD);
        
        if (search_result.found) {
            MPI_Send(peer_files[search_result.file_index].segments[search_result.segment_index].hash, HASH_SIZE, MPI_CHAR, requester, DOWNLOAD_TAG, MPI_COMM_WORLD);
        }
    }
    
    return NULL;
}

int find_file_index(const char *file_name, const struct tracker_file *tracker_files, int num_tracker_files) {
    for (int i = 0; i < num_tracker_files; i++) {
        if (strcmp(tracker_files[i].name, file_name) == 0) {
            return i;
        }
    }
    return -1;
}

void tracker(int numtasks, int rank) {
    struct tracker_file tracker_files[MAX_FILES];
    int num_files_to_download[numtasks + 1];
    int num_tracker_files = init_tracker(numtasks, tracker_files);

    // Initialize download counters
    int i = 0;
    while (i < numtasks + 1) {
        num_files_to_download[i] = MAX_FILES;
        i++;
    }

    while (1) {
        // Get command from any peer
        char command[20] = {0};
        MPI_Recv(command, sizeof(command), MPI_CHAR, MPI_ANY_SOURCE, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        int peer_rank = command[0] - '0';
        char* cmd = command + 1;

        // Handle REQ_FILE command
        if (strcmp(cmd, "REQ_FILE") == 0) {
            int num_files;
            MPI_Recv(&num_files, 1, MPI_INT, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            num_files_to_download[peer_rank] = num_files;

            i = 0; 
            while (i < num_files) {
                char file_name[MAX_FILENAME];
                MPI_Recv(file_name, MAX_FILENAME, MPI_CHAR, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int file_index = find_file_index(file_name, tracker_files, num_tracker_files);

                if (file_index == -1) {
                    printf("-- TRACKER => File %s not found\n", file_name);
                    continue;
                }

                // Send file data
                MPI_Send(tracker_files[file_index].name, 15, MPI_CHAR, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD);
                MPI_Send(&tracker_files[file_index].num_segments, 1, MPI_INT, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD);

                // Send segments
                int j = 0;
                while (j < tracker_files[file_index].num_segments) {
                    MPI_Send(&tracker_files[file_index].segments[j].num_peers, 1, MPI_INT, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD);
                    int k = 0; 
                    while (k < tracker_files[file_index].segments[j].num_peers) {
                        MPI_Send(&tracker_files[file_index].segments[j].peers[k], 1, MPI_INT, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD);
                        k++;
                    }
                    j++;
                }
                i++;
            }
        }

        // Handle UPDATE command 
        if (strcmp(cmd, "UPDATE") == 0) {
            MPI_Recv(&num_files_to_download[peer_rank], 1, MPI_INT, peer_rank, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            int i = 0;
            while (i < num_files_to_download[peer_rank]) {
                char file_name[MAX_FILENAME];
                MPI_Recv(file_name, MAX_FILENAME, MPI_CHAR, peer_rank, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                int file_index = find_file_index(file_name, tracker_files, num_tracker_files);

                if (file_index == -1) {
                    printf("TRACKER => File not found\n");
                    continue;
                }

                int num_downloaded;
                MPI_Recv(&num_downloaded, 1, MPI_INT, peer_rank, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                
                int j = 0;
                while (j < num_downloaded) {
                    char hash[HASH_SIZE];
                    MPI_Recv(hash, HASH_SIZE, MPI_CHAR, peer_rank, UPDATE_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                    int seg_index = -1, k = 0;
                    while (k < tracker_files[file_index].num_segments) {
                        if (strncmp(tracker_files[file_index].segments[k].hash, hash, HASH_SIZE) == 0) {
                            seg_index = k;
                            break;
                        }
                        k++;
                    }

                    if (seg_index == -1) {
                        printf("TRACKER => Segment not found\n");
                        continue;
                    }

                    // Add peer if not exists
                    int exists = 0;
                    
                    while (k < tracker_files[file_index].segments[seg_index].num_peers) {
                        if (tracker_files[file_index].segments[seg_index].peers[k] == peer_rank) {
                            exists = 1;
                            break;
                        }
                        k++;
                    }

                    if (!exists && tracker_files[file_index].segments[seg_index].num_peers < MAX_PEERS) {
                        int peer_count = tracker_files[file_index].segments[seg_index].num_peers;
                        tracker_files[file_index].segments[seg_index].peers[peer_count] = peer_rank;
                        tracker_files[file_index].segments[seg_index].num_peers++;
                    }
                    j++;
                }
                i++;
            }
        }

        // Handle FILE_COMPLETE command
        if (strcmp(cmd, "FILE_COMPLETE") == 0) {
            char file_name[MAX_FILENAME] = {0};
            MPI_Recv(file_name, sizeof(file_name), MPI_CHAR, peer_rank, DOWNLOAD_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            num_files_to_download[peer_rank]--;

            int file_index = find_file_index(file_name, tracker_files, num_tracker_files);

            if (file_index == -1) {
                printf("TRACKER => File %s not found\n", file_name);
                continue;
            }

            i = 0;
            while (i < tracker_files[file_index].num_segments) {
                int exists = 0, j = 0;
                int peer_count = tracker_files[file_index].segments[i].num_peers;
                
                while (j < peer_count) {
                    if (tracker_files[file_index].segments[i].peers[j] == peer_rank) {
                        exists = 1;
                        break;
                    }
                    j++;
                }

                if (!exists && peer_count < MAX_PEERS) {
                    tracker_files[file_index].segments[i].peers[peer_count] = peer_rank;
                    tracker_files[file_index].segments[i].num_peers++;
                }

                i++;
            }
        }

        // Handle FIN_ALL command
        if (strcmp(cmd, "FIN_ALL") == 0) {
            num_files_to_download[peer_rank] = 0;
        }

        // Check if everyone is done
        int all_done = 1, i = 1;
        while (i < numtasks && all_done) {
            if (num_files_to_download[i] > 0) {
                all_done = 0;
                break;
            }
            i++;
        }

        if (all_done) {
            i = 1;
            while (i < numtasks) {
                MPI_Send("FIN", 4, MPI_CHAR, i, UPLOAD_TAG, MPI_COMM_WORLD);
                i++;
            }
            break;
        }
    }
}

void peer(int numtasks, int rank) {
    // Initialize client state
    char input_file[MAX_FILENAME];
    snprintf(input_file, sizeof(input_file), "in%d.txt", rank);
    
    FILE* fp = fopen(input_file, "r");
    if (!fp) {
        printf("[PEER %d] Failed to open input file\n", rank);
        exit(-1);
    }

    if (init_client(numtasks, rank) != 0) {
        fprintf(stderr, "[PEER %d] Failed to initialize client state\n", rank);
        exit(-1);
    }

    // schelet
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }

}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}