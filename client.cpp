#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <list>
#include <iostream>
//#include <errno.h>

using namespace std;

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
}member;

typedef struct {
    char *name;
    char ip[20];
    int port;
}LEADER;

typedef list<member> MEMBER_LIST;
MEMBER_LIST member_list; // List of all members in chat

LEADER leader;
time_t server_check_time;

char *name; // User name
char *my_port; // User port
char *old_leader_name;

int sock_fd, sock_check;
struct sockaddr_in addr_server, sin;
struct hostent *server_host_name;
socklen_t sin_size;

time_t getRawTime(){
    /* Get the system current time */
    time_t rawtime;
    time(&rawtime);
    return rawtime;
}

void addUserToList(char *users) {
    /* Add current users to the list after receiving the user list */
    string user_list(users);
    int pos = user_list.find('\n');
    string l = user_list.substr(0, pos);
    user_list = user_list.substr(pos + 1, user_list.length());

    // Add leader
    int p = l.find(' ');
    leader.name = new char[l.substr(0, p).length()];
    strcpy(leader.name, l.substr(0, p).c_str());
    l = l.substr(p + 1, l.length());
    p = l.find(':');
    //strcpy(leader.ip, l.substr(0, p).c_str());
    char po[10];
    leader.port = atoi(l.substr(p + 1, l.length()).c_str());
    printf("%s %s:%d (Leader)\n", leader.name, leader.ip, leader.port);

    // Add all other users
    while(!user_list.empty()) {
        pos = user_list.find('\n');
        string m = user_list.substr(0, pos);
        user_list = user_list.substr(pos + 1, user_list.length());

        member new_member;
        p = m.find(' ');
        new_member.name = new char[p];
        strcpy(new_member.name, m.substr(0, p).c_str());
        m = m.substr(p + 1, m.length());
        p = m.find(':');
        strcpy(new_member.ip, m.substr(0, p).c_str());
        int q = m.find(',');
        new_member.port = atoi(m.substr(p + 1, q).c_str());
        m = m.substr(q + 1, m.length());
        p = m.find(',');
        strcpy(new_member.join_time, m.substr(0, p).c_str());
        new_member.num_messages = atoi(m.substr(p + 1, m.length()).c_str());
        printf("%s %s:%d\n", new_member.name, new_member.ip, new_member.port);
        member_list.push_back(new_member);
    }
}

void multicast(int sock_fd, message msg) {
    /* Multicast message to all clients in the list */
    int len;
    MEMBER_LIST::iterator i;
    for(i = member_list.begin(); i != member_list.end(); i++) {
        char *ip = (*i).ip;
        struct sockaddr_in addr_client;
        bzero(&addr_client, sizeof(struct sockaddr_in));
        addr_client.sin_family = AF_INET;
        addr_client.sin_port = htons((*i).port);
        addr_client.sin_addr.s_addr = inet_addr(ip);
        //printf("send to: %s %s:%d\n", (*i).name, ip, (*i).port);
        
        len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, sizeof(struct sockaddr));
        if(len < 0) {
            perror("Error in multicasting\n");
            exit(-1);
        }
    
        /*
        message m;
        struct timeval timeout;
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;
        if(strcmp(msg.type, "ELECTION") == 0) {
            if(setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
                printf("Cannot Set SO_RCVTIMEO for socket\n");
            if((len = recvfrom(sock_fd, &m, sizeof(m), 0, (struct sockaddr *)&addr_server, &sin_size)) < 0) {
                if(errno == EWOULDBLOCK || errno == EAGAIN)
                    printf("recvfrom timeout\n");
                else
                    printf("recvfrom error: %d\n", errno);
            }
            else {
                printf("Receive from %s: %s\n", m.name, m.type);
            }
        }
        */
    }   
}

void *checkNewLeader(void *) {
    /* Thread to check whether new leader has came up */
    message msg;
    sleep(2);
    if(strcmp(old_leader_name, leader.name) != 0) // New leader has already broadcasted himself
            pthread_exit(0);
    else {
        // New leader has no response, multicast ELECTION again
        printf("New leader crash!\n");
        strcpy(msg.type, "ELECTION");
        strcpy(msg.name, name);
        strcpy(msg.content, "NEW LEADER CRASH");
        multicast(sock_fd, msg);
    }
}

