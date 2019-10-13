#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

#include <sys/stat.h>
#include <fcntl.h>

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

pid_t fgpid = NULL;

struct bgelem {
  pid_t bgp;
  struct bgelem* next;
};

struct bgelem* bglist;

int cmd_exit(struct tokens *tokens);
int cmd_help(struct tokens *tokens);
int cmd_pwd(struct tokens *tokens);
int cmd_cd(struct tokens *tokens);
int cmd_wait(struct tokens *tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens *tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t *fun;
  char *cmd;
  char *doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
  {cmd_help, "?", "show this help menu"},
  {cmd_exit, "exit", "exit the command shell"},
  {cmd_cd, "cd", "changes the current working directory"},
  {cmd_pwd, "pwd", "shows the current working directory"},
  {cmd_wait, "wait", "waits for all background jobs have terminated"},
};

int cmd_pwd(unused struct tokens *tokens) {
  char address[4096];
  if (getcwd(address, sizeof(address)) == NULL) {
    return 1;
  }
  printf("%s\n", address);
  return 0;
}

int cmd_cd(struct tokens *tokens) {
  char address[4096];
  if (tokens_get_length(tokens) < 2) {
    return 1;
  }
  char* input2 = tokens_get_token(tokens, 1);   
  strcpy(address, input2);
  chdir(input2);
  return 0;
}

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens *tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens *tokens) {
  exit(0);
}

int cmd_wait(unused struct tokens *tokens) {
  struct bgelem* cur = bglist;
  struct bgelem* temp;
  while (cur != NULL) {
    waitpid(cur->bgp,NULL,0);
    temp = cur;
    cur = cur->next;
    free(temp);
  }
  return 0;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

void stop_fg_process(int signum) {
  if (!fgpid) {
    return;
  } 
  killpg(fgpid, signum);
}


int main(unused int argc, unused char *argv[]) {
  init_shell();

  struct sigaction oldaction, newaction;
  
  newaction.sa_handler = stop_fg_process;
  sigemptyset(&newaction.sa_mask);
  newaction.sa_flags = 0;
  
  sigaction(SIGINT, &newaction, &oldaction);
 
  bglist = NULL;

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  fgpid = NULL;

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens *tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      /* REPLACE this to run commands as programs. */
      char* envpath = getenv("PATH"); 
      char* pathappend;
      static char result[4096];
      char* progname = tokens_get_token(tokens,0);
      int numargs = tokens_get_length(tokens);
      char** argv = (char**) malloc((sizeof(char*))*(numargs+1));
      int i = 0, fw = 0, j = 0;
      int out = 0, in = 0, bg = 0;
      char* outto = NULL;
      char* infrom = NULL;
      char* temp = NULL;
        for (i = 0; i < numargs; i++) {
	  temp = tokens_get_token(tokens,i);
	  if (out == 1) {
	    outto = temp;
	    out = 2;
	  } else if (in == 1) {
	    infrom = temp;
	    in = 2;
          } else if (strcmp(temp,"<") == 0) {
	    in = 1;
	  } else if (strcmp(temp,">") == 0) {
	    out = 1;
	  } else if (strcmp(temp,"&") == 0) {
	    bg = 1;
	    //printf("Process will be run in bg");
	    break;
	  } else {
	    *(argv+j) = temp;
	    j++;
	  }
        }
        *(argv+j) = NULL;
        char* env[] = {"", NULL};
      pid_t cur = fork();
      
      if (cur < 0) {
	free(argv); //Free argv when program call fails
        printf("Fork failed, program not called.");
      }
      if (cur == 0) {
	sigaction(SIGINT, &oldaction, NULL);
	setpgrp();
	if (out == 2) {
	  freopen(outto,"w",stdout);
	  out = 0;
	}
	if (in == 2) {
	  fw = open(infrom,O_RDONLY);
	  dup2(fw,0);
	  in = 0;
	}
	execve(progname, argv, env);
        pathappend = strtok(envpath, ":");
        strcpy(result, pathappend);
	strcat(result, "/");
        strcat(result, progname);
        while (pathappend != NULL) {
	  execve(result, argv, env);
	  pathappend = strtok(NULL, ":");
	  if (pathappend == NULL) {
	    break;
	  }
    	  strcpy(result, pathappend);
	  strcat(result, "/");
    	  strcat(result, progname);
	}
        perror((char*) NULL);
	exit(0);
	printf("Didn't exit properly");
      } else {
	free(argv); //Free argv in parent process to avoid memory leaks
	setpgid(cur,cur);
	//printf("\n %d is bg\n",bg);
	if (bg == 0) {
          //printf("Wait called");
	  fgpid = cur;
          int wstatus;
          waitpid(cur,&wstatus,0);
	} else if (bg == 1) { //Must put args for bg process in a list or do another way
	  //bg_proc_list
	  //printf("Not waiting");
	  struct bgelem* newbgelem = malloc(sizeof(struct bgelem));
	  newbgelem->bgp = cur;
	  newbgelem->next = bglist;
	  bglist = newbgelem;
	}
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}
