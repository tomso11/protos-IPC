#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <assert.h>
#include "buffer.h"


static int pipeToChild[2] = {-1, -1};
static int pipeFromChild[2] = {-1, -1};
static int stdinFD = STDIN_FILENO;
static int stdoutFD = STDOUT_FILENO;

static unsigned char inParity = 0, outParity = 0;

static int calcParity(uint8_t * ptr, ssize_t size) {
    unsigned char b = 0;
    int i;

    for ( i = 0 ; i < size ; i++ ) {
        b ^= ptr[i] ;    
    }

    return b;
}

static void handleReadStdIn(struct buffer * buff) {

    ssize_t bytesRead = 0;

    size_t write_bytes[1] = {0};

    uint8_t * ptr = buffer_write_ptr(buff, write_bytes);

    bytesRead = read(stdinFD, ptr, *write_bytes);

    if (bytesRead > 0) {

        inParity ^= calcParity(ptr, bytesRead);

        buffer_write_adv(buff, bytesRead);

        printf("Read from stdin: %s\n", ptr);

    } else if(bytesRead == 0) {

        buffer_write_adv(buff, 0);

        close(stdinFD);
        stdinFD = -1;

    } else {

     perror("StdIn read failed");

 }

}

static void handleWriteStdOut(struct buffer * buff) {

    ssize_t bytesWritten = 0;

    size_t read_bytes[1] = {0};

    uint8_t * ptr = buffer_read_ptr(buff, read_bytes);

    bytesWritten = write(stdoutFD, ptr, *read_bytes);

    if (bytesWritten > 0) {

        buffer_read_adv(buff, bytesWritten);

        printf("Wrote to stdout: %s\n", ptr);

    } else if(bytesWritten == 0) {

        buffer_read_adv(buff, 0);

    } else {

        perror("StdOut write failed");

    }

}

static void handleReadPipeFromChild(struct buffer * buff) {

    size_t bytes[1] = {0};

    ssize_t bytesRead = 0;

    uint8_t * ptr = buffer_write_ptr(buff, bytes);

    // Read response from child
    bytesRead = read(pipeFromChild[0], ptr, *bytes);

    if (bytesRead > 0) {

        outParity ^= calcParity(ptr, bytesRead);

        buffer_write_adv(buff, bytesRead);

        // printf("Read from child: %s\n", ptr);

    } else if (bytesRead == 0) {

        buffer_write_adv(buff, 0);

        close(pipeFromChild[0]);
        pipeFromChild[0] = -1;

    } else {

        perror("ReadPipeFromChild read failed");
    }

}

static void handleWritePipeToChild(struct buffer * buff) {

    size_t read_bytes[1];

    uint8_t * ptr = buffer_read_ptr( buff, read_bytes );

    ssize_t writtenBytes = 0;

    if ( (writtenBytes = write(pipeToChild[1], ptr , *read_bytes )) > 0 ) {

        // printf("Wrote to child: %s \n", ptr);

        buffer_read_adv(buff, writtenBytes);

    } else if(writtenBytes == 0) {

        buffer_read_adv(buff, writtenBytes);

    }

}

// Source from next function: http://jhshi.me/2013/11/02/use-select-to-monitor-multiple-file-descriptors/index.html#.Wq8xTmaZNQI
// Add a fd to fd_set, and update max_fd
static int safeFdSet(int fd, fd_set* fds, int* max_fd) {
    assert(max_fd != NULL);

    FD_SET(fd, fds);
    if (fd > *max_fd) {
        *max_fd = fd;
    }
    return 0;
}

