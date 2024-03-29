#include <sys/types.h>   // socket
#include <sys/socket.h>  // socket
#include <netinet/in.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h>
#include "selector.h"
#include "pop3.h"
#include "buffer.h"
#include "stm.h"
#include "maidir_reader.h"
#include "./parser/parserADT.h"
#include "./parser/parser_definition/pop3_parser_definition.h"
#include "./parser/parser_definition/byte_stuffing_parser_definition.h"
#include "args.h"
#include "logging/logger.h"

#define MAX_CMD 5
#define MAX_ARG 249
#define WELCOME_MESSAGE "+OK POP3 server\r\n"
#define USER_INVALID_MESSAGE "-ERR INVALID USER\r\n"
#define USER_VALID_MESSAGE "+OK send PASS\r\n"
#define PASS_VALID_MESSAGE "+OK\r\n"
#define PASS_INVALID_MESSAGE "INVALID PASS\r\n"
#define OK_MESSSAGE "+OK\r\n"
#define USER_LOGGED "-ERR USER LOGGED\r\n"
#define ERROR_MESSSAGE "-ERR\r\n"
#define ERROR_COMMAND_MESSAGE "-ERR INVALID COMMAND\r\n"
#define ERROR_RETR_MESSAGE "INVALID MESSEGE NUMBER\r\n"
#define ERROR_RETR_ARG_MESSEGE "MISSING MESSEGE NUMBER\r\n"
#define ERROR_DELETED_MESSAGE "-ERR THIS MESSAGE IS DELETED\r\n"
#define ERROR_INDEX_MESSAGE "-ERR no such message\r\n"
#define QUIT_MESSAGE "+OK Logging out\r\n"
#define CAPA_MESSAGE "+OK Capability list follows\r\nUSER\r\nPIPELINING\r\n.\r\n"
#define LIST_MESSAGE "+OK scan listing follows\r\n"
#define NO_MAILDIR_MESSAGE "-ERR Could not open maildir\r\n"
#define UNKNOWN_ERROR_MESSAGE "-ERR Closing connection\r\n"
#define MAX_LIST_FIRST_LINE (3 + 1 + 20 + 1 + 20 + 3) //+OK %ld %ld \r\n
#define MAX_LIST_LINE (20+1+20+3) //%d %ld\r\n
#define MAX_RETR_FIRST_LINE (3+1+20+1+6+3) //+OK %ld octets\r\n
#define MAX_STAT_LINE (3+1+20+1+20+3) //+OK %zu %ld\r\n
/*
 * Estadísticas del servidor
 */
unsigned long historic_connections = 0;
unsigned long current_connections = 0;
unsigned long bytes_sent = 0;

/*
 * Estructura para guardar un comando de POP3
 */
struct command{
    //chequear argumentos para el comando
    char* name;
    bool (*check)(const char* arg);
    int (*action)(pop3* state);
};

typedef struct command command;

/*
 * Estructura para guardar datos asociados al estado de autorización de la conexion
 */
struct authorization{
    char * user;
    char * pass;
    bool user_is_present;
    char * path_to_user_data;
};


typedef enum{
    MULTILINE_STATE_FIRST_LINE,
    MULTILINE_STATE_MULTILINE,
    MULTILINE_STATE_END_LINE,
}multiline_state;

/*
 * Estructura para guardar datos asociados al estado de transaccion de la conexion
 */
struct transaction{
    int mail_index;
    bool has_arg;
    bool arg_processed;
    long arg;
    multiline_state multiline_state;
    bool file_opened;
    bool file_ended;
    int file_fd;
    int flag;
};

typedef enum{
    USER = 0,
    PASS,
    STAT,
    LIST,
    RETR,
    DELE,
    NOOP,
    RSET,
    QUIT,
    CAPA,
    ERROR_COMMAND //para escribir el mensaje de error y volver a recibir requests
} pop3_command;

typedef enum{
    AUTHORIZATION,
    TRANSACTION,
}protocol_state;

/*
 * Estructura para guardar toda la informacion asociada a una conexion al servidor
 */
struct pop3{
    int connection_fd;
    pop3_command command;
    const char* final_error_message;
    bool error_written;
    char  arg[MAX_ARG];
    char  cmd[MAX_CMD];
    struct state_machine stm;
    protocol_state pop3_protocol_state;
    uint8_t read_buff[BUFFER_SIZE];
    uint8_t write_buff[BUFFER_SIZE];
    uint8_t file_buff[BUFFER_SIZE];
    buffer info_file_buff;
    buffer info_read_buff;
    buffer info_write_buff;
    bool finished;
    parserADT pop3_parser;
    parserADT byte_stuffing_parser;
    email* emails;
    size_t emails_count;
    char* path_to_user_maildir;
    int references;
    struct pop3args* pop3_args;
    user_t * user_s;
    union{
        struct authorization authorization;
        struct transaction transaction;
    }state_data;
};

//Estados posibles del cliente
typedef enum{
    /*
    * HELLO: estado justo luego de establecer la conexión
    * Se usa para imprimir el mensaje de bienvenida
    */
    HELLO,
    /*
     * READING_REQUEST: estado donde esta leyendo informacion del socket
     * Se usa para indicar que se tiene que leer del socket
     */
    READING_REQUEST,
    /*
     * WRITING_RESPONSE: estando donde se esta escribiendo la respuesta en el buffer de salida y se escribe en el socket
     * Se usa para llevar la respuesta generada al cliente
     */
    WRITING_RESPONSE,
    /*
     * PROCESSING_RESPONSE: estado donde se esta leyendo de un archivo para hacer el RETR
     * Se usa para dejar los contenidos del archivo en un buffer intermedio, logrando no bloquearse en la lectura del archivo
     */
    PROCESSING_RESPONSE,
    /*
    * FINISHED: estado de finalización de la interaccion
    * Se utiliza para cerrar la conexion sin imprimir un mensaje de error
    */
    FINISHED,
    /*
    * ERROR: estado de error en la sesion
    * Se utiliza para cerrar la conexion imprimiendo un mensaje de error
    * previamente (por lo que debe suscribirse a escritura para que funcione)
    * (Considerado como max_state de la stm)
    */
    ERROR
} pop3_state;

