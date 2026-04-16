#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/*  Dynamic array (arraylist) for building argument lists              */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    int    len;
    int    cap;
} alist;

static void al_init(alist *a)
{
    a->cap   = 8;
    a->len   = 0;
    a->items = malloc(a->cap * sizeof(char *));
}

static void al_push(alist *a, char *s)
{
    if (a->len == a->cap) {
        a->cap *= 2;
        a->items = realloc(a->items, a->cap * sizeof(char *));
    }
    a->items[a->len++] = s;
}

static void al_free(alist *a)
{
    free(a->items);
    a->items = NULL;
    a->len = a->cap = 0;
}

/* ------------------------------------------------------------------ */
/*  Read a full line using read() – returns malloc'd string or NULL    */
/* ------------------------------------------------------------------ */

static char *read_line(int fd)
{
    char  *buf  = NULL;
    int    len  = 0;
    int    cap  = 0;
    char   c;
    ssize_t n;

    for (;;) {
        n = read(fd, &c, 1);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if (n == 0) {                       /* EOF */
            if (len == 0) { free(buf); return NULL; }
            break;
        }
        if (c == '\n') break;

        if (len == cap) {
            cap = cap ? cap * 2 : 64;
            buf = realloc(buf, cap + 1);
        }
        buf[len++] = c;
    }

    if (!buf) buf = malloc(1);
    buf[len] = '\0';
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Tokeniser – splits a line into tokens                             */
/*  >, <, | are always their own token                                */
/*  # starts a comment (rest of line ignored)                         */
/* ------------------------------------------------------------------ */

static char **tokenise(const char *line, int *ntokens)
{
    alist toks;
    al_init(&toks);

    const char *p = line;
    while (*p) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* comment – ignore rest */
        if (*p == '#') break;

        /* single-char tokens */
        if (*p == '<' || *p == '>' || *p == '|') {
            char *t = malloc(2);
            t[0] = *p; t[1] = '\0';
            al_push(&toks, t);
            p++;
            continue;
        }

        /* regular token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' &&
               *p != '<' && *p != '>' && *p != '|' && *p != '#')
            p++;
        size_t tlen = p - start;
        char *t = malloc(tlen + 1);
        memcpy(t, start, tlen);
        t[tlen] = '\0';
        al_push(&toks, t);
    }

    *ntokens = toks.len;
    al_push(&toks, NULL);          /* sentinel for execv-style usage */
    return toks.items;
}

static void free_tokens(char **tokens, int n)
{
    for (int i = 0; i < n; i++) free(tokens[i]);
    free(tokens);
}

/* ------------------------------------------------------------------ */
/*  Wildcard (glob) expansion                                         */
/* ------------------------------------------------------------------ */

/* Check if name matches pattern with a single '*'.                   */
static int glob_match(const char *pattern, const char *name)
{
    const char *star = strchr(pattern, '*');
    if (!star) return strcmp(pattern, name) == 0;

    size_t prefix_len = star - pattern;
    size_t suffix_len = strlen(star + 1);
    size_t name_len   = strlen(name);

    if (name_len < prefix_len + suffix_len) return 0;
    if (strncmp(pattern, name, prefix_len) != 0) return 0;
    if (strcmp(name + name_len - suffix_len, star + 1) != 0) return 0;

    /* patterns starting with * should not match hidden files */
    if (prefix_len == 0 && name[0] == '.') return 0;

    return 1;
}

/* Compare strings for qsort */
static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/*
 * Expand a single token that contains '*'.
 * Pushes matching names into `out`.  Returns number of matches.
 */
static int expand_wildcard(const char *token, alist *out)
{
    /* Separate directory portion from file-name pattern */
    const char *last_slash = strrchr(token, '/');
    char *dir_path  = NULL;
    const char *pattern;

    if (last_slash) {
        size_t dlen = last_slash - token;
        dir_path = malloc(dlen + 1);
        memcpy(dir_path, token, dlen);
        dir_path[dlen] = '\0';
        pattern = last_slash + 1;
    } else {
        dir_path = strdup(".");
        pattern  = token;
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        free(dir_path);
        return 0;
    }

    alist matches;
    al_init(&matches);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (glob_match(pattern, ent->d_name)) {
            char *full;
            if (last_slash) {
                size_t flen = strlen(dir_path) + 1 + strlen(ent->d_name) + 1;
                full = malloc(flen);
                snprintf(full, flen, "%s/%s", dir_path, ent->d_name);
            } else {
                full = strdup(ent->d_name);
            }
            al_push(&matches, full);
        }
    }
    closedir(d);
    free(dir_path);

    if (matches.len == 0) {
        al_free(&matches);
        return 0;
    }

    /* sort matches */
    qsort(matches.items, matches.len, sizeof(char *), cmpstr);

    for (int i = 0; i < matches.len; i++)
        al_push(out, matches.items[i]);

    int count = matches.len;
    free(matches.items);            /* items ownership transferred */
    return count;
}

