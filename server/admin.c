#include <sys/types.h>   // socket
#include <sys/socket.h>  // socket
#include <string.h>
#include "selector.h"
#include "stdio.h"
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
#include "args.h"
#include "usersADT.h"
#include "logging/logger.h"

#define MAX_LINES 10
#define DGRAM_SIZE 1024 //10 lineas de las que soportamos
#define DATA_SIZE 640
#define PROTOCOL_SIZE 6
#define TOKEN_SIZE 128
#define COMMAND_SIZE 128
#define ARG_SIZE 128
#define ARG_COUNT 5
#define ADMIN_PROTOCOL "PROTOS"
#define HEADER_LINES 5


typedef enum{
    ADMIN_ADD_USER,
    ADMIN_CHANGE_PASS,
    ADMIN_GET_MAX_MAILS,
    ADMIN_SET_MAX_MAILS,
    ADMIN_GET_MAILDIR,
    ADMIN_SET_MAILDIR,
    ADMIN_STAT_HISTORIC_CONNECTIONS,
    ADMIN_STAT_CURRENT_CONNECTIONS,
    ADMIN_STAT_BYTES_TRANSFERRED,
    ADMIN_ERROR
}admin_command;

typedef enum {
    OK,
    INVALID_PROTOCOL,
    INVALID_VERSION,
    INVALID_TOKEN,
    INVALID_ID,
    INVALID_COMMAND,
    FORMAT_ERROR,
    GENERAL_ERROR
}admin_status;

char* parser_messages[] = {
        "OK\n",
        "Invalid protocol\n",
        "Invalid protocol version\n",
        "Invalid authentication token\n",
        "Invalid request id\n",
        "Invalid command\n",
        "Format error\n"
};

typedef struct request request;

struct request{
    char protocol[PROTOCOL_SIZE+1];
    long version;
    char token[TOKEN_SIZE+1];
    admin_command  cmd;
    size_t req_id;
    char command[COMMAND_SIZE+1];
    size_t arg_c;
    char args[ARG_COUNT][ARG_SIZE+1];
};

typedef struct command command;

struct command{
    char* name;
    void (*action)(int,request*,struct pop3args* args,struct sockaddr_storage* client_addr, unsigned int client_len);
};