void *sendMessage(void *) {
    /* Thread to send message */
    int len;
    message msg;
    while(1) {
        strcpy(msg.type, "CHAT");
        strcpy(msg.name, name);
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
    message msg;
    char tempBuf[8192];

    while(1) {
        len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, &sin_size);
        if(len < 0) {
            perror("Error in receiving message\n");
            exit(-1);
        }

        /*** JOIN - Another client wants to join, send the information of leader to him ***/
        if(strcmp(msg.type, "JOIN") == 0) {
            strcpy(msg.type, "LEADER");
            sprintf(msg.content, "%s:%d", leader.ip, leader.port);
            len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
            if(len < 0) {
                perror("Error in sending message\n");
                exit(-1);
            }

            // Set leader's address back
            bzero(&addr_server, sizeof(struct sockaddr_in));
            addr_server.sin_family = AF_INET;
            addr_server.sin_port = htons(leader.port);
            addr_server.sin_addr.s_addr = inet_addr(leader.ip);
        }

        /*** LEADER - Receives leader's information from a current client ***/
        else if(strcmp(msg.type, "LEADER") == 0) {
            // Set leader's address
            string leader_addr(msg.content);
            int pos = leader_addr.find(':');
            strcpy(leader.ip, leader_addr.substr(0, pos).c_str());
            leader.port = atoi(leader_addr.substr(pos + 1, leader_addr.length()).c_str());

            bzero(&addr_server, sizeof(struct sockaddr_in));
            addr_server.sin_family = AF_INET;
            addr_server.sin_port = htons(leader.port);
            addr_server.sin_addr.s_addr = inet_addr(leader.ip);

            // Send JOIN message to leader
            strcpy(msg.type, "JOIN");
            strcpy(msg.name, name);
            strcpy(msg.content, my_port);
            len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
            if(len < 0) {
                perror("Error in sending message\n");
                exit(-1);
            }
        }

        /*** NEW USER - Receives information about a newly-joined user ***/
        else if(strcmp(msg.type, "NEW USER") == 0) {
            // Add the new user to the member list
            member new_member;
            new_member.name = new char[strlen(msg.name)];
            strcpy(new_member.name, msg.name);
            string new_user(msg.content);
            int pos = new_user.find(':');
            strcpy(new_member.ip, new_user.substr(0, pos).c_str());
            new_user = new_user.substr(pos + 1, new_user.length());
            pos = new_user.find(',');
            new_member.port = atoi(new_user.substr(0, pos).c_str());
            strcpy(new_member.join_time, new_user.substr(pos + 1, new_user.length()).c_str());
            new_member.num_messages = 0;
            member_list.push_back(new_member);

            printf("NOTICE - %s joined on %s:%d\n", new_member.name, new_member.ip, new_member.port);

            /*
            MEMBER_LIST::iterator i;
            for(i = member_list.begin(); i != member_list.end(); i++) {
                printf("member: %s %s:%d\n", (*i).name, (*i).ip, (*i).port);  
            }
            */
    /*************************
            if(member_list.size() == 3 && strcmp(name, "liu") == 0) {
                //printf("NOTICE - Current leader crashes, waiting for a new leader...\n");

                old_leader_name = new char[strlen(leader.name)];
                strcpy(old_leader_name, leader.name);
                strcpy(msg.type, "ELECTION");
                strcpy(msg.name, "liu");
                strcpy(msg.content, "");
                multicast(sock_fd, msg);

                // Create a new thread to check whether the new leader has been elected
                pthread_t checkNewLeaderThread;
                pthread_create(&checkNewLeaderThread, NULL, checkNewLeader, NULL);
                
            }*/
        }

        /*** USER LIST - Receives user list from leader after joining in the chat ***/
        else if(strcmp(msg.type, "USER LIST") == 0) {
            printf("Succeeded, current users:\n");
            addUserToList(msg.content);
        }

        /*** CHAT - Chat messages ***/
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
        }

        /*** ELECTION - Receives from a client, indicating that current leader has crashed ***/
        else if(strcmp(msg.type, "ELECTION") == 0) {
            if(strcmp(msg.content, "") == 0)
                printf("NOTICE - Current leader crashes, waiting for a new leader...\n");

            // Choose the client who has sent most messages to be the new leader
            int max = 0; // Maximum number of messages sent by current clients
            MEMBER_LIST::iterator i, new_leader = member_list.begin();
            for(i = member_list.begin(); i != member_list.end(); i++) {
                if(max < (*i).num_messages) {
                    max = (*i).num_messages;
                    new_leader = i;
                }   
            }

            // New leader also crashes
            if(strcmp(msg.content, "NEW LEADER CRASH") == 0) {
                printf("New leader crashes!\n");
                max = 0;
                member_list.erase(new_leader);
                new_leader = member_list.begin();
                for(i = member_list.begin(); i != member_list.end(); i++) {
                    if(max < (*i).num_messages) {
                        max = (*i).num_messages;
                        new_leader = i;
                    }  
                }
                //printf("new leader: %s\n", (*new_leader).name);
            }
            
            // This client is the new leader, broadcast himself
            if(strcmp((*new_leader).name, name) == 0) {
                strcpy(msg.type, "NEW LEADER");
                strcpy(msg.name, name);
                strcpy(msg.content, "");
                multicast(sock_fd, msg);
            }
        }

        /*** NEW LEADER - A new leader shows up ***/
        else if(strcmp(msg.type, "NEW LEADER") == 0) {
            server_check_time = getRawTime();
            printf("NOTICE - %s is the new leader from now on\n", msg.name);

            // Set the information of new leader
            strcpy(leader.name, msg.name);
            MEMBER_LIST::iterator i;
            for(i = member_list.begin(); i != member_list.end(); i++) {
                if(strcmp(leader.name, (*i).name) == 0) {
                    strcpy(leader.ip, (*i).ip);
                    leader.port = (*i).port;
                    member_list.erase(i); // Remove new leader from the member list
                    break;
                }   
            }

            // Set new leader's address
            bzero(&addr_server, sizeof(struct sockaddr_in));
            addr_server.sin_family = AF_INET;
            addr_server.sin_port = htons(leader.port);
            addr_server.sin_addr.s_addr = inet_addr(leader.ip);
        }

        /*** LEAVE - A client has left or crashed ***/
        else if(strcmp(msg.type, "LEAVE") == 0) {
            printf("NOTICE - %s left the chat or crashed\n", msg.name);
        }
        
    }
}