//funciones utilizadas por la stm
unsigned int hello_write(struct selector_key* key);
unsigned int read_request(struct selector_key* key);
unsigned int write_response(struct selector_key* key);
unsigned int process_response(struct  selector_key* key);
void finish_connection(const unsigned state, struct selector_key *key);
unsigned int finish_error(struct  selector_key* key);
void process_open_file(const unsigned state, struct selector_key *key);
//funcion para reiniciar estructuras asociadas a un estado del protocolo
void reset_structures(pop3* state);
//funciones utilizadas para crear y destruir la estructura que mantiene el estado de una conexion (pop3)
void pop3_destroy(pop3* state);
pop3* pop3_create(void * data);
//funciones auxiliares para las estructuras de los comandos de pop3
bool have_argument(const char* arg);
bool might_argument(const char* arg);
bool not_argument(const char* arg);
pop3_command get_command(const char* command);
bool check_command_for_protocol_state(protocol_state pop3_protocol_state, pop3_command command);
//acciones asociadas a un comando de pop3
int user_action(pop3* state);
int pass_action(pop3* state);
int stat_action(pop3* state);
int list_action(pop3* state);
int retr_action(pop3* state);
int dele_action(pop3* state);
int noop_action(pop3* state);
int quit_action(pop3* state);
int default_action(pop3* state);
int rset_action(pop3* state);
int capa_action(pop3* state);

static struct command commands[]={
        {
            .name = "USER",
            .check = have_argument,
            .action = user_action
        },
        {
            .name = "PASS",
            .check = have_argument,
            .action = pass_action
        },
        {
            .name = "STAT",
            .check = not_argument,
            .action = stat_action
        },
        {
            .name = "LIST",
            .check = might_argument,
            .action = list_action
        },
        {
            .name = "RETR",
            .check = have_argument,
            .action = retr_action
        },
        {
            .name = "DELE",
            .check = have_argument,
            .action = dele_action
        },
        {
            .name = "NOOP",
            .check = not_argument,
            .action = noop_action
        },{
            .name = "RSET",
            .check = not_argument,
            .action = rset_action
        },
        {
            .name = "QUIT",
            .check = not_argument,
            .action = quit_action
        },
        {
            .name = "CAPA",
            .check = not_argument,
            .action = capa_action,
        },
        {
            .name = "Error command", //no deberia llegar aca para buscar al comando
            .check = might_argument,
            .action = default_action
        }
};

/*
 * Estados utilizados por la stm
 */
static const struct state_definition state_handlers[] ={
    {
        .state = HELLO,
        .on_write_ready = hello_write
    },
    {
        .state = READING_REQUEST,
        .on_read_ready = read_request,
    },
    {
        .state = WRITING_RESPONSE,
        .on_write_ready = write_response,
    },
    {
        .state = PROCESSING_RESPONSE,
        .on_arrival = process_open_file,
        .on_read_ready = process_response ,
    },
    {
        .state = FINISHED,
        .on_arrival = finish_connection,
    },
    {
        .state = ERROR,
        .on_write_ready = finish_error
    }

};

//fd_handler que van a usar todas las conexiones al servidor (que usen el socket pasivo de pop3)
static const struct fd_handler handler = {
    .handle_read = pop3_read,
    .handle_write = pop3_write,
    .handle_block = NULL,
    .handle_close = pop3_close //se llama tambien cuando cierra el servidor
};

/*
 * Funcion utilizada en el socket pasivo para aceptar una nueva conexion y agregarla al selector
 */
void pop3_passive_accept(struct selector_key* key) {
    pop3 *state = NULL;
    // Se crea la estructura del socket activo para la conexion entrante
    struct sockaddr_storage address;
    socklen_t address_len = sizeof(address);
    // Se acepta la conexion entrante, se cargan los datos en el socket activo y se devuelve el fd del socket activo
    const int client_fd = accept(key->fd, (struct sockaddr *) &address, &address_len);

    //Si tuvimos un error al crear el socket activo o no lo pudimos hacer no bloqueante, no iniciamos la conexion
    if (client_fd == -1){
        log(LOG_ERROR, "Error creating active socket for user");
        goto fail;
    }
    if(selector_fd_set_nio(client_fd) == -1) {
        logf(LOG_ERROR, "Error setting not block for user %d",client_fd);
        goto fail;
    }

    if((state = pop3_create(key->data))==NULL){
        log(LOG_ERROR, "Error on pop3 create")
        goto fail;
    }
    state->connection_fd = client_fd;
    logf(LOG_INFO, "Registering client with fd %d", client_fd);
    //registramos en el selector al nuevo socket, y nos interesamos en escribir para mandarle el mensaje de bienvenida
    if(selector_register(key->s,client_fd,&handler,OP_WRITE,state)!= SELECTOR_SUCCESS){
        log(LOG_ERROR, "Failed to register socket")
        goto fail;
    }
    log(LOG_DEBUG,"Updating current and historic connections metrics");
    current_connections++;
    historic_connections++;

    return;

fail:
    if(client_fd != -1){
        //cerramos el socket del cliente
        close(client_fd);
    }

    //destuyo y libero el pop3
    pop3_destroy(state);
}

