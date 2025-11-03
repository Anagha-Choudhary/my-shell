#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/types.h>

#define BUF_SIZE 1024
#define MAX_ARGS 64

char *cmds[MAX_ARGS];
int num_cmds = 0;

void sig_handler(int sig) // Signal handler for Ctrl+C and Ctrl+Z
{
    if (sig == SIGINT || sig == SIGTSTP) 
    {
        printf("\n");
        char dir[BUF_SIZE];
        if (getcwd(dir, sizeof(dir))) printf("%s$", dir);
        fflush(stdout);
    }
}

char **split_cmd(char *cmd) // Split a command string into argv array 
{
    static char *argv[MAX_ARGS];
    int argc = 0;
    char *copy = strdup(cmd), *token = strsep(&copy, " ");
    
    while (token && argc < MAX_ARGS - 1)
    {
        if (strlen(token) > 0) argv[argc++] = strdup(token);
        token = strsep(&copy, " ");
    }
    argv[argc] = NULL;
    free(copy);
    return argv;
}

int is_builtin(char **argv) // Handle built-in commands: cd and exit 
{
    if (!argv[0]) return 0;
    
    if (!strcmp(argv[0], "cd")) 
    {
        if (!argv[1]) chdir(getenv("HOME"));
        else if (!strcmp(argv[1], "..")) chdir("..");
        else if (chdir(argv[1]) != 0) perror("cd");
        return 1;
    }
    
    if (!strcmp(argv[0], "exit")) 
    {
        printf("Exiting shell...\n");
        exit(0);
    }
    return 0;
}

int parseInput(char *input) // Parse input line and detect command type (&&, ##, >, |, or single)
{
    num_cmds = 0;
    char *copy = strdup(input), *token;
    copy[strcspn(copy, "\n")] = 0;

    char delims[][3] = {"&&", "##", ">", "|"};
    int types[] = {1, 2, 3, 4};
    for (int i = 0; i < 4; i++) 
    {
        if (strstr(copy, delims[i])) 
        {
            token = strsep(&copy, delims[i]);
            while (token && num_cmds < MAX_ARGS - 1) 
            {
                if (strlen(token) > 0) 
                {
                    while (*token == ' ') token++;
                    char *end = token + strlen(token) - 1;
                    while (end > token && *end == ' ') *(end--) = 0;

                    if (*token) 
                    { // Ensure token is not empty after trimming
                        cmds[num_cmds++] = strdup(token);
                    }
                }
                token = strsep(&copy, delims[i]);
            }
            free(copy);
            return types[i];
        }
    }

    // Single command
    token = strsep(&copy, " ");
    while (token && num_cmds < MAX_ARGS - 1) 
    {
        if (strlen(token) > 0) cmds[num_cmds++] = strdup(token);
        token = strsep(&copy, " ");
    }
    free(copy);
    return 0;
}

void executeCommand() // Execute a single command
{
    if (!num_cmds) return;
    
    char *argv[MAX_ARGS];
    for (int i = 0; i <= num_cmds; i++) 
    {
        argv[i] = (i < num_cmds) ? cmds[i] : NULL;
    }
    if (is_builtin(argv)) return;
    
    pid_t pid = fork();
    if (pid == 0) 
    {
        if (execvp(argv[0], argv) == -1) 
        {
            printf("Shell: Incorrect command\n");
            exit(1);
        }
    } 
    else if (pid > 0) 
    {
        waitpid(pid, NULL, 0);
    } else perror("fork");
}

void executeParallelCommands() // Execute multiple commands in parallel (&&)
{
    pid_t pids[MAX_ARGS];
    int num_procs = 0;
    
    for (int i = 0; i < num_cmds; i++) 
    {
        char **argv = split_cmd(cmds[i]);
        if (is_builtin(argv)) continue;
        
        pid_t pid = fork();
        if (pid == 0) 
        {
            if (execvp(argv[0], argv) == -1) 
            {
                printf("Shell: Incorrect command\n");
                exit(1);
            }
        } 
        else if (pid > 0) 
        {
            pids[num_procs++] = pid;
        } 
        else perror("fork");
    }
    for (int i = 0; i < num_procs; i++) waitpid(pids[i], NULL, 0);
}