void add_user_action(int socket, request* req, struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void change_pass_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void get_max_mails_action(int socket, request* req, struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void set_max_mails_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void get_maildir_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void set_maildir_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void stat_historic_connections_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void stat_current_connections_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len);
void stat_bytes_transferred_action(int socket, request* req, struct pop3args* args,struct sockaddr_storage* client_addr, unsigned int client_len);
const char * get_status_message(admin_status status);
admin_status parse_request(request* req, char * buff, size_t buff_len, struct pop3args* args);
static command commands[] = {
        {
            .name = "ADD_USER",
            .action = add_user_action
        },
        {
            .name = "CHANGE_PASS",
            .action = change_pass_action
        },
        {
            .name = "GET_MAX_MAILS",
            .action = get_max_mails_action
        },
        {
            .name = "SET_MAX_MAILS",
            .action = set_max_mails_action
        },
        {
            .name = "GET_MAILDIR",
            .action = get_maildir_action
        },
        {
            .name = "SET_MAILDIR",
            .action = set_maildir_action
        },
        {
            .name = "STAT_HISTORIC_CONNECTIONS",
            .action = stat_historic_connections_action
        },
        {
            .name = "STAT_CURRENT_CONNECTIONS",
            .action = stat_current_connections_action
        },
        {
            .name = "STAT_BYTES_TRANSFERRED",
            .action = stat_bytes_transferred_action
        }
};



static void send_response(int socket, admin_status status, char* data, request * req,struct sockaddr_storage* client_addr, size_t addr_len){
    char ans[DGRAM_SIZE];
    int len = snprintf(ans,DGRAM_SIZE,"PROTOS\n%ld\n%zu\n%c\n%s\n",req->version,req->req_id,status==OK?'+':'-',data);
    if(len<0){
        logf(LOG_ERROR, "[ADMIN] Cannot generate response to socket %d", socket);
        return;
    }
    if(sendto(socket,ans,len+1,0,(struct sockaddr*) client_addr,addr_len) < 0){
        logf(LOG_ERROR, "[ADMIN] Cannot send response to socket %d", socket);
    }
}

void admin_read(struct selector_key* key){
    //con el {0} me aseguro que todos los strings terminan en \0 (si no me paso escribiendo)
    static request req = {0}; //static para que no se reserve siempre
    char buff[DGRAM_SIZE+1]={0};
    struct sockaddr_storage client_addr;
    unsigned int len = sizeof (client_addr);

    long read_count = recvfrom(key->fd,buff,DGRAM_SIZE,0,(struct sockaddr*) &client_addr, &len);

    if(read_count<=0){ //si hay errores es -1, 0 no tiene sentido en UDP
        logf(LOG_ERROR,"[ADMIN] Cannot read from socket %d", key->fd);
        return;
    }

    struct pop3args* args = (struct pop3args*) key->data;
    admin_status status = parse_request(&req,buff,DGRAM_SIZE,args);
    if(status != OK){
        logf(LOG_WARNING, "[ADMIN] Invalid request from socket %d, got status '%s'", key->fd, get_status_message(status));
        send_response(key->fd,status,parser_messages[status],&req,&client_addr,len);
        return;
    }

    commands[req.cmd].action(key->fd,&req,args,&client_addr,len);
}



admin_command find_command(const char* cmd){
    for(admin_command command = ADMIN_ADD_USER; command <= ADMIN_STAT_BYTES_TRANSFERRED; command ++){
        if(strcmp(cmd,commands[command].name)==0){
            return command;
        }
    }
    return ADMIN_ERROR;
}

admin_status parse_request(request* request, char* buff, size_t buff_len, struct pop3args* args){
    //Vamos a ir buscando los \n para separar las cosas
    request->arg_c = 0;
    char * last;
    size_t len = strlen(buff);
    int i = 0;
    admin_status ret = OK;
    while (i<MAX_LINES){
        last = strchr(buff,'\n');
        if(last == NULL){
            if(ret!=OK){
                return ret;
            }
            return i+1>=HEADER_LINES?OK:FORMAT_ERROR;
        }
        *last = '\0';
        switch (i) {
            case 0:
                strncpy(request->protocol,buff,PROTOCOL_SIZE);
                if(strcasecmp(ADMIN_PROTOCOL,request->protocol)!=0){
                    ret = (ret == OK) ? INVALID_PROTOCOL:ret;
                }
                break;
            case 1:
                request->version = strtol(buff,NULL,10);
                if(errno == EINVAL || errno == ERANGE || request->version!=1){
                    ret = (ret == OK) ? INVALID_VERSION:ret;
                }
                break;
            case 2:
                strncpy(request->token,buff,TOKEN_SIZE);
                if(strncasecmp(request->token, args->access_token,TOKEN_SIZE) != 0){
                    ret = (ret == OK) ? INVALID_TOKEN:ret;
                }
                break;
            case 3:
                request->req_id = strtol(buff,NULL,10);
                if(errno == EINVAL || errno == ERANGE){
                    ret = (ret == OK) ? INVALID_ID:ret;
                }
                break;
            case 4:
                strncpy(request->command,buff,COMMAND_SIZE);
                if((request->cmd = find_command(request->command)) == ADMIN_ERROR){
                    ret = (ret == OK) ? INVALID_COMMAND:ret;
                }
                break;
            default:
                strncpy(request->args[request->arg_c],buff,ARG_SIZE);
                (request->arg_c)++;
        }
        i++;
        len -= (last - buff);
        if(len == 0){
            if(ret != OK){
                return ret;
            }
            return i+1>=HEADER_LINES?OK:FORMAT_ERROR;
        }
        buff = last + 1;
    }
    return OK;
}

void add_user_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(req->arg_c<2){
        logf(LOG_ERROR, "[ADMIN] Incorrect quantity of arguments, expected 2, got %ld", req->arg_c);
        send_response(socket,GENERAL_ERROR,"Cantidad de argumentos incorrecta",req,client_addr,client_len);
        return;
    }

    if(usersADT_add(args->users,req->args[0],req->args[1])!=0){
        log(LOG_ERROR, "[ADMIN] Couldn't add user");
        send_response(socket,GENERAL_ERROR,"No fue posible agregar el usuario",req,client_addr,client_len);
        return;
    }
    logf(LOG_INFO,"[ADMIN] Added new user '%s'", req->args[0]);
    if(snprintf(ans,DATA_SIZE,"%s\n","User added to server")<0){
        log(LOG_ERROR, "[ADMIN] Error generating add_user response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG, "[ADMIN] Sending add_user response");
    send_response(socket,OK,ans,req,client_addr,client_len);
}
void change_pass_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(req->arg_c<2){
        logf(LOG_ERROR, "[ADMIN] Incorrect quantity of arguments, expected 2, got %ld", req->arg_c);
        send_response(socket,GENERAL_ERROR,"Cantidad de argumentos incorrecta",req,client_addr,client_len);
        return;
    }

    if(usersADT_update_pass(args->users,req->args[0],req->args[1])!=true){
        log(LOG_ERROR, "[ADMIN] Couldn't change user password");
        send_response(socket,GENERAL_ERROR,"No fue posible cambiar la contraseña del usuario",req,client_addr,client_len);
        return;
    }
    logf(LOG_INFO,"[ADMIN] Changed password for user '%s'", req->args[0]);
    if(snprintf(ans,DATA_SIZE,"password changed for %s\n",req->args[0])<0){
        log(LOG_ERROR,"[ADMIN] Error generating change_pass response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG,"[ADMIN] Sending change_pass response");
    send_response(socket,OK,ans,req,client_addr,client_len);
}

void get_max_mails_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(snprintf(ans,DATA_SIZE,"%lu\n",args->max_mails)<0){
        log(LOG_ERROR,"[ADMIN] Error generating get_max_mails response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG,"[ADMIN] Sending get_max_mails response");
    send_response(socket,OK,ans,req,client_addr,client_len);
}
void set_max_mails_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(req->arg_c<1){
        logf(LOG_ERROR, "[ADMIN] Incorrect quantity of arguments, expected 1, got %ld", req->arg_c);
        send_response(socket,GENERAL_ERROR,"Cantidad de argumentos incorrecta",req,client_addr,client_len);
        return;
    }
    long max = strtol(req->args[0],NULL,10);
    if(((LONG_MIN == max || LONG_MAX == max) && ERANGE == errno ) || max<0){
        log(LOG_ERROR,"[ADMIN] Error: Invalid max argument");
        send_response(socket,GENERAL_ERROR,"Maximo invalido",req,client_addr,client_len);
        return;
    }
    args->max_mails = max;
    if(snprintf(ans,DATA_SIZE,"Maximum value for mails set to %ld\n",max)<0){
        log(LOG_ERROR,"[ADMIN] Error generating set_max_mails response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG,"[ADMIN] Sending set_max_mails response");
    send_response(socket,OK,ans,req,client_addr,client_len);
}
void get_maildir_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(snprintf(ans,DATA_SIZE,"%s\n",args->maildir_path)<0){
        log(LOG_ERROR,"[ADMIN] Error generating get_maildir response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG,"[ADMIN] Sending get_maildir response");
    send_response(socket,OK,ans,req,client_addr,client_len);
}
void set_maildir_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    char ans[DATA_SIZE];
    if(req->arg_c<1){
        logf(LOG_ERROR, "[ADMIN] Incorrect quantity of arguments, expected 1, got %ld", req->arg_c);
        send_response(socket,GENERAL_ERROR,"Cantidad de argumentos incorrecta",req,client_addr,client_len);
        return;
    }
    if(change_maildir(args,req->args[0])!=0){
        log(LOG_ERROR, "[ADMIN] Cannot change maildir");
        send_response(socket,GENERAL_ERROR,"Could not change maildir",req,client_addr,client_len);
        return;
    }
    logf(LOG_INFO,"[ADMIN] Changed maildir to '%s'", req->args[0]);
    if(snprintf(ans,DATA_SIZE,"Maildir set to %s\n",req->args[0])<0){
        log(LOG_ERROR,"[ADMIN] Error generating set_maildir response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    log(LOG_DEBUG,"[ADMIN] Sending set_maildir response");
    send_response(socket,OK,ans,req,client_addr,client_len);

}
void stat_historic_connections_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    extern unsigned long historic_connections;
    char ans[DATA_SIZE];
    if(snprintf(ans,DATA_SIZE,"%lu\n",historic_connections)<0){
        log(LOG_ERROR,"[ADMIN] Error generating historic_connections metric response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    logf(LOG_DEBUG,"[ADMIN] Sending historic_connections metric: %lu",historic_connections);
    send_response(socket,OK,ans,req,client_addr,client_len);

}
void stat_current_connections_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    extern unsigned long current_connections;
    char ans[DATA_SIZE];
    if(snprintf(ans,DATA_SIZE,"%lu\n",current_connections)<0){
        log(LOG_ERROR,"[ADMIN] Error generating current_connections metric response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    logf(LOG_DEBUG,"[ADMIN] Sending current_connections metric: %lu", current_connections);
    send_response(socket,OK,ans,req,client_addr,client_len);

}
void stat_bytes_transferred_action(int socket, request* req,struct pop3args* args, struct sockaddr_storage* client_addr, unsigned int client_len){
    extern unsigned long bytes_sent;
    char ans[DATA_SIZE];
    if(snprintf(ans,DATA_SIZE,"%lu\n",bytes_sent)<0){
        log(LOG_ERROR,"[ADMIN] Error generating bytes_transferred metric response");
        send_response(socket,GENERAL_ERROR,"Error al generar la respuesta",req,client_addr,client_len);
        return;
    }
    logf(LOG_DEBUG,"[ADMIN] Sending bytes_transferred metric: %lu", bytes_sent);
    send_response(socket,OK,ans,req,client_addr,client_len);

}

const char * get_status_message(admin_status status) {
    switch(status) {
        case OK:
            return "OK";
        case INVALID_PROTOCOL:
            return "Invalid protocol";
        case INVALID_VERSION:
            return "Invalid version";
        case INVALID_TOKEN:
            return "Invalid token";
        case INVALID_ID:
            return "Invalid id";
        case INVALID_COMMAND:
            return "Invalid command";
        case FORMAT_ERROR:
            return "Format error";
        default:
            return "General error";
    }
}
