// Copyright 2017 Brendan Miles  [legal/copyright] [5]
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>

typedef int bool;
#define false 0
#define true 1

typedef struct __instruction {
    char *command;
    char *args[10];
    int argSize;
    bool redirectInput;
    bool redirectOutput;
    char *fileIn;
    char *fileOut;
    bool background;
} instruction;

instruction *currentInstruction;
instruction *pipeInstruction;
int runningJobs[100];
int numJobs = 0;

void err(int shouldExit) {
    char error_message[30] = "An error has occurred\n";
    write(STDERR_FILENO, error_message, strlen(error_message));
    if (shouldExit == 1) {
        exit(0);
    }
}

bool parseInput(char *input, instruction *instrPtr) {
    int i;
    char *token, *saveptr;
    bool ret = true;

    token = strtok_r(input, " ", &saveptr);
    if (token == NULL) {
        err(1);
    }
    instrPtr->command = token;
    instrPtr->args[0] = token;
    instrPtr->argSize = 0;
    instrPtr->redirectInput = false;
    instrPtr->redirectOutput = false;
    instrPtr->background = false;

    int inputIndex = 0;
    int outputIndex = 0;
    for (i = 1; ; i++) {
        token = strtok_r(NULL, " ", &saveptr);
        if (token == NULL) {
            break;
        }

        if (inputIndex > 0) {
            if ((i > inputIndex + 1) && (strcmp(token, ">") != 0)) {
                ret = false;
                continue;
            } else {
                instrPtr->fileIn = token;
                inputIndex = 0;
                continue;
            }
        }
        if (outputIndex > 0) {
            if ((i > outputIndex + 1) && (strcmp(token, "<") != 0)) {
                ret = false;
                continue;
            } else {
                instrPtr->fileOut = token;
                continue;
            }
        }
        if (strcmp(token, "<") == 0) {
            instrPtr->redirectInput = true;
            inputIndex = i;
            outputIndex = 0;
            continue;
        } else if (strcmp(token, ">") == 0) {
            instrPtr->redirectOutput = true;
            outputIndex = i;
            inputIndex = 0;
            continue;
        } else if (strcmp(token, "&") == 0) {
            instrPtr->background = true;
        } else {
            instrPtr->argSize++;
            instrPtr->args[instrPtr->argSize] = token;
        }
    }
    instrPtr->args[instrPtr->argSize+1] = NULL;
    return ret;
}

bool tryBuiltInCommand(char *command, char **arguments,
    int argSize, char *input) {
    bool ret = false;
    if (strcmp(command, "cd") == 0) {
        char *dir;
        if (argSize == 0) {
            dir = getenv("HOME");
        } else {
            dir = arguments[1];
        }
        if (chdir(dir) != 0) {
            err(0);
        }
        ret = true;
    } else if (strcmp(command, "exit") == 0) {
        int i;
        for (i = 0; i < numJobs; i++) {
            int pid = runningJobs[i];
            int status;
            waitpid(pid, &status, WNOHANG);
            kill(pid, 9);
        }
        free(currentInstruction);
        free(input);
        exit(0);
    } else if (strcmp(command, "pwd") == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            err(1);
        } else {
            fprintf(stdout, "%s\n", cwd);
        }
        ret = true;
    }
    return ret;
}

void redirectInput() {
    int fi = open(currentInstruction->fileIn, O_RDONLY, S_IRWXU);
    if (fi < 0) {
      err(1);
    }
    dup2(fi, 0);
}

