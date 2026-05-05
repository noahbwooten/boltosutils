//
//  main.c
//  boltosutils
//
//  Created by Noah Wooten on 5/3/26.
//

#include <stdio.h>
#include <string.h>

void Version(int argc, char** argv);
void Resource(int argc, char** argv);
void Filesystem(int argc, char** argv);

int main(int argc, char** argv) {
    if (argc <= 1) {
        printf("[ERR]: Missing arguments.\n");
        return -1;
    }

    if (!strcmp(argv[1], "version")) {
        Version(argc, argv);
    } else if (!strcmp(argv[1], "resource")) {
        Resource(argc, argv);
    } else if (!strcmp(argv[1], "filesystem")) {
        Filesystem(argc, argv);
    }

    return 0;
}
