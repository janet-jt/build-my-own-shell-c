#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <dirent.h>

#define INPUT_SIZE 2048
#define MAX_ARGS 64
#define MAX_ARG_LEN 128
#define MAX_PIPE_LIMIT 32

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
  TOKEN_STDERR_REDIRECT,
  TOKEN_STDOUT_APPEND,
  TOKEN_STDERR_APPEND,
  TOKEN_PIPE
} TokenType;

typedef struct {
  TokenType type;
  char *text;
} Token;

enum redirection_mode{
  UNDEFINE,
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

static const char *builtin_commands[] = {
  "cd",
  "pwd",
  "echo",
  "exit",
  "type",
  NULL
};

Builtin type_command(const char *cmd);
int tokenize(const char *input, Token tokens[], size_t capacity);
int parse_token(const Token tokens[], size_t token_count, ParsedCommand commands[], size_t command_capacity);
int execute_command(ParsedCommand command);
static int is_first_command(int start);
char **shell_completion(const char *input, int start, int end);
char* dir_discovery();
static void free_candidates(char **items, size_t count);
static int add_candidates(char ***items, size_t *count, size_t *capacity, const char *name);
char *command_generator(const char *input, int state);
void echo_builtin(int argc, char *argv[]);
void type_builtin(const char *cmd);
void pwd_builtin();
void cd_builtin(const char *path);
char* search_path(const char *full_path);
void print_command_location(const char *cmd);
void run_program(char *argv[]);
int std_redirection(enum redirection_mode stdout_redirect_mode, 
                    enum redirection_mode stderr_redirect_mode, 
                    const char *stdout_output_file, 
                    const char *stderr_output_file);
int pipeline_executor(ParsedCommand commands[], size_t command_capacity);
void free_tokens(Token tokens[], size_t token_count);

int main(void) {
  setbuf(stdout, NULL);

  rl_attempted_completion_function = shell_completion;
  
  while (1) {  
    int token_count, executed_state;
    char *input;
    int stdin_fd = dup(STDIN_FILENO);
    int stdout_fd = dup(STDOUT_FILENO);
    int stderr_fd = dup(STDERR_FILENO);
    size_t command_capacity = 1;
    
    fflush(stdout);

    input = readline("$ ");
    if(input == NULL) {
      break;
    }

    if (input[0] == '\0') {
      free(input);
      continue;
    }

    add_history(input);

    Token tokens[INPUT_SIZE];
    
    token_count = tokenize(input, tokens, sizeof tokens / sizeof tokens[0]);  

    for (size_t i = 0; i < (size_t)token_count; i++) {
      const Token *token = &tokens[i];
      if(token->type == TOKEN_PIPE) {
        command_capacity++;
      }
    }

    if (command_capacity > MAX_PIPE_LIMIT) {
      free_tokens(tokens, token_count);
      fprintf(stderr, "maximum pipe limit exceeded\n");
      break;
    }

    ParsedCommand commands[command_capacity];

    int command_count = parse_token(tokens, (size_t)token_count, commands, command_capacity);

    if (command_count == 0) {
      continue;
    }

    if (command_count == -1) {
      continue;
    }

    if (command_count > 1) {
      executed_state = pipeline_executor(commands, (size_t)command_capacity);
    } else {
      executed_state = execute_command(commands[0]);
    }    

    if (executed_state == 1) {
      free_tokens(tokens, token_count);
      break;
    }

    if (executed_state == -1) {
      continue;
    }

    free_tokens(tokens, token_count);

    if (dup2(stdin_fd, STDIN_FILENO) == -1) {
      fprintf(stderr, "Failed to duplicate.\n");
      break;
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
        int is_append = (input[i + 1] == '>');
        int std_stream = 0;  

        if (arg_len == 1 && (current_arg[0] == '1' || current_arg[0] == '2')) {
          std_stream = (current_arg[0] == '2')
                          ? STDERR_FILENO
                          : STDOUT_FILENO;
          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
        }

        if (arg_started) {

          if((size_t)token_count >= capacity) {
            return -1;
          }

          current_arg[arg_len] = '\0';

          char *text = strdup(current_arg);

          tokens[token_count].type = TOKEN_WORD;
          tokens[token_count].text = text;
          token_count++;

          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
        }

        if((size_t)token_count >= capacity) {
          return -1;
        }

        if (std_stream == STDERR_FILENO) {
          tokens[token_count].type = is_append ? TOKEN_STDERR_APPEND : TOKEN_STDERR_REDIRECT;
        } else {
          tokens[token_count].type = is_append ? TOKEN_STDOUT_APPEND : TOKEN_STDOUT_REDIRECT;
        }
        
        tokens[token_count].text = NULL;
        token_count++;

        if (is_append) {
          i++;
        }
      }
      else if (c == '|' && escape_flag == 0 && single_quote == OUTSIDE && double_quote == OUTSIDE) {
        if (arg_started) {

          if((size_t)token_count >= capacity) {
            return -1;
          }

          current_arg[arg_len] = '\0';

          char *text = strdup(current_arg);

          tokens[token_count].type = TOKEN_WORD;
          tokens[token_count].text = text;
          token_count++;

          arg_len = 0;
          arg_started = 0;
          current_arg[0] = '\0';
        }

        if((size_t)token_count >= capacity) {
          return -1;
        }

        tokens[token_count].type = TOKEN_PIPE;
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

void initialize_command(ParsedCommand *command) {
  command->argc = 0;
  command->argv[0] = NULL;
  command->stdout_mode = UNDEFINE;
  command->stderr_mode = UNDEFINE;
  command->stdout_output_file = NULL;
  command->stderr_output_file = NULL;
}

int parse_token(const Token tokens[], size_t token_count, ParsedCommand commands[], size_t command_capacity) {
  if (token_count == 0) {
    return 0;
  }

  if (command_capacity == 0) {
    fprintf(stderr, "Out of capacity.\n");
    return -1;
  }
  int current_index = 0;
  initialize_command(&commands[current_index]);
  int expecting_stdout_output_file = 0;
  int expecting_stderr_output_file = 0;

  for (size_t i = 0; i < token_count; i++) {
    
    const Token *token = &tokens[i];

    // printf("token->type: %d, token->text: %s, STDMODE: %d, Outfile: %s\n", token->type, token->text, commands[current_index].stdout_mode, commands[current_index].stdout_output_file);

    if (token->type == TOKEN_PIPE) {
      if(expecting_stdout_output_file || expecting_stderr_output_file) {
        fprintf(stderr, "syntax error: expected redirection filename.\n");
        return -1;
      }

      if (commands[current_index].argc == 0) {
        fprintf(stderr, "syntax error: expected command before '|'.\n");
        return -1;
      }
      
      if ((size_t)current_index + 1 >= command_capacity) {
        fprintf(stderr, "Out of capacity");
        return -1;
      }

      current_index ++;
      initialize_command(&commands[current_index]);
      continue;
    }

    if (token->type == TOKEN_STDOUT_REDIRECT || token->type == TOKEN_STDOUT_APPEND) {
      if(expecting_stdout_output_file || expecting_stderr_output_file) {
        fprintf(stderr, "syntax error: expected redirection filename.\n");
        return -1;
      }
    
      commands[current_index].stdout_mode = (token->type == TOKEN_STDOUT_APPEND ? APPEND:OVERWRITE);
      expecting_stdout_output_file = 1;

      continue;
    }

    if (token->type == TOKEN_STDERR_REDIRECT || token->type == TOKEN_STDERR_APPEND) {
      if(expecting_stdout_output_file || expecting_stderr_output_file) {
        fprintf(stderr, "syntax error: expected redirection filename.\n");
        return -1;
      }
    
      commands[current_index].stderr_mode = (token->type == TOKEN_STDERR_APPEND ? APPEND:OVERWRITE);
      expecting_stderr_output_file = 1;

      continue;
    }

    if (token->type != TOKEN_WORD || token->text == NULL) {
      fprintf(stderr, "syntax error: expected word.\n");
      return -1;
    }

    if (expecting_stdout_output_file) {
      commands[current_index].stdout_output_file = token->text;
      expecting_stdout_output_file = 0;
    } 
    else if (expecting_stderr_output_file) {
      commands[current_index].stderr_output_file = token->text;
      expecting_stderr_output_file = 0;
    } 
    else {
      if (commands[current_index].argc >= MAX_ARGS) {
        fprintf(stderr, "Error: Too many arguments (max %d).\n", MAX_ARGS);
        return -1;
      }

      commands[current_index].argv[commands[current_index].argc++] = token->text;
      commands[current_index].argv[commands[current_index].argc] = NULL;
    }

  }

  if(expecting_stdout_output_file || expecting_stderr_output_file) {
    fprintf(stderr, "syntax error: expected redirection filename.\n");
    return -1;
  }

  if (commands[current_index].argc == 0) {
    fprintf(stderr, "syntax error: expected command.\n");
    return -1;
  }

  return current_index + 1;
}

int execute_command(ParsedCommand command) {
  if (command.stdout_output_file != NULL || command.stderr_output_file != NULL) {
    std_redirection(command.stdout_mode, command.stderr_mode, command.stdout_output_file, command.stderr_output_file);
  }

  if (strncmp(command.argv[0], "exit" , 4) == 0 && strlen(command.argv[0]) == 4) {
    return 1;
  }
  else if (strncmp(command.argv[0], "echo" , 4) == 0 && strlen(command.argv[0]) == 4) {
    echo_builtin(command.argc, command.argv);
  }
  else if (strncmp(command.argv[0], "type" , 4) == 0 && strlen(command.argv[0]) == 4) {
    if (command.argc == 1) {
      printf("usage: type command\n");
    } else {
      type_builtin(command.argv[1]);
    }
  }
  else if (strncmp(command.argv[0], "pwd", 3) == 0 && strlen(command.argv[0]) == 3) {
    pwd_builtin();
  }
  else if (strncmp(command.argv[0], "cd", 2) == 0 && strlen(command.argv[0]) == 2) {
    if (command.argc > 2) {
      fprintf(stderr, "cd: too many arguments\n");
      return -1;
    }
    else {
      cd_builtin(command.argv[1]);
    }
  }
  else {
    run_program(command.argv);
  }
  return 0;
}

static int is_first_command(int start) {
  for (int i = 0; i < start; i++) {
    if(!isspace((unsigned char)rl_line_buffer[i])) {
      return 0;
    }
  }
  return 1;
}

char **shell_completion(const char *input, int start, int end) {
  (void)end;
  char **matches;

  matches = (char **)NULL;

  if (start == 0 || is_first_command(start)) {
    matches = rl_completion_matches(input, command_generator);
    return (matches);
  }
  else {
    return rl_completion_matches(input, rl_filename_completion_function);
  }

  return NULL;
}

char* dir_discovery() {
  const char *path;
  path = getenv("PATH");
  
  if (path == NULL) {
    path = "/usr/bin:/bin";
  }

  return strdup(path);
}

static void free_candidates(char **items, size_t count) {
  for(size_t i = 0; i < count; i++) {
    free(items[i]);
  }

  free(items);
}

static int add_candidates(char ***items, size_t *count, size_t *capacity, const char *name) {
  for(size_t i = 0; i < *count; i++) {
    if(strcmp((*items)[i], name) == 0 ) {
      return 0;
    }
  }

  if(*count == *capacity){
    if(*capacity > SIZE_MAX / 2) {
      return -1;
    }
 
    size_t new_capacity = (*capacity ==0 ) ? 16 : *capacity * 2;

    if(new_capacity > SIZE_MAX / sizeof **items) {
      return -1;
    }

    char **new_items = realloc(*items, new_capacity * sizeof **items);

    if (new_items == NULL){
      return -1;
    }

    *items = new_items;
    *capacity = new_capacity;
  }

  char *copy = strdup(name);
  if(copy == NULL) {
    return -1;
  }

  (*items)[(*count)++] = copy;
  return 0;
}

char *command_generator(const char *input, int state) {
  static char **candidates = NULL;
  static size_t candidate_count = 0;
  static size_t candidate_capacity = 0;
  static size_t candidate_index = 0;
  
  if(state == 0) {
    char *env_path = NULL;
    DIR *current_dir = NULL;
    size_t input_len = strlen(input);

    free_candidates(candidates, candidate_count);
    candidates = NULL;
    candidate_count = 0;
    candidate_capacity = 0;
    candidate_index = 0;

    for(size_t i = 0; builtin_commands[i] != NULL; i++) {
      const char *name = builtin_commands[i];

      if(strncmp(name, input, input_len) == 0) {
        if(add_candidates(&candidates, &candidate_count, &candidate_capacity, name) == -1) {
          goto build_failed;
        }
      }
    }

    env_path = dir_discovery();
    if(env_path == NULL) {
      goto build_failed;
    }

    char *path_ptr = env_path;
    char *dir;

    while ((dir = strsep(&path_ptr, ":")) != NULL) {
      if(*dir == '\0') {
        dir = ".";
      }

      current_dir = opendir(dir);

      if(current_dir == NULL) {
        continue;
      }

      struct dirent *entry;

      while ((entry = readdir(current_dir)) != NULL) {
        const char *name = entry->d_name;

        if(strncmp(name, input, input_len) != 0) {
          continue;
        }

        size_t dir_len = strlen(dir);
        size_t name_len = strlen(name);

        if(dir_len > SIZE_MAX - name_len - 2){
          goto build_failed;
        }

        size_t path_size = dir_len + name_len + 2;
        char *full_path = malloc(path_size);

        if(full_path == NULL) {
          goto build_failed;
        }

        snprintf(full_path, path_size, "%s/%s", dir, name);

        struct stat file_stat;
        int is_executable = 
            stat(full_path, &file_stat) == 0 &&
            S_ISREG(file_stat.st_mode) &&
            access(full_path, X_OK) == 0;

        free(full_path);
        
        if(is_executable) {
          if (add_candidates(&candidates,
                            &candidate_count,
                            &candidate_capacity,
                            name) == -1) {
            goto build_failed;
          }
        }
      }

    
      closedir(current_dir);
      current_dir = NULL;
    }

    free(env_path);
    goto build_complete;

build_failed:
    if (current_dir != NULL) {
      closedir(current_dir);
    }

    free(env_path);
    free_candidates(candidates, candidate_count);

    candidates = NULL;
    candidate_count = 0;
    candidate_capacity = 0;
    candidate_index = 0;

    fprintf(stderr, "Failed to allocate completion candidates.\n");
    return NULL;

build_complete:
    ;
  }

  if(candidate_index < candidate_count) {
    char *match = strdup(candidates[candidate_index]);

    if(match != NULL) {
      candidate_index++;
      return match;
    }

    fprintf(stderr, "Falied to allocate completion match.\n");
  }

  free_candidates(candidates, candidate_count);

  candidates = NULL;
  candidate_count = 0;
  candidate_capacity = 0;
  candidate_index = 0;

  return NULL;
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

  raw_path = dir_discovery();

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
    if (execv(full_path, argv) == -1) {
      free(full_path);
      perror("execv failed");
      exit(EXIT_FAILURE);
    }
    free(full_path);
    exit(EXIT_SUCCESS);
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

int pipeline_executor(ParsedCommand commands[], size_t command_capacity) {
  int writer_status, reader_status;
  int writer_forked = 0;
  int reader_forked = 0;
  int pipe_fd[2];
  pid_t writer_pid, reader_pid;

  if (pipe(pipe_fd) == -1) {
    perror("pipe");
    return -1;
  }

  for(size_t i; i < command_capacity; i++) {

    if (writer_forked & reader_forked) {
      waitpid(writer_pid, &writer_status, 0);
      waitpid(reader_pid, &reader_status, 0);
    }
    
    if (i % 2 == 0) {
      writer_pid = fork();
      if (writer_pid == -1){
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
      }
      writer_forked = 1;
      if (writer_pid == 0) {
        close(pipe_fd[0]);
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[1]);

        int executed_state = execute_command(commands[i]);
        exit(executed_state == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
      }
    }
    else if (i % 2 != 0 && i != command_capacity - 1) {
      reader_pid = fork();
      reader_forked = 1;
      if (reader_pid == -1){
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
      }

      if (reader_pid == 0) {
        close(pipe_fd[1]);
        dup2(pipe_fd[0], STDIN_FILENO);
        close(pipe_fd[0]);
        
        int executed_state = execute_command(commands[i]);
        exit(executed_state == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
      }
    }

    if (i == command_capacity - 1) {
      reader_pid = fork();
      if (reader_pid == -1){
        perror("fork");
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return -1;
      }

      if (reader_pid == 0) {
        close(pipe_fd[1]);
        dup2(pipe_fd[0], STDIN_FILENO);
        close(pipe_fd[0]);
        
        int executed_state = execute_command(commands[i]);
        exit(executed_state == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
      }
    }

    close(pipe_fd[0]);
    close(pipe_fd[1]);
  }

  close(pipe_fd[0]);
  close(pipe_fd[1]);

  waitpid(writer_pid, &writer_status, 0);
  waitpid(reader_pid, &reader_status, 0);

  return 0;
}

void free_tokens(Token tokens[], size_t token_count) {
  for (size_t i = 0; i < token_count; i++){
    free(tokens[i].text);
    tokens[i].text = NULL;
  }
}