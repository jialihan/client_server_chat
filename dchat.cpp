#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <list>
#include <iostream>
#include <pthread.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

using namespace std;

#define PORT 5823
#define PORT_CHECK 4683

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
	int check_port;
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

LEADER leader;
char *name; // Current user name
char *my_port; // User port
char *old_leader_name;
int check_port;
int server_port = -1;
int flag_client_check = 1;
int flag_client_response = 1;

time_t server_check_time, last_election_time;

int sock_fd; // Socket for sending and receiving messages
int sock_check; // Socket for checking members' status
struct sockaddr_in addr_server, sin;
struct sockaddr_in addr_server_check;
socklen_t sin_size;
void leaderFunc();
void firstCheck(struct sockaddr_in addr_client);
void openCheckedSocket();

pthread_t client_check, client_response;

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

void multicast(int sock_fd, message msg, int flag) {
    /* Multicast message to all clients in the list */
    MEMBER_LIST::iterator i;
    for(i = member_list.begin(); i != member_list.end(); i++) {
         char *ip = (*i).ip;
		struct sockaddr_in addr_client;
        bzero(&addr_client, sizeof(struct sockaddr_in));
        addr_client.sin_family = AF_INET;
        addr_client.sin_addr.s_addr = inet_addr((*i).ip);
        //printf("send to: %s %s:%d\n", (*i).name, (*i).ip, (*i).port);

       if(flag == 1){
			if((*i).check_port > 0){
				addr_client.sin_port = htons((*i).check_port);
			}else{
				continue;
			}
		}
		else{
			addr_client.sin_port = htons((*i).port);
		}
        addr_client.sin_addr.s_addr = inet_addr(ip);


        int len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, sizeof(struct sockaddr));
        if(len < 0) {
            perror("Error in multicasting\n");
            exit(-1);
        }
    }
}

void *checkNewLeader(void *) {
    /* Thread to check whether new leader has came up */
    message msg;
    sleep(2);
    if(strcmp(old_leader_name, leader.name) != 0) // New leader has already broadcasted himself
        pthread_exit(0);
    /*
    else {
        // New leader has no response, multicast ELECTION again
        printf("New leader crash!\n");
        strcpy(msg.type, "ELECTION");
        strcpy(msg.name, name);
        strcpy(msg.content, "NEW LEADER CRASH");
        multicast(sock_fd, msg, 0);
    }
    */
}

void *sendMessage(void *) {
    /* Thread to send message */
    int len;
    while(!feof(stdin)) {
        message msg;
        strcpy(msg.type, "CHAT");
        strcpy(msg.name, name);
        fgets(msg.content, 8192, stdin);
        len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
        if(len < 0) {
            perror("Error in sending message\n");
            exit(-1);
        }
    }
    exit(1);
}

void *sendCheck(void *) {
	/* Thread to send CHECK messages */
    while(flag_client_check){
        if(member_list.size() != 0){
			message msg;
			strcpy(msg.type, "CHECK");
			multicast(sock_check, msg, 1);
		}
        sleep(3);
    }
}

void *checkResponse(void *) {
    /* Thread at server to check client's liveness, if last check time is more than 15 sec ago, assume the client has left */
    while(flag_client_response){
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
                if(diffTime > 10){
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
        sleep(4);
    }
}

void openCheckedSocket() {
    /* Open another socket to check whether clients are alive */
    //struct sockaddr_in addr_server_check;
    
    bzero(&addr_server_check, sizeof(struct sockaddr_in));
    addr_server_check.sin_family = AF_INET;
    addr_server_check.sin_port = htons(PORT_CHECK);
    addr_server_check.sin_addr.s_addr = htonl(INADDR_ANY);
	check_port = ntohs(addr_server_check.sin_port);	

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
    double diffTime, diff;
    time_t currentTime;
    while(1) {
        currentTime = getRawTime();
        diffTime = difftime(currentTime, server_check_time);
        diff = difftime(currentTime, last_election_time);
        if(diffTime > 9 && diff > 10) {
            printf("diffTime: %f, diff: %f\n", diffTime, diff);
            old_leader_name = new char[strlen(leader.name)];
            strcpy(old_leader_name, leader.name);
            strcpy(msg.type, "ELECTION");
            strcpy(msg.content, "");
            multicast(sock_fd, msg, 0);

            // Create a new thread to check whether the new leader has been elected
            //pthread_t checkNewLeaderThread;
            //pthread_create(&checkNewLeaderThread, NULL, checkNewLeader, NULL);
        }
        sleep(3);
    }
}

void *responseCheck(void *) {
    /* Thread at client to receive CHECK messages and then reply ALIVE */
    int len;
    message msg;
    struct sockaddr_in fix_server_check;
    socklen_t sin_size = sizeof(struct sockaddr_in);
    char *my_check_port = new char[10];
	pthread_create(&client_response, NULL, checkServer, NULL);
	while(server_port < 0){
	}
	printf("response check: %d\n", server_port);
	bzero(&fix_server_check, sizeof(struct sockaddr_in));
	fix_server_check.sin_family = AF_INET;
	fix_server_check.sin_port = htons(server_port);
	fix_server_check.sin_addr.s_addr = inet_addr(leader.ip);
    while(1){
        len = recvfrom(sock_check, &msg, sizeof(msg), 0, (struct sockaddr *)&fix_server_check, &sin_size);
        if(strcmp(msg.type, "CHECK") == 0) {
			server_check_time = getRawTime();
            server_check_time = getRawTime();
            strcpy(msg.type, "ALIVE");
            strcpy(msg.name, name);
            len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&fix_server_check, sin_size);
            if(len < 0) {
                perror("Error in sending message\n");
                exit(-1);
            }
        }
    }
}

