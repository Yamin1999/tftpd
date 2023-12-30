#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#define RRQ     1
#define WRQ     2
#define DATA    3
#define ACK     4
#define ERROR   5

#define ERR_FILE_NOT_FOUND 1

#define ERR_FILE_ALREADY_EXITS 6

#define RECV_TIMEOUT 5
#define RECV_RETRIES 5
#define TRANSFER_MODE_SIZE  10
#define MAX_FILENAME_SIZE   50

#define MAX_SESSION 3
#define BLOCKSIZE 512

#define BUFSIZE 65000

#define MAX_READ_SIZE 51200

#define PORT 3234

#define READ_SESSION_CLOSE(fp, socket_fd_s, index) do { \
    if (fp) fclose(fp); \
    if (socket_fd_s >= 0) close(socket_fd_s); \
    session_flag[index] = 0; \
    session_count--; \
    return NULL; \
} while (0)


#define WRITE_SESSION_CLOSE(fp, socket_fd_s, index) do { \
    if (fp) fclose(fp); \
    if (socket_fd_s >= 0) close(socket_fd_s); \
    session_flag[index] = 0; \
    session_count--; \
    write_flag = 0; \
    return NULL; \
} while (0)



typedef struct request{
        uint16_t opcode;           
        uint8_t filename_mode[0];
    } __attribute__((packed)) request_pkt;     

typedef struct data{
        uint16_t opcode; 
        uint16_t block_number;
        uint8_t data[0];
    } __attribute__((packed)) data_pkt;

typedef struct ack{
        uint16_t opcode;             
        uint16_t block_number;
    } __attribute__((packed)) ack_pkt;

typedef struct error{
        uint16_t opcode;  
        uint16_t error_code;
        uint8_t error_string[0];
    } __attribute__((packed)) error_pkt;

typedef struct option_packet{
        uint16_t opcode;  
        uint8_t option[0];
    } __attribute__((packed)) optn_pkt;


typedef struct sesion_headr
{
    struct sockaddr_in client_adderess;
    uint8_t filename[MAX_FILENAME_SIZE];
    uint8_t transfer_mode[TRANSFER_MODE_SIZE];
    uint8_t option[50];
    uint32_t option_flag;
    uint32_t option_len;
    uint32_t block_size;
    uint32_t current_block;
    uint32_t session_id;
    
}__attribute__((packed)) session_t;


session_t sessions[MAX_SESSION];
uint32_t session_flag[MAX_SESSION] = {0};

uint32_t session_count = 0;
uint32_t write_flag = 0;



void filename_mode_option_fetch(uint8_t *buff , int session_id)
{
    request_pkt *req_packet = ( request_pkt *) buff;

    uint8_t *file_name = req_packet->filename_mode;
    uint8_t *file_mode = file_name + strlen(file_name) + 1; 

    sessions[session_id].block_size = BLOCKSIZE;

    uint8_t *optN = file_mode + strlen(file_mode) + 1;

    int32_t len = strlen(optN);

    uint8_t *valueN;
    memset(sessions[session_id].option, 0 , sizeof(sessions[session_id].option));
    uint8_t *position = ( uint8_t *) sessions[session_id].option;

    
    while(len > 0)
    {
        if(strcmp(optN, "blksize") == 0)
        {
            uint8_t *valueN = optN + strlen(optN) + 1;
            memcpy(position, optN, strlen(optN) + 1);
            position += strlen(optN) + 1;
            memcpy(position, valueN, strlen(valueN) + 1);
            sessions[session_id].option_flag = 1;
            sessions[session_id].block_size = atoi(valueN);
            sessions[session_id].option_len = strlen(optN) + strlen(valueN) + 2;
            break;
        }
        optN = optN + strlen(optN) + 1;
        len = strlen(optN);
    }
    memcpy(sessions[session_id].filename, file_name, strlen(file_name) + 1);

    memcpy(sessions[session_id].transfer_mode, file_mode, strlen(file_mode) + 1);

    printf("tftp server: Received request for file: %s with mode: %s block_size: %d\n", sessions[session_id].filename, 
    sessions[session_id].transfer_mode,sessions[session_id].block_size);

}

void send_error(int socket_f, uint16_t error_code, uint8_t *error_string, struct sockaddr_in *client_sock, int slen)
{
    char buff[BUFSIZE];

    error_pkt *err = (error_pkt *)buff;

    err->opcode= htons(ERROR);
    err->error_code = htons(error_code);
    memcpy(err->error_string, error_string, strlen(error_string) + 1);

    sendto(socket_f, buff, sizeof(*err) + strlen(error_string) + 1, 0,(struct sockaddr *) client_sock, slen);
} 


