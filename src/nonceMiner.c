#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32 // Windows-unique preprocessor directives for compatiblity
    #ifdef _WIN32_WINNT
        #undef _WIN32_WINNT
    #endif
    #define _WIN32_WINNT 0x0600 // Necessary for GetTickCount64
    #include <winsock2.h>
    #include <windows.h>
    // Socket defines
    #define INIT_WINSOCK() do{\
        WSADATA wsa_data;\
        WSAStartup(MAKEWORD(1,1), &wsa_data);\
    }while(0)  // INIT_WINSOCK() must be called before using socket functions in Windows
    #define SET_TIMEOUT(soc, seconds) do{\
        unsigned long timeout = (seconds)*1000;\
        setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));\
        setsockopt(soc, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));\
    }while(0)
    #define CLOSE(soc) do{\
        shutdown(soc, SD_BOTH);\
        closesocket(soc);\
    }while(0)
    // Sleep defines
    #define SLEEP(seconds) Sleep((seconds)*1000)
    // Threading defines
    #define THREAD_T HANDLE // is type void*
    #define THREAD_CREATE(thread_ptr, func, arg_ptr) \
        *(thread_ptr) = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) func, arg_ptr, 0, NULL)
    #define MUTEX_T CRITICAL_SECTION
    #define MUTEX_CREATE(mutex_ptr) InitializeCriticalSection(mutex_ptr)
    #define MUTEX_LOCK(mutex_ptr) EnterCriticalSection(mutex_ptr)
    #define MUTEX_UNLOCK(mutex_ptr) LeaveCriticalSection(mutex_ptr)
    // Timing defines
    #define TIMESTAMP_T long long
    #define GET_TIME(t_ptr) *(t_ptr) = GetTickCount64()
    #define DIFF_TIME_MS(t1_ptr, t0_ptr) *(t1_ptr)-*(t0_ptr)
#else
    // Socket defines
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <pthread.h>
    #define INIT_WINSOCK() // No-op
    #define SET_TIMEOUT(soc, seconds) do{\
        struct timeval timeout = {.tv_sec = seconds, .tv_usec = 0};\
        setsockopt(soc, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));\
        setsockopt(soc, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));\
    }while(0)
    #define CLOSE(soc) do{\
        shutdown(soc, SHUT_RDWR);\
        close(soc);\
    }while(0)
    // Sleep defines
    #define SLEEP(seconds) sleep(seconds)
    // Threading defines
    #define THREAD_T pthread_t
    #define THREAD_CREATE(thread_ptr, func, arg_ptr) pthread_create(thread_ptr, NULL, func, arg_ptr)
    #define MUTEX_T pthread_mutex_t
    #define MUTEX_CREATE(mutex_ptr) pthread_mutex_init(mutex_ptr, NULL)
    #define MUTEX_LOCK(mutex_ptr) pthread_mutex_lock(mutex_ptr)
    #define MUTEX_UNLOCK(mutex_ptr) pthread_mutex_unlock(mutex_ptr)
    // Timing defines
    #define TIMESTAMP_T struct timespec
    #define GET_TIME(t_ptr) clock_gettime(CLOCK_MONOTONIC, t_ptr)
    #define DIFF_TIME_MS(t1_ptr, t0_ptr) \
        ((t1_ptr)->tv_sec-(t0_ptr)->tv_sec)*1000+((t1_ptr)->tv_nsec-(t0_ptr)->tv_nsec)/1000000
#endif

#include "mine_DUCO_S1.h"

MUTEX_T count_lock; // Protects access to shares counters
int server_is_online = 1;
int accepted = 0, rejected = 0;
char username[128];
char identifier[128];