/*
 * Funcion utilizada para crear la estructura pop3 que mantiene el estado de una conexion
 */
pop3* pop3_create(void * data){
    log(LOG_DEBUG, "Initializing pop3");
    extern const parser_definition pop3_parser_definition;
    extern const parser_definition byte_stuffing_parser_definition;
    pop3* ans = calloc(1,sizeof(pop3));
    if(ans == NULL || errno == ENOMEM){
        log(LOG_ERROR,"Error reserving memory for state");
        return NULL;
    }
    // Se inicializa la maquina de estados para el cliente
    ans->pop3_protocol_state = AUTHORIZATION;
    ans->stm.initial = HELLO;
    ans->stm.max_state = ERROR;
    ans->stm.states = state_handlers;
    stm_init(&ans->stm);
    ans->pop3_parser = parser_init(&pop3_parser_definition);
    ans->byte_stuffing_parser = parser_init(&byte_stuffing_parser_definition);
    ans->pop3_args = (struct pop3args*) data;

    // Se inicializan los buffers para el cliente (uno para leer y otro para escribir)
    buffer_init(&(ans->info_read_buff), BUFFER_SIZE ,ans->read_buff);
    buffer_init(&(ans->info_write_buff), BUFFER_SIZE ,ans->write_buff);
    buffer_init(&(ans->info_file_buff), BUFFER_SIZE, ans->file_buff);
    size_t max = 0;
    //Agregamos el mensaje de bienvenida
    uint8_t * ptr = buffer_write_ptr(&(ans->info_write_buff),&max);
    strncpy((char*)ptr,WELCOME_MESSAGE,max);
    buffer_write_adv(&(ans->info_write_buff),strlen(WELCOME_MESSAGE));

    log(LOG_DEBUG, "Finished initializing structure");
    return ans;
}

/*
 * Funcion para liberar memoria asociada a la estructura pop3
 */
void pop3_destroy(pop3* state){
    log(LOG_DEBUG, "Destroying pop3 state");
    if(state == NULL){
        return;
    }
    logf(LOG_INFO, "Closing connection with fd %d", state->connection_fd);
    parser_destroy(state->pop3_parser);
    parser_destroy(state->byte_stuffing_parser);
    free_emails(state->emails,state->emails_count);
    if(state->path_to_user_maildir != NULL){
        free(state->path_to_user_maildir);
    }
    free(state);
    log(LOG_DEBUG,"Reducing current connections metric");
    current_connections --; //se llama cuando se libera el estado de conexion (entonces termina la conexion)
}
/*
 * --------------------------------------------------------------------------------------
 * Funciones utilizadas por el selector ante los eventos de lectura, escritura y cierre
 * --------------------------------------------------------------------------------------
 */
/*
 * Funcion llamada por el selctor cuando puede leer de un fd
 */
void pop3_read(struct selector_key* key){
    // Se obtiene la maquina de estados del cliente asociado al key
    struct state_machine* stm = &(GET_POP3(key)->stm);
    // Se ejecuta la función de lectura para el estado actual de la maquina de estados
    stm_handler_read(stm,key);
}
/*
 * Funcion llamada por el selector cuando puede escribir en un fd
 */
void pop3_write(struct selector_key* key){
    // Se obtiene la maquina de estados del cliente asociado al key
    struct state_machine* stm = &(GET_POP3(key)->stm);
    // Se ejecuta la función de lectura para el estado actual de la maquina de estados
    stm_handler_write(stm,key);
}
/*
 * Funcion llamada por el selector cuando se usa selector_unregister_fd (es decir, cuando se saca al fd del selector)
 */
void pop3_close(struct selector_key* key){
    pop3 *data = GET_POP3(key);
    close(key->fd);
    //No puedo determinar el orden en el que se llama (puede ser primero con el archivo o con la conexion)
    //por lo que usamos references para que se libere al estado de la conexion en el ultimo que se llama
    if(data->references==0){
        //soy el ultimo con referencia al estado, lo libero
        pop3_destroy(data);
    }else{
        (data->references)--;
    }
}


/*
 * --------------------------------------------------------------------------------------
 * Funciones utilizadas por la stm para sus estados
 * --------------------------------------------------------------------------------------
 */

unsigned hello_write(struct selector_key* key){
    pop3* state = GET_POP3(key);
    size_t  max = 0;
    uint8_t* ptr = buffer_read_ptr(&(state->info_write_buff),&max);
    ssize_t sent_count = send(key->fd,ptr,max,MSG_NOSIGNAL);

    if(sent_count == -1){
        log(LOG_ERROR,"Error writing at socket");
        return FINISHED;
    }
    bytes_sent += sent_count;
    buffer_read_adv(&(state->info_write_buff),sent_count);
    //si no pude mandar el mensaje de bienvenida completo, vuelve a intentar
    if(buffer_can_read(&(state->info_write_buff))){
        return HELLO;
    }
    //Si ya no hay mas para escribir y termine con el mensaje de bienvenida
    if(selector_set_interest(key->s,key->fd,OP_READ) != SELECTOR_SUCCESS){
        log(LOG_ERROR,"Error changing socket interest to OP_READ in hello state");
        return FINISHED;
    }
    return READING_REQUEST;
}

