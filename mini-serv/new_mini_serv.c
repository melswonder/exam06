#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

/*
** cnt : 次に配る client id（0 から連番）
** mx  : 監視中の最大 fd（select の第1引数用）
** id  : id[fd] = その fd のクライアント番号
** buf : buf[fd] = その fd の受信途中データ（\n が来るまで溜める）
** rd/wr : select に渡す読み/書きセット（毎ループ all からコピー）
** all : 現在接続中の fd 全部（マスター集合）
** in  : recv 用バッファ / out : 送信文言の組み立て用
*/
int		cnt = 0, mx = 0, id[65536];
char	*buf[65536];
fd_set	rd, wr, all;
char	in[1001], out[42];

/* エラー時は "Fatal error" を出して終了 */
void err(void)
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

/* b の先頭から \n までを m に切り出す。残りは b に詰め直す。
** 戻り値 1=1行取れた / 0=まだ行が無い / -1=malloc 失敗 */
int get_msg(char **b, char **m)
{
	char	*nb;
	int		i;

	*m = 0;
	if (*b == 0)
		return (0);
	i = 0;
	while ((*b)[i])
	{
		if ((*b)[i] == '\n')
		{
			nb = calloc(1, sizeof(*nb) * (strlen(*b + i + 1) + 1));
			if (nb == 0)
				return (-1);
			strcpy(nb, *b + i + 1);
			*m = *b;
			(*m)[i + 1] = 0;
			*b = nb;
			return (1);
		}
		i++;
	}
	return (0);
}

/* b + a を新しい領域に連結して返す（古い b は free する） */
char *join(char *b, char *a)
{
	char	*nb;
	int		len;

	if (b == 0)
		len = 0;
	else
		len = strlen(b);
	nb = malloc(sizeof(*nb) * (len + strlen(a) + 1));
	if (nb == 0)
		return (0);
	nb[0] = 0;
	if (b != 0)
		strcat(nb, b);
	free(b);
	strcat(nb, a);
	return (nb);
}

/* 送信元 from 以外の全接続に s を送る */
void cast(int from, char *s)
{
	for (int fd = 0; fd <= mx; fd++)
		if (FD_ISSET(fd, &wr) && fd != from)
			send(fd, s, strlen(s), 0);
}

int main(int ac, char **av)
{
	if (ac != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		exit(1);
	}

	/* 待ち受けソケットを作る */
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0) err();

	/* 127.0.0.1 : av[1] にバインド（htons 相当のバイト入れ替えを手書き） */
	struct sockaddr_in ad;
	memset(&ad, 0, sizeof(ad));
	ad.sin_family = AF_INET;
	((unsigned char *)&ad.sin_addr.s_addr)[0] = 127;
	((unsigned char *)&ad.sin_addr.s_addr)[1] = 0;
	((unsigned char *)&ad.sin_addr.s_addr)[2] = 0;
	((unsigned char *)&ad.sin_addr.s_addr)[3] = 1;
	ad.sin_port = (atoi(av[1]) << 8) | (atoi(av[1]) >> 8);

	if (bind(sfd, (struct sockaddr *)&ad, sizeof(ad)) || listen(sfd, 128)) err();

	FD_ZERO(&all);
	FD_SET(sfd, &all);
	mx = sfd;

	while (1)
	{
		/* select は集合を書き換えるので毎回 all からコピーし直す */
		rd = wr = all;
		if (select(mx + 1, &rd, &wr, NULL, NULL) < 0) err();

		for (int fd = 0; fd <= mx; fd++)
		{
			if (!FD_ISSET(fd, &rd)) continue;

			if (fd == sfd)
			{
				/* 新規接続：id を振って監視対象に追加し、入室を通知 */
				int c = accept(sfd, NULL, NULL);
				if (c >= 0)
				{
					mx = c > mx ? c : mx;
					id[c] = cnt++;
					buf[c] = NULL;
					FD_SET(c, &all);
					sprintf(out, "server: client %d just arrived\n", id[c]);
					cast(c, out);
					break;
				}
			}
			else
			{
				int n = recv(fd, in, 1000, 0);
				if (n <= 0)
				{
					/* 切断：退室を通知して後始末（集合から外す→close） */
					sprintf(out, "server: client %d just left\n", id[fd]);
					cast(fd, out);
					free(buf[fd]);
					FD_CLR(fd, &all);
					close(fd);
					break;
				}
				in[n] = '\0';
				buf[fd] = join(buf[fd], in);
				if (buf[fd] == 0) err();

				/* 溜まったデータから 1 行ずつ取り出して他クライアントへ中継 */
				char *m;
				int r;
				while ((r = get_msg(&buf[fd], &m)) != 0)
				{
					if (r < 0) err();
					sprintf(out, "client %d: ", id[fd]);
					cast(fd, out);
					cast(fd, m);
					free(m);
				}
			}
		}
	}
	return 0;
}
