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

#define ATTACHMENT(key) ((buffer *)(key)->data)

static int pipeToChild[2] = {-1, -1};
static int pipeFromChild[2] = {-1, -1};
static int stdinFD = STDIN_FILENO;
static int stdoutFD = STDOUT_FILENO;

static uint8_t inParity = 0x00, outParity = 0x00;

static bool done = false;

static void
sigterm_handler(const int signal)
{
    printf("signal %d, cleaning up and exiting\n", signal);
    done = true;
}

static uint8_t calcParity(uint8_t *ptr, ssize_t size)
{
    uint8_t parity = 0x00;

    for (ssize_t i = 0; i < size; i++)
    {
        parity ^= ptr[i];
    }

    return parity;
}

static void handleReadStdIn(struct selector_key *key)
{

    printf("llegué a handleReadStdIn\n");

    buffer *buff = ATTACHMENT(key);

    ssize_t bytesRead = 0;

    size_t write_bytes[1] = {0};

    uint8_t *ptr = buffer_write_ptr(buff, write_bytes);

    bytesRead = read(stdinFD, ptr, *write_bytes);

    if (bytesRead > 0)
    {

        inParity ^= calcParity(ptr, bytesRead);

        buffer_write_adv(buff, bytesRead);

        printf("Read from stdin: %s\n", ptr);
    }
    else if (bytesRead == 0)
    {

        buffer_write_adv(buff, 0);

        if (SELECTOR_SUCCESS != selector_unregister_fd(key->s, stdinFD))
        {
            fprintf(stderr, "aborto handleReadStdIn\n");
            abort();
        }
        close(stdinFD);
        stdinFD = -1;

        fprintf(stderr, "me fui de handleReadStdIn\n");
    }
    else
    {
        selector_unregister_fd(key->s, stdinFD);
        close(stdinFD);
        stdinFD = -1;

        perror("StdIn read failed");
    }
}

static int cut = 0;
static void handleWriteStdOut(struct selector_key *key)
{

    buffer *buff = ATTACHMENT(key);

    ssize_t bytesWritten = 0;

    size_t read_bytes[1] = {0};

    uint8_t *ptr = buffer_read_ptr(buff, read_bytes);

    bytesWritten = write(stdoutFD, ptr, *read_bytes);

    if (bytesWritten > 0)
    {
        buffer_read_adv(buff, bytesWritten);
    }
    else if (bytesWritten == 0)
    {
        buffer_read_adv(buff, 0);
    }
    else
    {

        selector_unregister_fd(key->s, stdoutFD);
        close(stdoutFD);
        stdoutFD = -1;

        perror("StdOut write failed");
    }

    cut += 1;
}

static void handleReadPipeFromChild(struct selector_key *key)
{
    fprintf(stderr, "llegué a handleReadPipeFromChild\n");

    buffer *buff = ATTACHMENT(key);

    size_t bytes[1] = {0};

    ssize_t bytesRead = 0;

    uint8_t *ptr = buffer_write_ptr(buff, bytes);

    // Read response from child
    bytesRead = read(pipeFromChild[0], ptr, *bytes);

    if (bytesRead > 0)
    {

        outParity ^= calcParity(ptr, bytesRead);
        fprintf(stderr, "handleReadPipeFromChild ptr: %s - bR: %d\n", ptr, bytesRead);

        buffer_write_adv(buff, bytesRead);

        printf("Read from child: %s\n", ptr);
    }
    else if (bytesRead == 0)
    {

        buffer_write_adv(buff, 0);

        selector_unregister_fd(key->s, pipeFromChild[0]);
        close(pipeFromChild[0]);
        pipeFromChild[0] = -1;

        fprintf(stderr, "me fui de handleReadPipeFromChild\n");
    }
    else
    {
        selector_unregister_fd(key->s, pipeFromChild[0]);
        close(pipeFromChild[0]);
        pipeFromChild[0] = -1;

        perror("ReadPipeFromChild read failed");
    }
}

