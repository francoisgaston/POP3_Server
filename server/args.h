#ifndef ARGS_H_kFlmYm1tW9p5npzDr2opQJ9jM8
#define ARGS_H_kFlmYm1tW9p5npzDr2opQJ9jM8

#include <stdbool.h>
#include "usersADT.h"
#include "logging/logger.h"

#define DEFAULT_POP3_PORT 1100
#define DEFAULT_POP3_CONFIG_PORT 1101
#define DEFAULT_MAILDIR_PATH "/var/mail/"
#define DEFAULT_MAX_MAILS 20
#define MAX_USERS 500


struct pop3args {
    unsigned short  pop3_port;
    unsigned short  pop3_config_port;
    char *          maildir_path;
    log_level_t     log_level;
    usersADT        users;
    unsigned long   max_mails;
    char*           access_token;
};

/**
 * Interpreta la linea de comandos (argc, argv) llenando
 * args con defaults o la seleccion humana. Puede cortar
 * la ejecución.
 */
void 
parse_args(const int argc, const char **argv, struct pop3args *args);

int change_maildir(struct pop3args* args, const char* maildir);

#endif

