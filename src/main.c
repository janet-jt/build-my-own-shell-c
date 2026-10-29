#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

#define INPUT_SIZE 2048
#define MAX_ARGS 64
#define MAX_ARG_LEN 128

typedef enum {
  BUILTIN_ECHO,
  BUILTIN_EXIT,
  BUILTIN_TYPE,
  BUILTIN_PWD,
  BUILTIN_CD,
  BUILTIN_UNKNOWN
} Builtin;

typedef enum {
  TOKEN_WORD,
  TOKEN_STDOUT_REDIRECT,
  TOKEN_STDERR_REDIRECT
} TokenType;

typedef struct {
  TokenType type;
  char *text;
} Token;

enum redirection_mode{
  OVERWRITE,
  APPEND
};

typedef struct {
  int argc;
  enum redirection_mode stdout_mode;
  enum redirection_mode stderr_mode;
  char *stdout_output_file;
  char *stderr_output_file;
  char *argv[MAX_ARGS + 1];
} ParsedCommand;

enum quote_state {
  OUTSIDE,
  INSIDE
};

Builtin type_command(const char *cmd);
int tokenize(const char *input, Token tokens[], size_t capacity);
int parse_token(const Token tokens[], size_t token_count, ParsedCommand *command);
void echo_builtin(int argc, char *argv[]);
void type_builtin(const char *cmd);
void pwd_builtin();
void cd_builtin(const char *path);
char* search_path(const char *full_path);
void print_command_location(const char *cmd);
void run_program(char *argv[]);
int std_redirection(enum redirection_mode stdout_redirect_mode, enum redirection_mode stderr_redirect_mode, const char *stdout_output_file, const char *stderr_output_file);
void free_tokens(Token tokens[], size_t token_count);


int main(void) {
  setbuf(stdout, NULL);
  
  while (1) {  
    int argc, token_count;
    char input[INPUT_SIZE];
    int stdout_fd = dup(STDOUT_FILENO);
    int stderr_fd = dup(STDERR_FILENO);
    
    printf("$ ");
    fflush(stdout);

    if (fgets(input, sizeof input, stdin) == NULL ) {
      break;
    }

    size_t len = strcspn(input, "\r\n");
    input[len] = '\0';

    Token tokens[INPUT_SIZE];
    ParsedCommand command;

    token_count = tokenize(input, tokens, sizeof tokens / sizeof tokens[0]);  
    argc = parse_token(tokens, (size_t)token_count, &command);
    
    if (argc >= MAX_ARGS) {
      fprintf(stderr, "Error: Too many arguments (max %d).\n", MAX_ARGS);
      continue;
    }

    if (argc == 0) {
      continue;
    }

    if (argc == -1) {
      printf("> ");
      continue;
    }

    if (command.stdout_output_file != NULL || command.stderr_output_file != NULL) {
      std_redirection(command.stdout_mode, command.stderr_mode, command.stdout_output_file, command.stderr_output_file);
    }

    if (strncmp(command.argv[0], "exit" , 4) == 0) {
      free_tokens(tokens, token_count);
      break;
    }
    else if (strncmp(command.argv[0], "echo" , 4) == 0) {
      echo_builtin(command.argc, command.argv);
    }
    else if (strncmp(command.argv[0], "type" , 4) == 0) {
      if (argc == 1) {
        printf("usage: type command\n");
      } else {
        type_builtin(command.argv[1]);
      }
    }
    else if (strncmp(command.argv[0], "pwd", 3) == 0) {
      pwd_builtin();
    }
    else if (strncmp(command.argv[0], "cd", 2) == 0) {
      if (argc > 2) {
        fprintf(stderr, "cd: too many arguments\n");
      }
      else {
        cd_builtin(command.argv[1]);
      }
    }
    else {
      if (input[0] != '\0') {
        run_program(command.argv);
      }
    }

    if (dup2(stdout_fd, STDOUT_FILENO) == -1) {
      fprintf(stderr, "Failed to duplicate.\n");
      break;
    }

    if (dup2(stderr_fd, STDERR_FILENO) == -1) {
      fprintf(stderr, "Failed to duplicate.\n");
      break;
    }
  }
  
  return 0;
}