unsigned int read_request(struct selector_key* key){
    pop3* state = GET_POP3(key);
    //Guardamos lo que leemos del socket en el buffer de entrada
    size_t max = 0;
    uint8_t* ptr = buffer_write_ptr(&(state->info_read_buff),&max);
    ssize_t read_count = recv(key->fd, ptr, max, 0);

    if(read_count<=0){
        log(LOG_ERROR,"Error reading at socket");
        return FINISHED;
    }
    //Avanzamos la escritura en el buffer
    buffer_write_adv(&(state->info_read_buff),read_count);
    //Obtenemos un puntero para lectura
    ptr = buffer_read_ptr(&(state->info_read_buff),&max);
    for(size_t i = 0; i<max; i++){
        parser_state parser = parser_feed(state->pop3_parser, ptr[i]);
        if(parser == PARSER_FINISHED || parser == PARSER_ERROR){
            //avanzamos solo hasta el fin del comando
            buffer_read_adv(&(state->info_read_buff),i+1);
            get_pop3_cmd(state->pop3_parser,state->cmd,MAX_CMD);
            pop3_command command = get_command(state->cmd);
            logf(LOG_DEBUG,"Reading request for cmd: '%s'", command>=0 ? commands[command].name : "invalid command");
            state->command = command;
            get_pop3_arg(state->pop3_parser,state->arg,MAX_ARG);
            if(parser == PARSER_ERROR || command == ERROR_COMMAND){
                log(LOG_ERROR, "Unknown command");
                state->command = ERROR_COMMAND;
            }
            if(!commands[command].check(state->arg)){
                log(LOG_ERROR, "Bad arguments");
                state->command = ERROR_COMMAND;
            }
            if(!check_command_for_protocol_state(state->pop3_protocol_state, command)){
                logf(LOG_ERROR,"Command '%s' not allowed in this state",commands[command].name);
                state->command = ERROR_COMMAND;
            }
            parser_reset(state->pop3_parser);
            //Vamos a procesar la respuesta
            if(selector_set_interest(key->s,key->fd,OP_WRITE) != SELECTOR_SUCCESS){
                log(LOG_ERROR, "Error setting interest to OP_WRITE after reading request");
                return FINISHED;
            }
            return WRITING_RESPONSE; //vamos a escribir la respuesta
        }
    }
    //Avanzamos en el buffer, leimos lo que tenia
    buffer_read_adv(&(state->info_read_buff), (ssize_t) max);
    return READING_REQUEST; //vamos a seguir leyendo el request
}
unsigned int write_response(struct selector_key* key){
    pop3* state = GET_POP3(key);
    //ejecutamos la funcion para generar la respuesta, que va a setear a state->finished como corresponda
    command current_command = commands[state->command];
    if(!state->finished) {
        unsigned int ret_state = current_command.action(state); //ejecutamos la accion
        //Si tengo que irme de este estado (para leer del archivo) me voy
        if(ret_state == ERROR){ //me mantengo en escritura
            return ERROR;
        }
        if(ret_state!=WRITING_RESPONSE){
            //Dejo de suscribirme en donde estoy, tengo que ir a otro lado
            if(selector_set_interest(key->s,key->fd,OP_NOOP) != SELECTOR_SUCCESS){
                log(LOG_ERROR, "Error setting interest");
                return FINISHED;
            }
            return ret_state;
        }
    }
    //Escribimos lo que tenemos en el buffer de salida al socket
    size_t  max = 0;
    uint8_t* ptr = buffer_read_ptr(&(state->info_write_buff),&max);
    ssize_t sent_count = send(key->fd,ptr,max,MSG_NOSIGNAL);
    bytes_sent += sent_count;

    if(sent_count == -1){
        log(LOG_ERROR, "Error writing in socket");
        return FINISHED;
    }
    buffer_read_adv(&(state->info_write_buff),sent_count);
    //Si ya no hay mas para escribir y el comando termino de generar la respuesta
    if(!buffer_can_read(&(state->info_write_buff)) && state->finished){
        state->finished = false;
        //Terminamos de mandar la respuesta para el comando, vemos si nos queda otro
        size_t  max = 0;
        uint8_t* ptr = buffer_read_ptr(&(state->info_read_buff),&max);
        for(size_t i = 0; i<max; i++){
            parser_state parser = parser_feed(state->pop3_parser, ptr[i]);
            if(parser == PARSER_FINISHED || parser == PARSER_ERROR){
                //avanzamos solo hasta el fin del comando
                buffer_read_adv(&(state->info_read_buff),i+1);
                get_pop3_cmd(state->pop3_parser,state->cmd,MAX_CMD);
                pop3_command command = get_command(state->cmd);
                state->command = command;
                get_pop3_arg(state->pop3_parser,state->arg,MAX_ARG);
                if(parser == PARSER_ERROR || command == ERROR_COMMAND || !commands[command].check(state->arg)){
                    state->command = ERROR_COMMAND;
                    log(LOG_ERROR, "Unknown command");
                }
                if(!check_command_for_protocol_state(state->pop3_protocol_state, command)){
                    logf(LOG_ERROR,"Command '%s' not allowed in this state",commands[command].name);
                    state->command = ERROR_COMMAND;
                }
                parser_reset(state->pop3_parser);
                return WRITING_RESPONSE; //vamos a escribir la respuesta
            }
        }
        buffer_read_adv(&(state->info_read_buff),(ssize_t ) max);
        //No hay un comando completo, volvemos a leer
        if(selector_set_interest(key->s,key->fd,OP_READ) != SELECTOR_SUCCESS){
            log(LOG_ERROR, "Error setting interest");
            return FINISHED;
        }
        return READING_REQUEST;
    }
    //Va a volver a donde esta, tiene que seguir escribiendo
    return WRITING_RESPONSE;
}

