/*
Part2Part
Author: Filip Valentin
tried my best
*/

#include "server.hpp"
#include "md5/md5.c"

in_addr_t my_ip = 0;

FILE *log_fd = NULL;

uint8_t exitFlag = 0;
uint8_t serviceFlag = 0;
uint8_t networkFlag = 0;
uint8_t forceScanDBFlag = 0;
uint8_t forceRefreshDBFlag = 0;

char *upload_path;                                // don't forget to free this memory
uint routine_thread_long_check_interval = 60 * 2; // 2 mins

pthread_t serviceThread; // subordinate functions here:
sem_t connectedClientsList_sem;
std::vector<struct clientData *> *connectedClientsList; // I must know who is connected to this server to optimize how responses are handled

pthread_t routineThread;

pthread_t networkThread; // network related
sem_t searchRequestsQueue_sem;
std::queue<fileRequest> *searchRequestsQueue;
sem_t responseQueue_sem;
std::queue<responseData> *responseQueue;

std::vector<connectedPeerData> *peers_connected_to_this_server;    // peers that are connected to this server
std::vector<connectedPeerData> *peers_this_server_is_connected_to; // peers this server is connected to
uint activeServerConnections = 0;                                  // maybe use them to warn if the user wants to close

std::unordered_map<int, sem_t *> *peerWriteToSockDescriptorSem; // map a descriptor with a semaphore to use in the threads handling search requests
// and responses, maybe
sem_t requestCache_sem;
std::vector<cacheFileRequest> *requestCache; // search requests that have already been cascaded! not to cache when receive
sem_t responseCache_sem;
std::vector<cacheResponseData> *responseCache;

pthread_t networkThread_cascadeRequests;
pthread_t networkThread_cascadeResponses;

// remember different ports will be used
uint serverPort;  // port for clients to connect to
uint networkPort; // port for servers to connect to
int service_socket_desc = -1;
int network_sock_desc = -1;

uint activeClientConnections = 0;

sem_t fileDB_sem;
sqlite3 *fileDatabase;

#define log(str)            \
    {                       \
        if (log_fd != NULL) \
            log_fct(str);   \
    }

int main()
{
    setbuf(stdout, NULL);

    getMyIP();

    if ((log_fd = fopen("server.log", "a")) == NULL)
        perror("Cannot create/open log file"); // manage how to do logging,
    else
        setbuf(log_fd, NULL);
    log("New session started\n");

    // printf("Loading config file...");
    std::queue<std::string> *auto_commands = loadConfigFile();

    connectToFileDB();

    sem_init(&fileDB_sem, 0, 1); // initiate semaphore
    sem_init(&searchRequestsQueue_sem, 0, 1);
    sem_init(&responseQueue_sem, 0, 1);
    sem_init(&connectedClientsList_sem, 0, 1);
    sem_init(&requestCache_sem, 0, 1);
    sem_init(&responseCache_sem, 0, 1);

    searchRequestsQueue = new std::queue<fileRequest>();
    responseQueue = new std::queue<responseData>();

    peerWriteToSockDescriptorSem = new std::unordered_map<int, sem_t *>;

    peers_connected_to_this_server = new std::vector<connectedPeerData>;
    peers_this_server_is_connected_to = new std::vector<connectedPeerData>;

    requestCache = new std::vector<cacheFileRequest>;
    responseCache = new std::vector<cacheResponseData>;

    connectedClientsList = new std::vector<clientData *>;

    pthread_create(&routineThread, NULL, &executeRoutineThread, NULL);
    printf("Routine thread started.\n");

    pthread_create(&networkThread_cascadeRequests, NULL, &executeNetworkServiceThread_cascadeRequests, NULL);
    pthread_create(&networkThread_cascadeResponses, NULL, &executeNetworkServiceThread_cascadeResponses, NULL);

    printf("\nThe console is now available.\n");

    char *command = (char *)calloc(COMMAND_SIZE, sizeof(char)); // this must stay in heap for getline to work
    size_t len = 0;
    while (1)
    {

        if (auto_commands == nullptr)
        {
            printf(">\t");
            getline(&command, &len, stdin);
        }
        else
        {
            printf("auto>\t%s\n", auto_commands->front().c_str());
            strcpy(command, auto_commands->front().c_str());
            auto_commands->pop();
            if (auto_commands->empty())
            {
                delete auto_commands;
                auto_commands = nullptr;
            }
        }

        char *p;
        // forceexit
        if (strcmp(command, "exit\n") == 0 || strcmp(command, "ex\n") == 0)
        {
            exitFlag = 1;
            if (activeClientConnections != 0)
                printf("There are still %u connections active\n", activeClientConnections);
            if (service_socket_desc != -1)
                if (shutdown(service_socket_desc, SHUT_RDWR) == -1)
                    perror("Error when shutting down the service socket");
            if (network_sock_desc != -1)
                if (shutdown(network_sock_desc, SHUT_RDWR) == -1)
                    perror("Error when shutting down the network socket");
            // close(service_socket_desc);
            break;
        }
        else if ((p = strstr(command, "starts")) != NULL)
        {
            if (serviceFlag == 1)
            {
                printf("Service is already running.\n");
            }
            else
            {
                if (p == command)
                {
                    serviceFlag = 1;

                    char *p = strtok(command, " ");
                    p = strtok(NULL, " \n"); // security measures

                    if (p != NULL)
                        serverPort = atoi(p);

                    pthread_create(&serviceThread, NULL, &executeServerServiceThread, NULL);
                    printf("Service thread created, service is now running.\n");

                    usleep(1);
                }
                else
                    printf("Invalid command\nUsage: start <PORT>\n");
            }
        }
        else if ((p = strstr(command, "startn")) != NULL)
        {

            if (networkFlag == 1)
            {
                printf("Network thread is already running.\n");
            }
            else if (serviceFlag == 0)
                printf("You must first start the service!");
            else
            {
                if (p == command)
                {
                    char *p = strtok(command, " ");
                    p = strtok(NULL, " \n"); // security measures needed

                    in_port_t *network_port = (in_port_t *)malloc(sizeof(in_port_t));
                    *network_port = atoi(p); // usable only

                    networkFlag = 1;
                    pthread_create(&networkThread, NULL, &executeNetworkServiceThread, (void *)network_port);

                    printf("Network thread, requests and messages threads started, the server is ready to serve the network.\n");
                    usleep(10);
                }
                else
                    printf("Invalid command!\nUsage: startnetwork <PORT>\n"); // sntk
            }
        }
        else if (strcmp(command, "stops\n") == 0)
        {
            if (serviceFlag == 0)
            {
                printf("Service is not active.\n");
            }
            else
            {
                serviceFlag = 0;
                // printf("Awaiting service thread join\n")
                pthread_join(serviceThread, NULL);
                printf("Service thread stopped.\n");
            }
        }
        else if (strcmp(command, "stopn\n") == 0)
        {
            if (networkFlag == 0)
            {
                printf("Network service is not active.\n");
            }
            else
            {
                networkFlag = 0;
                // printf("Awaiting service thread join\n")
                pthread_join(networkThread, NULL);
                printf("Network service thread stopped.\n");
            }
        }
        else if (strcmp(command, "forcescan\n") == 0)
        {
            forceScanDBFlag = 1;
            printf("Database scan queued.\n");
            // scanFolderAndAddToDatabase(upload_path);
        }
        else if (strcmp(command, "forcerefresh\n") == 0)
        {
            forceRefreshDBFlag = 1;
            printf("Database scan queued.\n");
        }

        else if ((p = strstr(command, "connect")) != NULL) // connect to a server from this node to expand the network
        {

            if (p == command)
            {
                struct sockData *sock_data = getPeerSockData(command); // process and put ip and port into sock_data
                // must be freed [it is indeed]
                pthread_create(&serviceThread, NULL, &networkPeerConnectedFromThisServer, (void *)sock_data);
            }
            else
                printf("Invalid command\nUsage: connect <IP> <PORT>\n");
        }
        else if ((p = strstr(command, "upath")) != NULL)
        {
            if (p == command)
            {
                char *k = strstr(command, " ");
                if (k == NULL)
                {
                    printf("Current upload path: %s\n", upload_path);
                }
                else
                {
                    k++;
                    strcpy(upload_path, k);

                    saveConfigFile();
                    // delete database
                    // flagrescan
                }
            }
            else
                printf("Invalid command\nUsage: upath {<UPLOAD PATH>}\t"
                       "(if <UPLOAD PATH> is specified, modify the upload path)\n");
        }
        else if ((p = strstr(command, "dbri")) != NULL) // databaserefreshinterval
        {
            if (p == command)
            {
                char *k = strstr(command, " ");
                if (k == NULL)
                {
                    printf("Current refresh interval: %u\n", routine_thread_long_check_interval);
                }
                else
                {
                    k++;
                    routine_thread_long_check_interval = atoi(k);
                    saveConfigFile();
                }
            }
            else
                printf("Invalid command\nUsage: dbri {<REFRESH INTERVAL>}\t"
                       "(if <REFRESH INTERVAL> is specified, modify the database refresh interval"
                       "(long_check_interval value); set to 0 to disable)\n");
        }
        // else if (strcmp(command, "saveconfigs\n") == 0)
        // {
        //     saveConfigFile();
        // }
        else
            printf("Command not recognized\n");

        memset(command, 0, COMMAND_SIZE); // getline won't auto place null
    }

    // FREE THE RESOURCES

    if (serviceFlag == 1)
    {
        serviceFlag = 0;
        pthread_join(serviceThread, NULL);
        printf("Service thread stopped.\n");
    }

    pthread_join(routineThread, NULL);

    pthread_join(networkThread, NULL);

    pthread_join(networkThread_cascadeRequests, NULL);
    pthread_join(networkThread_cascadeResponses, NULL);

    sem_destroy(&fileDB_sem);
    sem_destroy(&searchRequestsQueue_sem);
    sem_destroy(&responseQueue_sem);
    sem_destroy(&requestCache_sem);
    sem_destroy(&responseCache_sem);

    if (service_socket_desc)
        close(service_socket_desc);

    free(upload_path);
    delete searchRequestsQueue;
    delete requestCache;
    delete responseQueue;
    delete responseCache;
    delete peerWriteToSockDescriptorSem;
    delete peers_connected_to_this_server;
    delete peers_this_server_is_connected_to;

    sqlite3_close(fileDatabase);

    log("Session terminated\n\n");
    if (log_fd != NULL)
        fclose(log_fd);

    return 0;
};

