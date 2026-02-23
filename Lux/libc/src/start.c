#include "stdlib.h"

extern int main(int argc, char** argv);

void _start() {
    // TODO: Parse arguments from stack
    int argc = 0;
    char** argv = NULL;
    
    int ret = main(argc, argv);
    exit(ret);
}