/* ------------------------------------------------------------------ */
/*  Sub-command structure (one segment of a pipeline)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char **argv;       /* NULL-terminated argument list */
    int    argc;
    char  *infile;     /* input  redirection (NULL = default) */
    char  *outfile;    /* output redirection (NULL = default) */
} subcmd;

/* ------------------------------------------------------------------ */
/*  Parse tokens into an array of subcmds separated by |              */
/*  Returns number of subcmds, or -1 on syntax error.                 */
/* ------------------------------------------------------------------ */

static int parse_command(char **tokens, int ntok, subcmd **out)
{
    alist cmds;          /* array of subcmd (stored as pointers) */
    al_init(&cmds);

    alist args;
    al_init(&args);
    char *infile  = NULL;
    char *outfile = NULL;
    int   redir   = 0;   /* 0 = normal, '<' or '>' = expect filename */
    int   error   = 0;

    for (int i = 0; i < ntok; i++) {
        char *t = tokens[i];

        if (redir) {
            /* expecting a filename after < or > */
            if (strcmp(t, "<") == 0 || strcmp(t, ">") == 0 || strcmp(t, "|") == 0) {
                error = 1; break;
            }
            if (redir == '<') infile  = strdup(t);
            else              outfile = strdup(t);
            redir = 0;
            continue;
        }

        if (strcmp(t, "<") == 0) { redir = '<'; continue; }
        if (strcmp(t, ">") == 0) { redir = '>'; continue; }

        if (strcmp(t, "|") == 0) {
            if (args.len == 0) { error = 1; break; }
            /* finish current subcmd */
            al_push(&args, NULL);
            subcmd *sc = malloc(sizeof(subcmd));
            sc->argv    = args.items;
            sc->argc    = args.len - 1;
            sc->infile  = infile;
            sc->outfile = outfile;
            al_push(&cmds, (char *)sc);
            al_init(&args);
            infile = outfile = NULL;
            continue;
        }

        /* wildcard expansion */
        if (strchr(t, '*')) {
            alist expanded;
            al_init(&expanded);
            int n = expand_wildcard(t, &expanded);
            if (n > 0) {
                for (int j = 0; j < expanded.len; j++)
                    al_push(&args, expanded.items[j]);
                free(expanded.items);
            } else {
                al_free(&expanded);
                al_push(&args, strdup(t));
            }
        } else {
            al_push(&args, strdup(t));
        }
    }

    if (redir) error = 1;          /* trailing < or > */

    if (error) {
        /* cleanup */
        for (int i = 0; i < args.len; i++) free(args.items[i]);
        al_free(&args);
        for (int i = 0; i < cmds.len; i++) {
            subcmd *sc = (subcmd *)cmds.items[i];
            for (int j = 0; j < sc->argc; j++) free(sc->argv[j]);
            free(sc->argv);
            free(sc->infile);
            free(sc->outfile);
            free(sc);
        }
        al_free(&cmds);
        free(infile); free(outfile);
        *out = NULL;
        return -1;
    }

    if (args.len == 0 && cmds.len > 0) {
        /* trailing pipe with no command after it */
        for (int i = 0; i < cmds.len; i++) {
            subcmd *sc = (subcmd *)cmds.items[i];
            for (int j = 0; j < sc->argc; j++) free(sc->argv[j]);
            free(sc->argv);
            free(sc->infile);
            free(sc->outfile);
            free(sc);
        }
        al_free(&cmds);
        al_free(&args);
        free(infile); free(outfile);
        *out = NULL;
        return -1;
    }

    if (args.len > 0) {
        al_push(&args, NULL);
        subcmd *sc = malloc(sizeof(subcmd));
        sc->argv    = args.items;
        sc->argc    = args.len - 1;
        sc->infile  = infile;
        sc->outfile = outfile;
        al_push(&cmds, (char *)sc);
    } else {
        al_free(&args);
    }

    /* build output array */
    int ncmds = cmds.len;
    if (ncmds == 0) {
        al_free(&cmds);
        *out = NULL;
        return 0;
    }
    subcmd *arr = malloc(ncmds * sizeof(subcmd));
    for (int i = 0; i < ncmds; i++) {
        subcmd *sc = (subcmd *)cmds.items[i];
        arr[i] = *sc;
        free(sc);
    }
    al_free(&cmds);
    *out = arr;
    return ncmds;
}