void finish_connection(const unsigned state, struct selector_key *key){
    pop3 * data = GET_POP3(key);
    if(data->pop3_protocol_state == TRANSACTION){
        logf(LOG_INFO, "Finishing connection of user '%s'", data->user_s->name);
        //Liberamos la casilla del usuario
        data->user_s->logged=false;
    }
    if(data->pop3_protocol_state == TRANSACTION && data->state_data.transaction.file_opened){
        //Cierro el archivo, lo saco del selector
        if(selector_unregister_fd(key->s, data->state_data.transaction.file_fd) != SELECTOR_SUCCESS){
            log(LOG_FATAL,"Error unregistering file fd");
            abort();
        }
    }
    //TODO: revisar que antes era con key->fd, pero esto tambien se puede llamar desde el archivo
    if (selector_unregister_fd(key->s, data->connection_fd) != SELECTOR_SUCCESS) {
        log(LOG_FATAL,"Error unregistering fd");
        abort();
    }
}
/*
 * --------------------------------------------------------------------------------------
 * Funciones auxiliares para los comandos (struct command)
 * --------------------------------------------------------------------------------------
 */
void reset_structures(pop3* state){
    memset(&(state->state_data),0,sizeof(state->state_data));
}

bool check_command_for_protocol_state(protocol_state pop3_protocol_state, pop3_command command){
    switch (pop3_protocol_state) {
        case AUTHORIZATION:
            return command == QUIT || command == USER || command == PASS || command == CAPA;
        case TRANSACTION:
            return command != USER && command!=PASS;
        default:
            return false;
    }
}

bool have_argument(const char* arg){
    return strlen(arg)!=0;
}

bool might_argument(const char* arg){
    return true;
}

bool not_argument(const char* arg){
    return arg == NULL || strlen(arg) == 0;
}

pop3_command get_command(const char* command){
    for(int i = USER; i<=CAPA; i++){
        if(strncasecmp(command,commands[i].name,4)==0){
            return i;
        }
    }
    return ERROR_COMMAND;
}

typedef enum{
    TRY_PENDING,
    TRY_DONE
}try_state;
/*
 * Funcion auxiliar para escribir el string str al buffer buff si tiene espacio
 *
 * Return
 * Devuelve TRY_DONE si se pudo escribir el string completo en el buffer, TRY_PENDING si no
 * (y en ese caso no escribe parte del string)
 */
try_state try_write(const char* str, buffer* buff){
    size_t max = 0;
    uint8_t * ptr = buffer_write_ptr(buff,&max);
    size_t message_len = strlen(str);
    if(max<message_len){
        //vuelvo a intentar despues
        return TRY_PENDING;
    }
    //Manda el mensaje parcialmente si no hay espacio
    memcpy(ptr, str, message_len); //eliminar warnings y es mas claro en lo que hacemos (no queremos el \0)
//    strncpy((char*)ptr, str, message_len);
    buffer_write_adv(buff,(ssize_t)message_len);
    return TRY_DONE;
}

unsigned finish_error(struct  selector_key* key){
    //Si llego aca tengo que estar en escritura
    pop3* state = GET_POP3(key);
    if(state->final_error_message == NULL){
        state->final_error_message = UNKNOWN_ERROR_MESSAGE;
    }
    if(!state->error_written){
        if(try_write(state->final_error_message,&(state->info_write_buff)) == TRY_DONE){
             state->error_written = true;
        }
    }


    //Escribimos lo que tenemos en el buffer de salida al socket
    size_t  max = 0;
    uint8_t* ptr = buffer_read_ptr(&(state->info_write_buff),&max);
    ssize_t sent_count = send(key->fd,ptr,max,MSG_NOSIGNAL);
    bytes_sent += sent_count;

    if(sent_count == -1){
        return FINISHED; //para que vaya a .on_departure, nunca deberia llegar a hello
    }
    buffer_read_adv(&(state->info_write_buff),sent_count);
    //Si ya no hay mas para escribir y el comando termino de generar la respuesta
    if(!buffer_can_read(&(state->info_write_buff))){
        return FINISHED;
    }
    return ERROR;//vuelvo a intentar
}

/*
 * Acciones asociadas a cada comando de pop3
 */
int user_action(pop3* state){
    char * msj = USER_INVALID_MESSAGE;
    for(unsigned int i=0; i<state->pop3_args->users->users_count; i++){
        if(strcmp(state->arg, state->pop3_args->users->users_array[i].name) == 0){
            state->state_data.authorization.user = state->pop3_args->users->users_array[i].name;
            state->state_data.authorization.pass = state->pop3_args->users->users_array[i].pass;
            state->user_s = state->pop3_args->users->users_array + i;
            msj = USER_VALID_MESSAGE;
        }
    }
    if(try_write(msj,&(state->info_write_buff)) == TRY_PENDING){
        //No deberia pasar nunca, si llego aca es porque el buffer de salida esta vacio
        return FINISHED;
    }
    state->finished = true;
    return WRITING_RESPONSE;
}

int pass_action(pop3* state){
    char * msj = PASS_INVALID_MESSAGE;
    if(state->state_data.authorization.pass != NULL && strcmp(state->arg, state->state_data.authorization.pass) == 0){
        if(state->user_s->logged){
            logf(LOG_INFO,"User '%s' already logged", state->state_data.authorization.user)
            msj = USER_LOGGED;
        }else{
            logf(LOG_INFO,"User '%s' logged in", state->state_data.authorization.user)
            msj = PASS_VALID_MESSAGE;
            state->user_s->logged = true;
            state->pop3_protocol_state = TRANSACTION;
            state->path_to_user_maildir = usersADT_get_user_mail_path(state->pop3_args->users,state->pop3_args->maildir_path, state->state_data.authorization.user);
            size_t mails_max = state->pop3_args->max_mails;
            state->emails = read_maildir(state->path_to_user_maildir,&mails_max);
            if(state->emails == NULL){
                state->final_error_message = NO_MAILDIR_MESSAGE;
                return ERROR;
            }
            state->emails_count = mails_max;
            reset_structures(state);
        }
    }
    if(try_write(msj,&(state->info_write_buff)) == TRY_PENDING){
        log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
        return FINISHED;
    }
    state->finished = true;
    return  WRITING_RESPONSE;
}