void tftp_send_data(int socket_f, uint16_t block_number, uint8_t *data, int dlen, struct sockaddr_in *client_sock, int slen)
{
    char buff[BUFSIZE];
    data_pkt *data_packet = (data_pkt *)buff;

    data_packet->opcode = htons(DATA);
    data_packet->block_number = htons(block_number);
    memcpy(data_packet->data, data, dlen);

    sendto(socket_f,(uint8_t *) buff , sizeof(*data_packet) + dlen, 0, (struct sockaddr *) client_sock, slen);
}

void tftp_send_ack(int socket_f, uint16_t block_number,struct sockaddr_in *client_sock, int slen)
{

    char buff[BUFSIZE];
    ack_pkt *ack_packet = (ack_pkt *)buff;

     ack_packet->opcode = htons(ACK);
     ack_packet->block_number = htons(block_number);

    sendto(socket_f,(uint8_t *) buff , sizeof(*ack_packet), 0,(struct sockaddr *) client_sock, slen);
}



void *RRQ_func(void *arg) {

    int index = *(int*)arg;
    sessions[index].session_id = index;

    printf("Start read Session : %d\n",index);

    int socket_fd_s, rv, block_read, block_size;
    struct sockaddr_in server_sock_addr,client_addrs, client_address;
    int slen = sizeof(client_addrs);
    int clen =  sizeof(sessions[index].client_adderess);
    client_address = sessions[index].client_adderess;
    uint16_t type;

    int block_number = 1;

    block_size = sessions[index].block_size;

    uint16_t block,error_code;
    uint8_t *error_message;


    socket_fd_s = socket(AF_INET, SOCK_DGRAM, 0);

    if(socket_fd_s < 0)
        printf("Socket creation Failed!\n");

    server_sock_addr.sin_family = AF_INET;
    server_sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_sock_addr.sin_port = 0;

    rv = bind(socket_fd_s, (struct sockaddr *) &server_sock_addr, sizeof(server_sock_addr)); 
    if(rv < 0)
        printf("Socket bind Failed!\n");

    uint8_t buff[BUFSIZE];
    uint8_t payload[BUFSIZE];

    FILE *fp;

    printf("Session %d : Request for read file : %s\n",index,sessions[index].filename);
    printf("Session %d : Transfer mode:  %s\n",index,sessions[index].transfer_mode);


    if (access(sessions[index].filename, F_OK) != -1) {
        printf("Session %d : File %s exists in the current directory.\n",index, sessions[index].filename);
    } else {
        printf("Session %d : File %s does not exist in the current directory.\n",index, sessions[index].filename);

        send_error(socket_fd_s, (uint16_t) ERR_FILE_NOT_FOUND, (uint8_t *) "File not exits", &client_address, clen);

        READ_SESSION_CLOSE(fp, socket_fd_s, index);
    }

    fp = fopen(sessions[index].filename,"r");

    if(fp == NULL)
    {
        printf("file opened failed!\n");
    }

    if(sessions[index].option_flag)
    {
        memset(&buff,0,sizeof(buff));

        optn_pkt *optn_packet = (optn_pkt *) buff;

        optn_packet->opcode = htons(6);

        memcpy(optn_packet->option, sessions[index].option, sessions[index].option_len);

        sendto(socket_fd_s,(uint8_t *) buff , 2 + sessions[index].option_len, 0, (struct sockaddr *) &client_address, clen);

        memset(&buff,0,sizeof(buff));

        recvfrom(socket_fd_s, (uint8_t *) buff, sizeof(buff), 0, (struct sockaddr *)&client_addrs, &slen);

        ack_pkt* ack_packet = (ack_pkt*) buff;

        printf("Received packet type: %d, block : %d\n", ntohs(ack_packet->opcode),ntohs(ack_packet->block_number));

    }
    int total_read = 0;
    

    char *position;
    char block_space[block_size + 100];

    while (1)
    {
        
        memset(&buff,0,sizeof(buff));

        // block_read = fread(fp, payload, BLOCKSIZE);

        if(!total_read)
        {
            memset(&payload,0,sizeof(payload));
            position = (char *) payload;
            total_read = fread(payload, 1, MAX_READ_SIZE, fp);
        }

        if(total_read >= block_size)
        {
            block_read = block_size;
            total_read = total_read - block_size;
        }
        else
        {
             block_read = total_read;
             total_read = 0;
        }

        printf("Session %d : Read data block: %d\n",index,block_read);

        memset(block_space,0,sizeof(block_space));
        memcpy(block_space, position, block_read);

        data_pkt *data_packet ;

        tftp_send_data(socket_fd_s, block_number, block_space, block_read, &client_address, clen);

        printf("Session %d : Send packet type: %d , data block : %d\n",index, DATA,block_number);

        position += block_read;

        memset(&buff,0,sizeof(buff));

        recvfrom(socket_fd_s, (uint8_t *) buff, sizeof(buff), 0, (struct sockaddr *)&client_addrs, &slen);

        type = ntohs(*(uint16_t *)buff);

        switch(type)
        {
            case ACK:

                ack_pkt *ack_packet = (ack_pkt *)buff;

                block = ntohs(ack_packet->block_number);

                if(block == block_number)
                {
                    printf("Session %d : ACK packet data block : %d\n\n",index,block);
                    block_number++;
                }
                else if(block == block_number - 1)
                {
                    tftp_send_data(socket_fd_s, block_number, payload, block_read, &client_address, clen);

                    printf("Session %d : Retransmiting packet type: %d , data block : %d\n",index, ntohs(data_packet->opcode),ntohs(data_packet->block_number));

                    continue;
                }
                else{
                    send_error(socket_fd_s, 0 , (uint8_t *) "Wrong block acknoledgement", &client_address, clen);

                    READ_SESSION_CLOSE(fp, socket_fd_s, index);
                }


                if(block_read < block_size)
                {
                    printf("Session %d : %s transfer complete.....\n",index,sessions[index].filename);

                    READ_SESSION_CLOSE(fp, socket_fd_s, index);
                }
                break;

            case ERROR:

                error_pkt *erro =  (error_pkt *)buff;
                error_code = erro->error_code;
                memcpy(error_message, erro->error_string, strlen(erro->error_string) + 1);

                printf("Session %d : Error Code : %d and Error Message: %s\n",index,error_code,error_message);

                READ_SESSION_CLOSE(fp, socket_fd_s, index);

            default:
                break;
        }
    }
}


