#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

#define NUM_THREADS 16
#define REQUEST_FILE "requests.txt"
#define PROGRESS_FILE "progress.json"
#define DONE_DIR "done"
#define CHUNK_BUF_SIZE 65536 // 64KB buffer for maximum throughput

struct ThreadArgs {
    int thread_id;
    char host[256];
    char path[2048];
    int port;
    long start_byte;
    long end_byte;
    long file_size;
    int out_fd;
    pthread_mutex_t* file_mutex;
    pthread_mutex_t* progress_mutex;
    long* thread_bytes_downloaded;
    long* thread_total_bytes;
    const char* current_url;
};

void clean_up_child_process(int signal_number) {
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0);
}

// Function to update progress.json atomically
void save_progress_json(const char* url, long file_size, long total_downloaded, long* thread_downloaded, long* thread_total) {
    char tmp_file[256];
    snprintf(tmp_file, sizeof(tmp_file), "%s.tmp", PROGRESS_FILE);

    FILE* fp = fopen(tmp_file, "w");
    if (!fp) return;

    double total_pct = (file_size > 0) ? ((double)total_downloaded * 100.0 / file_size) : 0.0;
    if (total_pct > 100.0) total_pct = 100.0;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"active\": true,\n");
    fprintf(fp, "  \"url\": \"%s\",\n", url);
    fprintf(fp, "  \"file_size\": %ld,\n", file_size);
    fprintf(fp, "  \"downloaded_bytes\": %ld,\n", total_downloaded);
    fprintf(fp, "  \"pct\": %.2f,\n", total_pct);
    fprintf(fp, "  \"num_threads\": %d,\n", NUM_THREADS);
    fprintf(fp, "  \"threads\": [\n");

    for (int i = 0; i < NUM_THREADS; ++i) {
        long t_total = thread_total[i];
        double t_pct = (t_total > 0) ? ((double)thread_downloaded[i] * 100.0 / t_total) : 0.0;
        if (t_pct > 100.0) t_pct = 100.0;

        fprintf(fp, "    {\"id\": %d, \"pct\": %.2f, \"downloaded\": %ld, \"total\": %ld}%s\n",
                i, t_pct, thread_downloaded[i], t_total, (i == NUM_THREADS - 1) ? "" : ",");
    }
    fprintf(fp, "  ]\n}\n");
    fclose(fp);

    rename(tmp_file, PROGRESS_FILE);
}

void mark_progress_complete(const char* url, long file_size) {
    FILE* fp = fopen(PROGRESS_FILE, "w");
    if (!fp) return;

    fprintf(fp, "{\n");
    fprintf(fp, "  \"active\": false,\n");
    fprintf(fp, "  \"url\": \"%s\",\n", url);
    fprintf(fp, "  \"file_size\": %ld,\n", file_size);
    fprintf(fp, "  \"downloaded_bytes\": %ld,\n", file_size);
    fprintf(fp, "  \"pct\": 100.0,\n");
    fprintf(fp, "  \"num_threads\": %d,\n", NUM_THREADS);
    fprintf(fp, "  \"threads\": []\n");
    fprintf(fp, "}\n");
    fclose(fp);
}

