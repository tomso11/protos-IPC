#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <assert.h>
#include <signal.h>

#include "buffer.h"
#include "selector.h"

#define INITIAL_FDS 128

#define N(x) (sizeof(x) / sizeof((x)[0]))

#define ATTACHMENT(key) ((struct io_struct *)(key)->data)

static int pipeToChild[2] = {-1, -1};
static int pipeFromChild[2] = {-1, -1};
static int stdinFD = STDIN_FILENO;
static int stdoutFD = STDOUT_FILENO;

static bool done = false;

static void
sigterm_handler(const int signal)
{
    printf("signal %d, cleaning up and exiting\n", signal);
    done = true;
}

static void
parity(const uint8_t *ptr, const ssize_t n, uint8_t *parity)
{
    for (ssize_t i = 0; i < n; i++)
    {
        *parity ^= ptr[i];
    }
}

struct io_struct
{
    int *fd;

    struct buffer *buff;

    uint8_t *par;
};

int doread(int *fd, struct buffer *buff, uint8_t *par)
{
    uint8_t *ptr;
    ssize_t n;
    size_t count = 0;
    int ret = 0;

    ptr = buffer_write_ptr(buff, &count);
    n = read(*fd, ptr, count);
    if (n == 0 || n == -1)
    {
        *fd = -1;
        ret = -1;
    }
    else
    {
        if (NULL != par)
        {
            parity(ptr, n, par);
        }
        buffer_write_adv(buff, n);
    }

    return ret;
}

int dowrite(int *fd, struct buffer *buff, uint8_t *par)
{
    uint8_t *ptr;
    ssize_t n;
    size_t count = 0;
    int ret = 0;

    ptr = buffer_read_ptr(buff, &count);
    n = write(*fd, ptr, count);
    if (n == -1)
    {
        *fd = -1;
        ret = -1;
    }
    else
    {
        if (NULL != par)
        {
            parity(ptr, n, par);
        }
        buffer_read_adv(buff, n);

    }

    return ret;
}

static void
doreadHandler(struct selector_key *key)
{
    struct io_struct *ios = ATTACHMENT(key);
    doread(ios->fd, ios->buff, ios->par);
}

static void
doWriteHandler(struct selector_key *key)
{
    struct io_struct *ios = ATTACHMENT(key);
    dowrite(ios->fd, ios->buff, ios->par);
}

