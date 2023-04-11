#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "err.c"
#include "utils.c"
#define MAX_N_TASKS 4096
#define MAX_OUT_LEN 1022

char* endedTasksInfo;
int endedTasksInfoPos;
pthread_mutex_t canSendEndMessage;
bool canPrintf;

struct Task {
    int id;
    pid_t pid;
    char* outMessage;
    char* errMessage;
    pthread_mutex_t outMutex;
    pthread_mutex_t errMutex;
    int pipeDescOut[2];
    int pipeDescErr[2];
    pthread_t checkThread;
    pthread_t outThread;
    pthread_t errThread;
};

void task_main(char** args, struct Task* task) {
    task->pid = getpid();
    close(task->pipeDescOut[0]);
    close(task->pipeDescErr[0]);
    dup2(task->pipeDescOut[1], STDOUT_FILENO);
    dup2(task->pipeDescErr[1], STDERR_FILENO);
    close(task->pipeDescOut[1]);
    close(task->pipeDescErr[1]);
    execvp(args[1], args + 1);
}

void* task_check_ended(void* data) {
    struct Task* task = (struct Task*)data;
    int status;

    waitpid(task->pid, &status, 0);

    if (WIFEXITED(status)) {
        pthread_mutex_lock(&canSendEndMessage);
        if (canPrintf) {
            printf("Task %d ended: status %d.\n", task->id, WEXITSTATUS(status));
            fflush(stdout);
        } else {
            endedTasksInfoPos += sprintf(endedTasksInfo + endedTasksInfoPos, "Task %d ended: status %d.\n", task->id,
                                         WEXITSTATUS(status));
        }
        pthread_mutex_unlock(&canSendEndMessage);
    } else if (WIFSIGNALED(status)) {
        pthread_mutex_lock(&canSendEndMessage);
        if (canPrintf) {
            printf("Task %d ended: signalled.\n", task->id);
            fflush(stdout);
        } else {
            endedTasksInfoPos += sprintf(endedTasksInfo + endedTasksInfoPos, "Task %d ended: signalled.\n", task->id);
        }
        pthread_mutex_unlock(&canSendEndMessage);
    }

    return 0;
}

void* task_output(void* data) {
    struct Task* task = (struct Task*)data;
    FILE* file = fdopen(task->pipeDescOut[0], "r");
    size_t bufferSize = 2;
    char* bufferOut = malloc(bufferSize * sizeof(char));
    int outMessage = 0;

    while ((outMessage = getline(&bufferOut, &bufferSize, file)) >= 0) {
        if (outMessage <= 0) {
            continue;
        }
        if (bufferOut[outMessage - 1] == '\n') {
            bufferOut[outMessage - 1] = '\0';
        }
        pthread_mutex_lock(&task->outMutex);
        strcpy(task->outMessage, bufferOut);
        pthread_mutex_unlock(&task->outMutex);
    }

    free(bufferOut);
    fclose(file);
    return 0;
}

void* task_error(void* data) {
    struct Task* task = (struct Task*)data;
    FILE* file = fdopen(task->pipeDescErr[0], "r");
    size_t bufferSize = 2;
    char* bufferErr = malloc(bufferSize * sizeof(char));
    int errMessage = 0;

    while ((errMessage = getline(&bufferErr, &bufferSize, file)) >= 0) {
        if (errMessage <= 0) {
            continue;
        }
        if (bufferErr[errMessage - 1] == '\n') {
            bufferErr[errMessage - 1] = '\0';
        }
        pthread_mutex_lock(&task->errMutex);
        strcpy(task->errMessage, bufferErr);
        pthread_mutex_unlock(&task->errMutex);
    }

    free(bufferErr);
    fclose(file);
    return 0;
}

