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

#define PORT 5822
#define PORT_CHECK 4688

int seq_num = 0;  //server
int local_msg_seq = 0; 
int local_seq = 0; //client
//int local_msg_seq = 0; 

typedef struct {
    char type[20];
    int number;
    char name[20];
    char content[8192];
	int msg_number;
	time_t send_time;
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

typedef list<message> QUEUE_LIST;
QUEUE_LIST multicast_list; // save all the mulicast message
QUEUE_LIST send_list;  // record the sent message
QUEUE_LIST queue_list;  // client hold back queue


LEADER leader;
char *name; // Current user name
char *my_port; // User port
char *old_leader_name;

time_t server_check_time, last_election_time;

int sock_fd; // Socket for sending and receiving messages
int sock_check; // Socket for checking members' status
struct sockaddr_in addr_server, sin;
struct sockaddr_in addr_server_check;
socklen_t sin_size;
message msg_temp;

void leaderFunc();
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
        struct sockaddr_in addr_client;
        bzero(&addr_client, sizeof(struct sockaddr_in));
        addr_client.sin_family = AF_INET;
        addr_client.sin_addr.s_addr = inet_addr((*i).ip);
        //printf("send to: %s %s:%d\n", (*i).name, (*i).ip, (*i).port);

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
void *checkSendList(void *){
	while (1){
		QUEUE_LIST::iterator i;
		time_t currentT;
		time(&currentT);
		double diffTime;
		char tempBuf[1024];
		if (!send_list.empty()){
			for (i = send_list.begin(); i != send_list.end(); i++) {
				time_t sendtime = (*i).send_time;
				diffTime = difftime(currentT, sendtime);
				if (diffTime > 5){
					send_list.erase(i);
					sprintf(tempBuf, "NOTICE - Send failure: NO.%d: %s", (*i).msg_number, (*i).content);
					printf("%s\n", tempBuf);
					break;
				}
			}
		}
		sleep(5);
	}

}

void openCheckedSocket() {
    /* Open another socket to check whether clients are alive */
    //struct sockaddr_in addr_server_check;
    
    bzero(&addr_server_check, sizeof(struct sockaddr_in));
    addr_server_check.sin_family = AF_INET;
    addr_server_check.sin_port = htons(PORT_CHECK);
    addr_server_check.sin_addr.s_addr = htonl(INADDR_ANY);
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
    //struct sockaddr_in addr_server_check;
    socklen_t sin_size = sizeof(struct sockaddr_in);
    
    while(1){
        len = recvfrom(sock_check, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server_check, &sin_size);
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

void *checkResponse(void *) {
    /* Thread at server to check client's liveness, if last check time is more than 15 sec ago, assume the client has left */
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
					msg.number = seq_num;
                    multicast(sock_check, msg, 0);
					seq_num++;
					multicast_list.push_back(msg);
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
bool DuplicatedError(message msg){
	/*duplicated error */
	QUEUE_LIST::iterator i;
	for (i = queue_list.begin(); i != queue_list.end(); i++) {
		if (msg.number == (*i).number){
			return true;
		}
		else if (msg.number < seq_num)
		{
			return true;
		}
		else {
			return false;
		}
	
}
void addMessageToQueue(message msg) {

	QUEUE_LIST new_queue_list;
	QUEUE_LIST::iterator i, j;
	/* Reorder: put the wrong number message in hold back queue*/

	if (queue_list.empty() || (msg.number > queue_list.back().number))
	{
		queue_list.push_back(msg);
	}
	else {
		for (i = queue_list.begin(); i != queue_list.end(); i++) {
			new_queue_list.push_back(*i);
			if (msg.number < (*i).number){
				// insert an new ordered message
				new_queue_list.pop_back();
				new_queue_list.push_back(msg);
				for (j = i; j != queue_list.end(); j++){
					new_queue_list.push_back(*j);
				}
				//queue_list = new_queue_list
				queue_list.clear();
				for (i = new_queue_list.begin(); i != new_queue_list.end(); i++) {
					queue_list.push_back(*i);
				}
				break;
			} // end if 
		} //end for
	} //end else	
}// end function
void *checkTimeOut(void *temp_local_seq)
{
	int len;
	int temp = *(int *)temp_local_seq;
	sleep(5); // timeout is 
	if (local_seq > temp)
	{
		pthread_exit(NULL);
	}
	len = sendto(sock_fd, &msg_temp, sizeof(msg_temp), 0, (struct sockaddr *)&addr_server, sin_size);
	if (len < 0) {
		perror("Error in sending message\n");
		exit(-1);
	}
	sleep(5);
	if (local_seq > temp)
		pthread_exit(NULL);
	len = sendto(sock_fd, &msg_temp, sizeof(msg_temp), 0, (struct sockaddr *)&addr_server, sin_size);
	if (len < 0) {
		perror("Error in sending message\n");
		exit(-1);
	}
	sleep(5);
	if (local_seq > temp)
		pthread_exit(NULL);
	else
		local_seq++;
}

void *sendMessage(void *) {
    /* Thread to send message */
   
		// server send
		int len;
		char request_msg[256];  // request content string
		while (!feof(stdin)) {
			message msg;
			strcpy(msg.type, "CHAT");
			strcpy(msg.name, name);
			time(&msg.send_time); // add current time
			fgets(msg.content, 8192, stdin);
			msg.msg_number = local_msg_seq;  // add chat msg number

			len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
			if (len < 0) {
				perror("Error in sending message\n");
				exit(-1);
			}

			if (strcmp(name, leader.name) == 0){

				send_list.push_back(msg); //if send successful, add it to send_list
				local_msg_seq++; // local_msg_seq add 1	
			}
			else{
				send_list.push_back(msg); //if send successful, add it to send_list
				local_msg_seq++; // local_msg_seq add 1	

				// request 3 times
				if (local_seq != queue_list.front().number && !queue_list.empty())
				{
					pthread_t timeout_thread;
					int temp_local_seq;
					temp_local_seq = local_seq; // record the last requested local squence#
					strcpy(msg.type, "REQUEST");
					sprintf(request_msg, "%d", temp_local_seq);  // only pass the number of next expected message#
					strcpy(msg.content, request_msg);
					msg_temp = msg;
					len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
					if (len < 0) {
						perror("Error in sending message\n");
						exit(-1);
					}
					pthread_create(&timeout_thread, NULL, checkTimeOut, (void *)temp_local_seq);
					pthread_join(timeout_thread, 0);
				}// end if 

			} // end else 
		}
      exit(1);
}

void *recvMessage(void *) {
	/* Thread to receive message at client */
	int len;
	message msg;
	char tempBuf[8192];
	struct sockaddr_in addr;

	while (1) {
	
		len = recvfrom(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, &sin_size);
		if (len < 0) {
			perror("Error in receiving message\n");
			exit(-1);
		}

		/*** JOIN - Another client wants to join, send the information of leader to him ***/
		if (strcmp(msg.type, "JOIN") == 0) {
			if (strcmp(name, leader.name) == 0) {
				// Server receives JOIN
				/* msg: name - new user's name, content - new user's port number */
				char *user_ip = inet_ntoa(addr.sin_addr);
				int user_port = atoi(msg.content);
				char *current = getCurrentTime();
				sprintf(tempBuf, "NOTICE - %s joined on %s:%d", msg.name, user_ip, user_port);
				printf("%s\n", tempBuf);

				// Multicast new user's join to all other users
				char info[100];
				sprintf(info, "%s:%d,%s", user_ip, user_port, current);
				strcpy(msg.type, "NEW USER");
				strcpy(msg.content, info);
				msg.number = seq_num; //notificationb message sequence #
				seq_num++;
				multicast(sock_fd, msg, 0);
				multicast_list.push_back(msg);// Add a multicast message to queue for history and record

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
				for (i = member_list.begin(); i != member_list.end(); i++) {
					sprintf(info, "%s %s:%d, %s, %d\n", (*i).name, (*i).ip, (*i).port, (*i).join_time, (*i).num_messages);
					users.append(info);
				}
				strcpy(msg.content, users.c_str());
				len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
				if (len < 0) {
					perror("Error in sending message\n");
					exit(-1);
				}
			}

			else {
				// A client receives JOIN
				strcpy(msg.type, "LEADER");
				sprintf(msg.content, "%s:%d", leader.ip, leader.port);
				len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
				if (len < 0) {
					perror("Error in sending message\n");
					exit(-1);
				}
			}
		}

		/*** LEADER - Receives leader's information from a current client ***/
		else if (strcmp(msg.type, "LEADER") == 0) {
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
			if (len < 0) {
				perror("Error in sending message\n");
				exit(-1);
			}
		}

		/*** NEW USER - Receives information about a newly-joined user ***/
		else if (strcmp(msg.type, "NEW USER") == 0) {
			if (msg.number != 0 && local_seq == 0)
			{
				local_seq = msg.number;
				local_seq++;
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
			else if (DuplicatedError(msg))
			{
				printf("Received Duplicated message, omit it!");
			}
			else if (msg.number != local_seq && local_seq != 0)
			{
				addMessageToQueue(msg);
			}
			else if (msg.number == local_seq){
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

				local_seq++;
			}
		}

		/*** USER LIST - Receives user list from leader after joining in the chat ***/
		else if (strcmp(msg.type, "USER LIST") == 0) {
			if (msg.number != 0 && local_seq == 0)
				local_seq = msg.number;
			//printf("server seq# is %d, local seq is %d\n", msg.number, local_seq);
			printf("Succeeded, current users:\n");
			addUserToList(msg.content);
			local_seq++;
		} // end USER LIST
		
		/*** LEADER revc Chat - Chat messages ***/
		else if (strcmp(msg.type, "CHAT") == 0 && strcmp(name, leader.name) == 0)
		{
			if (strcmp(msg.name, leader.name) == 0)
			{
				strcpy(msg.type, "OK");
				len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sin_size);
				if (len < 0) {
					perror("Error in sending message\n");
					exit(-1);
				}
				// then also multicast
				//// Multicast the chat message
				strcpy(msg.type, "CHAT");
				//printf("received chat once");
				msg.number = seq_num; //chat message sequence #
				seq_num++;
				sprintf(tempBuf, "NO.%d %s: %s\n", msg.number, msg.name, msg.content);
				printf("%s", tempBuf);
				//printf("MUlticast msg_number is : %d\n", msg.msg_number);// test message number
				multicast(sock_fd, msg, 0);
				multicast_list.push_back(msg);// Add a multicast message to queue for history and record	

			}
			else{
				// reply OK to the client:
				strcpy(msg.type, "OK");
				len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
				if (len < 0) {
					perror("Error in sending message\n");
					exit(-1);
				}
				// Add 1 to number of messages of the sender
				MEMBER_LIST::iterator i, new_leader;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(msg.name, (*i).name) == 0) {
						(*i).num_messages++;
						break;
					}
				}
				//// Multicast the chat message
				strcpy(msg.type, "CHAT");
				//printf("received chat once");
				msg.number = seq_num; //chat message sequence #
				seq_num++;
				sprintf(tempBuf, "NO.%d %s: %s\n", msg.number, msg.name, msg.content);
				printf("%s", tempBuf);
				//printf("MUlticast msg_number is : %d\n", msg.msg_number);// test message number
				multicast(sock_fd, msg, 0);
				multicast_list.push_back(msg);// Add a multicast message to queue for history and record			
			} // end else
		}

		/** * Client recv  Chat message ***/
		else if (strcmp(msg.type, "CHAT") == 0 && strcmp(name, leader.name) != 0)
		{
			if (msg.number != 0 && local_seq == 0)
			{
				local_seq = msg.number;
				sprintf(tempBuf, "N0.%d %s: %s\n", local_seq, msg.name, msg.content);
				printf("%s", tempBuf);
				local_seq++;
				// Add 1 to number of messages of the sender
				MEMBER_LIST::iterator i, new_leader;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(msg.name, (*i).name) == 0) {
						(*i).num_messages++;
						break;
					}
				}
			}
			else if (DuplicatedError(msg))
			{
				printf("Received Duplicated message, omit it!");
			}
			else if (msg.number != local_seq && local_seq != 0)
			{
				addMessageToQueue(msg);
			}
			else if (msg.number == local_seq){
				sprintf(tempBuf, "N0.%d %s: %s\n", msg.number, msg.name, msg.content);
				printf("%s", tempBuf);
				local_seq++;
				// Add 1 to number of messages of the sender
				MEMBER_LIST::iterator i, new_leader;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(msg.name, (*i).name) == 0) {
						(*i).num_messages++;
						break;
					} // end if 
				} // END for
			}
		}
		/*** RECEIVE ACK to delete the send_list message // for both client and server ***/
		else if (strcmp(msg.type, "OK") == 0) {
			QUEUE_LIST::iterator i;
			for (i = send_list.begin(); i != send_list.end(); i++) {
				if ((*i).msg_number == msg.msg_number)
				{
					i = send_list.erase(i);
					break;
				}//end if
			}
			/*if (send_list.empty())
				printf("ACK received!\n");
			else printf("ACK error\n");
			*/
		}// end OK(ACK)


		/*** REQUEST - Leader Receives request message and then reply to a certain client ***/
		else if (strcmp(msg.type, "REQUEST") == 0 && strcmp(name, leader.name) == 0) {
			int request_num = atoi(msg.content); //get the requested sequence number of message
			QUEUE_LIST::iterator i;
			for (i = multicast_list.begin(); i != multicast_list.end(); i++) {
				if (request_num == (*i).number)
				{
					msg = *i; // find the re-transmission msg
					len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr, sin_size);
					if (len < 0) {
						perror("Error in sending message\n");
						exit(-1);
					}
					break;
				}
			}
		}/*end request */


		/*** ELECTION - Receives from a client, indicating that current leader has crashed ***/
		else if (strcmp(msg.type, "ELECTION") == 0) {
			last_election_time = getRawTime();
			if (strcmp(msg.content, "") == 0)
				printf("NOTICE - Current leader crashes, waiting for a new leader...\n");

			// Choose the client who has sent most messages to be the new leader
			int max = 0; // Maximum number of messages sent by current clients
			MEMBER_LIST::iterator i, new_leader = member_list.begin();
			for (i = member_list.begin(); i != member_list.end(); i++) {
				if (max < (*i).num_messages) {
					max = (*i).num_messages;
					new_leader = i;
				}
			}

			// New leader also crashes
			if (strcmp(msg.content, "NEW LEADER CRASH") == 0) {
				printf("New leader crashes!\n");
				max = 0;
				member_list.erase(new_leader);
				new_leader = member_list.begin();
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (max < (*i).num_messages) {
						max = (*i).num_messages;
						new_leader = i;
					}
				}
				//printf("new leader: %s\n", (*new_leader).name);
			}

			/** This client is the new leader, broadcast himself **/
			if (strcmp((*new_leader).name, name) == 0) {
				printf("NOTICE - %s is the new leader from now on\n", name);
				strcpy(leader.name, name);
				MEMBER_LIST::iterator i;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(leader.name, (*i).name) == 0) {
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
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp((*i).name, name) != 0) {
						(*i).last_check_time = getRawTime();
					}
				}

				// Multicast himself to all other clients
				strcpy(msg.type, "NEW LEADER");
				strcpy(msg.name, name);
				strcpy(msg.content, "");
				multicast(sock_fd, msg, 0);

				pthread_cancel(client_check);
				pthread_cancel(client_response);


				// Create threads for new leader to check liveness of clients
				pthread_t tids_check, tids_response, tids_listen;
				pthread_create(&tids_check, NULL, sendCheck, NULL);
				pthread_create(&tids_response, NULL, checkResponse, NULL);
				pthread_create(&tids_listen, NULL, listenMessage, NULL);
			}
		}

		/*** NEW LEADER - A new leader shows up ***/
		else if (strcmp(msg.type, "NEW LEADER") == 0) {
			server_check_time = getRawTime();
			printf("NOTICE - %s is the new leader from now on\n", msg.name);

			// Set the information of new leader
			strcpy(leader.name, msg.name);
			MEMBER_LIST::iterator i;
			for (i = member_list.begin(); i != member_list.end(); i++) {
				if (strcmp(leader.name, (*i).name) == 0) {
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
			// send everyone's local_seq_num to new leader
			strcpy(msg.type, "ACK");
			msg.number = local_seq;
			strcpy(msg.content, "");
			len = sendto(sock_fd, &msg, sizeof(msg), 0, (struct sockaddr *)&addr_server, sizeof(struct sockaddr_in));
			if (len < 0) {
				perror("Error in sending message\n");
				exit(-1);
			}
			local_msg_seq = 0;
			send_list.clear(); // clear all client mssage number , no effect to seq_num;

		}

		/*** LEAVE - A client has left or crashed ***/
		else if (strcmp(msg.type, "LEAVE") == 0) {
			if (msg.number != 0 && local_seq == 0)
			{
				local_seq = msg.number;
				printf("NOTICE - %s left the chat or crashed\n", msg.name);
				local_seq++;
				// delete to number of messages of the sender
				MEMBER_LIST::iterator i;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(msg.name, (*i).name) == 0) {
						member_list.erase(i);
						break;
					}
				}// end for
			}
			else if (DuplicatedError(msg))
			{
				printf("Received Duplicated message, omit it!");
			}
			else if (msg.number != local_seq && local_seq != 0)
			{
				addMessageToQueue(msg);
			}
			else if (msg.number == local_seq){
				printf("NOTICE - %s left the chat or crashed\n", msg.name);
				local_seq++;
				// delete to number of messages of the sender
				MEMBER_LIST::iterator i;
				for (i = member_list.begin(); i != member_list.end(); i++) {
					if (strcmp(msg.name, (*i).name) == 0) {
						member_list.erase(i);
						break;
					}
				}// end for
			}
		}// END LEAVE

		/* NEW LEADER ACK only for the new leader to get the member's local largest SEQ_NUM*/
		else if (strcmp(msg.type, "ACK") == 0) {
			if (seq_num <= msg.number)
				seq_num = msg.number + 1;
			  // update seq_num for new group
		}// end ACK 

		// judge hold back queue
		if (strcmp(name, leader.name) != 0)
		{
			if (!queue_list.empty() && (local_seq == queue_list.front().number))
			{
				sprintf(tempBuf, "NO.%d %s: %s", local_seq, queue_list.front().name, queue_list.front().content);
				printf("%s", tempBuf);
				local_seq++;
			}
		}
	} //end while
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
    pthread_t send_thread, recv_thread, check_thread;

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
		pthread_create(&check_thread, NULL, checkSendList, NULL);

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

        // Create two threads to send and receive messages
        pthread_create(&send_thread, NULL, sendMessage, NULL);
        pthread_create(&recv_thread, NULL, recvMessage, NULL);
		pthread_create(&check_thread, NULL, checkSendList, NULL);
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
	pthread_join(check_thread, 0);

    close(sock_check);
    close(sock_fd);
    
    return 0;
}