static int startParent()
{

    close(pipeToChild[0]);
    close(pipeFromChild[1]);

    pipeToChild[0] = pipeFromChild[1] = -1;

    uint8_t inParity = 0x00, outParity = 0x00;

    uint8_t buffin[4096], buffout[4096];
    buffer bin, bou;
    buffer_init(&bin, N(buffin), buffin);
    buffer_init(&bou, N(buffout), buffout);

    const char *err_msg = NULL;

    fd_selector selector = NULL;

    struct io_struct *ios_stdin = NULL;
    ios_stdin = malloc(sizeof(*ios_stdin));
    memset(ios_stdin, 0x00, sizeof(*ios_stdin));
    ios_stdin->fd = &stdinFD;
    ios_stdin->buff = &bin;
    ios_stdin->par = &inParity;

    struct io_struct *ios_read_from_child = NULL;
    ios_read_from_child = malloc(sizeof(*ios_read_from_child));
    memset(ios_read_from_child, 0x00, sizeof(*ios_read_from_child));
    ios_read_from_child->fd = &pipeFromChild[0];
    ios_read_from_child->buff = &bou;
    ios_read_from_child->par = NULL;

    struct io_struct *ios_write_to_child = NULL;
    ios_write_to_child = malloc(sizeof(*ios_write_to_child));
    memset(ios_write_to_child, 0x00, sizeof(*ios_write_to_child));
    ios_write_to_child->fd = &pipeToChild[1];
    ios_write_to_child->buff = &bin;
    ios_write_to_child->par = NULL;

    struct io_struct *ios_stdout = NULL;
    ios_stdout = malloc(sizeof(*ios_stdout));
    memset(ios_stdout, 0x00, sizeof(*ios_stdout));
    ios_stdout->fd = &stdoutFD;
    ios_stdout->buff = &bou;
    ios_stdout->par = &outParity;

    /* Config structure for selector indicating SIGALARM signal. */
    const struct selector_init conf = {
        .signal = SIGALRM,
        .select_timeout = {
            .tv_sec = 10,
            .tv_nsec = 0,
        },
    };

    /* Initiate selector. */
    selector_status ss = SELECTOR_SUCCESS;
    if (selector_init(&conf))
    {
        err_msg = "initializing selector";
        goto finally;
    }

    /* Get new selector with initial fds. */
    selector = selector_new(INITIAL_FDS);
    if (selector == NULL)
    {
        err_msg = "creating selector";
        goto finally;
    }

    /* Set fd handlers. */
    const struct fd_handler read_stdin_handler = {
        .handle_read = &doreadHandler,
        .handle_write = NULL,
        .handle_close = NULL};

    const struct fd_handler write_pipe_to_child_handler = {
        .handle_read = NULL,
        .handle_write = &doWriteHandler,
        .handle_close = NULL};

    const struct fd_handler read_pipe_from_child_handler = {
        .handle_read = &doreadHandler,
        .handle_write = NULL,
        .handle_close = NULL};

    const struct fd_handler write_stdout_handler = {
        .handle_read = NULL,
        .handle_write = &doWriteHandler,
        .handle_close = NULL};

    /* Set fd to non-blocking. */
    if (selector_fd_set_nio(stdinFD) == -1)
    {
        err_msg = "setting fd to non-blocking";
        goto finally;
    }

    if (selector_fd_set_nio(pipeToChild[1]) == -1)
    {
        err_msg = "setting fd to non-blocking";
        goto finally;
    }

    if (selector_fd_set_nio(pipeFromChild[0]) == -1)
    {
        err_msg = "setting fd to non-blocking";
        goto finally;
    }

    if (selector_fd_set_nio(stdoutFD) == -1)
    {
        err_msg = "setting fd to non-blocking";
        goto finally;
    }

    /* Register fds to selector. */
    selector_status ss_read_stdin = selector_register(selector, stdinFD,
                                                      &read_stdin_handler, OP_READ, ios_stdin);
    selector_status ss_write_pipe_to_child = selector_register(selector, pipeToChild[1],
                                                               &write_pipe_to_child_handler, OP_WRITE, ios_write_to_child);
    selector_status ss_read_pipe_from_child = selector_register(selector, pipeFromChild[0],
                                                                &read_pipe_from_child_handler, OP_READ, ios_read_from_child);
    selector_status ss_write_stdout = selector_register(selector, stdoutFD,
                                                        &write_stdout_handler, OP_WRITE, ios_stdout);

    // registrar sigterm es útil para terminar el programa normalmente.
    // esto ayuda mucho en herramientas como valgrind.
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    if (ss_read_stdin != SELECTOR_SUCCESS || ss_write_pipe_to_child != SELECTOR_SUCCESS || ss_read_pipe_from_child != SELECTOR_SUCCESS || ss_write_stdout != SELECTOR_SUCCESS)
    {
        err_msg = "registering fd";
        goto finally;
    }

    for (; !done;)
    {
        ss = selector_select(selector);

        if (ss != SELECTOR_SUCCESS)
        {
            break;
        }

        // If you can't read, do not write
        if (-1 == stdinFD && -1 != pipeToChild[1] && !buffer_can_read(&bin))
        {

            if (SELECTOR_SUCCESS != selector_unregister_fd(selector, pipeToChild[1]))
            {
                abort();
            }
            close(pipeToChild[1]);
            pipeToChild[1] = -1;

            // printf("%s\n", "stdinFD & pipeToChild[1] -> CLOSED");
        }

        if (-1 == pipeFromChild[0] && -1 != stdoutFD && !buffer_can_read(&bou))
        {
            if (SELECTOR_SUCCESS != selector_unregister_fd(selector, stdoutFD))
            {
                abort();
            }
            close(stdoutFD);
            stdoutFD = -1;

            // fprintf(stderr, "%s\n", "pipeFromChild[0] & stdoutFD -> CLOSED");
        }

        //     // Return if every operation is finished
        if (stdinFD == -1 && pipeFromChild[0] == -1 && pipeToChild[1] == -1 && stdoutFD == -1)
        {

            fprintf(stderr, "in  parity: 0x%02X\n", inParity);
            fprintf(stderr, "out parity: 0x%02X\n", outParity);
            break;
        }
    }

    int ret = 0;
finally:
    if (ss != SELECTOR_SUCCESS)
    {
        fprintf(stderr, "%s: %s\n", (err_msg == NULL) ? "" : err_msg,
                ss == SELECTOR_IO
                    ? strerror(errno)
                    : selector_error(ss));
        ret = 2;
    }
    else if (err_msg)
    {
        perror(err_msg);
        ret = 1;
    }
    if (selector != NULL)
    {
        selector_destroy(selector);
    }
    selector_close();

    free(ios_stdout);
    free(ios_stdin);
    free(ios_write_to_child);
    free(ios_read_from_child);

    wait(NULL);

    return ret;
}

static void startChild(char *command)
{
    // Close unused ends of pipes
    close(stdinFD);
    close(stdoutFD);

    stdinFD = stdoutFD = -1;

    close(pipeToChild[1]);
    close(pipeFromChild[0]);

    pipeToChild[1] = pipeFromChild[0] = -1;

    // Duplicate ends of pipes to stdin and stdout
    // Connect child stdin and stdout to parent pipes
    dup2(pipeToChild[0], fileno(stdin));
    dup2(pipeFromChild[1], fileno(stdout));

    // Build arguments for args. Note that bash is set as environment
    char *args[4];
    args[0] = "bash";
    args[1] = "-c";
    args[2] = command;
    args[3] = NULL;

    execvp(args[0], args);
}

static void setUpPipes()
{
    // Create pipes
    if (pipe(pipeToChild) == -1 || pipe(pipeFromChild) == -1)
    {
        perror("Could not build pipe.");
        exit(1);
    }
}

int main(int argc, char **argv)
{

    int pid;

    //Chequeamos que tenga todos los argumentos necesarios
    if (argc < 2)
    {
        fprintf(stderr, "Too few arguments\n");
        return EXIT_FAILURE;
    }

    //agregar para leer desde archivos

    setUpPipes();

    // Fork
    if ((pid = fork()) == -1)
    {
        perror("Could not fork.");
        exit(1);
    }

    if (pid == 0)
    {
        startChild(argv[1]);
    }
    else
    {
        startParent();
    }

    return 0;
}