int tokenize(const char *input, Token tokens[], size_t capacity) {
  int token_count = 0;
  int arg_len = 0;
  int arg_started = 0;
  int escape_flag = 0;
  enum quote_state single_quote = OUTSIDE;
  enum quote_state double_quote = OUTSIDE;

  char current_arg[MAX_ARG_LEN];

  for(int i = 0; ; i++) {
    tokens[token_count].text = NULL;
    if ((size_t)token_count >= capacity) {
      return -1;
    }
    char c = input[i];
    if (c == '\0') {
      if (single_quote == OUTSIDE && double_quote == OUTSIDE && escape_flag == 0) {
        if (arg_started || arg_len > 0) {
          current_arg[arg_len] = '\0';
          tokens[token_count].type = TOKEN_WORD;
          tokens[token_count].text = strdup(current_arg);
          token_count++;
        }
        break;
      }
      else {
        return -1;
      }
    }
    else if (escape_flag) {
      if (single_quote == INSIDE) {
        current_arg[arg_len++] = c;
      }
      else if (double_quote == INSIDE) {
        if (c == '\\') {
          current_arg[arg_len++] = c;
        }
        else if (c == '"') {
          current_arg[arg_len++] = c;
        }
        else {
          current_arg[arg_len++] = '\\';
          current_arg[arg_len++] = c;
        }
      } else {
        current_arg[arg_len++] = c;
      }
      escape_flag = 0;
    }
    else if (c == '\\') {
      if (single_quote == INSIDE) {
        current_arg[arg_len++] = c;
        escape_flag = 0;
      } 
      else {
        escape_flag = 1;
        arg_started = 1;
      }
    }
    else if (c == ' ') {
      if (arg_started) {
        if (single_quote == INSIDE || double_quote == INSIDE || escape_flag) {
          if (arg_len < MAX_ARG_LEN - 1) {
              current_arg[arg_len++] = c;
              escape_flag = 0;
          }
        }
        else {
          current_arg[arg_len] = '\0';
          tokens[token_count].type = TOKEN_WORD;
          tokens[token_count].text = strdup(current_arg);
          arg_len = 0;
          arg_started = 0;
          token_count++;
        }
      }
    }
    else {
      if (c == '\'' && escape_flag == 0) {
        if (single_quote == INSIDE && double_quote == OUTSIDE) {
          single_quote = OUTSIDE;
        } 
        else if (single_quote == OUTSIDE && double_quote == OUTSIDE) {
          single_quote = INSIDE;
          arg_started = 1;
        }
        else {
          if (arg_len < MAX_ARG_LEN - 1) {
            current_arg[arg_len++] = c;
          }
          arg_started = 1;
        }
      } 
      else if (c == '\"' && escape_flag == 0) {
        if (single_quote == OUTSIDE && double_quote == INSIDE) {
          double_quote = OUTSIDE;
        } 
        else if (single_quote == OUTSIDE && double_quote == OUTSIDE) {
          double_quote = INSIDE;
          arg_started = 1;
        }
        else {
          if (arg_len < MAX_ARG_LEN - 1) {
            current_arg[arg_len++] = c;
          }
          arg_started = 1;
        }
      }
      else if (c == '>' && escape_flag == 0 && single_quote == OUTSIDE && double_quote == OUTSIDE) {    
        int std_stream = 0;  
        if (current_arg[0] == '1' && arg_len == 1) {
          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
          std_stream = 1;
        }

        if (current_arg[0] == '2' && arg_len == 1) {
          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
          std_stream = 2;
        }

        if (arg_started) {
          current_arg[arg_len] = '\0';

          tokens[token_count].type = TOKEN_WORD;
          tokens[token_count].text = strdup(current_arg);
          token_count++;

          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
        }

        if (std_stream == 1) {
          tokens[token_count].type = TOKEN_STDOUT_REDIRECT;
        }
        else if (std_stream == 2) {
          tokens[token_count].type = TOKEN_STDERR_REDIRECT;
        }
        else {
          tokens[token_count].type = TOKEN_STDOUT_REDIRECT;
        }
        
        tokens[token_count].text = NULL;
        token_count++;
      }
      else {
        if (arg_len < MAX_ARG_LEN - 1) {
            current_arg[arg_len++] = c;
        }
        arg_started = 1;
        escape_flag = 0;
      }
    }
  }
  
  return token_count;
}