void redirectOutput() {
    int fo = open(currentInstruction->fileOut,
        O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
    if (fo < 0) {
      err(1);
    }
    dup2(fo, 1);
}

void doPipe(int fd[]) {
    int pid;

    pid = fork();
    switch (pid) {
    default:
        dup2(fd[0], 0);
        close(fd[1]);
        execvp(pipeInstruction->command, pipeInstruction->args);
        err(1);
    case 0:
        dup2(fd[1], 1);
        close(fd[0]);
        execvp(currentInstruction->command, currentInstruction->args);
        err(1);
    case -1:
        err(1);
    }
}

char *trim(char *str) {
    size_t len = 0;
    char *frontp = str;
    char *endp = NULL;

    if (str == NULL ) { return NULL; }
    if (str[0] == '\0' ) { return str; }

    len = strlen(str);
    endp = str + len;

    while (isspace((unsigned char) *frontp)) {
        ++frontp;
    }
    if (endp != frontp) {
        while (isspace((unsigned char) *(--endp)) && endp != frontp ) {}
    }

    if (str + len - 1 != endp) {
        *(endp + 1) = '\0';
    } else if (frontp != str &&  endp == frontp) {
        *str = '\0';
        }

    endp = str;
    if (frontp != str) {
        while (*frontp) {
            *endp++ = *frontp++;
        }
        *endp = '\0';
    }
    return str;
}

int main(int argc, char *argv[]) {
    char *input;
    char *token;
    char *token2;
    char *saveptr;
    int count = 1;
    int status;
    if (argc > 1) {
        err(0);
        exit(1);
    }
    for (; ;) {
        input = malloc(129 * sizeof(char));  // max size input is 128 characters
        currentInstruction = malloc(sizeof(instruction));
        bool isPipe = false;
        fprintf(stdout, "mysh (%d)> ", count);
        fflush(stdout);
        if (fgets(input, 129, stdin) == NULL) {
            err(0);
        }
        int extra_data_found = 0;
        if (strchr(input, '\n') == NULL) {
            int ch;
            while ((ch = fgetc(stdin)) != '\n' && ch != EOF) {
                extra_data_found = 1;
            }
        }
        if (extra_data_found == 1) {
            count++;
            fflush(stdin);
            free(currentInstruction);
            free(input);
            err(0);
            continue;
        }
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) {
            free(currentInstruction);
            free(input);
            continue;
        }
        token = strtok_r(input, "|", &saveptr);
        token2 = trim(token);
        if (strlen(token2) == 0) {
            free(currentInstruction);
            free(input);
            continue;
        }
        if (parseInput(token2, currentInstruction) == false) {
            count++;
            free(currentInstruction);
            free(input);
            err(0);
            continue;
        }
        token = strtok_r(NULL, "|", &saveptr);
        if (token != NULL) {
            isPipe = true;
            token2 = trim(token);
            pipeInstruction = malloc(sizeof(instruction));
            parseInput(token2, pipeInstruction);
        }
        if (tryBuiltInCommand(currentInstruction->command,
            currentInstruction->args, currentInstruction->argSize, input)) {
            free(currentInstruction);
            if (isPipe == true) {
                free(pipeInstruction);
            }
            free(input);
            count++;
            continue;
        }
        int pid;
        if (currentInstruction->background == true) {
            signal(SIGCHLD, SIG_IGN);
        }
        switch (pid = fork()) {
        default:
            if (currentInstruction->background != true) {
                waitpid(pid, &status, WUNTRACED);
            } else {
                runningJobs[numJobs] = pid;
                numJobs++;
            }
            count++;
            free(currentInstruction);
            free(input);
            if (isPipe) {
                free(pipeInstruction);
            }
            break;
        case -1:
            err(1);
            break;
        case 0:
            if (isPipe == true) {
                int fd[2];
                int pid2;
                pipe(fd);
                doPipe(fd);
                close(fd[1]);
                close(fd[0]);
                while ((pid2 = wait(&status)) != -1) {}
                continue;
            }
            if (currentInstruction->redirectInput == true) {
                redirectInput();
            }
            if (currentInstruction->redirectOutput == true) {
                redirectOutput();
            }
            execvp(currentInstruction->command, currentInstruction->args);
            err(1);
        }
    }
    return 0;
}