int stat_action(pop3* state){
    char aux[MAX_STAT_LINE];
    //computamos el total de size
    long aux_len_emails = 0;
    int deleted_count = 0;
    for(size_t i=0; i<state->emails_count ; i++){
        if(!state->emails[i].deleted){
            aux_len_emails += state->emails[i].size;
        }else{
            deleted_count++;
        }
    }
    snprintf(aux,MAX_STAT_LINE,"+OK %zu %ld\r\n",state->emails_count - deleted_count,aux_len_emails);
    if(try_write(aux,&(state->info_write_buff)) == TRY_PENDING){
        log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
        return FINISHED;
    }
    state->finished = true;
    return  WRITING_RESPONSE;
}

int default_action(pop3* state){
    if(try_write(ERROR_COMMAND_MESSAGE,&(state->info_write_buff)) == TRY_PENDING){
        log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
        return FINISHED;
    }
    state->finished = true;
    return WRITING_RESPONSE;
}

int list_action(pop3* state){
    //procesamos el argumento recibido
    if(!state->state_data.transaction.arg_processed && strlen(state->arg) != 0){
        state->state_data.transaction.has_arg = true;
        state->state_data.transaction.arg = strtol(state->arg, NULL,10);
        if(errno == EINVAL || errno == ERANGE){
            if(try_write(ERROR_INDEX_MESSAGE,&(state->info_write_buff)) == TRY_PENDING){
                return FINISHED;
            }
            state->finished = true;
            reset_structures(state);
            return WRITING_RESPONSE;
        }
    }else{
        state->state_data.transaction.has_arg = false;
    }
    state->state_data.transaction.arg_processed = true;
    if(state->state_data.transaction.multiline_state == MULTILINE_STATE_FIRST_LINE){
        //estamos escribiendo la primera linea, ya sea de error u OK
        if(state->state_data.transaction.has_arg){
            if(state->state_data.transaction.arg > (long)state->emails_count || state->state_data.transaction.arg <= 0){
                //Error de indice
                if(try_write(ERROR_INDEX_MESSAGE,&(state->info_write_buff)) == TRY_PENDING){
                    log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                    return FINISHED;
                }
            }else if(state->emails[state->state_data.transaction.arg-1].deleted){
                if(try_write(ERROR_DELETED_MESSAGE,&(state->info_write_buff)) == TRY_PENDING){
                    log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                    return FINISHED;
                }
            }else {
                //Tengo que mostrar solo la informacion de ese mail
                char aux[MAX_LIST_FIRST_LINE] = {0};
                email send_email = state->emails[state->state_data.transaction.arg - 1];
                snprintf(aux, MAX_LIST_FIRST_LINE, "+OK %ld %ld\r\n", state->state_data.transaction.arg, send_email.size);
                if (try_write(aux, &(state->info_write_buff)) == TRY_PENDING) {
                    log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                    return FINISHED;
                }
            }
            state->finished = true;
            reset_structures(state);
            return WRITING_RESPONSE;
        }else{
            //Tengo que escribir la primera linea de una respuesta multilinea
            if (try_write(LIST_MESSAGE, &(state->info_write_buff)) == TRY_PENDING) {
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->state_data.transaction.multiline_state = MULTILINE_STATE_MULTILINE;
            state->state_data.transaction.mail_index = 0;
        }
    }
    if(state->state_data.transaction.multiline_state==MULTILINE_STATE_MULTILINE){
        //Tengo que imprimir todos los mails
        for(; state->state_data.transaction.mail_index< (long)state->emails_count; state->state_data.transaction.mail_index++){
            if(state->emails[state->state_data.transaction.mail_index].deleted){
                continue;
            }
            char aux[MAX_LIST_LINE];
            email send_email = state->emails[state->state_data.transaction.mail_index];
            snprintf(aux,MAX_LIST_LINE,"%d %ld\r\n",state->state_data.transaction.mail_index+1,send_email.size);
            if (try_write(aux, &(state->info_write_buff)) == TRY_PENDING) {
                //No entra la linea de este mail en el buffer
                //sigo intentando despues
                return WRITING_RESPONSE;
            }
        }
        state->state_data.transaction.multiline_state = MULTILINE_STATE_END_LINE;
    }
    if(state->state_data.transaction.multiline_state==MULTILINE_STATE_END_LINE){
        //Solo agrega .\r\n porque antes la ultima linea termino el \r\n
        if (try_write(".\r\n", &(state->info_write_buff)) == TRY_PENDING) {
            //No entra la linea final en el buffer
            //sigo intentando despues
            return WRITING_RESPONSE;
        }
        reset_structures(state);
        state->finished = true;
    }
    return WRITING_RESPONSE;
}

typedef enum{
    BYTE_STUFFING_CR,
    BYTE_STUFFING_LF,
    BYTE_STUFFING_DOT,
    BYTE_STUFFING_NOTHING
}byte_stuffing_state;

byte_stuffing_state get_flag(byte_stuffing_state curr_flag, char curr_char){
    switch (curr_char) {
        case '\r':
            return BYTE_STUFFING_CR;
        case '\n':
            return curr_flag==BYTE_STUFFING_CR?BYTE_STUFFING_LF:BYTE_STUFFING_NOTHING;
        case '.':
            return curr_flag==BYTE_STUFFING_LF?BYTE_STUFFING_DOT:BYTE_STUFFING_NOTHING;
        default:
            return BYTE_STUFFING_NOTHING;
    }
}

int retr_action(pop3* state){
    if(!state->state_data.transaction.arg_processed && strlen(state->arg) != 0){
        state->state_data.transaction.has_arg = true;
        state->state_data.transaction.arg = strtol(state->arg, NULL,10);
        if(errno == EINVAL || errno == ERANGE){
            if(try_write(ERROR_RETR_ARG_MESSEGE, &(state->info_write_buff)) == TRY_PENDING){
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->finished = true;
            reset_structures(state);
            return WRITING_RESPONSE;
        }
    }else{
        state->state_data.transaction.has_arg = false;
    }
//    size_t max = 0;
//    buffer_write_ptr(&(state->info_write_buff),&max);
    if(state->state_data.transaction.multiline_state == MULTILINE_STATE_FIRST_LINE){
        if(!state->state_data.transaction.has_arg){
            if(try_write(ERROR_RETR_ARG_MESSEGE, &(state->info_write_buff)) == TRY_PENDING){
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->finished = true;
            reset_structures(state);
            return WRITING_RESPONSE;
        }else if(state->state_data.transaction.arg > (long)state->emails_count || state->state_data.transaction.arg <=0){
            if(try_write(ERROR_RETR_MESSAGE, &(state->info_write_buff)) == TRY_PENDING){
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->finished = true;
            reset_structures(state);
            return WRITING_RESPONSE;
        }else if(state->emails[state->state_data.transaction.arg-1].deleted){
            if(try_write(ERROR_DELETED_MESSAGE, &(state->info_write_buff)) == TRY_PENDING){
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->finished = true;
            reset_structures(state);
            return  WRITING_RESPONSE;
        }else{
            char aux[MAX_RETR_FIRST_LINE] = {0};
            snprintf(aux,MAX_RETR_FIRST_LINE,"+OK %ld octets\r\n",state->emails[state->state_data.transaction.arg-1].size);
            if(try_write(aux, &(state->info_write_buff)) == TRY_PENDING){
                log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
                return FINISHED;
            }
            state->state_data.transaction.multiline_state = MULTILINE_STATE_MULTILINE;
            state->state_data.transaction.flag = BYTE_STUFFING_LF;
        }
    }
    if(state->state_data.transaction.multiline_state == MULTILINE_STATE_MULTILINE){
        //Tengo que empezar a leer el archivo
        //si no abri el archivo o no tengo mas para leer pero no lo termine
        if(!state->state_data.transaction.file_opened || (!buffer_can_read(&(state->info_file_buff)) && !state->state_data.transaction.file_ended)) {
            //Tenemos que abrir el archivo y nos interesamos para leer de el
            return PROCESSING_RESPONSE;
        }
        //Tengo cosas en el buffer del archivo para leer
        size_t write_max = 0;
        uint8_t *write_ptr = buffer_write_ptr(&(state->info_write_buff), &write_max);
        size_t file_max = 0;
        uint8_t *file_ptr = buffer_read_ptr(&(state->info_file_buff), &file_max);
        //siempre me quedo con al menos 2 espacios en el de write por si tengo que hacer byte stuffing
        size_t write = 0, file = 0;
        for (; file < file_max && write < write_max - 1; write++, file++) {
            state->state_data.transaction.flag = get_flag(state->state_data.transaction.flag,(char) file_ptr[file]);
            if(state->state_data.transaction.flag == BYTE_STUFFING_DOT) { //para ver si mejora la velocidad
                //tengo que hacer byte stuffing, vi un punto al inicio de una linea nueva
                logf(LOG_DEBUG, "Doing byte stuffing in file %ld and position %ld ", state->state_data.transaction.arg,file);
                write_ptr[write++] = '.'; //agrego un punto al principio
            }
            write_ptr[write] = file_ptr[file];
        }
        //No avanzar el maximo, por si se queda un caracter en el buffer del archivo que no se procesa por no tener 2 espacios en el de salida
        buffer_write_adv(&(state->info_write_buff), (ssize_t)write);
        buffer_read_adv(&(state->info_file_buff), (ssize_t)file);
        if(!buffer_can_read(&(state->info_file_buff)) && state->state_data.transaction.file_ended){
            state->state_data.transaction.multiline_state = MULTILINE_STATE_END_LINE;
        }
    }
    if(state->state_data.transaction.multiline_state == MULTILINE_STATE_END_LINE){
//        if(state->state_data.transaction.flag == BYTE_STUFFING_LF){
//        //esto no agrega una linea de mas
//            if(try_write(".\r\n", &(state->info_write_buff)) == TRY_PENDING){
//                return WRITING_RESPONSE;
//            }
//        }else{
//            //Usar esto solo, lo asegura
            if(try_write("\r\n.\r\n", &(state->info_write_buff)) == TRY_PENDING){
                return WRITING_RESPONSE;
            }
//        }
        parser_reset(state->byte_stuffing_parser);
        reset_structures(state);
        state->finished = true;
    }

    return WRITING_RESPONSE;
}

void process_open_file(const unsigned state, struct selector_key *key){
    //Aca abrimos el archivo y metemos el fd en el selector
    pop3* data = GET_POP3(key);
    if(!data->state_data.transaction.file_opened){
        log(LOG_DEBUG,"Opening file");
        //Tenemos que abrir el archivo y registrarlo en el selector buscando leer
        char * path =  data->path_to_user_maildir;
        int dir_fd = open(path,O_DIRECTORY);//abrimos el directorio
        if(dir_fd==-1){
            //hubo un error abriendo el directorio
            log(LOG_FATAL, "Error opening maildir directory");
            exit(1);
        }
        //Obtenemos el mail que se desea abrir
        email curr_email = data->emails[data->state_data.transaction.arg-1];
        int file_fd = openat(dir_fd,curr_email.name,O_RDONLY);
        if(file_fd==-1){
            log(LOG_FATAL, "Error opening current email");
            exit(1);
        }
        close(dir_fd);//cerramos el directorio, ya no nos sirve
        data->state_data.transaction.file_fd = file_fd; //lo guardamos para ir y volver
        data->state_data.transaction.file_opened = true;
        selector_register(key->s,file_fd,&handler,OP_READ,data);
        (data->references)++; //importante para no liberar si se usa
        return;
    }
    //Nos suscribimos para leer del archivo
    selector_set_interest(key->s,data->state_data.transaction.file_fd,OP_READ);
}

unsigned int process_response(struct  selector_key* key){
    pop3* state = GET_POP3(key);
    if(!state->state_data.transaction.file_opened){
        log(LOG_ERROR, "Error opening file");
        return FINISHED;//cerramos la conexion, no pudimos abrir el archivo
    }
    //Leer del archivo y mandarlo a el buffer intermedio
    size_t max = 0;
    uint8_t* ptr = buffer_write_ptr(&(state->info_file_buff), &max);
    //Estoy leyendo del archivo, y me deberian llamar aca con key en el archivo
    ssize_t read_count = read(key->fd, ptr, max);
    if(read_count==0){
        log(LOG_DEBUG, "Finished reading file");
        //terminamos de leer el archivo, lo señalo para no volver aca
        state->state_data.transaction.file_ended = true;
    }
    if(read_count<0){
        log(LOG_ERROR, "Error reading file");
        return FINISHED;
    }
    //Avanzamos la escritura en el buffer
    buffer_write_adv(&(state->info_file_buff), read_count);
    if(selector_set_interest(key->s,state->connection_fd, OP_WRITE) != SELECTOR_SUCCESS
        || selector_set_interest(key->s,state->state_data.transaction.file_fd,OP_NOOP)!= SELECTOR_SUCCESS){
        log(LOG_ERROR, "Error setting interest");
        return FINISHED;
    }
    if(state->state_data.transaction.file_ended == true){
        //Es la ultima vez que voy a venir al archivo, lo cerramos
        if(selector_unregister_fd(key->s,state->state_data.transaction.file_fd)!= SELECTOR_SUCCESS){
            log(LOG_ERROR, "Error unregistering file fd");
            return FINISHED;
        }
    }
    //aprovecho que es la misma maquina de estados
    return WRITING_RESPONSE;
}

int dele_action(pop3* state){
    char * msj_ret = ERROR_MESSSAGE;
    long index = strtol(state->arg, NULL,10);
    if( errno!= EINVAL && errno != ERANGE && index <= (long)state->emails_count &&  index>0 &&  !state->emails[index-1].deleted){
        logf(LOG_INFO, "Marking to delete email with index %ld", index);
        state->emails[index-1].deleted = true;
        msj_ret = OK_MESSSAGE;
    }
    if(try_write(msj_ret, &(state->info_write_buff)) == TRY_PENDING){
        return FINISHED;
    }
    state->finished = true;
    return  WRITING_RESPONSE;
}

int rset_action(pop3* state){
    //computamos el total de size
    for(size_t i=0; i<state->emails_count ; i++){
        logf(LOG_INFO,"Unmarking to delete file %lu",i+1);
        state->emails[i].deleted = false;
    }
    if(try_write(OK_MESSSAGE, &(state->info_write_buff)) == TRY_PENDING){
        return FINISHED;
    }
    state->finished = true;
    return  WRITING_RESPONSE;
}

int noop_action(pop3* state){
    if(try_write(OK_MESSSAGE, &(state->info_write_buff)) == TRY_PENDING){
        return FINISHED;
    }
    state->finished = true;
    return WRITING_RESPONSE;
}

int quit_action(pop3* state){
    state->final_error_message = QUIT_MESSAGE;
    if(state->pop3_protocol_state == AUTHORIZATION){
        logf(LOG_INFO, "Quitting in Authorization state for fd %d", state->connection_fd);
        return ERROR;//cierro la conexion
    }
    logf(LOG_INFO, "Finishing connection of user '%s'", state->user_s->name);
    state->user_s->logged=false;
    //Estamos en transaction, tengo que eliminar todos los archivos que marcaron para eliminar
    int dir_fd = open(state->path_to_user_maildir,O_DIRECTORY);
    if(dir_fd == -1){
        log(LOG_ERROR,"Error opening mail directory to delete mails");
        return FINISHED;
    }
    for(size_t i = 0; i<state->emails_count; i++){
        if(state->emails[i].deleted){
            logf(LOG_INFO, "Deleting email %zu",i+1);
            //elimnamos el archivo (cuando ningun proceso lo tenga abierto, lo va a sacar)
            unlinkat(dir_fd,state->emails[i].name,0);
        }
    }
    state->pop3_protocol_state = AUTHORIZATION;
    close(dir_fd);
    return ERROR;
}

int capa_action(pop3* state){
    if(try_write(CAPA_MESSAGE, &(state->info_write_buff)) == TRY_PENDING){
        log(LOG_ERROR,"Writing to exit buffer was not possible when it should be empty")
        return FINISHED;
    }
    state->finished = true;
    return WRITING_RESPONSE;
}