static int startParent() {

    close(pipeToChild[0]);
    close(pipeFromChild[1]);

    pipeToChild[0] = pipeFromChild[1] = -1;

    int n;

    uint8_t buffin[4096] = {0}, buffout[4096] = {0};
    struct buffer bin, bou;
    buffer_init(&bin, sizeof(buffin)/sizeof(*buffin), buffin);
    buffer_init(&bou, sizeof(buffout)/sizeof(*buffout), buffout);

    do {
        fflush(stdout);

        int max_fd = 0;

        fd_set readSet;
        fd_set writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);

        // Nos inscribimos segun las condiciones
        if( stdinFD != -1 && buffer_can_write(&bin) ) {
            safeFdSet( stdinFD, &readSet, &max_fd);
        }

        if( pipeFromChild[0] != -1 && buffer_can_write(&bou) ) {
            safeFdSet( pipeFromChild[0], &readSet, &max_fd);
        }

        if( pipeToChild[1] != -1 && buffer_can_read(&bin) ) {
            safeFdSet( pipeToChild[1], &writeSet, &max_fd);
        }

        if( stdoutFD != -1 && buffer_can_read(&bou) ) {
            safeFdSet( stdoutFD, &writeSet, &max_fd);
        }


        n = select(max_fd + 1, &readSet, &writeSet, NULL, NULL);
        // printf("WTF :\n select: %d\n Pipes: STDIN %d STDOUT %d CHIN %d CHOUT %d \n", n, stdinFD, stdoutFD, pipeFromChild[0], pipeToChild[1] );

        if ( stdinFD != -1 && FD_ISSET(stdinFD, &readSet) ) {

            // printf("%s\n", "stdinFD set");

            handleReadStdIn(&bin);
        }

        if ( pipeToChild[1] != -1 && FD_ISSET(pipeToChild[1], &writeSet)) {

            // printf("%s\n", "pipeToChild[1] set");

            handleWritePipeToChild(&bin);

        }

        if ( pipeFromChild[0] != -1 && FD_ISSET(pipeFromChild[0], &readSet)) {

            // printf("%s\n", "pipeFromChild[0] set");

            handleReadPipeFromChild(&bou);

        }

        if ( stdoutFD != -1 && FD_ISSET(stdoutFD, &writeSet)) {

            // printf("%s\n", "stdoutFD set");

            handleWriteStdOut(&bou);

        }

        // If you can't read, do not write
        if ( -1 == stdinFD && -1 != pipeToChild[1] && !buffer_can_read(&bin) ){
            close( pipeToChild[1] );
            pipeToChild[1] = -1;

            // printf("%s\n", "stdinFD & pipeToChild[1] -> CLOSED");
        }

        if ( -1 == pipeFromChild[0] && -1 != stdoutFD && !buffer_can_read(&bou) ){
            close( stdoutFD );
            stdoutFD = -1;

            // fprintf(stderr, "%s\n", "pipeFromChild[0] & stdoutFD -> CLOSED");
        }

        // Return if every operation is finished
        if ( stdinFD == -1 && pipeFromChild[0] == -1 && pipeToChild[1] == -1 && stdoutFD == -1) {

            fprintf(stderr, "In parity: %#X\nOut parity: %#X\n", inParity, outParity);
            return 0;
        }

        // printf("AFTER :\n select: %d\n Pipes: STDIN %d STDOUT %d CHIN %d CHOUT %d \n", n, stdinFD, stdoutFD, pipeFromChild[0], pipeToChild[1] );


    } while ( n != -1 );

    wait(NULL);

    return 0;

}

static void startChild(char * command) {
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

// Test with: clear && clang -Weverything ejIPC-4.c buffer.c -o ejipc && echo -n hola | pv | ./ejipc "sed s/o/0/g| sed s/a/4/g"
// Linux GCC : gcc -c ejIPC-4.c buffer.c buffer.h ; gcc -o ejipc ejIPC-4.o buffer.o
int main(int argc, char** argv) {

    int pid;

    //Chequeamos que tenga todos los argumentos necesarios
    if ( argc < 2 ){
        fprintf( stderr, "Too few arguments\n" );
        return EXIT_FAILURE;
    }

    //agregar para leer desde archivos

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