void executeSequentialCommands() // Execute multiple commands sequentially (##)
{
    for (int i = 0; i < num_cmds; i++) 
    {
        char **argv = split_cmd(cmds[i]);
        if (is_builtin(argv)) continue;
        
        pid_t pid = fork();
        if (pid == 0) {
            if (execvp(argv[0], argv) == -1) 
            {
                printf("Shell: Incorrect command\n");
                exit(1);
            }
        } 
        else if (pid > 0) 
        {
            waitpid(pid, NULL, 0);
        } else perror("fork");
    }
}

void executeCommandRedirection() // Execute a command with output redirection (>)
{
    if (num_cmds < 2) 
    {
        printf("Shell: Incorrect command\n");
        return;
    }
    
    char **argv = split_cmd(cmds[0]);
    char *outfile = cmds[1];
    pid_t pid = fork();
    if (pid == 0) 
    {
        int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1 || dup2(fd, STDOUT_FILENO) == -1) 
        {
            perror("redirection");
            exit(1);
        }
        close(fd);
        
        if (execvp(argv[0], argv) == -1) 
        {
            printf("Shell: Incorrect command\n");
            exit(1);
        }
    } 
    else if (pid > 0) 
    {
        waitpid(pid, NULL, 0);
    } else perror("fork");
}

void executePipeline() // Execute pipeline of commands (|)
{
    if (num_cmds < 2) 
    {
        printf("Shell: Incorrect command\n");
        return;
    }
    
    int pipes[2 * (num_cmds - 1)];
    pid_t pids[num_cmds];
    for (int i = 0; i < num_cmds - 1; i++) 
    {
        if (pipe(pipes + i * 2) == -1) 
        {
            perror("pipe");
            return;
        }
    }
    
    for (int i = 0; i < num_cmds; i++) 
    {
        char **argv = split_cmd(cmds[i]);
        pids[i] = fork();
        if (pids[i] == 0) 
        {
            if (i > 0) dup2(pipes[(i - 1) * 2], STDIN_FILENO);
            if (i < num_cmds - 1) dup2(pipes[i * 2 + 1], STDOUT_FILENO);
            
            for (int j = 0; j < 2 * (num_cmds - 1); j++) close(pipes[j]);
            
            if (execvp(argv[0], argv) == -1) 
            {
                printf("Shell: Incorrect command\n");
                exit(1);
            }
        } 
        else if (pids[i] < 0) 
        {
            perror("fork");
            return;
        }
    }
    for (int i = 0; i < 2 * (num_cmds - 1); i++) 
    {
        close(pipes[i]);
    }
    for (int i = 0; i < num_cmds; i++) 
    {
        waitpid(pids[i], NULL, 0);
    }
}

int main() 
{
    signal(SIGINT, sig_handler);
    signal(SIGTSTP, sig_handler);
    
    char *input = NULL;
    size_t len = 0;
    while (1) 
    {
        char cwd[BUF_SIZE];
        printf("%s$", getcwd(cwd, sizeof(cwd)) ? cwd : "unknown");
        
        if (getline(&input, &len, stdin) == -1) 
        {
            printf("\nExiting shell...\n");
            break;
        }
        
        if (strlen(input) <= 1) continue;
        
        int type = parseInput(input);
        if (num_cmds > 0 && !strcmp(cmds[0], "exit")) 
        {
            printf("Exiting shell...\n");
            break;
        }
        switch (type) 
        {
            case 1: executeParallelCommands(); 
                break;
            case 2: executeSequentialCommands(); 
                break;
            case 3: executeCommandRedirection(); 
                break;
            case 4: executePipeline(); 
                break;
            default: executeCommand(); 
                break;
        }
        for (int i = 0; i < num_cmds; i++) free(cmds[i]);
        num_cmds = 0;
    }
    free(input);
    return 0;
}