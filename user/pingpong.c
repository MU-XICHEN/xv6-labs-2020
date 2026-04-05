#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{

    int p[2];

    pipe(p);

    if (fork() == 0)
    {
        int buf[10];
        if (read(p[0], buf, 1) == 1)
        {
            printf("%d: received ping\n", getpid());
            write(p[1], buf, 1);
            exit(0);
        }
        else
            fprintf(2, "error: received ping\n");

        exit(0);
    }

    char *byte = "data";
    write(p[1], byte, 1);

    wait(0);
    int buf[10];
    if (read(p[0], buf, 1) == 1)
    {
        printf("%d: received pong\n", getpid());
    }
    else
    {
        fprintf(2, "error: received pong\n");
    }

    exit(0);
}