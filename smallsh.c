#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define INPUT_LENGTH 2048
#define MAX_ARGS 512
#define MAX_PROCESSES 200

volatile int foregroundState = 0;

struct command_line {
    char *argv[MAX_ARGS + 1];
    int argc;
    char *input_file;
    char *output_file;
    bool is_bg;
};

struct command_line *parse_input() {
    char input[INPUT_LENGTH];
    struct command_line *curr_command = (struct command_line *) calloc(1,
        sizeof(struct command_line));

    printf(": ");
    fflush(stdout);
    fgets(input, INPUT_LENGTH, stdin);

    if (input[0] == '\n' || input[0] == '#') {
        curr_command->argc = 0;
        return curr_command;
    }

    char *token = strtok(input, " \n");

    while(token){
        if(!strcmp(token,"<")) {
            curr_command->input_file = strdup(strtok(NULL," \n"));
        } 
        else if(!strcmp(token,">")){
            curr_command->output_file = strdup(strtok(NULL," \n"));
        } 
        else if(!strcmp(token,"&")){
            curr_command->is_bg = true;
        } 
        else{
            curr_command->argv[curr_command->argc++] = strdup(token);
        }
        token = strtok(NULL," \n");
    }

    return curr_command;
}

void handle_SIGTSTP(int signal) {
    if(!foregroundState) {
        write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n", 51);
        foregroundState = 1;
    } 
    else {
        write(STDOUT_FILENO, "\nExiting foreground-only mode\n", 31);
        foregroundState = 0;
    }
}

void changeDirectory(struct command_line *curr_command) {
    if(curr_command->argv[1] == NULL) {
        chdir(getenv("HOME"));
    } 
    else {
        if(chdir(curr_command->argv[1]) != 0) {
            char errorMessage[256];
            sprintf(errorMessage, "%s: no such file or directory\n", curr_command->argv[1]);
            write(STDERR_FILENO, errorMessage, strlen(errorMessage));
        }
    }
}

void statusCommand(int exitStatus) {
    if(WIFEXITED(exitStatus)) {
        printf("exit value %d\n", WEXITSTATUS(exitStatus));
        fflush(stdout);
    } 
    else if (WIFSIGNALED(exitStatus)) {
        printf("terminated by signal %d\n", WTERMSIG(exitStatus));
        fflush(stdout);
    }
}

void exitCommand(int processes[]) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i] != 0) {
            kill(processes[i], SIGTERM);
        }
    }
}

void ioRedirection(struct command_line *curr_command, int bgProcess) {
    if(curr_command->input_file) {
        int sourceFD = open(curr_command->input_file, O_RDONLY);
        if(sourceFD == -1) {
            char errorMessage[256];
            sprintf(errorMessage, "cannot open %s for input\n", curr_command->input_file);
            write(STDERR_FILENO, errorMessage, strlen(errorMessage));
            exit(1);
        }
        dup2(sourceFD, STDIN_FILENO);
        close(sourceFD);
    }
    else if(bgProcess) {
        int sourceFD = open("/dev/null", O_RDONLY);
        dup2(sourceFD, STDIN_FILENO);
        close(sourceFD); 
    }
    
    if(curr_command->output_file) {
        int targetFD = open(curr_command->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0640);
        if(targetFD == -1) {
            char errorMessage[256];
            sprintf(errorMessage, "cannot open %s for output\n", curr_command->output_file);
            write(STDERR_FILENO, errorMessage, strlen(errorMessage));
            exit(1);
        }
        dup2(targetFD, STDOUT_FILENO);
        close(targetFD);
    }
    else if(bgProcess) {
        int targetFD = open("/dev/null", O_WRONLY);
        dup2(targetFD, STDOUT_FILENO);
        close(targetFD);
    }
}

void checkBackgroundProcesses(int processes[]) {
    int status;
    pid_t pid;

    for(int i = 0; i < MAX_PROCESSES; i++) {
        if(processes[i] != 0) {
            pid = waitpid(processes[i], &status, WNOHANG);

            if(pid > 0) {
                if(WIFEXITED(status)) {
                    printf("background pid %d is done: exit value %d\n", pid, WEXITSTATUS(status));
                    fflush(stdout);
                } 
                else if (WIFSIGNALED(status)) {
                    printf("background pid %d is done: terminated by signal %d\n", pid, WTERMSIG(status));
                    fflush(stdout);
                }
                processes[i] = 0;
            }
        }
    }
}

void executeCommand(struct command_line *curr_command, int *exitStatus, int processes[]) {
    int bgProcess = curr_command->is_bg && !foregroundState;

    pid_t spwanPID = fork();

    switch (spwanPID) {
        case -1: {
            perror("fork() failed\n");
            exit(1);
        }
            break;
        case 0: {
            struct sigaction SIGINT_action = {0};
            struct sigaction SIGTSTP_action = {0};
            if(!bgProcess) {
                SIGINT_action.sa_handler = SIG_DFL;
            } 
            else {
                SIGINT_action.sa_handler = SIG_IGN;
            }
            sigaction(SIGINT, &SIGINT_action, NULL);
            SIGTSTP_action.sa_handler = SIG_IGN;
            sigaction(SIGTSTP, &SIGTSTP_action, NULL);

            ioRedirection(curr_command, bgProcess);

            execvp(curr_command->argv[0], curr_command->argv);

            char errorMessage[256];
            sprintf(errorMessage, "%s: no such file or directory\n", curr_command->argv[0]);
            write(STDERR_FILENO, errorMessage, strlen(errorMessage));
            exit(1);
        }
            break;
        default: {
            if(bgProcess) {
                printf("background pid is %d\n", spwanPID);
                fflush(stdout);

                for(int i = 0; i < MAX_PROCESSES; i++) {
                    if(processes[i] == 0) {
                        processes[i] = spwanPID;
                        break;
                    }
                }
            }
            else {
                int status;
                waitpid(spwanPID, &status, 0);
                *exitStatus = status;

                if(WIFSIGNALED(status)) {
                    printf("terminated by signal %d\n", WTERMSIG(status));
                    fflush(stdout);
                }
            }
        }
            break;
    }
}

void freeMem(struct command_line *curr_command) {
    for(int i = 0; i < curr_command->argc; i++) {
        free(curr_command->argv[i]);
    }
    if(curr_command->input_file) {
        free(curr_command->input_file);
    }
    if(curr_command->output_file) {
        free(curr_command->output_file);
    }
    free(curr_command);
}

int main() {
    int processes[MAX_PROCESSES] = {0};
    
    int exitStatus = 0; 

    struct command_line *curr_command;

    struct sigaction SIGINT_action = {0};
    struct sigaction SIGTSTP_action = {0};

    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);

    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
    
    while(true) {
        checkBackgroundProcesses(processes);

        curr_command = parse_input();

        if(curr_command->argc == 0) {
            freeMem(curr_command);
            continue;
        }

        if(strcmp(curr_command->argv[0], "exit") == 0) {
            exitCommand(processes);
            freeMem(curr_command);
            break;
        } 
        else if(strcmp(curr_command->argv[0], "cd") == 0) {
            changeDirectory(curr_command);
        } 
        else if(strcmp(curr_command->argv[0], "status") == 0) {
            statusCommand(exitStatus);
        } 
        else {
            executeCommand(curr_command, &exitStatus, processes);
        }

        freeMem(curr_command);
    }
    return EXIT_SUCCESS;
}