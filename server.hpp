/*
Part2Part
Author: Filip Valentin
tried my best
*/

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>

// #include <signal.h>
#include <dirent.h>

#include <pthread.h>
#include <semaphore.h>

#include <string.h>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>

#include <regex.h>
#include <sqlite3.h>

#define MAX_LISTEN_CAPACITY 100
#define COMMAND_SIZE 128
#define ROUTINE_SLEEP_INTERVAL 1                // time between checks for exitFlag
#define DEFAULT_ROUTINE_SLEEP_LONG_INTERVAL 120 // time between routine operations
#define DEFAULT_UPLOADS_FOLDER_PATH "./Uploads"
#define CONFIG_FILE_NAME "./config.txt"
#define DATABASE_NAME "serverdb"
#define BUFFER_SIZE 4096
#define BLOCK_SIZE 4096
#define MAX_PATH_SIZE 4096
#define RESPONSE_AWAIT_WINDOW 4 // max response await window to block the client to wait for responses to come
#define CACHE_PURGE_INTERVAL RESPONSE_AWAIT_WINDOW

#define DEFAULT_SEARCH_REQUEST_TTL 10 // de fiecare data cand perpetuez, voi scadea [optional?]

#define IP_GET_SCRIPT "./get_ip.sh"
#define IP_FILE_NAME "./ip.txt"

static void *executeServerServiceThread(void *);
static void *executeNetworkServiceThread(void *);
static void *executeNetworkServiceThread_cascadeRequests(void *);
static void *executeNetworkServiceThread_cascadeResponses(void *);
static void *executeRoutineThread(void *);
static void *workerThread(void *);
static void *networkPeerConnectedFromThisServer(void *);
static void *networkPeerConnectedToThisServer(void *);

struct sockData
{
    in_port_t port;
    in_addr_t ip;
};

struct fileEntry2 // every entry is a string, since the database returns the data as strings
{
    std::string path;
    std::string name;
    std::string extension;
    std::string hash;
    std::string disk_size;
};

struct responseData
{
    std::vector<fileEntry2> *response_list; // must be freed!
    in_addr_t receiver_ip;
    in_port_t receiver_port;
    in_addr_t sender_ip;
    in_port_t sender_port;

    int last_sock_desc; // this is for checking to not cascade on the same sock descriptor this response came through
};

struct clientData
{
    in_port_t port;
    in_addr_t ip;
    std::queue<responseData> *responses;
    sem_t responses_sem;
};

struct workerThreadData
{
    in_addr_t ip;
    in_port_t port;
    int socket_descriptor;
    in_addr_t public_ip;
};

struct fileEntry
{
    std::string name;
    std::string extension;
    long disk_size;
    std::string ip;
    unsigned int port;
    time_t last_update;
};

struct fileRequest
{
    uint8_t request_mask;
    std::string name_or_pattern;
    std::string extension;
    std::string disk_size;

    uint8_t time_to_live;

    in_addr_t sender_ip;
    in_port_t sender_port;
    in_addr_t sender_public_ip;
    // w/e
};

struct cacheFileRequest
{
    time_t time_when_cached;

    fileRequest request;
};

struct connectedPeerData
{
    int peer_sock_desc;
    in_port_t peer_port;
    in_addr_t peer_ip;
};

struct cacheResponseData // this will store only the first
{
    time_t time_when_cached; // use this for 1. when a new search request comes through, and 2. for the routine thread to delete it [telemetry maybe]

    std::string path;
    std::string name;
    std::string extension;
    std::string hash;
    std::string disk_size;

    in_addr_t receiver_ip;
    in_port_t receiver_port;
};

void getMyIP();

struct sockData *getPeerSockData(char *);

void treatSearchRequest(struct workerThreadData *sender_data);
void treatNetworkResponseRequest(int);

// void reportErrorToClient(int);

void treatNetworkSearchRequest(int);

uint8_t cascadeSearchRequestToPeer(struct fileRequest *, int);

void peer_awaitRequestsAndResolve(int);

void resolveSearchRequest(struct fileRequest *, in_addr_t, in_port_t);

uint8_t cascadeResponseToPeer(struct responseData *, int);

uint8_t checkRequestInCache(struct fileRequest *);
uint8_t checkRsponseInCache(struct responseData *);
uint8_t sendResponseStructToPeer(struct fileEntry2 *, int);

uint8_t readResponseStructFromPeer(int, std::vector<fileEntry2> *);

void awaitResponsesAndAnswer(struct workerThreadData *worker_data, struct clientData *current_client);
uint8_t sendResponsePacketToClient(responseData *packet, int sock_desc);

uint8_t checkIfConnectedToReceiver_AndPushToItsResponseList(responseData *response_data);

inline void eraseClientFromClientsList(struct clientData *client_todelete);

void treatFileRequest(int client_descriptor);

int16_t readExact(int socket, void *buffer, uint16_t x);
int16_t writeExact(int socket, const void *buffer, uint16_t x);

// Meaning of codes:

// 255 connexion shutdown

// worker thread
//  10 - request for network search
//  22 - signal for sending a search response packet
// 150 - request for file transfer

// network peer
//  11 - cascade the search request to network and process the request
//  21 - process a response: cascade, send directly, or drop

// 100 = service server type
// 110 = service client type
// 200 = network server type
// 210 = network client type

//

// OTHER

//

void log_fct(const char *to_log);

inline void purgeCache_oldRequests();
inline void purgeCache_oldResponses();

uint8_t *md5FileWrapper(char *path);
void binaryStringToHexaString(char *xp, uint8_t *bb, int n); // n must be int

long getFileSize(char *);

inline void eraseClientFromClientsList(struct clientData *client_todelete);

void sqlite_regexp(sqlite3_context *context, int argc, sqlite3_value **values);

std::queue<std::string> *loadConfigFile();
void saveConfigFile();

uint8_t connectToFileDB();

void scanFolderAndAddToDatabase(char *);
void searchInDatabase(fileRequest *, std::vector<fileEntry2> *);
void refreshDatabase();
