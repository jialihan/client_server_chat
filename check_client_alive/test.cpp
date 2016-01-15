#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

char *encrypt(char *mesg){
	int len, i;
	if(mesg != NULL){
		len = strlen(mesg);
		for(i = 0; i < len; i++){
			mesg[i] = mesg[i] + 5;
		}
	}
	return mesg;
}

char *decrypt(char *mesg){
	int len, i;
	if(mesg != NULL){
		len = strlen(mesg);
		for(i = 0; i < len; i++){
			mesg[i] = mesg[i] - 5;
		}
	}
	return mesg;
}


int main(){
	char msg[8192];
	char enmsg[8192];
	strcpy(msg, "Today is good!");
	strcpy(enmsg, encrypt(msg));
	printf("encrypt: %s\n", enmsg);
	printf("decrypt: %s\n", decrypt(enmsg));
}