void run_tasks(char* buffer, struct Task* task, int id) {
    task->outMessage = malloc(MAX_OUT_LEN * sizeof(char));
    task->errMessage = malloc(MAX_OUT_LEN * sizeof(char));
    strcpy(task->outMessage, "");
    strcpy(task->errMessage, "");
    pthread_mutex_init(&task->outMutex, NULL);
    pthread_mutex_init(&task->errMutex, NULL);
    pipe(task->pipeDescOut);
    pipe(task->pipeDescErr);
    task->id = id;

    pid_t pid;
    pid = fork();
    if (!pid) {
        char** args = split_string(buffer);
        task_main(args, task);
        return;
    }

    close(task->pipeDescOut[1]);
    close(task->pipeDescErr[1]);
    set_close_on_exec(task->pipeDescOut[0], true);
    set_close_on_exec(task->pipeDescErr[0], true);
    task->pid = pid;

    pthread_create(&task->checkThread, NULL, task_check_ended, task);
    pthread_create(&task->outThread, NULL, task_output, task);
    pthread_create(&task->errThread, NULL, task_error, task);

    printf("Task %d started: pid %d.\n", id, pid);
    fflush(stdout);
}

void output_tasks(struct Task* task) {
    pthread_mutex_lock(&task->outMutex);
    printf("Task %d stdout: '%s'.\n", task->id, task->outMessage);
    fflush(stdout);
    pthread_mutex_unlock(&task->outMutex);
}

void error_tasks(struct Task* task) {
    pthread_mutex_lock(&task->errMutex);
    printf("Task %d stderr: '%s'.\n", task->id, task->errMessage);
    fflush(stdout);
    pthread_mutex_unlock(&task->errMutex);
}

void killTask(struct Task* task) { kill(task->pid, SIGINT); }

void sleepExecutor(int n) {
    if (n * 1000 > n) n *= 1000;
    usleep(n);
}

void closeProgram(int n, struct Task* taskArr) {
    for (int i = 0; i < n; i++) {
        kill(taskArr[i].pid, SIGKILL);

        pthread_join(taskArr[i].outThread, NULL);
        pthread_join(taskArr[i].errThread, NULL);
        pthread_join(taskArr[i].checkThread, NULL);
    }
}
int main() {
    size_t bufferSize = 512;
    int bufferRead = 0;
    int n_tasks = 0;
    char* buffer = (char*)malloc(bufferSize * sizeof(char));
    endedTasksInfo = malloc(sizeof(char) * MAX_N_TASKS * MAX_OUT_LEN);
    struct Task* taskArr = malloc(MAX_N_TASKS * sizeof(struct Task));
    pthread_mutex_init(&canSendEndMessage, NULL);

    while ((bufferRead = getline(&buffer, &bufferSize, stdin)) > 0) {
        pthread_mutex_lock(&canSendEndMessage);
        canPrintf = false;
        pthread_mutex_unlock(&canSendEndMessage);

        if (bufferRead > 0 && buffer[bufferRead - 1] == '\n') buffer[bufferRead - 1] = '\0';
        if (!strcmp(buffer, "")) continue;
        char** args = split_string(buffer);

        if (!strcmp(args[0], "run")) {
            run_tasks(buffer, &taskArr[n_tasks], n_tasks);
            n_tasks++;
        } else if (!strcmp(args[0], "out")) {
            int id = atoi(args[1]);
            output_tasks(&taskArr[id]);
        } else if (!strcmp(args[0], "err")) {
            int id = atoi(args[1]);
            error_tasks(&taskArr[id]);
        } else if (!strcmp(args[0], "kill")) {
            int id = atoi(args[1]);
            killTask(&taskArr[id]);
        } else if (!strcmp(args[0], "sleep")) {
            sleepExecutor(atoi(args[1]));
        } else if (!strcmp(args[0], "quit")) {
            free_split_string(args);
            break;
        }

        pthread_mutex_lock(&canSendEndMessage);
        if (endedTasksInfoPos > 0) {
            printf("%s", endedTasksInfo);
            endedTasksInfo[0] = '\0';
            endedTasksInfoPos = 0;
        }
        canPrintf = true;
        pthread_mutex_unlock(&canSendEndMessage);
        free_split_string(args);
    }

    free(buffer);
    closeProgram(n_tasks, taskArr);
    for (int i = 0; i < n_tasks; i++) {
        free(taskArr[i].outMessage);
        free(taskArr[i].errMessage);
        pthread_mutex_destroy(&taskArr[i].outMutex);
        pthread_mutex_destroy(&taskArr[i].errMutex);
    }

    if (endedTasksInfoPos > 0) {
        printf("%s", endedTasksInfo);
        endedTasksInfo[0] = '\0';
        endedTasksInfoPos = 0;
    }

    pthread_mutex_destroy(&canSendEndMessage);
    free(taskArr);
    free(endedTasksInfo);
    return 0;
}