int parse_token(const Token tokens[], size_t token_count, ParsedCommand *command) {
  int expecting_stdout_output_file = 0;
  int expecting_stderr_output_file = 0;
  int append_stdout_output_file = 0;
  int append_stderr_output_file = 0;

  command->argc = 0;
  command->argv[0] = NULL;
  command->stdout_mode = OVERWRITE;
  command->stderr_mode = OVERWRITE;
  command->stdout_output_file = NULL;
  command->stderr_output_file = NULL;

  for (size_t i = 0; i < token_count; i++) {
    const Token *token = &tokens[i];
    // printf("token->type: %d, token->text: %s\n", token->type, token->text);
    if (token->type == TOKEN_STDOUT_REDIRECT) {
      if(expecting_stdout_output_file && !append_stdout_output_file) {
        append_stdout_output_file = 1;
        command->stdout_mode = APPEND;
      }
      else if (expecting_stdout_output_file && append_stdout_output_file) {
        fprintf(stderr, "syntax error near unexpected token '>'\n");
        return -1;
      } else {
        expecting_stdout_output_file = 1;
        append_stdout_output_file = 0;
        command->stdout_mode = OVERWRITE;
      }

      continue;
    }

    if (token->type == TOKEN_STDERR_REDIRECT) {
      if(expecting_stderr_output_file && !append_stderr_output_file) {
        append_stderr_output_file = 1;
        command->stderr_mode = APPEND;
      }
      else if (expecting_stderr_output_file && append_stderr_output_file) {
        fprintf(stderr, "syntax error near unexpected token '>'\n");
        return -1;
      } else {
        expecting_stderr_output_file = 1;
        append_stderr_output_file = 0;
        command->stderr_mode = OVERWRITE;
      }

      continue;
    }

    if (expecting_stdout_output_file) {
      command->stdout_output_file = token->text;
      expecting_stdout_output_file = 0;
      append_stdout_output_file = 0;
    } 
    else if (expecting_stderr_output_file) {
      command->stderr_output_file = token->text;
      expecting_stderr_output_file = 0;
      append_stderr_output_file = 0;
    } 
    else {
      if (command->argc >= MAX_ARGS) {
        fprintf(stderr, "Error: Too many arguments (max %d).\n", MAX_ARGS);
        return -1;
      }

      command->argv[command->argc++] = token->text;
      command->argv[command->argc] = NULL;
    }

  }

  return command->argc;
}

Builtin type_command(const char *cmd) {
  if (strncmp(cmd, "echo", 4) == 0) return BUILTIN_ECHO;
  if (strncmp(cmd, "exit", 4) == 0) return BUILTIN_EXIT;
  if (strncmp(cmd, "type", 4) == 0) return BUILTIN_TYPE;
  if (strncmp(cmd, "pwd", 3) == 0) return BUILTIN_PWD;
  if (strncmp(cmd, "cd", 2) == 0) return BUILTIN_CD;
  return BUILTIN_UNKNOWN;
}

void echo_builtin(int argc, char *argv[]) {
  for (int i = 1; i < argc; i++){
    printf("%s", argv[i]);
    if (i < argc - 1) {
      printf(" ");
    }
  }
  printf("\n");
}

void type_builtin(const char *cmd) {
  switch (type_command(cmd)) {
    case BUILTIN_ECHO:
    case BUILTIN_EXIT:
    case BUILTIN_TYPE:
    case BUILTIN_PWD:
    case BUILTIN_CD:
      printf("%s is a shell builtin\n", cmd);
      break;
    case BUILTIN_UNKNOWN:
    default:
      print_command_location(cmd);
      break;
  }
}

void pwd_builtin() {
  char cwd[PATH_MAX];
  getcwd(cwd, sizeof(cwd));
  printf("%s\n", cwd);
}

void cd_builtin(const char *path) {
  int status;
  char *home_path, *cd_path;

  home_path = getenv("HOME");
  if (home_path == NULL) {
    home_path = "/";
  }

  if (path == NULL || (strcmp(path, "~") == 0)) {
    cd_path = strdup(home_path);
  }
  else if (strncmp(path, "~/", 2) == 0) {
    size_t home_size = strlen(home_path);
    size_t remain_size = strlen(path + 1);
    size_t total_size = home_size + remain_size + 1;

    cd_path = malloc(total_size);
    if (cd_path != NULL) {
      int length_needed = snprintf(cd_path, total_size, "%s%s", home_path, path + 1);

      if (length_needed < 0 || (size_t)length_needed >= total_size) {
        fprintf(stderr, "Buffer too small (or encoding error)\n");
      }
    }
  }
  else {
    cd_path = strdup(path);
  }

  if (cd_path == NULL) {
    fprintf(stderr, "Memory Allocation Failed\n");
    return;
  }
  
  status = chdir(cd_path);
  if (status == -1) {
    fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
    free(cd_path);
    return;
  }
  free(cd_path);
}
 