void* mining_routine(void* arg){
    int len, *local_hashrate = (int *)arg;
    char buf[256], job_request[256];
    int job_request_len = sprintf(job_request, "JOB,%s,EXTREME", username);
    TIMESTAMP_T t1, t0;
    while(1){
        struct sockaddr_in server;
        server.sin_addr.s_addr = inet_addr("51.15.127.80");
        server.sin_family = AF_INET;
        server.sin_port = htons(2811);
        unsigned int soc = socket(PF_INET, SOCK_STREAM, 0);

        SET_TIMEOUT(soc, 16);

        len = connect(soc, (struct sockaddr *)&server, sizeof(server));
        if(len == -1) goto on_error;

        len = recv(soc, buf, 3, 0);
        if(len == -1 || len == 0) goto on_error;
        buf[len] = 0;

        GET_TIME(&t0);

        while(1){
            len = send(soc, job_request, job_request_len, 0);
            if(len == -1) goto on_error; // Boilerplate exit-on-failure line

            len = recv(soc, buf, 128, 0);
            if(len == -1 || len == 0) goto on_error; // Adds exit-on-disconnect
            buf[len] = 0;

            long nonce = mine_DUCO_S1_extend_cache(
                (const unsigned char*) &buf[0],
                (const unsigned char*) &buf[41],
                atoi((const char*) &buf[82])
            );
            
            // Assignment to *local_hashrate is generally atomic, no mutex needed
            GET_TIME(&t1);
            int tdelta_ms = DIFF_TIME_MS(&t1, &t0);
            *local_hashrate = nonce/tdelta_ms*1000;
            t0 = t1;

            len = sprintf(buf, "%ld,%d,nonceMiner v1.3.3,%s\n", nonce, *local_hashrate, identifier);
            len = send(soc, buf, len, 0);
            if(len == -1) goto on_error;

            len = recv(soc, buf, 128, 0); // May take up to 10 seconds as of server v2.2!
            if(len == -1 || len == 0) goto on_error;
            buf[len] = 0;

            // Increments are not atomic, requiring mutexes
            MUTEX_LOCK(&count_lock);
            if(!strcmp(buf, "GOOD\n") || !strcmp(buf, "BLOCK\n")) accepted++;
            else rejected++;
            MUTEX_UNLOCK(&count_lock);
        }

        on_error:
        CLOSE(soc);
        *local_hashrate = 0; // Zero out hashrate since the thread is not active
        do{
        SLEEP(2); // Does not attempt reconnect if server is not online
        }while(!server_is_online);
    }
}

void* ping_routine(void *arg){
    int len;
    char buf[128];
    while(1){
        struct sockaddr_in server;
        server.sin_addr.s_addr = inet_addr("51.15.127.80");
        server.sin_family = AF_INET;
        server.sin_port = htons(2811);
        unsigned int soc = socket(PF_INET, SOCK_STREAM, 0);
        
        SET_TIMEOUT(soc, 2);
        
        len = connect(soc, (struct sockaddr *)&server, sizeof(server));
        if(len == -1) goto on_error;

        len = recv(soc, buf, 3, 0);
        if(len == -1 || len == 0) goto on_error;
        buf[len] = 0;

        on_error:
        CLOSE(soc);
        // Updates server_is_online according to whether recv succeeded
        if(len == -1 || len == 0) server_is_online = 0;
        else server_is_online = 1;

        SLEEP(2);
    }
}

int main(){
    INIT_WINSOCK();

    int n_threads;
    puts("Initializing nonceMiner v1.3.3...");
    printf("Enter username: ");
    if(scanf("%127s", username) != 1){
        puts("Invalid username, exiting...");
        return 0;
    }
    printf("Enter identifier (\"None\" for no identifier): ");
    if(scanf("%127s", identifier) != 1){
        puts("Invalid identifier, exiting...");
        return 0;
    }
    printf("Enter # of threads: ");
    if(scanf("%d", &n_threads) != 1){
        puts("Invalid number of threads, exiting...");
        return 0;
    }
    
    int *local_hashrate = calloc(n_threads, sizeof(int));
    THREAD_T *mining_threads = malloc(n_threads*sizeof(THREAD_T));
    MUTEX_CREATE(&count_lock);
    puts("Starting threads...");
    for(int i = 0; i < n_threads; i++){
        THREAD_CREATE(&mining_threads[i], mining_routine, &local_hashrate[i]);
        SLEEP(1);
    }
    THREAD_T ping_thread;
    THREAD_CREATE(&ping_thread, ping_routine, NULL);
    
    // Zeroes out reported work done while starting up threads
    memset(local_hashrate, 0, sizeof(int));
    MUTEX_LOCK(&count_lock);
    accepted = 0;
    rejected = 0;
    MUTEX_UNLOCK(&count_lock);

    while(1){
        SLEEP(2); // Never exactly two seconds, so we time it ourselves
        
        int total_hashrate = 0;
        for(int i = 0; i < n_threads; i++) total_hashrate += local_hashrate[i];
        float megahash = (float)total_hashrate/1000000;

        MUTEX_LOCK(&count_lock);
        int accepted_copy = accepted;
        int rejected_copy = rejected;
        accepted = 0;
        rejected = 0;
        MUTEX_UNLOCK(&count_lock);

        time_t ts_epoch = time(NULL);
        struct tm *ts_clock = localtime(&ts_epoch);
        printf("%02d:%02d:%02d", ts_clock->tm_hour, ts_clock->tm_min, ts_clock->tm_sec);
        printf("  Hashrate: %.2f MH/s,", megahash);
        printf(" Accepted %d, Rejected %d", accepted_copy, rejected_copy);
        if(!server_is_online) printf(", server ping timeout\n");
        else printf("\n");
    }

    return 0;
}