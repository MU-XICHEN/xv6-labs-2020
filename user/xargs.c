#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/param.h"
#include "user/user.h"

const unsigned int ARGUMENT_LEN = MAXPATH;

char *
gets_line(char *buf, int max)
{
    int i, cc;
    char c;

    for (i = 0; i + 1 < max;)
    {
        cc = read(0, &c, 1);
        if (cc < 1)
            break;
        if (c == '\n' || c == '\r')
            break;
        buf[i++] = c;
    }
    buf[i] = 0;
    return buf;
}

int main(int argc, char *argv[])
{
    char *(child_argv[MAXARG]);

    // get initial arguments
    for (int i = 1; i < argc; i++)
    {
        child_argv[i - 1] = argv[i];
    }
    child_argv[argc - 1] = 0;

    char buf[ARGUMENT_LEN];

    while (strlen(gets_line(buf, ARGUMENT_LEN)))
    {
        if (fork() == 0)
        {

            child_argv[argc - 1] = buf;
            child_argv[argc] = 0;

            exec(child_argv[0], child_argv);
        }

        wait(0);
    }

    exit(0);
}