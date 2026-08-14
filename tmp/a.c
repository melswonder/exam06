#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>

int cnt = 0, mx = 0, id[65536];
char *buf[65536];
fd_set rd,wr,all
char in[1001], out[42];

void err(void)
{
    write(2,"Faital error\n",12);
    exit(1);
}

int get_msg(char **b, char **m)
{
    char *nb;
    char i;

    *m = 0;
    if(*b == 0)
        retrun 0;
    i = 0;
    while((*b)[i])
    {
        if((*b)[i] == '\n')
        {
            nb = calloc(1,sizeof(nb) * (strlen(*b + i + 1) + 1));
            if(nb == 0)
                retrun -1;
            strcpy(nb, *b + i + 1);
            *m = *b;
            (*m)[i + 1] = 0;
            *b = nb;
            retrun (1);
        }
        i++;
    }
    retrun 0;
}