static void handleWritePipeToChild(struct selector_key *key)
{

    printf("llegué a handleWritePipeToChild\n");

    buffer *buff = ATTACHMENT(key);

    ssize_t bytesWritten = 0;

    size_t read_bytes[1] = {0};

    uint8_t *ptr = buffer_read_ptr(buff, read_bytes);

    fprintf(stderr, "handleWritePipeToChild ptr: %s - read_bytes: %d \n", ptr, read_bytes[0]);

    bytesWritten = write(pipeToChild[1], ptr, *read_bytes);

    if (bytesWritten > 0)
    {

        printf("Wrote to child: %s \n", ptr);

        buffer_read_adv(buff, bytesWritten);

        fprintf(stderr, "handleWritePipeToChild bytesWritten: %d\n", bytesWritten);
        fprintf(stderr, "handleWritePipeToChild buff: %s\n", buff->data);
    }
    else if (bytesWritten == 0)
    {

        buffer_read_adv(buff, bytesWritten);

        fprintf(stderr, "handleWritePipeToChild buff bW=0: %s\n", buff->data);

        // selector_unregister_fd(key->s, pipeToChild[1]);
        // close(pipeToChild[1]);
        // pipeToChild[1] = -1;

        fprintf(stderr, "me fui de handleWritePipeToChild\n");
    }
    else
    {
        buffer_read_adv(buff, bytesWritten);
        selector_unregister_fd(key->s, pipeToChild[1]);
        close(pipeToChild[1]);
        pipeToChild[1] = -1;

        perror("WritePipeToChild read failed");
    }
}

static void FDSETWithMax(int fd, fd_set *fds, int *max)
{
    FD_SET(fd, fds);

    if (fd > *max)
    {
        *max = fd;
    }
}