char* search_path(const char *cmd) {
  if (cmd == NULL || *cmd == '\0') {
    return NULL;
  }

  const char *raw_path;
  char *env_path, *path_ptr, *dir;
  char *found_path = NULL;
  
  raw_path = getenv("PATH");
  
  if (raw_path == NULL) {
    raw_path = "/usr/bin:/bin";
  }

  env_path = strdup(raw_path);

  if (env_path == NULL) {
    fprintf(stderr, "Memory Allocation Failed\n");
    return NULL;
  }

  path_ptr = env_path;

  while((dir = strsep(&path_ptr, ":")) != NULL) {

    if (*dir == '\0') {
      dir = ".";
    }

    char full_path[PATH_MAX];
    int length_needed = snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);

    if (length_needed < 0 || (size_t) length_needed >= sizeof full_path) {
      fprintf(stderr, "Buffer too small (or encoding error)\n");
      continue;
    }

    struct stat path_stat;
    if (access(full_path, X_OK) == 0 && stat(full_path, &path_stat) == 0) {
      if (S_ISREG(path_stat.st_mode)) {
        found_path = strdup(full_path);
        break;
      }
    }
    
  }
  
  free(env_path);
  return found_path;
}

void print_command_location(const char *cmd) {
  char *full_path = search_path(cmd);
  if (full_path == NULL) {
    printf("%s: not found\n", cmd);
  } else {
    printf("%s is %s\n", cmd, full_path);
    free(full_path);
  }
}

void run_program(char *argv[]) {
  char *full_path = search_path(argv[0]);
  if (full_path == NULL) {
    printf("%s: not found\n", argv[0]);
    return;
  }
  
  pid_t pid = fork();
  
  if (pid < 0) {
    perror("fork failed");
    free(full_path);
    return;
  }

  if (pid == 0) {
    execv(full_path, argv);
    perror("execv failed");
    free(full_path);
    exit(1);
  } else {
    int status;
    waitpid(pid, &status, 0);
    free(full_path);
  }
  
}

int std_redirection(enum redirection_mode stdout_redirect_mode, enum redirection_mode stderr_redirect_mode, const char *stdout_output_file, const char *stderr_output_file) {
  int stdout_output_file_fd, stderr_output_file_fd;

  if (stdout_output_file != NULL) {
    if (stdout_redirect_mode == OVERWRITE) {
      if ( (stdout_output_file_fd = open (stdout_output_file, O_WRONLY | O_CREAT | O_TRUNC, 0664)) == -1) {
        fprintf(stderr, "Failed to open file.\n");
        return -1;
      }
    } else {
      if ( (stdout_output_file_fd = open (stdout_output_file, O_WRONLY | O_CREAT | O_APPEND, 0664)) == -1) {
        fprintf(stderr, "Failed to open file.\n");
        return -1;
      }
    }

    if (dup2(stdout_output_file_fd, STDOUT_FILENO) == -1) {
      fprintf(stderr, "Failed to duplicate.\n");
      return -1;
    }

    close(stdout_output_file_fd);
  }

  if (stderr_output_file != NULL) {
    if(stderr_redirect_mode == OVERWRITE) {
      if ( (stderr_output_file_fd = open (stderr_output_file, O_WRONLY | O_CREAT | O_TRUNC, 0664)) == -1) {
        fprintf(stderr, "Failed to open file.\n");
        return -1;
      }
    }else {
      if ( (stderr_output_file_fd = open (stderr_output_file, O_WRONLY | O_CREAT | O_APPEND, 0664)) == -1) {
        fprintf(stderr, "Failed to open file.\n");
        return -1;
      }  
    }
    

    if (dup2(stderr_output_file_fd, STDERR_FILENO) == -1) {
      fprintf(stderr, "Failed to duplicate.\n");
      return -1;
    }

    close(stderr_output_file_fd);
  }
  
  return 0;
}

void free_tokens(Token tokens[], size_t token_count) {
  for (size_t i = 0; i < token_count; i++){
    free(tokens[i].text);
    tokens[i].text = NULL;
  }
}