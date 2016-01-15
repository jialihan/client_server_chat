#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <list>
#include <iostream>
#include <pthread.h>

using namespace std;

#define PORT 5820
#define PORT_CHECK 4690

typedef struct {
    char type[20];
    int number;
    char name[20];
    char content[8192];
}message;

typedef struct {
    char *name;
    char ip[20];
    int port;
    char join_time[30];
    int num_messages;
    time_t last_check_time;
}member;

typedef struct {
    char *name;
    char ip[20];
    int port;
}LEADER;

typedef list<member> MEMBER_LIST;
MEMBER_LIST member_list; // List of all members in chat

char *name;
LEADER leader;

int sock_fd; // Socket for sending and receiving messages
int sock_check; // Socket for checking members' status
struct sockaddr_in addr_server, addr_client;
socklen_t sin_size;

time_t getRawTime(){
    /* Get the system current time */
    time_t rawtime;
    time(&rawtime);
    return rawtime;
}

char *getCurrentTime() {
    /* Get the system current time to set user's join time */
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    //printf ("The current date/time is: %s\n", asctime(timeinfo));

    char *current_time = new char[30];
    strftime(current_time, 24, "%Y-%m-%d %H:%M:%S", timeinfo);
    return current_time;
}

void multicast(int sock_fd, message msg, int flag) {
    /* Multicast message to all clients in the list */
    MEMBER_LIST::iterator i;
    for(i = member_list.begin(); i != member_list.end(); i++) {
        //char *ip = (*i).ip;
        struct sockaddr_in addr_client;
        bzero(&addr_client, sizeof(struct sockaddr_in));
        addr_client.sin_family = AF_INET;
        addr_client.sin_addr.s_addr = inet_addr((*i).ip);

        if(flag == 1) // CHECK messages
            addr_client.sin_port = htons(PORT_CHECK);
        else // Normal messages
            addr_client.sin_port = htons((*i).port);

        int len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, sizeof(struct sockaddr));
        if(len < 0) {
            perror("Error in multicasting\n");
            exit(-1);
        }
    }
}

void *sendMessage(void *) {
    /* Thread to send message */
    int len;
    while(1) {
        message msg;
        strcpy(msg.type, "CHAT");
        strcpy(msg.name, leader.name);
        fgets(msg.content, 8192, stdin);
        len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
        if(len < 0) {
            perror("Error in sending message\n");
            exit(-1);
        }
    }
}

void *recvMessage(void *) {
    /* Thread to receive message */
    int len;
    char tempBuf[8192];
    while(1) {
        message msg;
        len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, &sin_size);
        if(len < 0) {
            perror("Error in receiving message\n");
            exit(1);
        }
            
        /*** JOIN - A new user joins ***/
        if(strcmp(msg.type, "JOIN") == 0) {
            /* msg: name - new user's name, content - new user's port number */
            char *user_ip = inet_ntoa(addr_client.sin_addr);
            int user_port = atoi(msg.content);
            char *current = getCurrentTime();
            sprintf(tempBuf, "NOTICE - %s joined on %s:%d", msg.name, user_ip, user_port);
            printf("%s\n", tempBuf);
            //printf("port %d\n",addr_client.sin_port);

            // Multicast new user's join to all other users
            char info[100];
            sprintf(info, "%s:%d,%s", user_ip, user_port, current);
            strcpy(msg.type, "NEW USER");
            strcpy(msg.content, info);
            multicast(sock_fd, msg, 0);

            // Add the new user to the member list
            member new_member;
            new_member.name = new char[strlen(msg.name)];
            strcpy(new_member.name, msg.name);
            strcpy(new_member.ip, user_ip);
            strcpy(new_member.join_time, current);
            new_member.port = user_port;
            new_member.num_messages = 0;
            new_member.last_check_time = getRawTime();
            member_list.push_back(new_member);

            // Send list of current users to the new user
            strcpy(msg.type, "USER LIST");
            string users("");
            sprintf(info, "%s %s:%d\n", leader.name, leader.ip, leader.port);
            users.append(info);
            MEMBER_LIST::iterator i;
            for(i = member_list.begin(); i != member_list.end(); i++) {
                sprintf(info, "%s %s:%d, %s, %d\n", (*i).name, (*i).ip, (*i).port, (*i).join_time, (*i).num_messages);
                users.append(info);
            }
            strcpy(msg.content, users.c_str());
            len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, sin_size);
            if(len < 0) {
                perror("Error in sending message\n");
                exit(-1);
            }              
        }

        /*** CHAT - Receives chat messages and then broadcasts to all clients ***/
        else if(strcmp(msg.type, "CHAT") == 0) {
            sprintf(tempBuf, "%s: %s", msg.name, msg.content);
            printf("%s", tempBuf);

            // Add 1 to number of messages of the sender
            MEMBER_LIST::iterator i, new_leader;
            for(i = member_list.begin(); i != member_list.end(); i++) {
                if(strcmp(msg.name, (*i).name) == 0) {
                    (*i).num_messages++;
                    break;
                }   
            }

            // Multicast the chat message
            multicast(sock_fd, msg, 0);
        }
    }
}