struct sockData *getPeerSockData(char *command) // don't forget to free
{
    struct sockData *socketData = (struct sockData *)malloc(sizeof(struct sockData));

    char *p = strtok(command, " "); //"connect aaa.bbb.ccc.ddd 12345"
    p = strtok(NULL, " \n");        // aaa.bbb.ccc.ddd
    socketData->ip = inet_addr(p);

    p = strtok(NULL, " \n"); // 12345
    int peerPort = atoi(p);
    socketData->port = htons(peerPort);

    return socketData;
}

static void *executeServerServiceThread(void *arg)
{
    log("Service thread started.\n");

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(serverPort);

    // int service_socket_desc; // socket descriptor

    if ((service_socket_desc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("[server]Error at socket()");
        serviceFlag = 0;
        return NULL;
    }

    int on = 1;
    setsockopt(service_socket_desc, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(service_socket_desc, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        perror("[service]Error at bind()");
        serviceFlag = 0;
        return NULL;
    }

    if (listen(service_socket_desc, MAX_LISTEN_CAPACITY) == -1)
    {
        perror("[service]Error at listen()");
        serviceFlag = 0;
        return NULL;
    }

    // printf("[service thread] Socket opened, awaiting requests.\n");

    while (1)
    {
        if (exitFlag == 1 || serviceFlag == 0)
            break;

        int client_descriptor;

        struct sockaddr_in from;
        memset(&from, 0, sizeof(from));
        socklen_t length = sizeof(from);

        if ((client_descriptor = accept(service_socket_desc, (struct sockaddr *)&from, &length)) == -1)
        {
            // perror("[service thread] Error at accept().\n");
            // serviceFlag = 0;
            // break;
        }
        // log("A client has been accepted.\n");

        struct workerThreadData *worker_data = (struct workerThreadData *)malloc(sizeof(struct workerThreadData));
        worker_data->ip = from.sin_addr.s_addr; // the ip is the same
        worker_data->port = from.sin_port;      // remember the client gets another port it communicates through
        worker_data->socket_descriptor = client_descriptor;

        // this is a check point for the right clinets to connect, namely clients and not networking clients
        uint8_t type = 100;
        if (write(client_descriptor, &type, sizeof(type)) <= 0) // send type
        {
            // error
            continue;
        }
        else
        {
            uint8_t status;
            if (read(client_descriptor, &status, sizeof(status)) <= 0) // await confirmation
            {
                // error
                continue;
            }
            if (status == 110) // just convetion
            {
                pthread_t th;
                pthread_create(&th, NULL, &workerThread, worker_data); // create a thread only if the right type of client is connected
            }
            else
                close(client_descriptor);
        }
    }

    close(service_socket_desc);

    log("Service thread stopped.\n");
    serviceFlag = 0;
    return NULL;
}
// timer to disconnect!!!!
static void *workerThread(void *arg)
{
    fprintf(log_fd, "\tWorker thread with id: %lu started\n", pthread_self());

    activeClientConnections++;

    pthread_detach(pthread_self());

    struct workerThreadData *worker_data = (struct workerThreadData *)arg;
    int client_descriptor = worker_data->socket_descriptor;

    ///
    if (readExact(client_descriptor, &(worker_data->public_ip), sizeof(in_addr_t)) <= 0)
        printf("Error getting client's public IP.");

    ///

    struct clientData current_client;    // object on stack, auto managed
    current_client.ip = worker_data->ip; // public_ip;
    current_client.port = worker_data->port;
    current_client.responses = new std::queue<responseData>; // minus this pointer

    sem_init(&current_client.responses_sem, 0, 1);

    sem_wait(&connectedClientsList_sem);
    connectedClientsList->push_back(&current_client); // the object shall live only on stack and send a reference
                                                      // since it won't be destroyed until the client leaves [at least I hope so]
    sem_post(&connectedClientsList_sem);

    uint8_t request_no = 0;
    while (1)
    {     // no immediate close, must send flag before reading
        { // if exitflag / send close
            // else send ok flag
            if (write(client_descriptor, &exitFlag, sizeof(uint8_t)) <= 0)
            {
                // error
                break;
            }
            if (exitFlag == 1)
                break;
        }

        if (read(client_descriptor, &request_no, sizeof(uint8_t)) <= 0)
        {
            fprintf(log_fd, "\tClient connected to worker thread: %lu disconnected unexpectedly.\n", pthread_self());
            break;
        }
        fprintf(log_fd, "\tWorker thread with id: %lu received request: %hhu\n", pthread_self(), request_no);

        if (request_no == 10) // request for searching the network
        {
            treatSearchRequest(worker_data);
            awaitResponsesAndAnswer(worker_data, &current_client); // because I have no control over when the responses will arrive, just set a time window and reject everything else
        }
        else if (request_no == 150) // request for file transfer
        {
            treatFileRequest(client_descriptor); //
        }
        else if (request_no == 255) // terminate the connection
            break;

        request_no = 0;
    }

    // user disconnected, delete it from the connected clients list
    sem_wait(&connectedClientsList_sem);
    eraseClientFromClientsList(&current_client);
    sem_post(&connectedClientsList_sem);

    sem_destroy(&current_client.responses_sem);

    close(client_descriptor);
    activeClientConnections--;

    delete current_client.responses; // free the responses queue for this client

    free(worker_data);

    fprintf(log_fd, "\tWorker thread with id: %lu terminated\n", pthread_self());
    return (NULL);
};

void treatFileRequest(int client_descriptor)
{
    uint16_t length;
    if (readExact(client_descriptor, &length, sizeof(uint16_t)) <= 0)
        return;
    char *path = (char *)calloc(length + 1, sizeof(char)); //+1 for null
    if (readExact(client_descriptor, path, length * sizeof(char)) <= 0)
        return;
    char hash[33] = {0};
    if (readExact(client_descriptor, hash, 32 * sizeof(char)) <= 0)
        return;

    FILE *file = fopen(path, "r");
    uint8_t status = 2;
    if (file == NULL)
        status = 255;
    else
    {
        uint8_t *computed_hash = md5FileWrapper(path); // compute the hash again to check if it is the same file
        char hash2[32 + 1] = {0};
        binaryStringToHexaString(hash2, computed_hash, 32);
        free(computed_hash);
        if (strcmp(hash, hash2) != 0)
            status = 255;
    }

    if (write(client_descriptor, &status, sizeof(uint8_t)) <= 0)
        return;
    if (status == 255)
        return;

    char buffer[BLOCK_SIZE] = {0};
    size_t bytes_read;
    while ((bytes_read = fread(buffer, sizeof(char), BLOCK_SIZE, file)) >= 0)
    {
        if (writeExact(client_descriptor, &bytes_read, sizeof(size_t)) <= 0) // since I want to reuse this connection, I MUST send the size,
            // otherwise the client won't know when to stop, because I send the exit flag the client mistakens it
            break;
        if (writeExact(client_descriptor, buffer, bytes_read * sizeof(char)) < 0)
        {
            perror("[worker]Error writing the file content to client");
            break;
        }

        if (bytes_read != BLOCK_SIZE)
            break;

        memset(buffer, 0, BLOCK_SIZE);
    }

    fclose(file);

    log("A file has been transferred\n");
}

void treatSearchRequest(struct workerThreadData *sender_data)
{
    int client_desc = sender_data->socket_descriptor;

    // 1 receive request number [already received]
    // 2 receive the request mask
    // 3 receive the necessary data
    // if mask == 0b00000001 => name
    // if mask == 0b00000010 => pattern
    // if mask == 0b00000100 => extension
    // if mask == 0b00001000 => don't cascade
    // if mask == 0b10000000 => size less than
    // if mask == 0b01000000 => size equal
    // if mask == 0b00100000 => size greater than

    // 2
    uint8_t request_mask;
    if (read(client_desc, &request_mask, sizeof(uint8_t)) <= 0)
    {
        perror("[worker]Error reading the request mask from client, request aborted.");
        return;
    }

    // 3
    char file_name_or_pattern[256] = {0};
    if (request_mask & 0b00000001 ||
        request_mask & 0b00000010)
    {
        if (readExact(client_desc, file_name_or_pattern, 255 * sizeof(char)) <= 0)
        {
            perror("[worker]Error reading the request data from client, request aborted!");
            return;
        }
    }
    char extension[20] = {0};
    if (request_mask & 0b00000100)
    {
        if (readExact(client_desc, extension, 19 * sizeof(char)) <= 0)
        {
            perror("[worker]Error reading the request mask from client, request aborted!");
            return;
        }
    }
    char size[20] = {0};
    if (request_mask & 0b10000000 ||
        request_mask & 0b01000000 ||
        request_mask & 0b00100000)
    {
        if (readExact(client_desc, size, 19 * sizeof(char)) <= 0)
        {
            perror("[worker]Error reading the request mask from client, request aborted!");
            return;
        }
        // no need to send the mode, it is already send in the request mask
    }

    // purgeCache_oldRequests(sender_data->ip, sender_data->port);
    // purgeCache_oldResponses(sender_data->ip, sender_data->port);

    fileRequest request;
    request.request_mask = request_mask;
    request.name_or_pattern = file_name_or_pattern;
    request.extension = extension;
    request.disk_size = size;
    request.sender_public_ip = sender_data->public_ip;
    request.sender_ip = sender_data->ip;     // sender_data->public_ip;
    request.sender_port = sender_data->port; // htons(serverPort);

    request.time_to_live = DEFAULT_SEARCH_REQUEST_TTL;

    // I assume the client gives me valid search querys, so I don't have to report back

    if ((request_mask & 0b00001000) == 0) // this is the bit for 'don't cascade'; if it's set don't cascade
    {
        sem_wait(&searchRequestsQueue_sem);
        searchRequestsQueue->push(request); // push to cascade
        sem_post(&searchRequestsQueue_sem);
    }

    // struct sockaddr_in local_server_address;            // get the address of this server from the socket and make the response follow the logic:
    // socklen_t addr_size = sizeof(local_server_address); // the sender becomes the receiver, so this server will be the sender of the response
    // getsockname(client_desc, (struct sockaddr *)&local_server_address, &addr_size);

    if (sender_data->ip == 16777343) // if this server is on localhost, send as localhost, not as public IP [because localhost client can't connect to servers on public IP]
        resolveSearchRequest(&request, 16777343);
    else
        resolveSearchRequest(&request, my_ip);
}

// await for a timeframe and if there is no activity just exit... timeout
// treat with more threads???
void awaitResponsesAndAnswer(workerThreadData *worker_data, struct clientData *current_client)
{
    time_t i = time(NULL);
    time_t end = i + RESPONSE_AWAIT_WINDOW;

    while (i < end || !current_client->responses->empty()) // second confition is just in case
    {
        sem_wait(&current_client->responses_sem);
        while (!current_client->responses->empty())
        {
            uint8_t request_no = 22; // signal a packet is coming
            if (write(worker_data->socket_descriptor, &request_no, sizeof(uint8_t)) <= 0)
                break;
            if (sendResponsePacketToClient(&current_client->responses->front(), worker_data->socket_descriptor) == 0)
            {
                // delete everything.
                break;
            }
            delete current_client->responses->front().response_list;
            current_client->responses->pop();
        }
        sem_post(&current_client->responses_sem);
        i = time(NULL);
    }

    uint8_t request_no = 220; // signal the stop of arrival of requests
    if (write(worker_data->socket_descriptor, &request_no, sizeof(uint8_t)) <= 0)
        return;

    while (!current_client->responses->empty()) // delete the remainder of requests
        current_client->responses->pop();
}

uint8_t sendResponsePacketToClient(responseData *packet, int sock_desc)
{

    if (write(sock_desc, &packet->sender_ip, sizeof(in_addr_t)) <= 0)
        return 0;
    if (write(sock_desc, &packet->sender_port, sizeof(in_port_t)) <= 0)
        return 0;

    uint responses_count = packet->response_list->size();

    if (write(sock_desc, &responses_count, sizeof(uint)) <= 0)
        return 0;

    // printf("\n");

    // for (auto j : *packet->response_list)
    //     printf("%s | %s | %s\n", j.name.c_str(), j.extension.c_str(), j.disk_size.c_str());

    // printf("\n");

    for (auto i : *packet->response_list)
        if (sendResponseStructToPeer(&i, sock_desc) == 0)
            return 0;

    return 1;
}

static void *executeRoutineThread(void *)
{
    log("Routine thread started.\n");
    uint8_t secondsElapsed = 0;

    while (1)
    {
        if (exitFlag == 1)
        {
            if (activeClientConnections == 0)
            {
                // if (shutdown(service_socket_desc, SHUT_RD) == -1)
                //     perror("[routine] Error trying to shut down the service socket");
                log("[routine] Service socket was shut down.\n");
                break;
            }
        }

        if (routine_thread_long_check_interval != 0 && secondsElapsed >= routine_thread_long_check_interval)
        {
            secondsElapsed = 0;
            refreshDatabase();
            scanFolderAndAddToDatabase(upload_path);
            log("[routine] Database automation: refreshed and (re)scanned.\n");
            forceRefreshDBFlag = 0; // delete request to refresh/rescan since these tasks were just completed
            forceScanDBFlag = 0;
        }
        if (forceRefreshDBFlag == 1)
        {
            forceRefreshDBFlag = 0;
            refreshDatabase();
            log("[routine] User scheduled database refresh done.\n");
        }

        if (forceScanDBFlag == 1)
        {
            forceScanDBFlag = 0;
            scanFolderAndAddToDatabase(upload_path);
            log("[routine] User scheduled database scan done.\n");
        }

        if (exitFlag == 0)
        {
            purgeCache_oldRequests();
            purgeCache_oldResponses();
        }

        sleep(ROUTINE_SLEEP_INTERVAL);
        secondsElapsed += ROUTINE_SLEEP_INTERVAL;
    }

    const char *msg = "Routine thread stopped.\n";
    printf("%s", msg);
    log(msg);

    return NULL;
}

static void *executeNetworkServiceThread(void *arg)
{ // it's supposed to accept connections from other servers in order to spawn connection threads
    // fprintf(log_fd, "Network service thread with id: %lu started\n", pthread_self());
    log("Network service thread started.\n");
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(*((in_port_t *)arg));

    // int network_sock_desc; // socket descriptor

    if ((network_sock_desc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("[server]Error at socket()");
        networkFlag = 0;
        return NULL;
    }

    int on = 1;
    setsockopt(network_sock_desc, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(network_sock_desc, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        perror("[network]Error at bind()");
        networkFlag = 0;
        return NULL;
    }

    if (listen(network_sock_desc, MAX_LISTEN_CAPACITY) == -1)
    {
        perror("[network]Error at listen()");
        networkFlag = 0;
        return NULL;
    }

    // debug!
    // printf("[network thread] Socket opened, awaiting requests from servers.\n");

    while (1)
    {
        if (exitFlag == 1 || networkFlag == 0)
            break;

        int peer_descriptor;

        struct sockaddr_in from;
        memset(&from, 0, sizeof(from));
        socklen_t length = sizeof(from);

        // waiting for servers to join this node
        if ((peer_descriptor = accept(network_sock_desc, (struct sockaddr *)&from, &length)) < 0)
        {
            // perror("[service thread] Error or at accept().\n");
            networkFlag = 0;
            break;
        }

        uint8_t type = 200;
        if (write(peer_descriptor, &type, sizeof(type)) <= 0) // send type
        {
            // error
            continue;
        }
        else
        {
            uint8_t status;
            if (read(peer_descriptor, &status, sizeof(status)) <= 0) // await confirmation
            {
                // error
                continue;
            }
            if (status == 210) // just convetion
            {

                connectedPeerData *peer_data = new struct connectedPeerData;
                peer_data->peer_sock_desc = peer_descriptor;
                peer_data->peer_ip = from.sin_addr.s_addr;
                peer_data->peer_port = from.sin_port;

                pthread_t th;
                pthread_create(&th, NULL, &networkPeerConnectedToThisServer, peer_data);
            }
            else
            {
                close(peer_descriptor);
            }
        }
    }

    close(network_sock_desc);

    // delete peers_connected_to_this_server;

    // fprintf(log_fd, "Network service thread with id: %lu terminated\n", pthread_self());
    log("Network service thread terminated.\n");
    networkFlag = 0;
    return NULL;
}

// thread dedicated to peers connected to this server
static void *networkPeerConnectedToThisServer(void *arg)
{
    fprintf(log_fd, "\tnetworkPeerConnectedToThisServer thread with id: %lu started\n", pthread_self());
    pthread_detach(pthread_self());

    struct connectedPeerData *peer_data = (struct connectedPeerData *)arg;

    // need a semaphore
    peers_connected_to_this_server->push_back(*peer_data); // just copy

    activeServerConnections++;

    // int peer_descriptor = peer_data->peer_sock_desc;

    // peerWriteToSockDescriptorSem->insert(std::make_pair(peer_data->peer_sock_desc, (sem_t *)malloc(sizeof(sem_t)))); // must check if this sock desc is dead when doing smth then put it in queue to deallocate this mem
    // sem_init((*peerWriteToSockDescriptorSem)[peer_data->peer_sock_desc], 0, 1);

    peer_awaitRequestsAndResolve(peer_data->peer_sock_desc); // when this function returns, the connexion is terminated

    // sem_destroy((*peerWriteToSockDescriptorSem)[peer_data->peer_sock_desc]);
    // peerWriteToSockDescriptorSem->erase(peer_data->peer_sock_desc);

    close(peer_data->peer_sock_desc);
    activeServerConnections--;

    { // a quick and dirty delete, bc I don't want to bloat the code with even more functions
        auto i = peers_connected_to_this_server->begin();
        while (i != peers_connected_to_this_server->end())

            if (i->peer_ip == peer_data->peer_ip && i->peer_port == peer_data->peer_port)
                i = peers_connected_to_this_server->erase(i);
            else
                i++;
    }

    fprintf(log_fd, "\tnetworkPeerConnectedToThisServer thread with id: %lu terminated\n", pthread_self());

    return NULL;
}

// thread dedicated to peers I am connected to
static void *networkPeerConnectedFromThisServer(void *arg)
{
    pthread_detach(pthread_self());
    fprintf(log_fd, "\tnetworkPeerConnectedFromThisServer thread with id: %lu started\n", pthread_self());
    struct sockData *data = (struct sockData *)arg;

    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = data->ip;
    server.sin_port = data->port;

    free(arg);

    int peer_descriptor; // socket descriptor

    if ((peer_descriptor = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("[peer connection thread]Error at socket(). Try again later!\n");
        return NULL;
    }

    if (connect(peer_descriptor, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        perror("[peer connection thread]Error at connect(). Try again later!\n");
        return NULL;
    }

    uint8_t server_type;
    if (read(peer_descriptor, &server_type, sizeof(server_type)) <= 0) // send type
    {
        // error
        // continue;
    }
    if (server_type == 200) // if the server is a networking server, proceed
    {
        uint8_t client_type = 210; // send this "client's" type, for network threads is 210
        if (write(peer_descriptor, &client_type, sizeof(client_type)) <= 0)
        {
            // error
            // continue;
        }

        printf("Connection established.\n>\t");
    }
    else
    {
        printf("Cannot connect to this server [possible service server]!\n>\t");
        close(peer_descriptor);

        goto terminate_thread;
    }

    activeServerConnections++;

    connectedPeerData peer_data;
    peer_data.peer_sock_desc = peer_descriptor;
    peer_data.peer_ip = server.sin_addr.s_addr;
    peer_data.peer_port = server.sin_port;
    peers_this_server_is_connected_to->push_back(peer_data);

    // this will manage the requests
    peer_awaitRequestsAndResolve(peer_descriptor);

    activeServerConnections--;

    { // a quick and dirty delete, bc I don't want to bloat the code with even more functions
        auto i = peers_this_server_is_connected_to->begin();
        while (i != peers_this_server_is_connected_to->end())

            if (i->peer_ip == peer_data.peer_ip && i->peer_port == peer_data.peer_port)
                i = peers_this_server_is_connected_to->erase(i);
            else
                i++;
    }

terminate_thread:
    fprintf(log_fd, "\tnetworkPeerConnectedFromThisServer thread with id: %lu terminated\n", pthread_self());

    return NULL;
}

// function that treats requests from server peers,
// this function removes redundancies in threads created by servers connected to this server
// and threads that this server is connected to
// since these threads are only reading, I can write separately when needed and keep the threads blocked for reading from the descriptor
// Note: this function also manages the peerWriteToSockDescriptorSem map! previously every thread did
void peer_awaitRequestsAndResolve(int peer_descriptor) // ip+port for this instance
{
    peerWriteToSockDescriptorSem->insert(std::make_pair(peer_descriptor, (sem_t *)malloc(sizeof(sem_t))));
    sem_init((*peerWriteToSockDescriptorSem)[peer_descriptor], 0, 1);

    uint8_t request_no = 0;

    while (1)
    {
        if (read(peer_descriptor, &request_no, sizeof(uint8_t)) <= 0)
        {
            // perror("[worker thread]Error at reading request number\n");
            fprintf(log_fd, "\tPeer disconnected unexpectedly from network thread: %lu\n", pthread_self());
            break;
        }

        fprintf(log_fd, "\t\tNetwork peer with thread id: %lu received request number: %hhu\n", pthread_self(), request_no);

        if (request_no == 11) // request for searching the network [server-server]
            treatNetworkSearchRequest(peer_descriptor);

        else if (request_no == 21)                        // request to receive a message, then figure out if to cascade the response or to just send it through a valid connection
            treatNetworkResponseRequest(peer_descriptor); // treat network response cascade request!!!

        else if (request_no == 255) // terminate the connection
            break;

        request_no = 0;
    }

    sem_close((*peerWriteToSockDescriptorSem)[peer_descriptor]);
    peerWriteToSockDescriptorSem->erase(peer_descriptor);

    return;
}

void treatNetworkSearchRequest(int peer_descriptor)
{
    // read the incoming data and create a new search request and push it to the request queue which will be managed by the request thread

    uint8_t request_mask;
    if (read(peer_descriptor, &request_mask, sizeof(uint8_t)) <= 0)
        return; // abort

    char file_name_or_pattern[256] = {0};
    if (request_mask & 0b00000001 ||
        request_mask & 0b00000010)
    {
        uint8_t length;
        if (read(peer_descriptor, &length, sizeof(uint8_t)) <= 0)
            return;
        if (readExact(peer_descriptor, file_name_or_pattern, length * sizeof(char)) <= 0)
            return;
    }

    char extension[20] = {0};
    if (request_mask & 0b00000100)
    {
        uint8_t length;
        if (read(peer_descriptor, &length, sizeof(uint8_t)) <= 0)
            return;
        if (readExact(peer_descriptor, extension, length * sizeof(char)) <= 0)
            return;
    }

    char size[20] = {0};
    if (request_mask & 0b10000000 ||
        request_mask & 0b01000000 ||
        request_mask & 0b00100000)
    {
        uint8_t length;
        if (read(peer_descriptor, &length, sizeof(uint8_t)) <= 0)
            return;
        if (readExact(peer_descriptor, size, length * sizeof(char)) <= 0)
            return;
    }

    in_addr_t sender_ip;
    if (readExact(peer_descriptor, &sender_ip, sizeof(in_addr_t)) <= 0)
        return;
    in_addr_t sender_public_ip;
    if (readExact(peer_descriptor, &sender_public_ip, sizeof(in_addr_t)) <= 0)
        return;

    in_port_t sender_port;
    if (readExact(peer_descriptor, &sender_port, sizeof(in_port_t)) <= 0)
        return;

    uint8_t ttl;
    if (read(peer_descriptor, &ttl, sizeof(uint8_t)) <= 0)
        return;

    fileRequest request;
    request.request_mask = request_mask;
    request.name_or_pattern = file_name_or_pattern;
    request.extension = extension;
    request.disk_size = size;
    request.sender_public_ip = sender_public_ip;
    request.sender_ip = sender_ip; // those are the client's address and port, not peer's!
    request.sender_port = sender_port;

    // printf("%d\n%s %s %s\n%d %d %d\n\n", request_mask, file_name_or_pattern, extension, size, sender_ip, sender_port, ttl);

    // purgeCache_oldRequests(sender_ip, sender_port);
    // purgeCache_oldResponses(sender_ip, sender_port);

    if (ttl > 0)
    {
        request.time_to_live = ttl - 1;
        // printf("%d %d\n", request.sender_ip, request.sender_port);

        if (checkRequestInCache(&request) == 0)
        { // to prevent infinte cascading
            sem_wait(&searchRequestsQueue_sem);
            searchRequestsQueue->push(request); // broadcast the search request to other servers; the dedicated thread will cascade the request
            sem_post(&searchRequestsQueue_sem);

            // struct sockaddr_in local_server_side; // server side data
            // socklen_t addr_size = sizeof(local_server_side);
            // getsockname(peer_descriptor, (struct sockaddr *)&local_server_side, &addr_size);

            if (sender_public_ip == my_ip)                // test if it's localhost, if so then send as localhost
                resolveSearchRequest(&request, 16777343); // search in database and broadcast the response; the dedicated thread will cascade the response
            else
                resolveSearchRequest(&request, my_ip);
            // I am sure the sql command is correct since I check if it is before the initial server cascades the request
            // update: not even that; i am sure i compose a correct sql since the program composes it based on the mask
            return;
        }
        else
            log("Search request dropped: cache hit\n");
    }
    else
        log("Search request dropped: TTL expired\n");
}

// PORT IS USELESS! TREBUIE SA FIE PORNIT SERVICIUL CA SA POT FACE ORICE ALTCEVA, SI TREBUIE SA TRIMIT PORTUL [CA IPUL E UNIC PE INSTANTA] CA SA SE CONECTEZE CLIENTUL LA PORTUL ALA!!!
//  used by both peers and worker
void resolveSearchRequest(fileRequest *request, in_addr_t sender_ip) // interrogate the database and push the response to a queue
{
    // send back: full file name, extension, disk size, file hash

    std::vector<fileEntry2> *response_list = new std::vector<fileEntry2>;

    sem_wait(&fileDB_sem);
    searchInDatabase(request, response_list); // send origin adres
    sem_post(&fileDB_sem);

    if (!response_list->empty()) // it makes sense to send a response only if it not empty
    {
        responseData current_response;
        current_response.response_list = response_list;

        current_response.receiver_ip = request->sender_ip; // switching places, the receiver is now the request sender
        current_response.receiver_port = request->sender_port;

        // ip-ul privat...
        current_response.sender_ip = sender_ip;           // this is this server, since this server will be the sender of this response
        current_response.sender_port = htons(serverPort); // sender_port; I HAVE TO TELL THIS PORT SINCE THE CLIENT WILL CONNECT TO THIS PORT, NOT THE NETWORK'S

        // special case if the server has a response, just send it to the client
        if (checkIfConnectedToReceiver_AndPushToItsResponseList(&current_response) == 0) // if the 'don't cascade flag' is set, there's no need to check it again, the response will be sent
        {                                                                                // first check if this program is connected to the desired client and send the response directly,
            sem_wait(&responseQueue_sem);
            responseQueue->push(current_response); // else, cascade the response
            sem_post(&responseQueue_sem);
        }
    }

    // delete response_list;
}

void treatNetworkResponseRequest(int peer_sock_desc)
{
    in_addr_t receiver_ip;
    if (readExact(peer_sock_desc, &receiver_ip, sizeof(in_addr_t)) <= 0)
        return;
    in_port_t receiver_port;
    if (readExact(peer_sock_desc, &receiver_port, sizeof(in_port_t)) <= 0)
        return;
    in_addr_t sender_ip;
    if (readExact(peer_sock_desc, &sender_ip, sizeof(in_addr_t)) <= 0)
        return;
    in_port_t sender_port;
    if (readExact(peer_sock_desc, &sender_port, sizeof(in_port_t)) <= 0)
        return;

    // send the count of responses
    uint responses_count;
    if (readExact(peer_sock_desc, &responses_count, sizeof(uint)) <= 0)
        return;

    std::vector<fileEntry2> *responses_list = new std::vector<fileEntry2>;

    for (; responses_count > 0; responses_count--)
    {
        // printf("%u\n", responses_count);
        if (readResponseStructFromPeer(peer_sock_desc, responses_list) == 0)
        {
            printf("Error reading response struct from peer\n");
            return;
        }
    }

    responseData response_data;
    response_data.receiver_ip = receiver_ip;
    response_data.receiver_port = receiver_port;
    response_data.response_list = responses_list;
    response_data.sender_ip = sender_ip;
    response_data.sender_port = sender_port;
    // response_data.received_from_sock_desc = peer_sock_desc; // with this I want to send back the response
    //  ideally on the same socket descriptors as the requests came

    // check if the receiver has a connected client that has to receive this response
    uint8_t receiver_found = checkIfConnectedToReceiver_AndPushToItsResponseList(&response_data);

    // printf("recv: %d\n", receiver_found);

    if (receiver_found == 0)
    {
        sem_wait(&responseQueue_sem);
        responseQueue->push(response_data); // prepare to cascade the response to other servers
        sem_post(&responseQueue_sem);
    }
}

uint8_t checkIfConnectedToReceiver_AndPushToItsResponseList(responseData *response_data)
{
    for (auto i : *connectedClientsList)
    {
        if (response_data->receiver_ip == i->ip && response_data->receiver_port == i->port)
        {
            sem_wait(&i->responses_sem);
            i->responses->push(*response_data);
            sem_post(&i->responses_sem);
            return 1;
        }
    }
    return 0;
}

// ia sizeul
uint8_t readResponseStructFromPeer(int peer_sock_desc, std::vector<fileEntry2> *responses_list)
{
    // it doesn't make sense to send the path; if I can check the file by hash and name [even others for sake of security]
    // it would be the same file from each and every path it is found in the database!

    uint16_t length;
    if (readExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    char path[4096] = {0};
    if (readExact(peer_sock_desc, path, length * sizeof(char)) <= 0)
        return 0;

    if (readExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    char name[256] = {0};
    if (readExact(peer_sock_desc, name, length * sizeof(char)) <= 0)
        return 0;

    // if (read(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
    //     return 0;
    char hash[32 + 1]; // 32 the length of hash + 1 null termination
    hash[32] = 0;      // here it makes sense to set hash[32] = null since my hash will be always 32 bytes long and I shouldn't waste 32 clock cycles fore zero-ing the memory
    if (readExact(peer_sock_desc, hash, 32 * sizeof(char)) <= 0)
        return 0;

    if (readExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    char extension[20] = {0};
    if (readExact(peer_sock_desc, extension, length * sizeof(char)) < 0)
        return 0;

    if (readExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    char size[20] = {0};
    if (readExact(peer_sock_desc, size, length * sizeof(char)) <= 0)
        return 0;

    fileEntry2 entry;
    entry.path = path;
    entry.name = name;
    entry.hash = hash;
    entry.extension = extension;
    entry.disk_size = size;

    responses_list->push_back(entry);
    return 1;
}

static void *executeNetworkServiceThread_cascadeRequests(void *arg) // request cascader
{
    log("Request manager thread started.\n");

    // aici ar trebui sa citesc din queue, trimiti tuturor vecinilor conectati un request number de perpetuare si continutul cautat
    // natura blocanta? requesturile sunt puse in queue, si in threadul asta citesc o singura data si trimit tuturor serverelor
    // pentru ca threadurile astea au cam singurul scop de-a perpetua sau raspunde cererilor astora, ramane de vazut ce consecinte ar avea natura iterativa

    while (1)
    {
        sem_wait(&searchRequestsQueue_sem);
        if (!searchRequestsQueue->empty())
        {
            log("[request manager] Processing queued request\n");
            struct fileRequest request_data = searchRequestsQueue->front();

            // if this server already received this request, don't cascade, there is another server that sent it
            // meaning this server already cascaded this request in the past
            if (!checkRequestInCache(&request_data))
            {
                cacheFileRequest to_cache;
                to_cache.request = request_data;
                to_cache.time_when_cached = time(NULL);
                // printf("\nreq %d\n%s %s %s\n%d %d\n\n", to_cache.request.request_mask, to_cache.request.name_or_pattern.c_str(), to_cache.request.extension.c_str(), to_cache.request.disk_size.c_str(), to_cache.request.sender_ip, to_cache.request.sender_port);
                sem_wait(&requestCache_sem);
                requestCache->push_back(to_cache); // the request is cached immediately for the worker threads receiving
                sem_post(&requestCache_sem);
                // printf("cached\n");
                //  the requests to not get the same request back from a peer and insert it back in the queue

                for (auto i : *peers_this_server_is_connected_to)
                {
                    sem_wait((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                    if (cascadeSearchRequestToPeer(&request_data, i.peer_sock_desc) == 0) // this fct checks if there's a cached request
                        log("[request manager] Error cascading to peer!");

                    sem_post((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                }

                for (auto i : *peers_connected_to_this_server)
                {
                    sem_wait((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                    if (cascadeSearchRequestToPeer(&request_data, i.peer_sock_desc) == 0)
                        log("[request manager] Error cascading to peer!");

                    sem_post((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                }
            }

            searchRequestsQueue->pop(); // either way this request needs to be popped

            log("[request manager] Request processed\n");
        }
        sem_post(&searchRequestsQueue_sem);

        if (exitFlag == 1)
            break;

        else
            usleep(100);
    }

    log("Request manager thread stopped.\n");
    return NULL;
}

// trimit raspunsurile secvential, deci pot verifica/pastra numai primul element din lista si in cache

uint8_t checkRequestInCache(struct fileRequest *request_data)
{
    for (auto i : *requestCache)
    {
        if (request_data->sender_ip == i.request.sender_ip &&
            request_data->sender_port == i.request.sender_port &&
            request_data->request_mask == i.request.request_mask)
        {
            if (request_data->name_or_pattern == i.request.name_or_pattern &&
                request_data->extension == i.request.extension &&
                request_data->disk_size == i.request.disk_size)
            {
                log("Search request cache hit\n");
                return 1;
            }
        }
    }
    log("Search request cache miss\n");
    return 0;
}

uint8_t cascadeSearchRequestToPeer(struct fileRequest *request_data, int peer_sock_desc)
{
    // 1 send the request number
    // 2 send the request mask
    // 3 send the relevant data according to the request mask
    // 4 send the ip & port to check the responses back for who is connected to the owner of the request
    // 5 send time to live - 1

    // send the request only if it doesn't already exist in cache
    // 1
    uint8_t request_no = 11;
    if (write(peer_sock_desc, &request_no, sizeof(uint8_t)) <= 0)
        return 0;

    // 2
    if (write(peer_sock_desc, &(request_data->request_mask), sizeof(uint8_t)) <= 0)
        return 0;

    // 3
    { // data relevant for searching
        if (request_data->request_mask & 0b00000001 ||
            request_data->request_mask & 0b00000010)
        {
            uint8_t length = request_data->name_or_pattern.size();
            if (write(peer_sock_desc, &length, sizeof(uint8_t)) <= 0)
                return 0;
            if (writeExact(peer_sock_desc, request_data->name_or_pattern.c_str(), length * sizeof(char)) <= 0)
                return 0;
        }
        if (request_data->request_mask & 0b00000100)
        {
            uint8_t length = request_data->extension.size();
            if (write(peer_sock_desc, &length, sizeof(uint8_t)) <= 0)
                return 0;
            if (writeExact(peer_sock_desc, request_data->extension.c_str(), length * sizeof(char)) <= 0)
                return 0;
        }
        if (request_data->request_mask & 0b10000000 ||
            request_data->request_mask & 0b01000000 ||
            request_data->request_mask & 0b00100000)
        {

            uint8_t length = request_data->disk_size.size();
            if (write(peer_sock_desc, &length, sizeof(uint8_t)) <= 0)
                return 0;
            if (writeExact(peer_sock_desc, request_data->disk_size.c_str(), length * sizeof(char)) <= 0)
                return 0;
        }
    }

    // 4
    if (writeExact(peer_sock_desc, &request_data->sender_ip, sizeof(in_addr_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, &request_data->sender_public_ip, sizeof(in_addr_t)) <= 0)
        return 0;

    if (writeExact(peer_sock_desc, &request_data->sender_port, sizeof(in_port_t)) <= 0)
        return 0;

    // 6
    if (write(peer_sock_desc, &request_data->time_to_live, sizeof(uint8_t)) <= 0) // the ttl is decremented when received [afaik]
        return 0;

    return 1;
    // now write rest of data
}

// NOTE: HERE ARIVE ONLY RESPONSES THAT ARE TO BE CASCADED FOR REAL, SINCE RESPONSES FOR THE CLIENT CONNECTED TO THIS
// SERVER ARE HANDLED IN TREATNETWORKRESPONSE, LONG BEFORE ENTERING THIS FUNCTION, JUST WHEN THE RESPONSES ARE RECEIVED!
static void *executeNetworkServiceThread_cascadeResponses(void *arg) // this gets only the responses that need to be cascaded
{
    log("Response manager thread started.\n");

    while (1)
    {
        sem_wait(&responseQueue_sem);
        if (!responseQueue->empty())
        {
            log("[response manager thread] Processing queued response\n");
            struct responseData response_data = responseQueue->front();

            // if this server already received this response, don't cascade, there is another server that sent this response
            // meaning this server already cascaded this response in the past
            if (!checkResponseInCache(&response_data))
            {
                {                                      // quick and dirty; to not bloat the code with more functions
                    struct cacheResponseData to_cache; // prepare cache but with only the first entry of the response [faster]
                    to_cache.disk_size = response_data.response_list->front().disk_size;
                    to_cache.extension = response_data.response_list->front().extension;
                    to_cache.hash = response_data.response_list->front().hash;
                    to_cache.name = response_data.response_list->front().name;
                    to_cache.path = response_data.response_list->front().path;
                    to_cache.receiver_ip = response_data.receiver_ip;
                    to_cache.receiver_port = response_data.receiver_port;
                    to_cache.time_when_cached = time(NULL);
                    responseCache->push_back(to_cache);
                }

                uint8_t conn_lost = 1; // ca sa trimit inapoi de la un alt peer trebuie sa caut in cache
                // if the channel the request was received through is still up, send the response back through it and don't flood the network;
                // sem_wait((*peerWriteToSockDescriptorSem)[response_data.received_from_sock_desc]);

                // uint8_t request_no = 21;
                //  if (write(response_data.received_from_sock_desc, &request_no, sizeof(request_no)) <= 0)
                //      conn_lost = 1;
                //  else

                // if (cascadeResponseToPeer(&response_data, response_data.received_from_sock_desc) == 0)
                // { // try to cascade, if not, close the descriptor and propagate the response to everyone
                //     conn_lost = 1;
                //     close(response_data.received_from_sock_desc); // closing the descriptor will terminate the corresponding thread
                // }
                // sem_post((*peerWriteToSockDescriptorSem)[response_data.received_from_sock_desc]);

                // failsafe is in the cache
                if (conn_lost == 1) // if I there was an error sending the request no, then cascade to other servers that might be connected
                {
                    for (auto i : *peers_this_server_is_connected_to)
                    {
                        if (response_data.last_sock_desc != i.peer_sock_desc) // don't send back on the socket that this message was received from
                        {
                            sem_wait((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                            cascadeResponseToPeer(&response_data, i.peer_sock_desc); // this fct checks if the response was already
                            sem_post((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                        }
                    }

                    for (auto i : *peers_connected_to_this_server)
                    {
                        if (response_data.last_sock_desc != i.peer_sock_desc)
                        {
                            sem_wait((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                            cascadeResponseToPeer(&response_data, i.peer_sock_desc);
                            sem_post((*peerWriteToSockDescriptorSem)[i.peer_sock_desc]);
                        }
                    }
                }
            }

            delete response_data.response_list;

            responseQueue->pop(); // either way this response needs to be popped

            log("[response manager thread] Response processed\n");
        }
        sem_post(&responseQueue_sem);

        if (exitFlag == 1)
            break;
        else
            usleep(100);
    }
    log("Response manager thread terminated.\n");
    return NULL;
}

uint8_t checkResponseInCache(struct responseData *response_data)
{
    for (auto i : *responseCache)
        if (response_data->receiver_ip == i.receiver_ip &&
            response_data->receiver_port == i.receiver_port) // if there is a partial hit
        {                                                    // continue checking to make sure if it is exactly a cached response
            struct fileEntry2 *first_element = &(response_data->response_list->at(0));
            // checking if the first element matches means that I got the same response
            if (first_element->path == i.path &&
                first_element->name == i.name &&
                first_element->hash == i.hash &&
                first_element->extension == i.extension &&
                first_element->disk_size == i.disk_size)
            {
                log("Response cache hit\n");
                return 1;
            }
        }
    log("Response cache miss\n");
    return 0;
}

uint8_t cascadeResponseToPeer(struct responseData *response_data, int peer_sock_desc)
{
    // 1 send request no
    // 2 send the receiver ip [who should receive this message]
    // 3 send the receiver port
    // 4 send the count number
    // 5 send the responses as a struct
    uint8_t request_no = 21;

    if (write(peer_sock_desc, &request_no, sizeof(uint8_t)) <= 0)
        return 0;

    if (writeExact(peer_sock_desc, &response_data->receiver_ip, sizeof(in_addr_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, &response_data->receiver_port, sizeof(in_port_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, &response_data->sender_ip, sizeof(in_addr_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, &response_data->sender_port, sizeof(in_port_t)) <= 0)
        return 0;

    // send the count of responses
    uint responses_count = response_data->response_list->size();
    if (writeExact(peer_sock_desc, &responses_count, sizeof(uint)) <= 0)
        return 0;

    for (uint i = 0; i < responses_count; i++)
    {
        // printf("%u\n", i);
        if (sendResponseStructToPeer(&response_data->response_list->at(i), peer_sock_desc) == 0)
            printf("Error sending response struct to peer\n");
    }

    // uint8_t response_signal;
    // if (read(peer_sock_desc, &response_signal, sizeof(uint8_t)) <= 0)
    //     return 0;

    return 1;
}

uint8_t sendResponseStructToPeer(struct fileEntry2 *data, int peer_sock_desc)
{
    uint16_t length = data->path.size();
    if (writeExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, data->path.c_str(), length * sizeof(char)) <= 0)
        return 0;

    length = data->name.size();
    if (writeExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, data->name.c_str(), length * sizeof(char)) <= 0)
        return 0;

    if (writeExact(peer_sock_desc, data->hash.c_str(), 32 * sizeof(char)) <= 0) // hash is fixed
        return 0;

    length = data->extension.size();
    if (writeExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, data->extension.c_str(), length * sizeof(char)) <= 0)
        return 0;

    length = data->disk_size.size();
    if (writeExact(peer_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    if (writeExact(peer_sock_desc, data->disk_size.c_str(), length * sizeof(char)) <= 0)
        return 0;

    return 1;
}

int16_t readExact(int socket, void *buffer, uint16_t x)
{
    uint16_t bytes_read = 0;
    int16_t result;
    while (bytes_read < x)
    {
        result = read(socket, (uint8_t *)buffer + bytes_read, x - bytes_read);
        if (result < 1)
            perror("Error reading from socket");
        bytes_read += result;
    }
    return result;
}

int16_t writeExact(int socket, const void *buffer, uint16_t x)
{
    uint16_t written_bytes = 0;
    int16_t result;
    while (written_bytes < x)
    {
        result = write(socket, (uint8_t *)buffer + written_bytes, x - written_bytes);
        if (result < 1)
            perror("Error reading from socket");
        written_bytes += result;
    }
    return result;
}

///

//  FUNCTIONS & HELPER FUNCTIONS // functions you do once and never touch again

///

void log_fct(const char *to_log)
{
    if (log_fd != NULL)
    {
        if (fprintf(log_fd, "%s", to_log) < 0)
            perror("Error at logging");
        fflush(log_fd);
    }
}

void getMyIP()
{
    FILE *ip_file = fopen(IP_FILE_NAME, "w+");

    if (ip_file != NULL)
    {
        printf("Please wait while the public IP is gathered\n");
        int pid = fork();

        if (pid == 0)
        { // child process
            if (access(IP_GET_SCRIPT, F_OK) != 0)
            { // test if the script exists, else create it, this code exists only for portability reasons
                const char *get_ip_file_contents = "#!/bin/bash\nfile=./ip.txt\n\nip=$(curl -s https://api.ipify.org)\necho \"$ip\" >$file";
                FILE *ip_script = fopen(IP_GET_SCRIPT, "w+");
                if (ip_script != NULL)
                {
                    if (fwrite(get_ip_file_contents, 1, 80, ip_script) <= 0) // 80 = 81 - null
                        perror("Error writing to IP script");
                    fflush(ip_script);
                    fclose(ip_script);

                    chmod(IP_GET_SCRIPT, 0777);
                }
                else
                    perror("Couldn't open the IP script");
            }

            if (execlp(IP_GET_SCRIPT, IP_GET_SCRIPT, NULL) <= 0)
            {
                perror("execlp failed");
                exit(1);
            }
            exit(0);
        }
        wait(NULL);

        char ip[16] = {0};
        fscanf(ip_file, "%s", ip);

        printf("This device's public IP is: %s\n", ip);

        my_ip = inet_addr(ip);

        fclose(ip_file);

        // remove(IP_GET_SCRIPT);
        remove(IP_FILE_NAME);
    }
    else
        perror("Couldn't open/create a temp file to get the IP");
}

inline void purgeCache_oldRequests()
{
    sem_wait(&requestCache_sem);
    if (!requestCache->empty())
    {
        auto i = requestCache->begin();
        time_t current_time = time(NULL); // one fact is clear: there won't ever be so many entrys to make the processor take more than 1000ms
        while (i != requestCache->end())
            if (i->time_when_cached + CACHE_PURGE_INTERVAL <= current_time) // this migh actually be better, since it's one less call to do
                i = requestCache->erase(i);
            else
                i++;
    }
    sem_post(&requestCache_sem);
}
inline void purgeCache_oldResponses()
{

    sem_wait(&responseCache_sem);
    if (!responseCache->empty())
    {
        auto i = responseCache->begin();
        time_t current_time = time(NULL); // one fact is clear: there won't ever be so many entrys to make the processor take more than 1000ms
        while (i != responseCache->end())
            if (i->time_when_cached + CACHE_PURGE_INTERVAL < current_time) // this migh actually be better, since it's one less call to do
                i = responseCache->erase(i);
            else
                i++;
    }
    sem_post(&responseCache_sem);
}

uint8_t *md5FileWrapper(char *path)
{
    FILE *fd = fopen(path, "r"); // I sure know this file exists
    uint8_t *hash = md5File(fd);
    fclose(fd);
    return hash;
}

void binaryStringToHexaString(char *xp, uint8_t *bb, int n)
{
    const char xx[] = "0123456789ABCDEF";
    while (--n >= 0)
        xp[n] = xx[(bb[n >> 1] >> ((1 - (n & 1)) << 2)) & 0xF];
}

long getFileSize(char *path) // asuming I get the full path
{
    FILE *fp;
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    int prev = ftell(fp);
    fseek(fp, 0L, SEEK_END);
    long size = ftell(fp);
    fseek(fp, prev, SEEK_SET);

    fclose(fp);

    return size;
}

inline void eraseClientFromClientsList(struct clientData *client_todelete)
{
    for (uint i = 0; i < connectedClientsList->size(); i++)
    {
        if (connectedClientsList->at(i) == client_todelete)
        {
            connectedClientsList->erase(connectedClientsList->begin() + i);
            return;
        }
    }
}

void sqlite_regexp(sqlite3_context *context, int argc, sqlite3_value **values)
{
    int ret;
    regex_t regex;
    char *reg = (char *)sqlite3_value_text(values[0]);
    char *text = (char *)sqlite3_value_text(values[1]);

    if (argc != 2 || reg == 0 || text == 0)
    {
        sqlite3_result_error(context, "SQL function regexp() called with invalid arguments.\n", -1);
        return;
    }

    ret = regcomp(&regex, reg, REG_EXTENDED | REG_NOSUB);
    if (ret != 0)
    {
        sqlite3_result_error(context, "error compiling regular expression", -1);
        return;
    }

    ret = regexec(&regex, text, 0, NULL, 0);
    regfree(&regex);

    sqlite3_result_int(context, (ret != REG_NOMATCH));
}

std::queue<std::string> *loadConfigFile()
{
    FILE *config_file = fopen(CONFIG_FILE_NAME, "r");

    std::queue<std::string> *auto_commands = nullptr;

    if (config_file == NULL)
    {
        printf("Couldn't open config file, a default one will be created\n");
        if ((config_file = fopen(CONFIG_FILE_NAME, "w")) == NULL)
            perror("Couldn't create a config file. Reason: ");
        else
        {
            fprintf(config_file, "long_check_interval: %u\nupload_path: %s\n", DEFAULT_ROUTINE_SLEEP_LONG_INTERVAL, DEFAULT_UPLOADS_FOLDER_PATH);
            fclose(config_file);
            printf("A config file was created and default values were inserted.\n");
        }
        routine_thread_long_check_interval = DEFAULT_ROUTINE_SLEEP_LONG_INTERVAL;
        upload_path = strdup(DEFAULT_UPLOADS_FOLDER_PATH);

        printf("Runtime variables defaulted to:\n\tupload_path: %s\n\tlong_check_interval: %u\n", upload_path, routine_thread_long_check_interval);

        return nullptr; // return null for main to know there aren't auto commands to be executed
    }
    else
    {
        uint8_t should_heapalloc = 0;

        char buffer[4096 + 22] = {0};                         // path_max + lengthiest entry name
        while (fscanf(config_file, "%[^\n] ", buffer) != EOF) // read line by line
        {
            if (strstr(buffer, "long_check_interval: ") != NULL)
            {
                char *p = buffer + 21;
                routine_thread_long_check_interval = atoi(p);
            }
            else if (strstr(buffer, "upload_path: ") != NULL)
            {
                char *p = buffer + 13;
                upload_path = (char *)calloc(strlen(p), sizeof(char));
                strcpy(upload_path, p);
            }
            else if (strstr(buffer, "[") == buffer) // insert into config.txt in backward order!!
            {
                if (should_heapalloc == 0)
                {
                    auto_commands = new std::queue<std::string>; // initialize only if there is at least one entry.
                    should_heapalloc = 1;
                }
                std::string s;
                char *p = strtok(buffer + 1, "]");
                s = p;
                auto_commands->push(s);
            }
            memset(buffer, 0, sizeof(buffer));
        }

        fclose(config_file);
        printf("Configs loaded\n");
        fprintf(log_fd, "Configs loaded:\n\t\t\tupload_path: %s\n\t\t\tlong_check_interval: %u\n", upload_path, routine_thread_long_check_interval);

        return auto_commands;
    }
}

void saveConfigFile() // quick and dirty, nici eu n-as vrea implementarea asta, dar atat timp am avut
{
    FILE *config_file;

    if ((config_file = fopen(CONFIG_FILE_NAME, "w")) == NULL)
    {
        perror("[config save]Couldn't open the config file! Try again later.");
        return;
    }

    uint length = 21 + 9 + 13 + 4096 + 1; // entry1+maxint + entry2+maxpath + 1 null
    char *buffer = (char *)calloc(length, sizeof(char));
    strcpy(buffer, "long_check_interval: ");
    char num[10] = {0};
    snprintf(num, 10, "%d", routine_thread_long_check_interval);
    strcat(buffer, num);

    strcat(buffer, "\nupload_path: ");
    strcat(buffer, upload_path);
    strcat(buffer, "\n");

    fwrite(buffer, 1, strlen(buffer), config_file);

    fclose(config_file);
    free(buffer);
}

// Returns 0 if something goes wrong, 1 if all ok.
uint8_t connectToFileDB()
{
    if (sqlite3_open(DATABASE_NAME, &fileDatabase) != 0)
    {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(fileDatabase));
        return 0; // this program cannot function without the db
    }
    else
    {
        log("Opened database successfully\n");
        fprintf(stderr, "Opened database successfully\n");
    }

    const char *query = "CREATE TABLE IF NOT EXISTS FILEHASH(PATH VARCHAR(4096), NAME VARCHAR(255), HASH CHAR(32), EXTENSION VARCHAR(20), SIZE INTEGER, LAST_UPDATE INTEGER, UNIQUE(PATH, HASH));";
    // no extension limit on linux, but included in the 255 file name limit

    char *errMsg;
    if (sqlite3_exec(fileDatabase, query, NULL, 0, &errMsg) != SQLITE_OK)
    {
        fprintf(stderr, "Error at executing 'create tabe': %s\n", sqlite3_errmsg(fileDatabase));
        sqlite3_free(errMsg);
        return 0;
    }

    sqlite3_create_function(fileDatabase, "regexp", 2, SQLITE_ANY, 0, &sqlite_regexp, 0, 0);
    // add the regexp function inside sql. works like this: <column> regexp 'pattern'

    return 1;
}

// just a reminder: there was a debate once about if the database should store the name [255 chars linux max comprised of name+extension]
// and do nothing for extension or just remove the extension from the name and insert it into another column.
// the latter seems to be the best solution since searches are made by the db using 'select * from filehash where <condition>'
void scanFolderAndAddToDatabase(char *path)
{
    DIR *dir;
    struct dirent *entry;

    if (!(dir = opendir(path)))
        return;

    while ((entry = readdir(dir)) != NULL)
    {

        if (entry->d_name[0] != '.')
        {
            if (entry->d_type == DT_DIR)
            {
                char new_path[4096];
                // if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                //     continue;
                // if (path[0] != '/')
                char *p;
                if (path[1] == '/') // if there is an extra /, just don't use it to not appear listed in db as having extra useless /
                    p = path + 1;
                else
                    p = path;
                snprintf(new_path, sizeof(new_path), "%s/%s", p, entry->d_name);
                // else
                //     snprintf(new_path, sizeof(new_path), "/%s", entry->d_name);
                // printf("%s\n", new_path);
                scanFolderAndAddToDatabase(new_path);
            }
            else if (entry->d_type == DT_REG) // check if it's a regular file
            {
                //(path, name, hash, extension, size, last_update)
                char query[BUFSIZ] = "insert or ignore into filehash values ('"; // this might be just a tiny bit faster since
                // the dbms it's supposed (if columns are declared) to search for those columns while if no columns are specified,
                // it defaults to insert and check on insert, being faster

                char full_path[4096] = {0};

                snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
                strcat(query, full_path);
                strcat(query, "','");

                long filesize = getFileSize(full_path);
                if (filesize == -1)
                {
                    printf("Won't insert: size error processing file metadata for: %s Reason: ", full_path);
                    perror("");
                    return;
                }

                char *extension = strrchr(entry->d_name, '.');
                if (extension != entry->d_name && extension != NULL) // if the dot exists and p is not null
                {
                    char name[256] = {0}; // insert name without extension
                    char *k = entry->d_name;
                    uint8_t i = 0;
                    while (k != extension) // quick and dirty copy
                    {
                        name[i] = entry->d_name[i];
                        i++;
                        k++;
                    }
                    strcat(query, name);
                }
                else
                    strcat(query, entry->d_name);

                strcat(query, "','");

                // if (filesize >= 0)
                // {
                u_int8_t *hash = md5FileWrapper(full_path); // if the hashing algorithm is bad, change the wrapper or just return the pointer
                char hash2[32 + 1] = {0};                   // of the hash from the corresponding library; ofc you need to change the storage size;
                binaryStringToHexaString(hash2, hash, 32);  // index. the function counts alone the 0, but does --n
                strcat(query, hash2);
                free(hash);
                //}
                // else
                //     strcat(query, "_NOSIZE_DO_NOT_ACCESS_0000000000");
                strcat(query, "','");

                // char *extension = strrchr(full_path, '.');//getFileExtension(full_path);
                // extension location found already
                if (extension != NULL)
                {
                    extension++; // extension points to the rightmost point in filename, including the point, I don't need it
                    strcat(query, extension);
                }
                else
                    strcat(query, "-");
                strcat(query, "','");

                char file_size[20] = {0}; // long can be 19 digits long + null

                snprintf(file_size, sizeof(file_size), "%ld", filesize);
                strcat(query, file_size);

                strcat(query, "', strftime('%s'));"); // sqlite fct for unix time

                // printf("%s\n", query);
                char *errMsg;
                if (sqlite3_exec(fileDatabase, query, NULL, 0, &errMsg) != SQLITE_OK)
                {
                    fprintf(stderr, "SQL error: %s\n", errMsg);
                    sqlite3_free(errMsg);
                }
            }
        }
    }
    closedir(dir);
}

static int callback_database_search(void *data, int argc, char **argv, char **azColName)
{
    struct fileEntry2 current_file;
    current_file.path = argv[0];
    current_file.name = argv[1];
    current_file.hash = argv[2];
    current_file.extension = argv[3];
    current_file.disk_size = argv[4];

    ((std::vector<fileEntry2> *)data)->push_back(current_file);

    return 0;
}
void searchInDatabase(fileRequest *request, std::vector<fileEntry2> *response_list)
{
    char query[BUFFER_SIZE] = "select * from filehash where ";
    { // query string preparation
        // if mask == 0b00000001 => name
        // if mask == 0b00000010 => pattern
        // if mask == 0b00000100 => extension
        // if mask == 0b10000000 => size less than
        // if mask == 0b01000000 => size equal          //if each 0b111 are set, then search by interval of [x-0.5, x+0.5]
        // if mask == 0b00100000 => size greater than
        uint8_t previousFlag = 0;

        if (request->request_mask & 0b00000001) // search by name
        {
            strcat(query, "name='");
            strcat(query, request->name_or_pattern.c_str());
            strcat(query, "'");

            previousFlag = 1;
        }
        if (request->request_mask & 0b00000010) // pattern
        {
            strcat(query, "name regexp '");
            strcat(query, request->name_or_pattern.c_str());
            strcat(query, "'");

            previousFlag = 1;
        }
        if (request->request_mask & 0b00000100) // extension
        {
            if (previousFlag == 1)
                strcat(query, " and ");

            strcat(query, "extension='");
            strcat(query, request->extension.c_str());
            strcat(query, "'");

            previousFlag = 1;
        }
        if ((request->request_mask & 0b10000000) ||
            (request->request_mask & 0b01000000) ||
            (request->request_mask & 0b00100000))
        {
            if (previousFlag == 1)
                strcat(query, " and ");

            if ((request->request_mask & 0b10000000) &&
                (request->request_mask & 0b01000000) &&
                (request->request_mask & 0b00100000))
            {
                strcat(query, "( size >= ");
                strcat(query, request->disk_size.c_str());
                strcat(query, "-500000 and size <= "); //-0.5 MB
                strcat(query, request->disk_size.c_str());
                strcat(query, "+500000 )");
            }
            else
            {
                strcat(query, "size");

                if (request->request_mask & 0b10000000)
                    strcat(query, "<=");
                if (request->request_mask & 0b01000000)
                    strcat(query, "=");
                if (request->request_mask & 0b00100000)
                    strcat(query, ">=");

                strcat(query, request->disk_size.c_str());
            }
        }
        strcat(query, ";");
    }
    char *errMsg;
    if (sqlite3_exec(fileDatabase, query, callback_database_search, (void *)(response_list), &errMsg) != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: %s\n", errMsg); // would never happen, the client has to make sure it sends valid query
        sqlite3_free(errMsg);
    }

    fprintf(log_fd, "\tThread with id: %lu interrogated the database with query:\n\t\t%s\n", pthread_self(), query);
}

void refreshDatabase()
{
    char query[64] = "delete from filehash where last_update<=strftime('%s')-"; // "creatively" and uselessly complicated:\
        // compose this string every time the value is changed, not when the routine refreshes the db, but this will do

    char buf[11] = {0}; // since uint is of max 9 digits
    snprintf(buf, sizeof(buf), "%u", routine_thread_long_check_interval);
    strcat(query, buf);
    strcat(query, ";");

    char *errMsg;
    if (sqlite3_exec(fileDatabase, query, NULL, 0, &errMsg) != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: %s\n", errMsg);
        sqlite3_free(errMsg);
    }
    log("[routine] Database refreshed.\n");
}
