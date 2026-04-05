#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAX_LEN 35
typedef int max_arr[MAX_LEN];

// // 方法2：使用 errno 和 strerror
// fprintf(stderr, "read failed: %s (errno=%d)\n", strerror(errno), errno);

void sieve(int *prev_p)
{
    if (fork() == 0)
    {

        /* 以下处理上一个管道 */
        close(prev_p[1]); // 不往前一个 pipe 写

        max_arr buf;

        int num;
        int arr_index = 0, word_size = 0;
        while ((word_size = read(prev_p[0], &num, sizeof(int))))
        {
            if (word_size < 0)
            {
                fprintf(2, "read failed\n");
                exit(1);
            };
            buf[arr_index++] = num;
        }
        close(prev_p[0]); // 读完之后 close，此时上一个 pipe 就会被回收了

        if (!arr_index)
            return;

        printf("prime %d\n", buf[0]); // 打印当前进程处理的内容

        /* 以下处理下一个管道 */

        int p[2];
        pipe(p);

        sieve(p); // 创建下一个管道，fork 之后

        close(p[0]); // 不读，只写

        // sieve
        int target_num = buf[0];
        for (int i = 0; i < arr_index; i++)
        {
            if (!(buf[i] % target_num)) // 如果没有余数，则说明存在其他除数，则跳过
            {
                continue;
            }

            // 否则写给下一个管道
            if (
                write(p[1], &(buf[i]), sizeof(int)) != sizeof(int))
            {
                fprintf(2, "write failed\n");
                exit(1);
            }
        }

        close(p[1]);

        wait(0);
        exit(0);
    }
}

int main(int argc, char *argv[])
{

    int p[2];

    pipe(p);

    sieve(p); // fork 并过滤

    close(p[0]); // 不能放在 fork 前面...不然子进程无法获得 p[0]

    int i = 2;
    while (i <= MAX_LEN)
    {
        if (
            write(p[1], &i, sizeof(int)) != sizeof(int))
        {
            fprintf(2, "write failed\n");
            exit(1);
        }
        i++;
    }
    close(p[1]);

    wait(0);

    exit(0);
}