static int startParent()
{

    close(pipeToChild[0]);
    close(pipeFromChild[1]);

    pipeToChild[0] = pipeFromChild[1] = -1;

    int n;

    uint8_t buffin[4096], buffout[4096];
    buffer bin, bou;
    buffer_init(&bin, N(buffin), buffin);
    buffer_init(&bou, N(buffout), buffout);

    // const char *error_msg;

    fd_selector selector;

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
        fprintf(stderr, "Error: initializing selector.");
        return 1;
    }

    /* Get new selector with initial fds. */
    selector = selector_new(INITIAL_FDS);
    if (selector == NULL)
    {
        fprintf(stderr, "Error: creating selector.");
        selector_close();
        return 1;
    }

    /* Set fd handlers. */
    const struct fd_handler read_stdin_handler = {
        .handle_read = &handleReadStdIn,
        .handle_write = NULL,
        .handle_close = NULL};

    const struct fd_handler write_pipe_to_child_handler = {
        .handle_read = NULL,
        .handle_write = &handleWritePipeToChild,
        .handle_close = NULL};

    const struct fd_handler read_pipe_from_child_handler = {
        .handle_read = &handleReadPipeFromChild,
        .handle_write = NULL,
        .handle_close = NULL};

    const struct fd_handler write_stdout_handler = {
        .handle_read = NULL,
        .handle_write = &handleWriteStdOut,
        .handle_close = NULL};

    if (selector_fd_set_nio(stdinFD) == -1)
    {
        close(stdinFD);
        selector_destroy(selector);
        selector_close();
        return 1;
    }

    if (selector_fd_set_nio(pipeToChild[1]) == -1)
    {
        close(pipeToChild[1]);
        selector_destroy(selector);
        selector_close();
        return 1;
    }

    if (selector_fd_set_nio(pipeFromChild[0]) == -1)
    {
        close(pipeFromChild[0]);
        selector_destroy(selector);
        selector_close();
        return 1;
    }

    if (selector_fd_set_nio(stdoutFD) == -1)
    {
        close(stdoutFD);
        selector_destroy(selector);
        selector_close();
        return 1;
    }

    /* Register fds to selector. */
    selector_status ss_read_stdin = selector_register(selector, stdinFD,
                                                      &read_stdin_handler, OP_READ, &bin);
    selector_status ss_write_pipe_to_child = selector_register(selector, pipeToChild[1],
                                                               &write_pipe_to_child_handler, OP_WRITE, &bin);
    selector_status ss_read_pipe_from_child = selector_register(selector, pipeFromChild[0],
                                                                &read_pipe_from_child_handler, OP_READ, &bou);
    selector_status ss_write_stdout = selector_register(selector, stdoutFD,
                                                        &write_stdout_handler, OP_WRITE, &bou);

    // registrar sigterm es útil para terminar el programa normalmente.
    // esto ayuda mucho en herramientas como valgrind.
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    if (ss_read_stdin != SELECTOR_SUCCESS || ss_write_pipe_to_child != SELECTOR_SUCCESS || ss_read_pipe_from_child != SELECTOR_SUCCESS || ss_write_stdout != SELECTOR_SUCCESS)
    {
        fprintf(stderr, "Error: registering fd.\n");
        selector_destroy(selector);
        selector_close();
        return 1;
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

            fprintf(stderr, "In parity: %#X\nOut parity: %#X\n", inParity, outParity);
            break;
        }
    }

    if (ss != SELECTOR_SUCCESS)
    {
        fprintf(stderr, "Error: selector_select. %s\n",
                ss == SELECTOR_IO
                    ? strerror(errno)
                    : selector_error(ss));

        selector_destroy(selector);
        selector_close();
        return 1;
    }

    selector_destroy(selector);
    selector_close();

    // do
    // {

    //     int max_fd = 0;

    //     fd_set readSet;
    //     fd_set writeSet;
    //     FD_ZERO(&readSet);
    //     FD_ZERO(&writeSet);

    //     // Nos inscribimos segun las condiciones
    //     if (stdinFD != -1 && buffer_can_write(&bin))
    //     {
    //         FDSETWithMax(stdinFD, &readSet, &max_fd);
    //     }

    //     if (pipeFromChild[0] != -1 && buffer_can_write(&bou))
    //     {
    //         FDSETWithMax(pipeFromChild[0], &readSet, &max_fd);
    //     }

    //     if (pipeToChild[1] != -1 && buffer_can_read(&bin))
    //     {
    //         FDSETWithMax(pipeToChild[1], &writeSet, &max_fd);
    //     }

    //     if (stdoutFD != -1 && buffer_can_read(&bou))
    //     {
    //         FDSETWithMax(stdoutFD, &writeSet, &max_fd);
    //     }

    //     n = select(max_fd + 1, &readSet, &writeSet, NULL, NULL);
    //     // printf("WTF :\n select: %d\n Pipes: STDIN %d STDOUT %d CHIN %d CHOUT %d \n", n, stdinFD, stdoutFD, pipeFromChild[0], pipeToChild[1] );

    //     if (stdinFD != -1 && FD_ISSET(stdinFD, &readSet))
    //     {

    //         // printf("%s\n", "stdinFD set");

    //         handleReadStdIn(&bin);
    //     }

    //     if (pipeToChild[1] != -1 && FD_ISSET(pipeToChild[1], &writeSet))
    //     {

    //         // printf("%s\n", "pipeToChild[1] set");

    //         handleWritePipeToChild(&bin);
    //     }

    //     if (pipeFromChild[0] != -1 && FD_ISSET(pipeFromChild[0], &readSet))
    //     {

    //         // printf("%s\n", "pipeFromChild[0] set");

    //         handleReadPipeFromChild(&bou);
    //     }

    //     if (stdoutFD != -1 && FD_ISSET(stdoutFD, &writeSet))
    //     {

    //         // printf("%s\n", "stdoutFD set");

    //         handleWriteStdOut(&bou);
    //     }

    //     // If you can't read, do not write
    //     if (-1 == stdinFD && -1 != pipeToChild[1] && !buffer_can_read(&bin))
    //     {
    //         close(pipeToChild[1]);
    //         pipeToChild[1] = -1;

    //         // printf("%s\n", "stdinFD & pipeToChild[1] -> CLOSED");
    //     }

    //     if (-1 == pipeFromChild[0] && -1 != stdoutFD && !buffer_can_read(&bou))
    //     {
    //         close(stdoutFD);
    //         stdoutFD = -1;

    //         // fprintf(stderr, "%s\n", "pipeFromChild[0] & stdoutFD -> CLOSED");
    //     }

    //     // Return if every operation is finished
    //     if (stdinFD == -1 && pipeFromChild[0] == -1 && pipeToChild[1] == -1 && stdoutFD == -1)
    //     {

    //         fprintf(stderr, "In parity: %#X\nOut parity: %#X\n", inParity, outParity);
    //         return 0;
    //     }

    //     // printf("AFTER :\n select: %d\n Pipes: STDIN %d STDOUT %d CHIN %d CHOUT %d \n", n, stdinFD, stdoutFD, pipeFromChild[0], pipeToChild[1] );

    // } while (n != -1);

    wait(NULL);

    return 0;
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
