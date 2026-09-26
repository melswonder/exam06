#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

/*
** next_id    : 次に配るクライアント番号（0 から連番）
** max_fd     : 監視中の最大 fd（select の第1引数に使う）
** client_id  : client_id[fd]  = その fd のクライアント番号
** client_buf : client_buf[fd] = その fd の受信途中データ（\n が来るまで溜める）
** read_set / write_set : select に渡す集合（毎ループ all_set からコピー）
** all_set    : 現在接続中の fd 全部（マスター集合）
** recv_buf   : recv 用のバッファ
** msg_buf    : "server: client 1 just left\n" などの文言を組み立てる場所
*/
int		next_id = 0;
int		max_fd = 0;
int		client_id[65536];
char	*client_buf[65536];
fd_set	read_set, write_set, all_set;
char	recv_buf[1001];
char	msg_buf[42];

/* エラー時は "Fatal error" を出して即終了 */
void err(void)
{
	write(2, "Fatal error\n", 12);
	exit(1);
}

/*
** 受信バッファ *pending の先頭から「\n まで」を 1 行として切り出す。
**   *line    に 1 行（\n 込み）が入る
**   *pending は残りのデータで置き換えられる
** 戻り値:  1 = 1 行取り出せた / 0 = まだ \n が来ていない / -1 = メモリ確保失敗
*/
int get_msg(char **pending, char **line)
{
	char	*src;
	char	*rest;
	int		i;

	*line = NULL;
	src = *pending;
	if (src == NULL)
		return (0);

	/* \n の位置を探す */
	i = 0;
	while (src[i] != '\0' && src[i] != '\n')
		i++;
	if (src[i] != '\n')
		return (0);

	/* src は "1行\n" + "残り" という形。残りを別の領域にコピーする */
	rest = calloc(strlen(src + i + 1) + 1, sizeof(char));
	if (rest == NULL)
		return (-1);
	strcpy(rest, src + i + 1);

	/* src を \n の直後で切ると、src 自体が「1 行」になる */
	src[i + 1] = '\0';
	*line = src;
	*pending = rest;
	return (1);
}

/* base + add を新しい領域に連結して返す（古い base は free する） */
char *join(char *base, char *add)
{
	char	*joined;
	int		base_len;

	if (base == NULL)
		base_len = 0;
	else
		base_len = strlen(base);

	joined = malloc(base_len + strlen(add) + 1);
	if (joined == NULL)
		return (NULL);
	joined[0] = '\0';
	if (base != NULL)
		strcat(joined, base);
	free(base);
	strcat(joined, add);
	return (joined);
}

/* 送信元 from 以外の、書き込み可能な全接続に msg を送る */
void broadcast(int from, char *msg)
{
	for (int fd = 0; fd <= max_fd; fd++)
	{
		if (FD_ISSET(fd, &write_set) && fd != from)
			send(fd, msg, strlen(msg), 0);
	}
}

/* 127.0.0.1:port で待ち受けるソケットを作って返す */
int create_server(char *port_str)
{
	struct sockaddr_in	addr;
	int					sfd;
	int					port;

	sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0)
		err();

	port = atoi(port_str);
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	/* 127.0.0.1 を 1 バイトずつ入れる（inet_addr の代わり） */
	((unsigned char *)&addr.sin_addr.s_addr)[0] = 127;
	((unsigned char *)&addr.sin_addr.s_addr)[1] = 0;
	((unsigned char *)&addr.sin_addr.s_addr)[2] = 0;
	((unsigned char *)&addr.sin_addr.s_addr)[3] = 1;
	/* htons の代わりに上位/下位バイトを自分で入れ替える */
	addr.sin_port = (port << 8) | (port >> 8);

	if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		err();
	if (listen(sfd, 128) < 0)
		err();
	return (sfd);
}

/*
** 新しい接続を受け入れて、番号を振り、監視対象に加え、入室を通知する。
** 戻り値: 1 = 受け入れた（fd ループを抜ける） / 0 = 失敗したので何もしない
*/
int accept_client(int sfd)
{
	int	fd;

	fd = accept(sfd, NULL, NULL);
	if (fd < 0)
		return (0);

	if (fd > max_fd)
		max_fd = fd;
	client_id[fd] = next_id++;
	client_buf[fd] = NULL;
	FD_SET(fd, &all_set);

	sprintf(msg_buf, "server: client %d just arrived\n", client_id[fd]);
	broadcast(fd, msg_buf);
	return (1);
}

/* 切断されたクライアントの後始末をして、退室を通知する */
void disconnect_client(int fd)
{
	sprintf(msg_buf, "server: client %d just left\n", client_id[fd]);
	broadcast(fd, msg_buf);

	free(client_buf[fd]);
	client_buf[fd] = NULL;
	FD_CLR(fd, &all_set);
	close(fd);
}

/*
** fd から受信し、溜まったデータを 1 行ずつ他のクライアントへ中継する。
** 戻り値: 1 = 切断された（fd ループを抜ける） / 0 = 継続
*/
int handle_client(int fd)
{
	char	*line;
	int		n;
	int		ret;

	n = recv(fd, recv_buf, 1000, 0);
	if (n <= 0)
	{
		disconnect_client(fd);
		return (1);
	}
	recv_buf[n] = '\0';

	/* 受信分を今までの残りにつなげる（行の途中で届くことがあるため） */
	client_buf[fd] = join(client_buf[fd], recv_buf);
	if (client_buf[fd] == NULL)
		err();

	/* 完成した行がある限り取り出して中継する */
	while ((ret = get_msg(&client_buf[fd], &line)) != 0)
	{
		if (ret < 0)
			err();
		sprintf(msg_buf, "client %d: ", client_id[fd]);
		broadcast(fd, msg_buf);
		broadcast(fd, line);
		free(line);
	}
	return (0);
}

int main(int ac, char **av)
{
	int	sfd;

	if (ac != 2)
	{
		write(2, "Wrong number of arguments\n", 26);
		exit(1);
	}

	sfd = create_server(av[1]);
	FD_ZERO(&all_set);
	FD_SET(sfd, &all_set);
	max_fd = sfd;

	while (1)
	{
		/* select は渡した集合を書き換えるので、毎回 all_set からコピーし直す */
		read_set = write_set = all_set;
		if (select(max_fd + 1, &read_set, &write_set, NULL, NULL) < 0)
			err();

		for (int fd = 0; fd <= max_fd; fd++)
		{
			if (!FD_ISSET(fd, &read_set))
				continue;

			/* 待ち受けソケットが読める = 新しい接続が来ている */
			if (fd == sfd)
			{
				if (accept_client(sfd))
					break;
			}
			else
			{
				if (handle_client(fd))
					break;
			}
		}
	}
	return (0);
}
