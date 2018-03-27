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
#include "buffer.h"

#define BUFF_SIZE 256

int pipeToChild[2];
int pipeFromChild[2];

char buf[BUFF_SIZE] = "";

unsigned char inParity = 0, outParity = 0;

static int calcParity( struct buffer * buff, const int size ) {
    unsigned char b = 0;
    int i;
    size_t * read_bytes = 0;
    uint8_t * ptr;

    ptr = buffer_read_ptr( buff, read_bytes );

    for ( i = 0 ; i < *read_bytes ; i++ ) {
        b^= ptr[i] ;    
    }

    return b;
}

static int handleReadStdIn(struct buffer * buff) {

    int bytesRead = 0;

    size_t * bytes = 0;

    bytesRead = read(STDIN_FILENO, buffer_write_ptr(buff, bytes), *bytes);

    if (bytesRead > 0) {

        inParity ^= calcParity(buff, bytesRead);

    } else {

        // perror("StdIn read failed");
        return 0;
    }

    return bytesRead;
}

static int handleReadPipeFromChild(struct buffer * buff) {

    // Empty buffer
    // memset(buf, 0, sizeof(buf));

    int bytesRead = 0;

    size_t * bytes = 0;

    // Read response from child
    bytesRead = read(pipeFromChild[0], buffer_write_ptr(buff, bytes), *bytes) ;

    if (bytesRead > 0) {

        outParity ^= calcParity(buff, bytesRead);

        fprintf(stderr, "Read from child: %d\n", ((int)*bytes));

    } else if (bytesRead == 0) {

        return 0;
    } else {

        perror("ReadPipeFromChild read failed");
        return -1;
    }

    close(pipeFromChild[0]);

    return bytesRead;
}

static void handleWritePipeToChild(struct buffer * buff) {

    size_t * write_bytes = 0;

    if ( write(pipeToChild[1], buffer_read_ptr( buff, write_bytes ), *write_bytes ) > 0 ) {
        printf("Wrote to child: %d bytes\n", ((int)*write_bytes));
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
    int n;


    //deberiamos anotarnos depende de la condicion, no ?
   /* safeFdSet(STDIN_FILENO, &readSet, &max_fd);
    safeFdSet(pipeFromChild[0], &readSet, &max_fd);

    safeFdSet(STDOUT_FILENO, &writeSet, &max_fd);
    safeFdSet(pipeToChild[1], &writeSet, &max_fd);
*/
    uint8_t buffin[4096] = {0}, buffout[4096] = {0};
    struct buffer bin, bou;
    buffer_init(&bin, sizeof(buffin)/sizeof(*buffin), buffin);
    buffer_init(&bou, sizeof(buffout)/sizeof(*buffout), buffout);

    int available = 0;

    fd_set readSetBackup = readSet;
    fd_set writeSetBackup = writeSet;

    do {
        fflush(stdout);

        int bytesRead = 0;


        // Nos inscribimos segun las condiciones
        if( STDIN_FILENO != -1 && buffer_can_write(&bin) ) {
            safeFdSet( STDIN_FILENO, &readSet, &max_fd);
        }

        if( pipeFromChild[0] != -1 && buffer_can_write(&bou) ) {
            safeFdSet( pipeFromChild[0], &readSet, &max_fd);
        }

        if( pipeToChild[1] != -1 && buffer_can_read(&bin) ) {
            safeFdSet( pipeToChild[1], &writeSet, &max_fd);
        }

        if( STDOUT_FILENO != -1 && buffer_can_read(&bou) ) {
            safeFdSet( STDOUT_FILENO, &writeSet, &max_fd);
        }


        n = select(max_fd + 1, &readSet, &writeSet, NULL, NULL);
        //printf("WTF :\n select: %d\n Pipes: STDIN %d STDOUT %d CHIN %d CHOUT %d \n", n, STDIN_FILENO, STDOUT_FILENO, pipeFromChild[0], pipeToChild[1] );

        //handlers por si alguno esta disponible
        if ( FD_ISSET(STDIN_FILENO, &readSet) ) {

            // Close unused ends of pipes
            //close(pipeToChild[0]);
            //close(pipeFromChild[1]);

            bytesRead = handleReadStdIn(&bin);
        }

        if (FD_ISSET(pipeToChild[1], &writeSet)) {

            handleWritePipeToChild(&bin);

            close(pipeToChild[1]);

        }

        if (FD_ISSET(pipeFromChild[0], &readSet)) {

            handleReadPipeFromChild(&bou);
        }

        if (FD_ISSET(STDOUT_FILENO, &writeSet)) {
            // Is it ok ?
            fprintf(stderr, "In parity: %#X\nOut parity: %#X\n", inParity, outParity);
        }

        // If you can't read, do not write
        if ( -1 == STDIN_FILENO && -1 != pipeToChild[1] && !buffer_can_read(&bin) ){
            close( pipeToChild[1] );
            pipeToChild[1] = -1;
        }

        if ( -1 == pipeFromChild[0] && -1 != STDOUT_FILENO && !buffer_can_read(&bou) ){
            close( STDOUT_FILENO );
        }

        // return if every operation is finished
        if ( STDIN_FILENO == -1 && pipeFromChild[0] == -1
            && pipeToChild[1] == -1 && STDOUT_FILENO == -1){
            return 0;
        }

        fd_set readSetBackup = readSet;
        fd_set writeSetBackup = writeSet;

    }
    while ( n != -1 );

    wait(NULL);

}

static void startChild(char * command) {
    // Close unused ends of pipes
    close(pipeToChild[1]);
    close(pipeFromChild[0]);

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

// Test with: clear && clang  -Weverything ejIPC.c -o ejIPC && echo -n hola | pv | ./ejIPC "sed s/o/0/g| sed s/a/4/g"
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