void openCheckedSocket(){
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

void *checkServer(void *) {
    /* Thread to check whether server is alive */
    message msg;
    double diffTime;
    time_t currentTime;
    while(1){
        currentTime = getRawTime();
        diffTime = difftime(currentTime, server_check_time);
        if(diffTime > 9){
            old_leader_name = new char[strlen(leader.name)];
            strcpy(old_leader_name, leader.name);
            strcpy(msg.type, "ELECTION");
            multicast(sock_fd, msg);

            // Create a new thread to check whether the new leader has been elected
            pthread_t checkNewLeaderThread;
            pthread_create(&checkNewLeaderThread, NULL, checkNewLeader, NULL);
        }
        sleep(3);
    }
}

void *responseCheck(void *) {
    int len;
    message msg;
    struct sockaddr_in addr_server_check;
    socklen_t sin_size = sizeof(struct sockaddr);
    
    while(1){
        len = recvfrom(sock_check, &msg, 256, 0, (struct sockaddr *)&addr_server_check, &sin_size);
        if(strcmp(msg.type, "CHECK") == 0) {
            server_check_time = getRawTime();
            strcpy(msg.type, "ALIVE");
            strcpy(msg.name, name);
            len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server_check, sin_size);
            if(len < 0) {
                perror("Error in sending message\n");
                exit(-1);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    int len;
    message msg;
    pthread_t send_thread, recv_thread;
    pthread_t tids_check, tids_response;

    if(argc != 3) { // Wrong number of parameters
        printf("Input Format Error! Should be [User] [Addr]:[Port]\n");
        exit(-1);
    }

    // Set leader's IP and port
    name = argv[1];
    string addr = argv[2];
    int pos = addr.find(':');
    strcpy(leader.ip, addr.substr(0, pos).c_str());
    leader.port = atoi(addr.substr(pos + 1, addr.length()).c_str());
    
    if((server_host_name = gethostbyname(leader.ip)) == 0) {
        perror("Error in resolving local host\n");
        exit(-1);
    }

    if((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("Error in opening socket\n");
        exit(-1);
    }

    // Set leader's address
    bzero(&addr_server, sizeof(struct sockaddr_in));
    addr_server.sin_family = AF_INET;
    addr_server.sin_port = htons(leader.port);
    addr_server.sin_addr.s_addr = inet_addr(leader.ip);
    //addr_server.sin_addr.s_addr = ((struct in_addr *)(server_host_name->h_addr))->s_addr;  

    sin_size = sizeof(struct sockaddr_in);

    // Get the local ip and port number
	char hostname[1024];
	hostname[1023] = '\0';
	gethostname(hostname,1023);
	struct hostent *localhost = gethostbyname(hostname);
    char *my_ip = inet_ntoa(*((struct in_addr *)localhost->h_addr));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = 0;
    bind(sock_fd, (struct sockaddr *)&sin, sizeof(struct sockaddr));
    getsockname(sock_fd, (struct sockaddr *)&sin, &sin_size);
    int local_port = ntohs(sin.sin_port);
    my_port = new char[10];
    sprintf(my_port, "%d", local_port);

    // Send the JOIN message to leader
    strcpy(msg.type, "JOIN");
    strcpy(msg.name, name);
    strcpy(msg.content, my_port);
    printf("%s joining a new chat on %s:%d, listening on %s:%d\n", name, leader.ip, leader.port, my_ip, local_port);
    len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
    if(len < 0) {
        perror("Error in sending message\n");
        exit(-1);
    }

    // Create two threads to send and receive messages
    pthread_create(&send_thread, NULL, sendMessage, NULL);
    pthread_create(&recv_thread, NULL, recvMessage, NULL);

    openCheckedSocket();
    server_check_time = getRawTime();
    pthread_create(&tids_check, NULL, responseCheck, NULL);
    pthread_create(&tids_response, NULL, checkServer, NULL);

    pthread_join(send_thread,0);
    pthread_join(recv_thread,0);

    pthread_join(tids_check,0);
    pthread_join(tids_response,0);

    close(sock_check);
    close(sock_fd);
    return 0;
}