void updateCheckPort(char *name, char *port){
	MEMBER_LIST::iterator i;
	char* target =  name;

    for(i = member_list.begin(); i != member_list.end(); i++) {
        char* n = (*i).name;
        if(strcmp(target, n) == 0){
			(*i).check_port = atoi(port);
			break;
		}
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
		
		time(&currentTime);
        
        /*** ALIVE - Receives alive message from members ***/
        if(strcmp(msg.type, "ALIVE") == 0) {
			if(strlen(msg.content) != 0){
				updateCheckPort(msg.name, msg.content);
			}
			addLastCheckedTime(msg.name, currentTime);
        }
    }
}

void firstCheck(struct sockaddr_in addr_client){
	//send check message and port info
	int len;
	message msg;
	char *my_check_port = new char[10];
	strcpy(msg.type, "CHECK");
	sprintf(my_check_port, "%d", PORT_CHECK);
	strcpy(msg.content, my_check_port);//new port number for background check
	len = sendto(sock_check, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_client, sizeof(struct sockaddr));
}

void *recvMessage(void *) {
    /* Thread to receive message at client */
    int len;
    message msg;
    char tempBuf[8192];
    struct sockaddr_in addr;
	char *my_check_port = new char[10];
	struct sockaddr_in server_addr;

    while(1) {
        len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, &sin_size);
        if(len < 0) {
            perror("Error in receiving message\n");
            exit(-1);
        }

        /*** JOIN - Another client wants to join, send the information of leader to him ***/
        if(strcmp(msg.type, "JOIN") == 0) {
            if(strcmp(name, leader.name) == 0) {
                // Server receives JOIN
                /* msg: name - new user's name, content - new user's port number */
                char *user_ip = inet_ntoa(addr.sin_addr);
                int user_port = atoi(msg.content);
                char *current = getCurrentTime();
                sprintf(tempBuf, "NOTICE - %s joined on %s:%d", msg.name, user_ip, user_port);
                printf("%s\n", tempBuf);

				//send check message and port info
				firstCheck(addr);	
				
                // Multicast new user's join to all other users
                char info[100];
                sprintf(info, "%s:%d,%s", user_ip, user_port, current);
                strcpy(msg.type, "NEW USER");
                strcpy(msg.content, info);
                multicast(sock_fd, msg, 0);

                // Add the new user to the member list
                member new_member;
                new_member.check_port = -1;
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
                len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
                if(len < 0) {
                    perror("Error in sending message\n");
                    exit(-1);
                }              
            }

            else {
                // A client receives JOIN
                strcpy(msg.type, "LEADER");
                sprintf(msg.content, "%s:%d", leader.ip, leader.port);
                len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
                if(len < 0) {
                    perror("Error in sending message\n");
                    exit(-1);
                }
            }
        }
		
		/*** CHECK - give server its check port ***/
		else if(strcmp(msg.type, "CHECK") == 0){
				server_check_time = getRawTime();
				server_port = atoi(msg.content);
				strcpy(msg.type, "ALIVE");
				sprintf(my_check_port, "%d", check_port);
				strcpy(msg.content, my_check_port);
				strcpy(msg.name, name);
				len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
				if(len < 0) {
					perror("Error in sending message\n");
					exit(-1);
				}
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
        }

        /*** USER LIST - Receives user list from leader after joining in the chat ***/
        else if(strcmp(msg.type, "USER LIST") == 0) {
            printf("Succeeded, current users:\n");
            addUserToList(msg.content);
        }

        /*** CHAT - Chat messages ***/
        else if(strcmp(msg.type, "CHAT") == 0) {
            if(strcmp(name, leader.name) == 0) {
                // Leader
                sprintf(tempBuf, "%s:: %s", msg.name, msg.content);
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

            else { // Client
                sprintf(tempBuf, "%s:: %s", msg.name, msg.content);
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
        }

        /*** ELECTION - Receives from a client, indicating that current leader has crashed ***/
        else if(strcmp(msg.type, "ELECTION") == 0) {
            last_election_time = getRawTime();
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
            
            /** This client is the new leader, broadcast himself **/
            if(strcmp((*new_leader).name, name) == 0) {
                printf("NOTICE - %s is the new leader from now on\n", name);
                strcpy(leader.name, name);
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

                // Set initial check time
                for(i = member_list.begin(); i != member_list.end(); i++) {
                    if(strcmp((*i).name, name) != 0) {
                        (*i).last_check_time  = getRawTime();
                    }
                }

                // Multicast himself to all other clients
                strcpy(msg.type, "NEW LEADER");
                strcpy(msg.name, name);
                strcpy(msg.content, "");
                multicast(sock_fd, msg, 0);

                /*
                int numAck = 0;
                while(numAck != member_list.size()) {
                    len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, &sin_size);
                    if(len < 0) {
                        perror("Error in receiving message\n");
                        exit(-1);
                    }
                    if(strcmp(msg.type, "ACK") == 0) numAck++;
                }
                */

                // Kill these check threads in client function
                //pthread_kill(client_check, SIGKILL);
                //pthread_kill(client_response, SIGKILL);

               // pthread_cancel(client_check);
               // pthread_cancel(client_response);
				flag_client_response = 0;
				flag_client_check = 0;
                /*
                int rc = pthread_kill(client_check, 0);
                if(rc == ESRCH) printf("already quit\n");
                else if(rc == EINVAL) printf("signal invalid\n");
                else printf("thread alive\n");
                */

                // Create threads for new leader to check liveness of clients
                pthread_t tids_check, tids_response, tids_listen;
                pthread_create(&tids_check, NULL, sendCheck, NULL);
                pthread_create(&tids_response, NULL, checkResponse, NULL);
                pthread_create(&tids_listen, NULL, listenMessage, NULL);
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

            printf("leader: %s %s:%d\n", leader.name, leader.ip, leader.port);

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

        else if(strcmp(msg.type, "OK") == 0) {
            printf("%s says OK\n", msg.name);
        }
        
    }
}

void leaderFunc() {
    pthread_t tids_check, tids_response, tids_listen;

    pthread_create(&tids_check, NULL, sendCheck, NULL);
    pthread_create(&tids_response, NULL, checkResponse, NULL);
    pthread_create(&tids_listen, NULL, listenMessage, NULL);

    pthread_join(tids_check,0);
    pthread_join(tids_response,0);
    pthread_join(tids_listen,0);
}

void clientFunc() {
    server_check_time = getRawTime();

    pthread_create(&client_check, NULL, responseCheck, NULL);
    pthread_create(&client_response, NULL, checkServer, NULL);

    pthread_join(client_check,0);
    pthread_join(client_response,0);
}

int main(int argc, char* argv[]) {
    pthread_t send_thread, recv_thread;

    /* Leader. Start a chat */
    if(argc == 2) {
        name = argv[1];
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

        leaderFunc();
    }

    /* Client. Join a chat */
    else if(argc == 3) {
        // Set leader's IP and port
        name = argv[1];
        string addr = argv[2];
        int pos = addr.find(':');
        strcpy(leader.ip, addr.substr(0, pos).c_str());
        leader.port = atoi(addr.substr(pos + 1, addr.length()).c_str());

        if((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) == -1) {
            perror("Error in opening socket\n");
            exit(-1);
        }

        // Set leader's address
        bzero(&addr_server, sizeof(struct sockaddr_in));
        addr_server.sin_family = AF_INET;
        addr_server.sin_port = htons(leader.port);
        addr_server.sin_addr.s_addr = inet_addr(leader.ip); 

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
        message msg;
        strcpy(msg.type, "JOIN");
        strcpy(msg.name, name);
        strcpy(msg.content, my_port);
        printf("%s joining a new chat on %s:%d, listening on %s:%d\n", name, leader.ip, leader.port, my_ip, local_port);
		
        int len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
        if(len < 0) {
            perror("Error in sending message\n");
            exit(-1);
        }
		
		struct timeval timeout;
		timeout.tv_sec = 8;
		timeout.tv_usec = 0;
		//len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, &sin_size);
		if(setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
			printf("Cannot Set SO_RCVTIMEO for socket\n");
		if((len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, &sin_size)) < 0) {
			printf("1Sorry, no chat is active on %s, try again later.\n", argv[2]);
			printf("Bye.\n");
			close(sock_check);
			close(sock_fd);
			exit(1);
		}
	
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		//len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, &sin_size);
		if(setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
			printf("Cannot Set SO_RCVTIMEO for socket\n");

        // Create two threads to send and receive messages
        pthread_create(&send_thread, NULL, sendMessage, NULL);
        pthread_create(&recv_thread, NULL, recvMessage, NULL);

        // Open another socket and three other threads to check liveness
        openCheckedSocket();    

        clientFunc();
    }
    
    else { // Wrong number of parameters
        printf("Input Format Error!\n");
        exit(-1);
    }

    pthread_join(send_thread,0);
    pthread_join(recv_thread,0);

    close(sock_check);
    close(sock_fd);
    
    return 0;
}