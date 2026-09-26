#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

int extract_message(char **buf, char **msg)
{
    char    *newbuf;
    int    i;

    *msg = 0;
    if (*buf == 0)
        return (0);
    i = 0;
    while ((*buf)[i])
    {
        if ((*buf)[i] == '\n')
        {
            newbuf = calloc(1, sizeof(*newbuf) * (strlen(*buf + i + 1) + 1));
            if (newbuf == 0)
                return (-1);
            strcpy(newbuf, *buf + i + 1);
            *msg = *buf;
            (*msg)[i + 1] = 0;
            *buf = newbuf;
            return (1);
        }
        i++;
    }
    return (0);
}

char *str_join(char *buf, char *add)
{
    char    *newbuf;
    int        len;

    if (buf == 0)
        len = 0;
    else
        len = strlen(buf);
    newbuf = malloc(sizeof(*newbuf) * (len + strlen(add) + 1));
    if (newbuf == 0)
        return (0);
    newbuf[0] = 0;
    if (buf != 0)
        strcat(newbuf, buf);
    free(buf);
    strcat(newbuf, add);
    return (newbuf);
}

int     next_id = 0;            /* 次に配るクライアント番号 */
int     max_fd = 0;             /* select に渡す最大 fd */
int     client_id[65536];             /* client_id[fd] = クライアント番号 */
char    *client_buf[65536];           /* client_buf[fd] = \n 待ちの受信途中データ */
fd_set  all_set, read_set, write_set;
char    recv_buf[1001];
char    msg_buf[42];

void err(void)
{
    write(2, "Fatal error\n", 12);
    exit(1);
}

void broadcast(int from, char *str)
{
    for (int fd = 0; fd <= max_fd; fd++)
    {
        if (fd != from && FD_ISSET(fd, &write_set))
            send(fd, str, strlen(str), 0);
    }
}

int main(int ac, char **av) {
    int sockfd, connfd, len, n, ret;
    struct sockaddr_in servaddr, cli;
    char *line;

    if (ac != 2) {
        write(2, "Wrong number of arguments\n", 26);
        exit(1);
    }

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        err();
    bzero(&servaddr, sizeof(servaddr));

    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(2130706433); //127.0.0.1
    servaddr.sin_port = htons(atoi(av[1]));

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr))) != 0)
        err();
    if (listen(sockfd, 10) != 0)
        err();

    FD_ZERO(&all_set);
    FD_SET(sockfd, &all_set);
    max_fd = sockfd;

    while (1) {
        // select は集合を書き換えるので毎回コピーし直す
        read_set = write_set = all_set;
        if (select(max_fd + 1, &read_set, &write_set, NULL, NULL) < 0)
            err();

        for (int fd = 0; fd <= max_fd; fd++) {
            if (!FD_ISSET(fd, &read_set))
                continue;

            if (fd == sockfd) {
                // 新規接続: 番号を振って監視対象に加え、入室を通知
                len = sizeof(cli);
                connfd = accept(sockfd, (struct sockaddr *)&cli, (socklen_t *)&len);
                if (connfd < 0)
                    continue;
                if (connfd > max_fd)
                    max_fd = connfd;
                client_id[connfd] = next_id++;
                client_buf[connfd] = NULL;
                FD_SET(connfd, &all_set);
                sprintf(msg_buf, "server: client %d just arrived\n", client_id[connfd]);
                broadcast(connfd, msg_buf);
                break;
            }

            // 既存クライアントからの受信
            n = recv(fd, recv_buf, 1000, 0);
            if (n <= 0) {
                // 切断: 退室を通知して後始末
                sprintf(msg_buf, "server: client %d just left\n", client_id[fd]);
                broadcast(fd, msg_buf);
                free(client_buf[fd]);
                client_buf[fd] = NULL;
                FD_CLR(fd, &all_set);
                close(fd);
                break;
            }
            recv_buf[n] = 0;

            // 途中まで来ていたデータに連結し、完成した行だけ中継する
            client_buf[fd] = str_join(client_buf[fd], recv_buf);
            if (client_buf[fd] == NULL)
                err();
            while ((ret = extract_message(&client_buf[fd], &line)) != 0) {
                if (ret < 0)
                    err();
                sprintf(msg_buf, "client %d: ", client_id[fd]);
                broadcast(fd, msg_buf);
                broadcast(fd, line);
                free(line);
            }
        }
    }
    return (0);
}
