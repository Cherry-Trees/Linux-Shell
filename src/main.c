#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdint.h>
#include <signal.h>

#define COMM_PIPE 8
#define COMM_BG 4
#define COMM_REDIR_IN 2
#define COMM_REDIR_OUT 1

char *token_str[6] = {
    "Undefined token",
    "End of input",
    "|",
    "&",
    "<"
    ">"
};

typedef enum {
    EOL,
    PIPE,
    BG,
    REDIR_IN,
    REDIR_OUT
} simple_token_t;

typedef enum {
    UNDEF,
    SIMPLE,
    ID
} token_tag_t;

typedef struct {
    char *begin;
    char *end;
} str_slice_t;

typedef struct {
    token_tag_t tag;
    union {
        simple_token_t simple;
        str_slice_t id;
    } data;
} token_t;

typedef struct {
    str_slice_t args[32];
    uint8_t arity;
    uint8_t flags;
    str_slice_t in;
    str_slice_t out;
} command_t;

size_t get(char *is, size_t idx, token_t *tok) {
    if (!is[idx]) {
        tok->tag = SIMPLE;
        tok->data.simple = EOL;
        return idx;
    }

    if (isspace(is[idx]))
        return get(is, idx + 1, tok);

    if (isalpha(is[idx]) || is[idx] == '.' || is[idx] == '-' || is[idx] == '\"') {
        tok->tag = ID;
        tok->data.id.begin = is + idx;

        while (isalpha(is[idx]) || is[idx] == '.' || is[idx] == '-' || is[idx] == '\"')
            idx++;

        tok->data.id.end = is + idx;
        return idx;
    }

    tok->tag = SIMPLE;
    switch (is[idx]) {
        case '|':
            tok->data.simple = PIPE;
            idx++;
            break;
        case '&':
            tok->data.simple = BG;
            idx++;
            break;
        case '<':
            tok->data.simple = REDIR_IN;
            idx++;
            break;
        case '>':
            tok->data.simple = REDIR_OUT;
            idx++;
            break;
        default:
            tok->tag = UNDEF;
            tok->data.id.begin = is + idx;

            while (is[idx] && !isspace(is[idx]))
                idx++;

            tok->data.id.end = is + idx;  
    }
    return idx;
}


void print_token(token_t *tok) {
    switch (tok->tag) {
        case SIMPLE:
            printf("%s", token_str[tok->data.simple]);
            break;
        default:
            for (char *c = tok->data.id.begin; c != tok->data.id.end; c++)
                printf("%c", *c);
    }
}

void error(char *msg, size_t idx, token_t *tok) {
    printf("<%zu>: %s: \"", idx, msg);
    print_token(tok);
    printf("\"\n");
}

size_t parse(char *is, size_t idx, command_t *comm) {
    comm->arity = 0;
    comm->flags = 0;

    token_t tok;
    idx = get(is, idx, &tok);
    while (tok.data.simple != EOL && !(comm->flags & COMM_PIPE)) {
        switch (tok.tag) {
            case SIMPLE:
                switch (tok.data.simple) {
                    case PIPE:
                        comm->flags |= COMM_PIPE;
                        break;
                    case BG:
                        comm->flags |= COMM_BG;
                        break;
                    case REDIR_IN:
                        comm->flags |= COMM_REDIR_IN;
                        idx = get(is, idx, &tok);
                        if (tok.tag != ID) {
                            error("Expected file name after \'<\'. Instead, got", idx, &tok);
                            while (tok.data.simple != EOL)
                                idx = get(is, idx, &tok);
                            return idx;
                        }
                        comm->in = tok.data.id;
                        break;
                    case REDIR_OUT:
                        comm->flags |= COMM_REDIR_OUT;
                        idx = get(is, idx, &tok);
                        if (tok.tag != ID) {
                            error("Expected file name after \'>\'. Instead, got", idx, &tok);
                            while (tok.data.simple != EOL)
                                idx = get(is, idx, &tok);
                            return idx;
                        }
                        comm->out = tok.data.id;
                        break;
                }
                break;
            case ID:
                comm->args[comm->arity] = tok.data.id;
                comm->arity++;
                break;
            case UNDEF:
                error("Unexpected token", idx, &tok);
                break;
        }
        if (!(comm->flags & COMM_PIPE))
            idx = get(is, idx, &tok);
    }
}

bool slccmp(str_slice_t *slc, const char *str) {
    size_t i = 0;
    for (char *c = slc->begin; c != slc->end; c++, i++) {
        if (*c != str[i])
            return false;
    }
    return true;
}

void slccpy(str_slice_t *slc, char *dest) {
    size_t i = 0;
    for (char *c = slc->begin; c != slc->end; c++, i++)
        dest[i] = *c;
}

void bg_handler(int) {
    pid_t pid = waitpid(0, NULL, WNOHANG);
    if (pid > 0)
        printf("[%d]\n", pid);
}

void execute(char *is, size_t idx, int pipe_in[2]) {
    command_t comm;
    idx = parse(is, idx, &comm);
    char **arg_buf = (char **) calloc(comm.arity, sizeof(char *));
    for (uint8_t i = 0; i < comm.arity; i++) {
        arg_buf[i] = (char *) calloc(32, sizeof(char));
        slccpy(comm.args + i, arg_buf[i]);
    }
    
    char buf[32];
    if (slccmp(comm.args + 0, "cd")) {
        chdir(arg_buf[0]);
    }

    int pipe_out[2];
    pipe(pipe_out);
    pid_t cpid = fork();
    if (!cpid) {
        
        if (pipe_in) {
            close(pipe_in[1]);
            dup2(pipe_in[0], fileno(stdin));
        }

        if (comm.flags & COMM_PIPE) {
            close(pipe_out[0]);
            dup2(pipe_out[1], fileno(stdout));
        } else {
            close(pipe_out[1]);
        }

        if (comm.flags & COMM_REDIR_IN) {
            slccpy(&comm.in, buf);
            FILE *in_fp = fopen(buf, "r");
            dup2(fileno(in_fp), fileno(stdin));
        }

        if (comm.flags & COMM_REDIR_OUT) {
            slccpy(&comm.out, buf);
            FILE *out_fp = fopen(buf, "w");
            dup2(fileno(out_fp), fileno(stdout));
        }

        execvp(arg_buf[0], arg_buf);
    }

    if (pipe_in) {
        close(pipe_in[0]);
        close(pipe_in[1]);
    }

    if (!(comm.flags & COMM_BG)) {
        waitpid(cpid, NULL, 0);
    } else {
        signal(SIGCHLD, bg_handler);
    }

    if (comm.flags & COMM_PIPE) {
        return execute(is, idx, pipe_out);
    } else {
        close(pipe_out[0]);
        close(pipe_out[1]);
    }
}

int main(int argc, char *argv[]) {
    char *line = NULL;
    size_t len = 0;

    while (1) {
        printf(">>> ");
        getline(&line, &len, stdin);
        execute(line, 0, NULL);
    }
    
    return 0;
}