void *WRQ_func(void *arg) {

    int index = *(int*)arg;
    sessions[index].session_id = index;

    printf("Start write Session : %d\n",index);

    int socket_fd_s, rv, block_write,n, block_size;
    struct sockaddr_in server_sock_addr,client_addrs,client_address;
    int slen = sizeof(client_addrs);
    int clen =  sizeof(sessions[index].client_adderess);
    client_address = sessions[index].client_adderess;
    ssize_t c;

    int block_number = 0;

    block_size = sessions[index].block_size;

    uint16_t block,error_code,opcode,type;
    uint8_t *error_message;
    ack_pkt* ack_packet;
    FILE *fp;

    socket_fd_s = socket(AF_INET, SOCK_DGRAM, 0);

    if(socket_fd_s < 0)
        printf("Socket creation Failed!\n");

    server_sock_addr.sin_family = AF_INET;
    server_sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_sock_addr.sin_port = 0;

    rv = bind(socket_fd_s, (struct sockaddr *) &server_sock_addr, sizeof(server_sock_addr)); 
    if(rv < 0)
        printf("Socket bind Failed!\n");

    printf("Session %d : Request for write file : %s\n",index,sessions[index].filename);
    printf("Session %d : Transfer mode: %s\n",index,sessions[index].transfer_mode);


    if (access(sessions[index].filename, F_OK) != -1) {
        printf("Session %d : File %s is already exists in the current directory.\n",index, sessions[index].filename);

        send_error(socket_fd_s, (uint16_t) ERR_FILE_ALREADY_EXITS, (uint8_t *) "File already exits", &client_address, clen);

        WRITE_SESSION_CLOSE(fp, socket_fd_s, index);

    } else {
        printf("Session %d : File %s does not exist in the current directory.\n",index, sessions[index].filename);
    }

    uint8_t buff[BUFSIZE];
    uint8_t payload[BUFSIZE];

    if(sessions[index].option_flag)
    {
        memset(&buff,0,sizeof(buff));

        optn_pkt *optn_packet = (optn_pkt *) buff;

        optn_packet->opcode = htons(6);

        memcpy(optn_packet->option, sessions[index].option, sessions[index].option_len);

        sendto(socket_fd_s,(uint8_t *) buff , 2 + sessions[index].option_len, 0, (struct sockaddr *) &client_address, clen);
        block_number++;
    }
    else
    {
        memset(&buff,0,sizeof(buff));

        tftp_send_ack(socket_fd_s, block_number, &client_address, clen);
        
        printf("Sent ACK for write file\n");
        block_number++;
    }

    

    fp = fopen(sessions[index].filename,"w");

    if(fp == NULL)
    {
        printf("file opened failed!\n");
    }
    
    while(1)
    {
        memset(&payload,0,sizeof(payload));
        memset(&buff,0,sizeof(buff));

        c = recvfrom(socket_fd_s, (uint8_t *) buff, sizeof(buff), 0, (struct sockaddr *)&client_addrs, &slen);

        type = ntohs(*(uint16_t *)buff);

        switch(type)
        {
            case DATA:
                data_pkt* data_packet = (data_pkt*)buff;

                block_write = c - 4;

                opcode = ntohs(data_packet->opcode);
                block =  ntohs(data_packet->block_number);
                memcpy(payload, data_packet->data, block_write);

                printf("Receive packet type: %d, data block: %d\n",opcode,block);

                n = fwrite(payload, 1, block_write, fp);

                printf("Write block: %d\n",n);

                memset(&buff,0,sizeof(buff));

                ack_packet = (ack_pkt *)buff;

                ack_packet->opcode = htons(ACK);

                if(block == block_number)
                {
                    ack_packet->block_number = htons(block_number);
                }
                else if(block == block_number - 1)
                {
                    ack_packet->block_number = htons(block_number - 1);
                }
                else
                {
                    send_error(socket_fd_s, 0 , (uint8_t *) "Wrong Data block send", &client_address, clen);
                    WRITE_SESSION_CLOSE(fp, socket_fd_s, index);
                }
        
                sendto(socket_fd_s, (uint8_t *) buff, sizeof(* ack_packet), 0, (struct sockaddr *)&client_address, clen);

                printf("Sent ACK for data block: %d\n\n",block_number);

                if(block_write < block_size)
                {
                    printf("File receiving complete...\n");
                    WRITE_SESSION_CLOSE(fp, socket_fd_s, index);
                }
                block_number++;
                break;

            case ERROR:

                error_pkt *erro =  (error_pkt *)buff;
                error_code = erro->error_code;
                memcpy(error_message, erro->error_string, strlen(erro->error_string) + 1);

                printf("Session %d : Error Code : %d and Error Message: %s\n",index,error_code,error_message);

                WRITE_SESSION_CLOSE(fp, socket_fd_s, index);

                break;

            default:
                break;
        }
    }

}