void* download_chunk(void* arg) {
    struct ThreadArgs* targs = (struct ThreadArgs*) arg;
    int sockfd;
    struct sockaddr_in server_addr;
    struct hostent* hostinfo;
    char buffer[CHUNK_BUF_SIZE];
    char request[4096];

    sockfd = socket(PF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) pthread_exit(NULL);

    // Socket tuning: Set 1MB receive buffer for maximum TCP throughput
    int sock_buf = 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &sock_buf, sizeof(sock_buf));

    hostinfo = gethostbyname(targs->host);
    if (hostinfo == NULL) { close(sockfd); pthread_exit(NULL); }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(targs->port);
    server_addr.sin_addr = *((struct in_addr*) hostinfo->h_addr);

    if (connect(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        pthread_exit(NULL);
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Range: bytes=%ld-%ld\r\n"
             "Connection: close\r\n\r\n",
             targs->path, targs->host, targs->start_byte, targs->end_byte);

    write(sockfd, request, strlen(request));

    int header_ended = 0;
    int bytes_read;
    long current_offset = targs->start_byte;

    char header_buf[4] = {0};
    while (!header_ended && read(sockfd, &header_buf[3], 1) > 0) {
        if (header_buf[0] == '\r' && header_buf[1] == '\n' &&
            header_buf[2] == '\r' && header_buf[3] == '\n') {
            header_ended = 1;
        } else {
            header_buf[0] = header_buf[1];
            header_buf[1] = header_buf[2];
            header_buf[2] = header_buf[3];
        }
    }

    while ((bytes_read = read(sockfd, buffer, sizeof(buffer))) > 0) {
        pthread_mutex_lock(targs->file_mutex);
        lseek(targs->out_fd, current_offset, SEEK_SET);
        write(targs->out_fd, buffer, bytes_read);
        pthread_mutex_unlock(targs->file_mutex);

        current_offset += bytes_read;

        pthread_mutex_lock(targs->progress_mutex);
        targs->thread_bytes_downloaded[targs->thread_id] += bytes_read;

        long total_downloaded = 0;
        for (int i = 0; i < NUM_THREADS; ++i) {
            total_downloaded += targs->thread_bytes_downloaded[i];
        }

        // Save progress to JSON for Flask Web UI
        save_progress_json(targs->current_url, targs->file_size, total_downloaded,
                           targs->thread_bytes_downloaded, targs->thread_total_bytes);

        // Terminal Progress Output
        double total_pct = (targs->file_size > 0) ? ((double)total_downloaded * 100.0 / targs->file_size) : 0.0;
        if (total_pct > 100.0) total_pct = 100.0;

        printf("\r[Worker %d] [", getpid());
        int bar_width = 20;
        int pos = (int)(total_pct / 100.0 * bar_width);
        for (int i = 0; i < bar_width; ++i) {
            if (i < pos) printf("=");
            else if (i == pos) printf(">");
            else printf(" ");
        }
        printf("] %.1f%% (%.2f / %.2f MB) - %d Threads",
               total_pct,
               (double)total_downloaded / (1024.0 * 1024.0),
               (double)targs->file_size / (1024.0 * 1024.0),
               NUM_THREADS);
        fflush(stdout);

        pthread_mutex_unlock(targs->progress_mutex);
    }

    close(sockfd);
    pthread_exit(NULL);
}