static void free_subcmds(subcmd *cmds, int n)
{
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < cmds[i].argc; j++) free(cmds[i].argv[j]);
        free(cmds[i].argv);
        free(cmds[i].infile);
        free(cmds[i].outfile);
    }
    free(cmds);
}

/* ------------------------------------------------------------------ */
/*  Path search for bare names                                        */
/* ------------------------------------------------------------------ */

static const char *search_dirs[] = {
    "/usr/local/bin",
    "/usr/bin",
    "/bin",
    NULL
};

/* Returns malloc'd path or NULL */
static char *find_program(const char *name)
{
    for (int i = 0; search_dirs[i]; i++) {
        size_t plen = strlen(search_dirs[i]) + 1 + strlen(name) + 1;
        char *path = malloc(plen);
        snprintf(path, plen, "%s/%s", search_dirs[i], name);
        if (access(path, X_OK) == 0)
            return path;
        free(path);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Built-in detection                                                 */
/* ------------------------------------------------------------------ */

static int is_builtin(const char *name)
{
    return (strcmp(name, "cd")    == 0 ||
            strcmp(name, "pwd")   == 0 ||
            strcmp(name, "which") == 0 ||
            strcmp(name, "exit")  == 0);
}

/* ------------------------------------------------------------------ */
/*  Execute a built-in command.  Returns 0 on success, 1 on failure.  */
/*  Sets *do_exit to 1 if exit was invoked.                           */
/*  stdout_fd is the fd to write output to (for redirection/pipe).    */
/* ------------------------------------------------------------------ */

static int exec_builtin(subcmd *sc, int stdout_fd, int *do_exit)
{
    const char *cmd = sc->argv[0];

    if (strcmp(cmd, "exit") == 0) {
        *do_exit = 1;
        return 0;
    }

    if (strcmp(cmd, "cd") == 0) {
        const char *dir;
        if (sc->argc == 1) {
            dir = getenv("HOME");
            if (!dir) {
                write(STDERR_FILENO, "cd: HOME not set\n", 17);
                return 1;
            }
        } else if (sc->argc == 2) {
            dir = sc->argv[1];
        } else {
            write(STDERR_FILENO, "cd: too many arguments\n", 23);
            return 1;
        }
        if (chdir(dir) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "pwd") == 0) {
        char cwd[4096];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            perror("pwd");
            return 1;
        }
        write(stdout_fd, cwd, strlen(cwd));
        write(stdout_fd, "\n", 1);
        return 0;
    }

    if (strcmp(cmd, "which") == 0) {
        if (sc->argc != 2) return 1;
        const char *name = sc->argv[1];
        /* built-in names → fail */
        if (is_builtin(name)) return 1;
        char *path = find_program(name);
        if (!path) return 1;
        write(stdout_fd, path, strlen(path));
        write(stdout_fd, "\n", 1);
        free(path);
        return 0;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Resolve the executable path for a subcmd.                         */
/*  Returns malloc'd string or NULL on failure (prints error).        */
/* ------------------------------------------------------------------ */

static char *resolve_path(const char *name)
{
    if (strchr(name, '/')) {
        if (access(name, X_OK) != 0) {
            char msg[1024];
            int n = snprintf(msg, sizeof(msg), "%s: ", name);
            write(STDERR_FILENO, msg, n);
            const char *e = strerror(errno);
            write(STDERR_FILENO, e, strlen(e));
            write(STDERR_FILENO, "\n", 1);
            return NULL;
        }
        return strdup(name);
    }
    char *path = find_program(name);
    if (!path) {
        char msg[1024];
        int n = snprintf(msg, sizeof(msg), "%s: command not found\n", name);
        write(STDERR_FILENO, msg, n);
        return NULL;
    }
    return path;
}

/* ------------------------------------------------------------------ */
/*  Execute a full command (possibly a pipeline).                      */
/*  Returns: 0 = success, 1 = failure.  Sets *do_exit if exit seen.   */
/* ------------------------------------------------------------------ */

static int execute(subcmd *cmds, int ncmds, int interactive, int *do_exit)
{
    if (ncmds == 0) return 0;

    /* ---- single command, might be a built-in ---- */
    if (ncmds == 1 && is_builtin(cmds[0].argv[0])) {
        int out_fd = STDOUT_FILENO;
        int opened_out = 0;

        if (cmds[0].outfile) {
            out_fd = open(cmds[0].outfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
            if (out_fd < 0) {
                perror(cmds[0].outfile);
                return 1;
            }
            opened_out = 1;
        }

        int ret = exec_builtin(&cmds[0], out_fd, do_exit);

        if (opened_out) close(out_fd);
        return ret;
    }

    /* ---- pipeline: create pipes ---- */
    int npipes = ncmds - 1;
    int (*pipes)[2] = NULL;
    if (npipes > 0) {
        pipes = malloc(npipes * sizeof(int[2]));
        for (int i = 0; i < npipes; i++) {
            if (pipe(pipes[i]) < 0) {
                perror("pipe");
                /* close already-created pipes */
                for (int j = 0; j < i; j++) {
                    close(pipes[j][0]);
                    close(pipes[j][1]);
                }
                free(pipes);
                return 1;
            }
        }
    }

    pid_t *pids = malloc(ncmds * sizeof(pid_t));
    int has_exit = 0;       /* track if exit is part of pipeline */

    for (int i = 0; i < ncmds; i++) {
        /* check for exit in pipeline */
        if (strcmp(cmds[i].argv[0], "exit") == 0) {
            has_exit = 1;
        }

        /* For builtins in a pipeline, we fork a child to run them */
        int builtin = is_builtin(cmds[i].argv[0]);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            pids[i] = -1;
            continue;
        }

        if (pid == 0) {
            /* ---- child process ---- */

            /* set up pipe connections */
            if (i > 0) {
                dup2(pipes[i - 1][0], STDIN_FILENO);
            } else if (cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) { perror(cmds[i].infile); _exit(1); }
                dup2(fd, STDIN_FILENO);
                close(fd);
            } else if (!interactive && i == 0) {
                /* batch mode: default stdin is /dev/null */
                int fd = open("/dev/null", O_RDONLY);
                if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
            }

            if (i < npipes) {
                dup2(pipes[i][1], STDOUT_FILENO);
            } else if (cmds[i].outfile) {
                int fd = open(cmds[i].outfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
                if (fd < 0) { perror(cmds[i].outfile); _exit(1); }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            /* close all pipe fds */
            for (int j = 0; j < npipes; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (builtin) {
                int dummy = 0;
                int ret = exec_builtin(&cmds[i], STDOUT_FILENO, &dummy);
                _exit(ret);
            }

            char *path = resolve_path(cmds[i].argv[0]);
            if (!path) _exit(1);
            execv(path, cmds[i].argv);
            perror(cmds[i].argv[0]);
            _exit(1);
        }

        pids[i] = pid;
    }

    /* parent: close all pipe fds */
    for (int i = 0; i < npipes; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    free(pipes);

    /* wait for all children */
    int last_status = 0;
    for (int i = 0; i < ncmds; i++) {
        if (pids[i] <= 0) continue;
        int status;
        waitpid(pids[i], &status, 0);
        if (i == ncmds - 1) {
            /* success/failure determined by last command in pipeline */
            if (WIFEXITED(status)) {
                last_status = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                last_status = 128 + WTERMSIG(status);
            }
        }
    }

    free(pids);

    if (has_exit) *do_exit = 1;

    return last_status;
}

/* ------------------------------------------------------------------ */
/*  Prompt helpers                                                     */
/* ------------------------------------------------------------------ */

static void print_prompt(void)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        write(STDOUT_FILENO, "mysh$ ", 6);
        return;
    }

    const char *home = getenv("HOME");
    char prompt[4200];

    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        const char *after = cwd + strlen(home);
        if (*after == '\0') {
            snprintf(prompt, sizeof(prompt), "~$ ");
        } else if (*after == '/') {
            snprintf(prompt, sizeof(prompt), "~%s$ ", after);
        } else {
            snprintf(prompt, sizeof(prompt), "%s$ ", cwd);
        }
    } else {
        snprintf(prompt, sizeof(prompt), "%s$ ", cwd);
    }
    write(STDOUT_FILENO, prompt, strlen(prompt));
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int fd;
    int interactive;

    if (argc > 2) {
        write(STDERR_FILENO, "Usage: mysh [script]\n", 21);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }
        interactive = 0;
    } else {
        fd = STDIN_FILENO;
        interactive = isatty(STDIN_FILENO);
    }

    if (interactive) {
        const char *welcome = "Welcome to my shell!\n";
        write(STDOUT_FILENO, welcome, strlen(welcome));
    }

    int last_status = 0;  /* track exit status for interactive feedback */

    for (;;) {
        if (interactive) {
            /* print exit status of previous command */
            if (last_status > 0 && last_status < 128) {
                char msg[64];
                int n = snprintf(msg, sizeof(msg), "Exited with status %d\n", last_status);
                write(STDERR_FILENO, msg, n);
            } else if (last_status >= 128) {
                int sig = last_status - 128;
                const char *sigstr = strsignal(sig);
                char msg[256];
                int n;
                if (sigstr) {
                    n = snprintf(msg, sizeof(msg), "Terminated by signal %d: %s\n", sig, sigstr);
                } else {
                    n = snprintf(msg, sizeof(msg), "Terminated by signal %d\n", sig);
                }
                write(STDERR_FILENO, msg, n);
            }
            print_prompt();
        }

        char *line = read_line(fd);
        if (!line) break;                   /* EOF */

        /* tokenise */
        int ntok;
        char **tokens = tokenise(line, &ntok);
        free(line);

        if (ntok == 0) {
            free_tokens(tokens, ntok);
            continue;
        }

        /* handle conditionals: then / else */
        int conditional = 0;  /* 0=none, 1=then, 2=else */
        int skip_start = 0;
        if (strcmp(tokens[0], "then") == 0) {
            conditional = 1;
            skip_start = 1;
        } else if (strcmp(tokens[0], "else") == 0) {
            conditional = 2;
            skip_start = 1;
        }

        if (conditional == 1 && last_status != 0) {
            /* then: previous failed, skip this command */
            free_tokens(tokens, ntok);
            continue;
        }
        if (conditional == 2 && last_status == 0) {
            /* else: previous succeeded, skip this command */
            free_tokens(tokens, ntok);
            continue;
        }

        /* strip the then/else token before parsing */
        int parse_ntok = ntok - skip_start;
        char **parse_tokens = tokens + skip_start;

        if (parse_ntok == 0) {
            free_tokens(tokens, ntok);
            continue;
        }

        /* parse into subcmds */
        subcmd *cmds = NULL;
        int ncmds = parse_command(parse_tokens, parse_ntok, &cmds);
        free_tokens(tokens, ntok);

        if (ncmds < 0) {
            /* syntax error */
            last_status = 1;
            continue;
        }
        if (ncmds == 0) {
            last_status = 0;
            continue;
        }

        int do_exit = 0;
        last_status = execute(cmds, ncmds, interactive, &do_exit);
        free_subcmds(cmds, ncmds);

        if (do_exit) break;
    }

    if (fd != STDIN_FILENO) close(fd);

    if (interactive) {
        const char *goodbye = "mysh: exiting\n";
        write(STDOUT_FILENO, goodbye, strlen(goodbye));
    }

    return EXIT_SUCCESS;
}