int main()
{
    int socket_fd, rv;
    struct sockaddr_in server_sock, client_sock;
    int len = sizeof(client_sock);
    uint8_t recv_req[BUFSIZE];
    request_pkt* request;

    uint32_t session_id, i;
    

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);

    if(socket_fd < 0)
        printf("Socket creation Failed!\n");

    server_sock.sin_family = AF_INET;
    server_sock.sin_addr.s_addr = htonl(INADDR_ANY);
    server_sock.sin_port = htons(PORT);

    rv = bind(socket_fd, (struct sockaddr *) &server_sock, sizeof(server_sock)); 
    if(rv < 0)
        printf("Socket bind Failed!\n");

    printf("tftp server: listening on %d\n", ntohs(server_sock.sin_port));

    while (1) {

        memset(&client_sock, 0, sizeof(client_sock));
        memset(&recv_req, 0, sizeof(recv_req));

        printf("tftp server: Waiting for socket request...\n");

        recvfrom(socket_fd, (uint8_t *) recv_req, sizeof(recv_req), 0, (struct sockaddr *)&client_sock, &len);



        uint16_t opcoode = ntohs(*(uint16_t *)recv_req);

        if (opcoode == RRQ) { 

            pthread_t thread_id;

            if(session_count < 3 && !write_flag)
            {
                for(i=0; i<MAX_SESSION; i++)
                {
                    if(!session_flag[i])
                    {
                        session_id = i;
                        session_flag[i] = 1;
                        break;
                    }
                }
            }
            else
            {
                printf("tftp server: Can't start the read session\n");
                continue;
            }
           
            memcpy(&sessions[session_id].client_adderess, &client_sock, sizeof(sessions[session_id].client_adderess));


            filename_mode_option_fetch(recv_req, session_id);

            
            int result = pthread_create(&thread_id, NULL, RRQ_func, (void*)&session_id);
            if (result != 0) {
                printf("tftp server: Thread creation failed!\n");
            }

            session_count++; // Increment after thread creation
        }

        else if(opcoode == WRQ)
        {
            pthread_t thread_id;

            if(session_count == 0)
            {
                session_id = 0;
                write_flag = 1;
                session_flag[session_id] = 1;
            }
            else
            {
                printf("tftp server: Can't start the write session\n");
                continue;
            }

            memcpy(&sessions[session_id].client_adderess, &client_sock, sizeof(sessions[session_id].client_adderess));

            filename_mode_option_fetch(recv_req, session_id);


            int result = pthread_create(&thread_id, NULL, WRQ_func, (void*)&session_id);
            if (result != 0) {
                printf("tftp server: Thread creation failed!\n");
            }
            session_count++;
        }
    }    
}