void process_download_task(const char* raw_url) {
    char host[256] = {0}, path[2048] = {0}, filename[256] = {0};
    int port = 80;

    const char* p = raw_url;

    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;

    char clean_url[2048] = {0};

    strncpy(clean_url, p, sizeof(clean_url) -1 );

    int len = strlen(clean_url);

    while(len > 0 && (clean_url[len-1] == ' ' || clean_url[len-1] == '\t' || clean_url[len-1] == '\r' || clean_url[len-1] == '\n')) {
    	
	    clean_url[--len] = '\0';
    
    }

    char* start = clean_url;
    if(strncasecmp(start, "http://",7) == 0 ){
	    start += 7;
    } else if(strncasecmp(start, "https://",8) ==0) {
	    
	    start += 8;
    
    }


    char* slash = strchr(start, '/');

    if(slash != NULL){
	    size_t host_len = slash - start;
	    if(host_len >= sizeof(host)) host_len = sizeof(host) - 1;
	    strncpy(host, start, host_len);
	    host[host_len] = '\0';

	    strncpy(path, slash, sizeof(path) -1);

    } else {
    		strncpy(host, start, sizeof(host) -1);
		strcpy(path, "/");
    	
    }


    if(strlen(host) == 0) {
    
    	fprintf(stderr, "invalid url format(host empty): %s\n", raw_url);
	exit(1);
    }

    char* last_slash = strrchr(path, '/');

    if (last_slash && strlen(last_slash + 1) > 0){
	    strcpy(filename, last_slash + 1);
    } 
    else 
    {
	    strcpy(filename, "download_file.iso");
    }
    char dest_path[1024];
    snprintf(dest_path, sizeof(dest_path), "%s/%s", DONE_DIR, filename);

    printf("\n[Worker %d] Connecting to HOST: %s, Path: '%s'... \n", getpid(), host, path);

    int sockfd = socket(PF_INET, SOCK_STREAM, 0);
    struct hostent* hostinfo = gethostbyname(host);
    if (!hostinfo) { exit(1); }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr*) hostinfo->h_addr);

    if (connect(sockfd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        exit(1);
    }

    char head_req[2048];
    snprintf(head_req, sizeof(head_req),
             "HEAD %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    write(sockfd, head_req, strlen(head_req));

    char response[4096];
    int rlen = read(sockfd, response, sizeof(response) - 1);
    close(sockfd);
    if (rlen <= 0) exit(1);
    response[rlen] = '\0';

    long file_size = 0;
    char* cl_ptr = strstr(response, "Content-Length:");
    if (cl_ptr) {
        sscanf(cl_ptr, "Content-Length: %ld", &file_size);
    } else {
        file_size = 10 * 1024 * 1024;
    }

    printf("[Worker %d] File size: %ld bytes (%.2f MB). Spawning %d threads for MAX SPEED...\n",
           getpid(), file_size, (double)file_size / (1024.0 * 1024.0), NUM_THREADS);

    int out_fd = open(dest_path, O_CREAT | O_RDWR | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (out_fd < 0) { perror("open"); exit(1); }

    // Fast allocation on disk
    lseek(out_fd, file_size - 1, SEEK_SET);
    write(out_fd, "", 1);

    pthread_t threads[NUM_THREADS];
    struct ThreadArgs targs[NUM_THREADS];
    pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t progress_mutex = PTHREAD_MUTEX_INITIALIZER;

    long thread_bytes_downloaded[NUM_THREADS] = {0};
    long thread_total_bytes[NUM_THREADS] = {0};

    long chunk_size = file_size / NUM_THREADS;

    for (int i = 0; i < NUM_THREADS; ++i) {
        targs[i].thread_id = i;
        strcpy(targs[i].host, host);
        snprintf(targs[i].path, sizeof(targs[i].path), "/%s", path);
        targs[i].port = port;
        targs[i].start_byte = i * chunk_size;
        targs[i].end_byte = (i == NUM_THREADS - 1) ? (file_size - 1) : ((i + 1) * chunk_size - 1);
        targs[i].file_size = file_size;
        targs[i].out_fd = out_fd;
        targs[i].file_mutex = &file_mutex;
        targs[i].progress_mutex = &progress_mutex;
        targs[i].thread_bytes_downloaded = thread_bytes_downloaded;
        targs[i].thread_total_bytes = thread_total_bytes;
        targs[i].current_url = raw_url;

        thread_total_bytes[i] = targs[i].end_byte - targs[i].start_byte + 1;

        pthread_create(&threads[i], NULL, download_chunk, &targs[i]);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    close(out_fd);
    mark_progress_complete(raw_url, file_size);
    printf("\n[Worker %d] Download Complete 100%%! File saved to %s\n", getpid(), dest_path);
    exit(0);
}

int main() {
    mkdir(DONE_DIR, 0755);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &clean_up_child_process;
    sigaction(SIGCHLD, &sa, NULL);

    printf("=== C++ High-Speed Downloader Daemon Started (%d Threads) ===\n", NUM_THREADS);

    while (1) {
        int fd = open(REQUEST_FILE, O_RDWR | O_CREAT, 0666);
        if (fd >= 0) {
            struct flock lock;
            memset(&lock, 0, sizeof(lock));
            lock.l_type = F_WRLCK;

            if (fcntl(fd, F_SETLKW, &lock) == 0) {
                char buffer[4096];
                ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);
                if (bytes > 0) {
                    buffer[bytes] = '\0';
                    ftruncate(fd, 0);

                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    close(fd);

                    char* line = strtok(buffer, "\r\n");
                    while (line != NULL) {
                        if (strlen(line) > 0) {
                            printf("\nNew download request: %s\n", line);
                            pid_t pid = fork();
                            if (pid == 0) {
                                process_download_task(line);
                            }
                        }
                        line = strtok(NULL, "\r\n");
                    }
                } else {
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    close(fd);
                }
            } else {
                close(fd);
            }
        }

        sleep(1);
    }

    return 0;
}
