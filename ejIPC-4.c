#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <assert.h>

//JUAN
#include <buffer.h>

#define BUFF_SIZE 256

int pipeToChild[2];
int pipeFromChild[2];

char buf[BUFF_SIZE] = "";

unsigned char inParity = 0, outParity = 0;

static int calcParity( const char * string, const int size ) {
    unsigned char b = 0;
    int i;

    for ( i = 0 ; i < size ; i++ ) {
        b ^= string[i];
    }

    return b;
}

static int handleReadStdIn() {

    int bytesRead = 0;

    bytesRead = read(STDIN_FILENO, buf, sizeof(buf));

    if (bytesRead > 0) {

        inParity ^= calcParity(buf, bytesRead);

    } else {

        // perror("StdIn read failed");
        return 0;
    }

    return bytesRead;
}

static int handleReadPipeFromChild() {

    // Empty buffer
    memset(buf, 0, sizeof(buf));

    int bytesRead = 0;

    // Read response from child
    bytesRead = read(pipeFromChild[0], buf, sizeof(buf));

    if (bytesRead > 0) {

        outParity ^= calcParity(buf, bytesRead);

        fprintf(stderr, "Read from child: %s", buf);

    } else if (bytesRead == 0) {

        return 0;
    } else {

        perror("ReadPipeFromChild read failed");
        return -1;
    }

    close(pipeFromChild[0]);

    return bytesRead;
}

static void handleWritePipeToChild(int bytesRead) {

    if (write(pipeToChild[1], buf, bytesRead) > 0) {
        printf("Wrote to child: %s\n", buf);
    }

}

// Source from next 2 functions: http://jhshi.me/2013/11/02/use-select-to-monitor-multiple-file-descriptors/index.html#.Wq8xTmaZNQI
// add a fd to fd_set, and update max_fd
static int safeFdSet(int fd, fd_set* fds, int* max_fd) {
    assert(max_fd != NULL);

    FD_SET(fd, fds);
    if (fd > *max_fd) {
        *max_fd = fd;
    }
    return 0;
}

// clear fd from fds, update max fd if needed
static int safeFdClr(int fd, fd_set* fds, int* max_fd) {
    assert(max_fd != NULL);

    FD_CLR(fd, fds);
    if (fd == *max_fd) {
        (*max_fd)--;
    }
    return 0;
}

static int startParent() {

    // Set up select parameters
    fd_set readSet;
    fd_set writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);

    int max_fd = -1;


    //deberiamos anotarnos depende de la condicion, no ?
   /* safeFdSet(STDIN_FILENO, &readSet, &max_fd);
    safeFdSet(pipeFromChild[0], &readSet, &max_fd);

    safeFdSet(STDOUT_FILENO, &writeSet, &max_fd);
    safeFdSet(pipeToChild[1], &writeSet, &max_fd);
*/
    uint8_t buffin[4096] = {0}, buffout[4096] = {0};
    buffer bin, bou;
    buffer_init(bin, sizeof(buffin)/sizeof(*buffin), buffin);
    buffer_init(bou, sizeof(buffout)/sizeof(*buffout), buffout);

    int available = 0;

    fd_set readSetBackup = readSet;
    fd_set writeSetBackup = writeSet;

    while ( select(max_fd + 1, &readSet, &writeSet, NULL, NULL) != -1 ) {

        fflush(stdout);

        int bytesRead = 0;


        // Nos inscribimos segun las condiciones
        if( STDIN_FILENO =! -1 && buffer_can_write ) {
            safeFdSet( STDIN_FILENO, &readSet);
        }

        if( pipeFromChild[0] =! -1 && buffer_can_write ) {
            safeFdSet( pipeFromChild[0], &readSet);
        }

        if( STDOUT_FILENO =! -1 && buffer_can_read ) {
            safeFdSet( STDOUT_FILENO, &readSet);
        }

        if( pipeToChild[1] =! -1 && buffer_can_write ) {
            safeFdSet( pipeToChild[1], &readSet);
        }

        //handlers por si alguno esta disponible
        if ( FD_ISSET(STDIN_FILENO, &readSetBackup) ) {

            // Close unused ends of pipes
            close(pipeToChild[0]);
            close(pipeFromChild[1]);

            bytesRead = handleReadStdIn();
        }

        if (FD_ISSET(pipeToChild[1], &writeSetBackup)) {

            handleWritePipeToChild(bytesRead);

            close(pipeToChild[1]);

        }

        if (FD_ISSET(pipeFromChild[0], &readSetBackup)) {

            handleReadPipeFromChild();
        }

        fd_set readSetBackup = readSet;
        fd_set writeSetBackup = writeSet;

    }

    // Print to stderr
    fprintf(stderr, "In parity: %#X\nOut parity: %#X\n", inParity, outParity);

    wait(NULL);

    return 0;
}

static void startChild(char * command) {
    // Close unused ends of pipes
    close(pipeToChild[1]);
    close(pipeFromChild[0]);

    // Duplicate ends of pipes to stdin and stdout
    dup2(pipeToChild[0], fileno(stdin));
    dup2(pipeFromChild[1], fileno(stdout));

    // Build arguments for args. Note that bash is set as environment
    char * args[4];
    args[0] = "bash";
    args[1] = "-c";
    args[2] = command;
    args[3] = NULL;

    execvp(args[0], args);
}

static void setUpPipes() {
    // Create pipes
    if (pipe(pipeToChild) == -1 || pipe(pipeFromChild) == -1) {
        perror("Could not build pipe.");
        exit(1);
    }
}

// Test with: clear && clang  -Weverything ejIPC.c -o ejIPC && echo -n hola | pv | ./ejIPC "sed s/o/0/g| sed s/a/4/g"
int main(int argc, char** argv) {

    int pid;

    setUpPipes();

    // Fork
    if ((pid = fork()) == -1) {
        perror("Could not fork.");
        exit(1);
    }

    if (pid == 0) {
        startChild(argv[1]);
    } else {
        startParent();
    }

    return 0;
}
