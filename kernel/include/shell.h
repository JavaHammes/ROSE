#ifndef SHELL_H
#define SHELL_H

typedef void (*shell_command_handler)(int argc, char **argv);

struct shell_command {
        const char *name;
        const char *description;
        shell_command_handler handler;
};

void shell_execute(char *line);

#endif