void openCheckedSocket() {
    /* Open another socket to check whether clients are alive */
    struct sockaddr_in addr_server_check;
    socklen_t sin_size;
        
    bzero(&addr_server_check, sizeof(struct sockaddr_in));
    addr_server_check.sin_family = AF_INET;
    addr_server_check.sin_port = htons(PORT_CHECK);
    addr_server_check.sin_addr.s_addr = htonl(INADDR_ANY);
    sin_size = sizeof(struct sockaddr_in);
    if((sock_check = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Error in opening socket\n");
        exit(-1);
    }
    if(bind(sock_check, (struct sockaddr *)&addr_server_check, sizeof(struct sockaddr)) == -1) {
        perror("Error in binding\n");
        exit(-1);
    }
}

void *checkResponse(void *) {
    /* Thread to check liveness, if last check time is more than 15 sec ago, assume the client has left */
    while(1){
        MEMBER_LIST::iterator i;
        time_t currentT;
        time(&currentT);
        double diffTime;
        message msg;
        char tempBuf[256];
        if(member_list.size() != 0){
            for(i = member_list.begin(); i != member_list.end(); i++) {
                time_t lastTime = (*i).last_check_time;
                diffTime = difftime(currentT, lastTime);
                if(diffTime > 15){
                    member_list.erase(i);
                    stpcpy(msg.type, "LEAVE");
                    stpcpy(msg.name, (*i).name);
                    multicast(sock_check, msg, 0);
                    sprintf(tempBuf, "NOTICE - %s left the chat or crashed", msg.name);
                    printf("%s\n", tempBuf);
                    break;
                }
            }
        }
        sleep(6);
    }
}

void addLastCheckedTime(char *cname, time_t time) {
    MEMBER_LIST::iterator i;
    for(i = member_list.begin(); i != member_list.end(); i++) {
        if(strcmp((*i).name, cname) == 0) {
            (*i).last_check_time  = time;
            break;
        }
    }
}

void *listenMessage(void *) {
    /* Thread to receive ALIVE messages */
    int len;
    time_t currentTime;
    struct sockaddr_in addr_client;
    socklen_t sin_size = sizeof(struct sockaddr);
    while(1){
        message msg;    
        len = recvfrom(sock_check, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, &sin_size);
        if(len < 0) {
            perror("Error in receiving message\n");
            exit(1);
        }
        
        /*** ALIVE - Receives alive message from members ***/
        if(strcmp(msg.type, "ALIVE") == 0) {
            time(&currentTime);
            addLastCheckedTime(msg.name, currentTime);
        }
    }
}

void *sendCheck(void *) {
    /* Thread to send CHECK messages */
    while(1){
        if(member_list.size() != 0){
            message msg;
            strcpy(msg.type, "CHECK");
            //strcpy(msg.content, "4699");//new port number for background check
            multicast(sock_check, msg, 1);
        }
        sleep(3);
    }
}

int main(int argc, char* argv[]) {
    pthread_t send_thread, recv_thread;
    pthread_t tids_check, tids_response, tids_listen;

    if(argc != 2) { // Wrong number of parameters
        printf("Input Format Error!\n");
        exit(-1);
    }
    
    //name = argv[1];
    leader.name = argv[1];

    if((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Error in opening socket\n");
        exit(1);
    }

    // Set it's own address
    bzero(&addr_server, sizeof(struct sockaddr_in));
    addr_server.sin_family = AF_INET;
    addr_server.sin_port = htons(PORT);
    addr_server.sin_addr.s_addr = htonl(INADDR_ANY);
    sin_size = sizeof(struct sockaddr_in);
  
    if(bind(sock_fd, (struct sockaddr *)&addr_server, sizeof(struct sockaddr)) == -1) {
        perror("Error in binding\n");
        exit(1);
    }
    
    // Get local IP
	char hostname[1024];
	hostname[1023] = '\0';
	gethostname(hostname,1023);
    struct hostent *localhost = gethostbyname(hostname);
    char *my_ip = inet_ntoa(*((struct in_addr *)localhost->h_addr));
    strcpy(leader.ip, my_ip);
    leader.port = PORT;

    printf("%s started a new chat, listening on %s:%d\n", leader.name, my_ip, PORT);
    printf("Succeeded, current users:\n%s %s:%d (Leader)\n", leader.name, my_ip, PORT);
    printf("Waiting for others to join...\n");
    
    // Create two threads to send and receive messages
    pthread_create(&send_thread, NULL, sendMessage, NULL);
    pthread_create(&recv_thread, NULL, recvMessage, NULL);

    // Open another socket and three other threads to check liveness
    openCheckedSocket();
    pthread_create(&tids_check, NULL, sendCheck, NULL);
    pthread_create(&tids_response, NULL, checkResponse, NULL);
    pthread_create(&tids_listen, NULL, listenMessage, NULL);

    pthread_join(send_thread,0);
    pthread_join(recv_thread,0);

    pthread_join(tids_check,0);
    pthread_join(tids_response,0);
    pthread_join(tids_listen,0);

    close(sock_check);
    close(sock_fd);
    return 0;
}