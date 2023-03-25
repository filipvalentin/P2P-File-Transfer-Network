/*
Part2Part
Author: Filip Valentin
tried my best
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>

#include <dirent.h>
#include <regex.h>

#include <string.h>
#include <vector>
#include <string>

#include "md5/md5.c"

#define DOWNLOADS_FOLDER_PATH "./Downloads"
#define COMMAND_SIZE 128
#define BUFFER_SIZE 4096

#define IP_GET_SCRIPT "./get_ip.sh"
#define IP_FILE_NAME "./ip.txt"

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

    in_addr_t sender_ip;
    in_port_t sender_port;
};

uint8_t getSockDesc(struct sockData *);
void setSockDesc(char *);

uint8_t processSearchCommand(char *);
void sendDisconnectRequest();
uint8_t checkFreeDiskSpace(long);

void sendSearchRequest(uint8_t request_mask, char *file_name_or_pattern, char *extension, char *size);
uint8_t checkServerExitStatus();
// uint8_t fileExists(const char *name);

uint8_t check_ip(char *p);

void await_receiveResponses();
void receiveResponses();
void showResponses();

void clearResponsesList();

char *get_unique_filename(const char *filename);
void proceedGet(char *command);
void proceedGetfrom(char *command);

int16_t readExact(int socket, void *buffer, uint16_t x);
int16_t writeExact(int socket, const void *buffer, uint16_t x);

uint8_t retrieveFile(int get_sock_desc, fileEntry2 *entry);

int socket_desc = -1;

std::vector<responseData> received_responses;

// uint8_t invalid_search_queryFlag = 0;

in_addr_t server_currently_connected_ip;
in_port_t server_currently_connected_port;

in_addr_t getMyIP();
in_addr_t my_ip;

// char *downloads_path;

int main()
{
    setbuf(stdout, NULL);

    my_ip = getMyIP();

    mkdir(DOWNLOADS_FOLDER_PATH, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);

    while (1)
    {
        char *command = (char *)calloc(COMMAND_SIZE, sizeof(char));
        size_t len = 0;

        printf(">\t");
        getline(&command, &len, stdin);

        char *p;

        if (strcmp(command, "help\n") == 0)
        {
            printf("Usage:\n"
                   "  \e[1m connect \e[0m <IP> <PORT>\n"
                   "  \e[1m search \e[0m <OPTIONS>      search in network\n"
                   "      OPTIONS:\n"
                   "          -n <FULL NAME>\tSearch by name (without extension)\n"
                   "          -p <PATTERN>\t\tSearch by regex pattern\n"
                   "          -x <EXTENSION>\tSearch by extension\n"
                   "          -s <SIZE> [SIZE OPTIONS]\t Search by size in MB\n"
                   "                SIZE OPTIONS:\n"
                   "                  -l \tless or equal than\n"
                   "                  -e \tequal to (default if not provided)\n"
                   "                  -g \tgreater or equal than\n"
                   "          -1\tinterrogate only the server currently connected to\n"
                   "          -t\tset the TimeToLive of the request to max\n"
                   "      Note: - only one out of -n or -p options shall be used\n"
                   "            - can be provided with decimal point\n"
                   "            - if -s SIZE -l -e -g are provided, \n"
                   "                the size will be checked in the interval [SIZE-0.5, SIZE+0.5]\n"
                   "            - default TTL: 10;\tmax: 255 peers in direct path\n"
                   "            - for viewing every file exposed in the network you can use\n"
                   "                search -s 0 -g, requesting every file of size >= 0\n\n"
                   "  \e[1m get \e[0m <IDS>\tdownloads from the server currently connected to\n"
                   "  \e[1m getfrom \e[0m <LIST_NO> <IDS>\n"
                   "      - <LIST_NO> is the order number in front of the IP+PORT that provides the files\n"
                   "      - <IDS> can be a numerical ID (eg: 1), or a range of IDs (eg: 1-9)\n"
                   "            Note: the provided IDs must correspond to IDs returned by a previous search request\n\n"
                   "  \e[1m sr \e[0m or \e[1m showresponses \e[0m\tshow last received responses\n"
                   "  \e[1m exit \e[0m \t(autodisconnects)\n");
        }
        else if (strcmp(command, "exit\n") == 0)
        {
            sendDisconnectRequest();
            break;
        }
        else if (strcmp(command, "disconnect\n") == 0)
        {
            sendDisconnectRequest();
        }
        else if ((p = strstr(command, "connect")) != NULL)
        {

            if (p == command)
            {
                if (socket_desc <= 0)
                    setSockDesc(command);
                else
                    printf("The client is already connected!\n");
            }
            else
                printf("Invalid command\n");
        }
        else if ((p = strstr(command, "search")) != NULL)
        {
            if (socket_desc != -1)
            {
                if (p == command)
                {
                    if (checkServerExitStatus() == 0)
                    {
                        clearResponsesList();

                        if (processSearchCommand(command) == 1) // if (invalid_search_queryFlag == 0)
                        {
                            printf("Please wait while responses are gathered.\n\n"); // average response time is:
                            await_receiveResponses();                                // flag for additional info
                            if (received_responses.size() == 0)
                                printf("No files were found.\n");
                        }
                    }
                    else
                    {
                        printf("Server closed. Disconnected!\n");
                        close(socket_desc);
                        socket_desc = -1;
                    }
                }
                else
                    printf("Invalid command\n");
            }
            else
                printf("Error: client is disconnected\n");
        }
        else if ((p = strstr(command, "sr")) != NULL || (p = strstr(command, "showresponses")) != NULL)
        {
            if (p == command)
            {
                if (!received_responses.empty())
                    showResponses();
                else
                    printf("No responses received!\n");
            }
            else
                printf("Invalid command\n");
        }
        else if ((p = strstr(command, "getfrom")) != NULL)
        {
            // proceedReceiveFile(socket_desc, NULL);
            if (p == command)
            {
                if (socket_desc > 0)
                {
                    // if (checkServerExitStatus() == 0)

                    proceedGetfrom(command);
                }
                else
                    printf("Error: client is disconnected\n");
            }
            else
                printf("Invalid command\n");
        }
        else if ((p = strstr(command, "get")) != NULL)
        {
            // proceedReceiveFile(socket_desc, NULL);
            if (p == command)
            {
                if (socket_desc > 0)
                {

                    proceedGet(command);

                    // else printf("Server unexpectedly disconnected\n");
                }
                else
                    printf("Error: client is disconnected\n");
            }
            else
                printf("Invalid command\n");
        }

        else
            printf("Command not recognized\n");

        free(command);
    }

    if (socket_desc != -1)
        close(socket_desc);
}
// getwrapper

void proceedGet(char *command) // get from current server
{
    char *p = strtok(command, " "); // get 1
    p = strtok(NULL, " \n");        // 1

    if (received_responses.at(0).sender_ip == server_currently_connected_ip && received_responses.at(0).sender_port == server_currently_connected_port)
    {

        if (strchr(p, '-') != NULL)
        {
            uint lower, upper;
            if (sscanf(p, "%u-%u", &lower, &upper) <= 0)
            {
                if (sscanf(p, "%u - %u", &lower, &upper) <= 0)
                {
                    printf("Error parsing the given range\n");
                    return;
                }
            }
            lower--;
            upper--; // accomodate for index
            if (lower < upper && lower < received_responses.at(0).response_list->size() && upper <= received_responses.at(0).response_list->size())
            {

                while (lower <= upper)
                {
                    if (retrieveFile(socket_desc, &received_responses.at(0).response_list->at(lower)) == 1) // maybe choinces ang global choices for all files
                        printf("%s downloaded succesfully\n\n", strrchr(received_responses.at(0).response_list->at(lower).path.c_str(), '/') + 1);
                    else
                        printf("%s couldn't be downloaded\n\n", strrchr(received_responses.at(0).response_list->at(lower).path.c_str(), '/') + 1);
                    lower++;
                }
            }
        }
        else
        {
            uint id;
            if (sscanf(p, "%u", &id) <= 0)
            {
                printf("Error parsing the given range\n");
                return;
            }
            id--; // index
            if (id < received_responses.at(0).response_list->size())
                if (retrieveFile(socket_desc, &received_responses.at(0).response_list->at(id)) == 1)
                    printf("%s downloaded succesfully\n\n", strrchr(received_responses.at(0).response_list->at(id).path.c_str(), '/') + 1);
                else
                    printf("%s couldn't be downloaded\n\n", strrchr(received_responses.at(0).response_list->at(id).path.c_str(), '/') + 1);
        }
    }
    else
        printf("Last search request did not return any records from the server this client is currently connected to.\n");
}

void proceedGetfrom(char *command)
{

    char *p = strtok(command, " "); // get index id
    // p = strtok(NULL, " \n");        //

    // in_addr_t ip;
    // if (check_ip(p) != 0)
    // {
    //     printf("Error parsing the IP\n");
    //     return;
    // }
    // ip = inet_addr(p);
    // p = strtok(NULL, " \n");
    // int port = atoi(p);
    // if (port == 0)
    // {
    //     printf("Error parsing the port\n");
    //     return;
    // }

    p = strtok(NULL, " \n"); // index
    uint index;
    sscanf(p, "%u", &index);
    index--;
    if (index == 0 && received_responses.at(0).sender_ip == server_currently_connected_ip && received_responses.at(0).sender_port == server_currently_connected_port)
    {
        printf("Please use the 'get' command to download from the server you currently are connected to.\n");
        return;
    }

    p = strtok(NULL, " \n"); // this is for ids

    int sock_desc_file_download;
    int found_at = -1;
    { // this sets up the socket
        struct sockaddr_in server;
        memset(&server, 0, sizeof(server));
        server.sin_family = AF_INET;
        server.sin_addr.s_addr = received_responses.at(index).sender_ip; // ip;
        server.sin_port = received_responses.at(index).sender_port;      // htons(port);

        { // just use the already written code to make checks later

            for (uint i = 0; i < received_responses.size(); i++)
            {
                if (received_responses.at(i).sender_ip == server.sin_addr.s_addr && received_responses.at(i).sender_port == server.sin_port)
                {
                    found_at = i;
                    break;
                }
            }
            if (found_at == -1)
            {
                printf("Last search request did not return any records from the requested server.\n");
                return;
            }
        }

        if ((sock_desc_file_download = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        {
            perror("Error at socket(). Try again later! Reason: ");
            return;
        }

        if (connect(sock_desc_file_download, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
        {
            perror("Error at connect(). Try again later! Reason: ");
            return;
        }

        uint8_t type = 0;
        if (read(sock_desc_file_download, &type, sizeof(uint8_t)) <= 0)
        {
            perror("Error reading the type of server, reason: ");
            return;
        }
        if (type == 100)
        {
            type = 110;
            if (write(sock_desc_file_download, &type, sizeof(uint8_t)) <= 0)
            {
                // error
            }
            // printf("Successfully created a temporar\n");
        }
        else
        {
            type = 50;
            if (write(sock_desc_file_download, &type, sizeof(uint8_t)) <= 0)
            {
                // error
            }
            close(sock_desc_file_download);
            sock_desc_file_download = -1;
            printf("Cannot connect to this server [possible networking server]\nPlease try on another adress+port.\n");
        }
    } // socket is up

    if (writeExact(sock_desc_file_download, &my_ip, sizeof(in_addr_t)) <= 0)
        perror("Error sending the public IP");

    if (strchr(p, '-') != NULL)
    {
        uint lower, upper;
        if (sscanf(p, "%u-%u", &lower, &upper) <= 0)
        {
            if (sscanf(p, "%u - %u", &lower, &upper) <= 0)
            {
                printf("Error parsing the given range\n");
                return;
            }
        }
        lower--;
        upper--; // accomodate for index

        if (lower < upper && lower < received_responses.at(found_at).response_list->size() && upper <= received_responses.at(found_at).response_list->size())
        {
            while (lower != upper)
            {
                if (retrieveFile(sock_desc_file_download, &received_responses.at(found_at).response_list->at(lower)) == 1)
                    printf("%s downloaded succesfully\n\n", strrchr(received_responses.at(found_at).response_list->at(lower).path.c_str(), '/') + 1);
                else
                    printf("%s couldn't be downloaded\n\n", strrchr(received_responses.at(found_at).response_list->at(lower).path.c_str(), '/') + 1);
                lower++;
            }
        }
        else
            printf("Invalid index and/or ID!\n");
    }
    else
    {
        uint id = 0;
        if (sscanf(p, "%u", &id) <= 0)
        {
            printf("Error parsing the given range\n");
            return;
        }
        id--; // index
        if (id < received_responses.at(found_at).response_list->size())
        {
            if (retrieveFile(sock_desc_file_download, &received_responses.at(found_at).response_list->at(id)) == 1)
                printf("%s downloaded succesfully\n\n", strrchr(received_responses.at(found_at).response_list->at(id).path.c_str(), '/') + 1);
            else
                printf("%s couldn't be downloaded\n\n", strrchr(received_responses.at(found_at).response_list->at(id).path.c_str(), '/') + 1);
        }
        else
            printf("Invalid ID!\n");
    }

    uint8_t status;
    read(sock_desc_file_download, &status, sizeof(status));
    if (status == 0)
    { // if 0 means flag on server side is not set
        uint8_t request_no = 255;
        write(sock_desc_file_download, &request_no, sizeof(uint8_t));
    }
}

uint8_t retrieveFile(int get_sock_desc, fileEntry2 *entry)
{
    uint8_t serverStatusFlag = 1;
    read(get_sock_desc, &serverStatusFlag, sizeof(uint8_t));
    if (serverStatusFlag == 1)
    {
        printf("Server disconnected unexpectedly!\n");
        return 0;
    }

    // the server is
    long size = atol(entry->disk_size.c_str());
    if (checkFreeDiskSpace(size) == 0)
    {
        printf("Cannot download this file! Insufficient space available.\n"); /// needs work
        return 0;
    }

    // uint8_t choice;
    // const char *filename = strrchr(entry->path.c_str(), '/');
    //  if (fileExists(filename) == 1) // fileExists needs the slash
    //  {
    //      printf("The file %s already exists, choose what to do:\n\t1 replace\n\t2 rename and continue\n\t3 do nothing\n", filename + 1);
    //      while (1)
    //      {
    //          size_t len = 0;
    //          printf("\t> ");
    //          // fread(choice, 2, 1, stdin);
    //          choice = getchar();
    //          getchar();
    //          if (choice == '1' || choice == '2')
    //              break;
    //          else if (choice == '3')
    //          {
    //              // printf("Download aborted\n");
    //              return 0;
    //          }
    //      }
    //  }

    uint8_t request_no = 150;
    if (write(get_sock_desc, &request_no, sizeof(uint8_t)) <= 0)
        return 0;

    uint16_t length = strlen(entry->path.c_str());
    if (writeExact(get_sock_desc, &length, sizeof(uint16_t)) <= 0)
        return 0;
    if (writeExact(get_sock_desc, entry->path.c_str(), length * sizeof(char)) <= 0)
        return 0;
    if (writeExact(get_sock_desc, entry->hash.c_str(), 32 * sizeof(char)) <= 0)
        return 0;
    uint8_t status;
    if (read(get_sock_desc, &status, sizeof(uint8_t)) <= 0)
        return 0;
    if (status == 255)
    {
        printf("Error: the server cannot open/find the file\n");
        return 0;
    }

    char buffer[BUFFER_SIZE] = {0};
    strcpy(buffer, DOWNLOADS_FOLDER_PATH);
    strcat(buffer, strrchr(entry->path.c_str(), '/'));

    char *unique_filename = get_unique_filename(buffer); // get a unique name, to not prompt to user
    strcpy(buffer, unique_filename);
    free(unique_filename);

    // if (choice == '2')
    // {
    //     // logica pentru expandat
    //     strcat(buffer, "(-)"); // get the name
    // }
    FILE *file = fopen(buffer, "w");

    size_t bytes_read = 0;          //
    memset(buffer, 0, BUFFER_SIZE); // reuse the buffer
    while (1)
    {
        if (readExact(get_sock_desc, &bytes_read, sizeof(size_t)) <= 0)
            break;
        if (readExact(get_sock_desc, buffer, bytes_read * sizeof(char)) < 0)
            break; // more flags

        size_t bytes_written = fwrite(buffer, sizeof(char), bytes_read, file);
        if (bytes_written < bytes_read)
            printf("Couldn't write the file, possibly because of insufficient space\n");

        if (bytes_read == 0 || bytes_read != BUFFER_SIZE)
            break;

        memset(buffer, 0, BUFFER_SIZE);
    }

    // fflush(file);
    fclose(file);

    return 1;
}

// 0 means server's exitFlag is not set, 1 otherwise
uint8_t checkServerExitStatus()
{
    uint8_t response;
    if (read(socket_desc, &response, sizeof(uint8_t)) <= 0)
    {
        perror("Error at reading the status code\n");
        // fprintf(log_fd, "\tClient connected to worker thread: %lu disconnected unexpectedly.\n", pthread_self());
    }
    return response;
}

void sendDisconnectRequest()
{
    if (socket_desc >= 0)
    {
        uint8_t request_no = 255; // valoare aleatoare, who cares
        if (write(socket_desc, &request_no, sizeof(uint8_t)) < 0)
        {
            perror("Couldn't write to server");
        }

        close(socket_desc);
        socket_desc = -1;
        printf("Cliend succesfuly disconnected\n");
    }
    else
        printf("Already disconnected!\n");
}

uint8_t processSearchCommand(char *command)
{
    char *p = strtok(command, " "); //"search -n filename -x ext -s 1.5 -l"

    uint8_t sizeflag = 0;
    uint8_t sizeflagpermit = 0;
    uint8_t namepatternflag = 0;

    char file_name_or_pattern[256];
    char extension[20];
    char size[20];

    uint8_t request = 0b00000000;

    while (p = strtok(NULL, " \n"))
    {
        if (strcmp(p, "-n") == 0 && namepatternflag == 0)
        {
            request = request | 0b00000001;
            namepatternflag = 1;
            p = strtok(NULL, " \n");
            strcpy(file_name_or_pattern, p);
        }
        else if (strcmp(p, "-p") == 0 && namepatternflag == 0)
        {
            request = request | 0b00000010;
            namepatternflag = 1;
            p = strtok(NULL, " \n");
            strcpy(file_name_or_pattern, p);
        }
        else if (strcmp(p, "-x") == 0)
        {
            request = request | 0b00000100;
            p = strtok(NULL, " \n");
            strcpy(extension, p);
        }
        else if (strcmp(p, "-s") == 0 && sizeflagpermit == 0)
        {
            sizeflagpermit = 1;
            p = strtok(NULL, " \n");
            if (p == NULL)
                return 0;
            double size_mb = atof(p); // size is given in mb
            size_mb *= 1000000;
            snprintf(size, 20, "%ld", (long)size_mb);
        }
        else if (sizeflagpermit == 1 && strcmp(p, "-l") == 0)
        {
            request = request | 0b10000000;
            sizeflag = 1;
        }
        else if (sizeflagpermit == 1 && strcmp(p, "-e") == 0)
        {
            request = request | 0b01000000;
            sizeflag = 1;
        }
        else if (sizeflagpermit == 1 && strcmp(p, "-g") == 0)
        {
            request = request | 0b00100000;
            sizeflag = 1;
        }
        else if (strcmp(p, "-1") == 0) //-1 is to ask only the connected server, no cascade
            request = request | 0b00001000;

        else
        {
            printf("Error processing the command!\n");
            // invalid_search_queryFlag = 1;
            return 0;
        }
    }

    if (sizeflag == 0 && sizeflagpermit == 1) // if the user didn't supply any, default to -equal
        request = request | 0b01000000;

    sendSearchRequest(request, file_name_or_pattern, extension, size);

    return 1;
}

void sendSearchRequest(uint8_t request_mask, char *file_name_or_pattern, char *extension, char *size)
{
    // 1 send request number
    // 2 send the request mask
    // 3 send the necessary data
    // if mask == 0b00000001 => name
    // if mask == 0b00000010 => pattern
    // if mask == 0b00000100 => extension
    // if mask == 0b00001000 => search only current server
    // if mask == 0b10000000 => size less than
    // if mask == 0b01000000 => size equal
    // if mask == 0b00100000 => size greater  than

    // 1
    uint8_t request_no = 10;
    if (write(socket_desc, &request_no, sizeof(uint8_t)) <= 0)
    {
        perror("[client]Error writing the request number to server, request aborted!\n");
        return;
    }

    // 2
    if (write(socket_desc, &request_mask, sizeof(uint8_t)) <= 0)
    {
        perror("[client]Error writing the request mask to server, request aborted!\n");
        return;
    }

    // 3
    if (request_mask & 0b00000001 || request_mask & 0b00000010)
    {
        if (write(socket_desc, file_name_or_pattern, 255 * sizeof(char)) <= 0)
        {
            perror("[client]Error writing search query to server, request aborted!\n");
            return;
        }
    }

    if (request_mask & 0b00000100)
    {

        if (write(socket_desc, extension, 19 * sizeof(char)) <= 0)
        {
            perror("[client]Error writing search query to server, request aborted!\n");
            return;
        }
    }
    if (request_mask & 0b10000000 || request_mask & 0b01000000 || request_mask & 0b00100000)
    {
        if (write(socket_desc, size, 19 * sizeof(char)) <= 0)
        {
            perror("[client]Error writing search query to server, request aborted!\n");
            return;
        }
        // no need to send the mode, it is already send in the request mask
    }
}

uint8_t getSockDesc(struct sockData *arg)
{
    struct sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = arg->ip;
    server.sin_port = arg->port;

    server_currently_connected_ip = arg->ip;
    server_currently_connected_port = arg->port;

    // int socket_desc; // socket descriptor

    if ((socket_desc = socket(AF_INET, SOCK_STREAM, 0)) == -1)
    {
        perror("Error at socket(). Try again later! Reason");
        return 0;
    }

    if (connect(socket_desc, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
    {
        perror("Error at connect(). Try again later! Reason");
        return 0;
    }

    uint8_t type = 0;
    if (read(socket_desc, &type, sizeof(type)) <= 0)
    {
        perror("Error reading the type of server, reason");
        return 0;
    }
    if (type == 100)
    {
        type = 110;
        if (write(socket_desc, &type, sizeof(type)) <= 0)
        {
            // error
        }
        printf("Client connected sucessfully\n");
    }
    else
    {
        type = 50;
        if (write(socket_desc, &type, sizeof(type)) <= 0)
        {
            // error
        }
        close(socket_desc);
        // socket_desc = -1;
        printf("Cannot connect to this server [possible networking server]\nPlease try on another adress+port.\n");
        return 0;
    }

    if (write(socket_desc, &my_ip, sizeof(in_addr_t)) <= 0)
        printf("Error writing IP\n");

    return 1;
}

uint8_t check_ip(char *p)
{
    int ret;
    regex_t regex;

    ret = regcomp(&regex,
                  "^([0-9]|[1-9][0-9]|1([0-9][0-9])|2([0-4][0-9]|5[0-5]))."
                  "([0-9]|[1-9][0-9]|1([0-9][0-9])|2([0-4][0-9]|5[0-5]))."
                  "([0-9]|[1-9][0-9]|1([0-9][0-9])|2([0-4][0-9]|5[0-5]))."
                  "([0-9]|[1-9][0-9]|1([0-9][0-9])|2([0-4][0-9]|5[0-5]))$",
                  REG_EXTENDED);

    ret = regexec(&regex, p, 0, NULL, 0);
    regfree(&regex);
    return ret;
}

void setSockDesc(char *command)
{
    struct sockData *socketData = (struct sockData *)malloc(sizeof(struct sockData));

    char *p = strtok(command, " "); //"connect aaa.bbb.ccc.ddd 12345"
    p = strtok(NULL, " \n");        // aaa.bbb.ccc.ddd

    if (check_ip(p) == 0)
        socketData->ip = inet_addr(p); // inet_addr("127.0.0.1");
    else
    {
        printf("Error parsing the command, please retry.\n\tSyntax: connect <IP> <PORT>\n");
        return;
    }

    p = strtok(NULL, " \n");

    int port = atoi(p);
    if (port != 0)
        socketData->port = htons(port); // htons(5556);
    else
    {
        printf("Error parsing the command, please retry.\n\tSyntax: connect <IP> <PORT>\n");
        return;
    }

    if (getSockDesc(socketData) == 0)
        socket_desc = -1;

    free(socketData);
}

uint8_t checkFreeDiskSpace(long sizeToCheck)
{
    struct statvfs s;
    statvfs(DOWNLOADS_FOLDER_PATH, &s);
    if (sizeToCheck > s.f_bsize * s.f_bfree) // cannot transfer a file bigger than the available disk space
        return 0;
    else
        return 1;
}

char *get_unique_filename(const char *filename)
{
    // check if the file already exists
    if (access(filename, F_OK) == -1) // if the file does not exist, return a copy of the filename
        return strdup(filename);

    // if the file already exists, alter the filename until a file with that name doesn't exist
    char *buffer = (char *)malloc(strlen(filename) + 32);
    const char *extension = strrchr(filename, '.');
    if (extension == NULL)
        extension = "";

    uint count = 1;
    do
    {
        sprintf(buffer, "%.*s(%d)%s", (int)(extension - filename), filename, count, extension);
        count++;
    } while (access(buffer, F_OK) != -1);

    return buffer;
}

// uint8_t fileExists(const char *name)
// {
//     // char buffer[BUFFER_SIZE];
//     // strcpy(buffer, DOWNLOADS_FOLDER_PATH);
//     // //strcat(buffer, name);

//     uint8_t found = 0;
//     DIR *d;
//     struct dirent *dir;

//     d = opendir(DOWNLOADS_FOLDER_PATH);
//     if (d == NULL)
//         return 0;
//     while ((dir = readdir(d)) != NULL)
//     {
//         if (strcmp(dir->d_name, name + 1) == 0)
//         {
//             found = 1;
//             break;
//         }
//     }
//     closedir(d);
//     return found;
// }

void await_receiveResponses()
{
    while (1)
    {
        uint8_t request_no;
        if (read(socket_desc, &request_no, sizeof(request_no)) <= 0)
            break;

        if (request_no == 220)
        {
            printf("End of waiting window reached\n\n");
            break;
        }
        else if (request_no == 22)
            receiveResponses();
    }
}

time_t waitingtime;

void receiveResponses()
{
    waitingtime = time(NULL);
    struct responseData received;

    received.response_list = new std::vector<fileEntry2>; // don't forget to free

    if (read(socket_desc, &received.sender_ip, sizeof(in_addr_t)) <= 0)
        return;
    if (read(socket_desc, &received.sender_port, sizeof(in_port_t)) <= 0)
        return;

    { // just fancying up user experience with human-readable data
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in local; // server side data
        inet_ntop(AF_INET, &received.sender_ip, ip, INET_ADDRSTRLEN);

        uint16_t port = ntohs(received.sender_port);
        printf("%lu  %s:%d\t", received_responses.size() + 1, ip, port);
    }

    uint responses_count;
    if (read(socket_desc, &responses_count, sizeof(uint)) <= 0)
        return;
    printf("%u responses\n", responses_count);

    uint count = 1;

    for (uint16_t i = 0; i < responses_count; i++)
    {
        uint16_t length;
        if (read(socket_desc, &length, sizeof(uint16_t)) <= 0)
            return;
        char path[4096] = {0};
        if (read(socket_desc, path, length * sizeof(char)) <= 0)
            return;

        if (read(socket_desc, &length, sizeof(uint16_t)) <= 0)
            return;
        char name[256] = {0};
        if (read(socket_desc, name, length * sizeof(char)) <= 0)
            return;

        printf("  %u\t%s | ", count, name);

        char hash[33];
        hash[32] = 0;
        if (read(socket_desc, hash, 32 * sizeof(char)) <= 0)
            return;

        if (read(socket_desc, &length, sizeof(uint16_t)) <= 0)
            return;
        char extension[20] = {0};
        if (read(socket_desc, extension, length * sizeof(char)) <= 0)
            return;

        printf("%s | ", extension);

        if (read(socket_desc, &length, sizeof(uint16_t)) <= 0)
            return;
        char size[20] = {0};
        if (read(socket_desc, size, length * sizeof(char)) <= 0)
            return;

        double size_d = atof(size);
        size_d /= 1000000;
        printf("%.03lf MB\n", size_d);

        struct fileEntry2 entry;
        entry.path = path;
        entry.name = name;
        entry.hash = hash;
        entry.extension = extension;
        entry.disk_size = size;

        received.response_list->push_back(entry);

        count++;
    }

    received_responses.push_back(received);
    waitingtime = time(NULL) - waitingtime;
}

void showResponses()
{
    printf("\n");
    uint id_count = 1;
    for (auto &i : received_responses)
    {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in local; // server side data
        inet_ntop(AF_INET, &i.sender_ip, ip, INET_ADDRSTRLEN);

        uint16_t port = ntohs(i.sender_port);
        printf("%u  %s:%d\n", id_count, ip, port);

        uint count = 1;

        for (auto &j : *i.response_list)
        {
            double size = atof(j.disk_size.c_str());
            size /= 1000000;
            printf("  %u\t%s | %s | %.3lf MB\n", count, j.name.c_str(), j.extension.c_str(), size);
            count++;
        }
        id_count++;
    }
    printf("\n");
}

void clearResponsesList()
{
    for (auto i : received_responses)
        delete i.response_list;

    received_responses.clear();
}

in_addr_t getMyIP()
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

        fclose(ip_file);

        // remove(IP_GET_SCRIPT);
        remove(IP_FILE_NAME);
        return inet_addr(ip);
    }
    else
        perror("Couldn't open/create a temp file to get the IP");
    return 0;
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
