#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#define	__USE_XOPEN_EXTENDED
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <pty.h>
#include <termios.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "cJSON.h"
//#include <stropts.h>
#include <errno.h>
#include <signal.h>

void sigpipe(int signum)
{
	//printf("ignore epipe!\n");
}

char username[64] = {0};
char password[64] = {0};
char address[64] = {0};
char clientmode[64] = {0};
bool servermode = false;
bool localmode = false;

struct multi_fd {
	int sockfd;
	int ptmfd;
	int ptsfd;
};

struct header {
	unsigned int magic_id;
	unsigned int data_len;
	unsigned int address;
	unsigned int reserved1;
} __attribute__((packed));

struct instance {
	bool in_use;
	bool is_slave;
	int fd;
	char username[32];
	struct instance *peer;
};

//int f_out;
#define	MAXINSTANCES	8
struct instance instances[MAXINSTANCES] = {0};

bool is_slave(struct instance *ins)
{
	if (ins == NULL) {
		return false;
	} else {
		return ins->is_slave;
	}
}

struct instance *link_client(int fd)
{
	int len;
	int data_len;
	struct header *h;
	unsigned char *data;
	cJSON *j;
	struct instance *ins = NULL;

	h = (struct header *)malloc(sizeof(struct header));
	memset(h, '\0', sizeof(struct header));
	len = read(fd, h, sizeof(struct header));
	data_len = h->data_len;
	data = (unsigned char *)malloc(data_len);
	memset(data, '\0', data_len);
	len = read(fd, data, data_len);
	//printf("data:%s\n", data);
	j = cJSON_Parse(data);
	if (j == NULL) {
		shutdown(fd, SHUT_RDWR);
		goto out_data;
	}
	for (int i=0; i<MAXINSTANCES; i++) {
		if (instances[i].in_use == false) {
			instances[i].in_use = true;
			ins = &instances[i];
			//printf("instance: %d\n", i);
			break;
		}
	}
	if (ins == NULL) {
		goto out;
	}
	ins->fd = fd;
	if (strcmp(cJSON_GetObjectItem(j, "clientmode")->valuestring, "slave") == 0) {
		//printf("is slave\n");
		ins->is_slave = true;
		strcpy(ins->username, cJSON_GetObjectItem(j, "username")->valuestring);
	} else if (strcmp(cJSON_GetObjectItem(j, "clientmode")->valuestring, "master") == 0) {
		//printf("is master\n");
		ins->is_slave = false;
		strcpy(ins->username, cJSON_GetObjectItem(j, "username")->valuestring);
	}
	for (int i=0; i<MAXINSTANCES; i++) {
		if ((instances[i].peer == NULL) && (strcmp(instances[i].username, ins->username) == 0) && (ins != &instances[i]) && (instances[i].in_use == true) && (instances[i].is_slave ^ ins->is_slave)) {
			instances[i].peer = ins;
			ins->peer = &instances[i];
			break;
		}
	}
	//printf("link done\n");
out:
	cJSON_Delete(j);
out_data:
	free(data);
	free(h);
	return ins;
}

void *client_thread(void *f)
{
	char c;
	int fd = *(int *)f;
	int len;
	struct instance *ins;

	pthread_detach(pthread_self());
	free(f);
	//printf("client fd: %d\n", fd);
	ins = link_client(fd);
	if (is_slave(ins)) {
wait_master:
		while (ins->peer == NULL) {
			sleep(1);
		}
		//printf("slave find master\n");
		while ((len = read(ins->fd, &c, 1)) != EOF) {
			if (len <= 0) {
				//printf("close slave socket\n");
				shutdown(ins->peer->fd, SHUT_RDWR);
				close(ins->peer->fd);
				ins->peer = NULL;
				memset(ins->username, '\0', 32);
				ins->in_use = false;
				//printf("close slave socket done\n");
				break;
			}
			if (ins->peer == NULL) {
				//printf("slave lost master\n");
				goto wait_master;
			}
			len = write(ins->peer->fd, &c, 1);
		}
	} else {
		if (ins == NULL) {
			goto out;
		}
		//while (ins->peer == NULL) {
		//	sleep(1);
		//}
		if (ins->peer == NULL) {
			ins->in_use = false;
			//printf("master not find slave\n");
			goto out;
		}
		//printf("master find slave\n");
		while ((len = read(ins->fd, &c, 1)) != EOF) {
			if (len <= 0) {
				//printf("close master socket\n");
				//ins->peer->peer = NULL;
				ins->peer = NULL;
				memset(ins->username, '\0', 32);
				ins->in_use = false;
				//printf("close master socket done\n");
				break;
			}
			len = write(ins->peer->fd, &c, 1);
		}
	}
out:
	close(fd);
}

void *slave_input_thread(void *fd)
{
	char c;
	int len;
	struct multi_fd mfd = *(struct multi_fd *)fd;
	//pthread_detach(pthread_self());
	//while ((len = read(0, &c, 1)) != EOF) {
	while ((len = read(mfd.sockfd, &c, 1)) != EOF) {
		//usleep(1000);
		//printf("read from server: %c\n", c);
		if (len <= 0) {
			break;
		}
		len = write(mfd.ptmfd, &c, 1);
		//printf("read from server: %c [%d]done\n", c, len);
	}
}

void *slave_output_thread(void *fd)
{
	char c;
	int len;
	struct multi_fd mfd = *(struct multi_fd *)fd;
	//pthread_detach(pthread_self());
	while ((len = read(mfd.ptmfd, &c, 1)) != EOF) {
		//len = write(1, &c, 1);
		//usleep(1000);
		//printf("write to server: %c\n", c);
		len = write(mfd.sockfd, &c, 1);
		if (len <= 0) {
			break;
		}
		//printf("write to server: %c [%d]done\n", c, len);
	}
}

int start_slave(int sockfd)
{
	int fd_m, fd_s;
	int len;
	int retval;
	int status;
	int flags;
	const char *pts_name;
	pid_t fpid;
	struct header h;
	unsigned char *d;
	cJSON *j;

	//f_out = open("temp", O_RDWR);
	//fd_m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
	fd_m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	retval = grantpt(fd_m);
	retval = unlockpt(fd_m);
	pts_name = (const char *)ptsname(fd_m);
	//printf("pts_name: %s\n", pts_name);
	//fd_s = open(pts_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
	fd_s = open(pts_name, O_RDWR | O_NOCTTY);
	j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "clientmode", "slave");
	cJSON_AddStringToObject(j, "username", "ephraim");
	d = cJSON_Print(j);
	cJSON_Delete(j);
	h.data_len = strlen(d);
	write(sockfd, &h, sizeof(struct header));
	len = write(sockfd, d, strlen(d));
	//printf("send %d bytes\n", len);
	free(d);
	if (len != h.data_len) {
		printf("start slave error\n");
		goto out;
	}
	fpid = fork();
	if (fpid == 0) {
		char *pargv[] = {"/bin/bash", NULL};
		//char *penvp[] = {"TERM=xterm-256color", NULL};
		struct termios ori_settings, new_settings;
		tcgetattr(fd_s, &ori_settings);
		new_settings = ori_settings;
		new_settings.c_lflag |= ECHO;
		new_settings.c_lflag |= ICANON;
		new_settings.c_lflag |= ISIG;
		new_settings.c_cc[VMIN] = 1;
		new_settings.c_cc[VTIME] = 0;
		tcsetattr(fd_s, TCSANOW, &new_settings);
		setsid();
		dup2(fd_s, 0);
		dup2(fd_s, 1);
		dup2(fd_s, 2);
		//execve("/bin/bash", pargv, penvp);
		execve("/bin/bash", pargv, NULL);
	} else {
		char c;
		pthread_t i_thread, o_thread;
		struct termios ori_settings, new_settings;
		struct multi_fd mfd;
		tcgetattr(fd_m, &ori_settings);
		new_settings = ori_settings;
		new_settings.c_lflag &= ~ECHO;
		new_settings.c_lflag &= ~ICANON;
		new_settings.c_lflag &= ~ISIG;
		new_settings.c_cc[VMIN] = 1;
		new_settings.c_cc[VTIME] = 0;
		tcsetattr(fd_m, TCSANOW, &new_settings);
		mfd.sockfd = sockfd;
		mfd.ptmfd = fd_m;
		mfd.ptsfd = fd_s;
		retval = pthread_create(&i_thread, NULL, slave_input_thread, &mfd);
		retval = pthread_create(&o_thread, NULL, slave_output_thread, &mfd);
		wait(&status);
		//printf("parent exit child\n");
		shutdown(sockfd, SHUT_RDWR);
		close(sockfd);
		close(fd_s);
		close(fd_m);
		pthread_join(i_thread, NULL);
		//printf("parent exit i_thread\n");
		pthread_join(o_thread, NULL);
		//printf("parent exit o_thread\n");
		//tcsetattr(0, TCSANOW, &ori_settings);
		//tcsetattr(1, TCSANOW, &ori_settings);
		return 0;
	}
out:
	close(fd_s);
	close(fd_m);
	//close(f_out);
	return 0;
}

void *master_input_thread(void *fd)
{
	char c;
	int len;
	struct multi_fd mfd = *(struct multi_fd *)fd;
	struct termios ori_settings, new_settings;
	//pthread_detach(pthread_self());
	tcgetattr(0, &ori_settings);
	new_settings = ori_settings;
	new_settings.c_iflag |= IGNPAR;
	new_settings.c_iflag &= ~(ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
	new_settings.c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	new_settings.c_oflag &= ~OPOST;
	new_settings.c_cc[VMIN] = 1;
	new_settings.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &new_settings);
	while ((len = read(0, &c, 1)) != EOF) {
		len = write(mfd.sockfd, &c, 1);
		if (len <= 0) {
			//printf("\ndisconnect i_thread!\n");
			break;
		}
	}
	tcsetattr(0, TCSANOW, &ori_settings);
	//tcsetattr(1, TCSANOW, &ori_settings);
}

void *master_output_thread(void *fd)
{
	char c;
	int len;
	struct multi_fd mfd = *(struct multi_fd *)fd;
	//struct termios ori_settings, new_settings;
	//pthread_detach(pthread_self());
	/*tcgetattr(1, &ori_settings);
	new_settings = ori_settings;
	new_settings.c_iflag |= IGNPAR;
	new_settings.c_iflag &= ~(ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
	new_settings.c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	new_settings.c_oflag &= ~OPOST;
	new_settings.c_cc[VMIN] = 1;
	new_settings.c_cc[VTIME] = 0;
	tcsetattr(1, TCSANOW, &new_settings);*/
	while ((len = read(mfd.sockfd, &c, 1)) != EOF) {
		if (len <= 0) {
			printf("\ndisconnect o_thread!\n");
			break;
		}
		len = write(1, &c, 1);
	}
	//tcsetattr(1, TCSANOW, &ori_settings);
}

int start_master(int sockfd)
{
	int retval;
	pthread_t i_thread, o_thread;
	struct multi_fd mfd;
	struct header h;
	unsigned char *d;
	cJSON *j;

	mfd.sockfd = sockfd;
	j = cJSON_CreateObject();
	cJSON_AddStringToObject(j, "clientmode", "master");
	cJSON_AddStringToObject(j, "username", "ephraim");
	d = cJSON_Print(j);
	cJSON_Delete(j);
	h.data_len = strlen(d);
	write(sockfd, &h, sizeof(struct header));
	write(sockfd, d, strlen(d));
	free(d);
	retval = pthread_create(&i_thread, NULL, master_input_thread, &mfd);
	retval = pthread_create(&o_thread, NULL, master_output_thread, &mfd);
	pthread_join(i_thread, NULL);
	pthread_join(o_thread, NULL);
	return 0;
}

int main(int argc, char *argv[])
{
	int option;
	const char *opts = "u:p:a:sc:l";
	struct option lopts[] = {
		{"username", 1, 0, 'u'},
		{"password", 1, 0, 'p'},
		{"address", 1, 0, 'a'},
		{"servermode", 0, 0, 's'},
		{"clientmode", 1, 0, 'c'},
		{"localmode", 0, 0, 'l'},
		{0, 0, 0, 0},
	};

	signal(SIGPIPE, sigpipe);
	//f_out = open("test/temp", O_RDWR);
	while ((option = getopt_long(argc, argv, opts, lopts, NULL)) != -1) {
		switch (option) {
			case 'u':
				strcpy(username, optarg);
				break;
			case 'p':
				strcpy(password, optarg);
				break;
			case 'a':
				strcpy(address, optarg);
				break;
			case 's':
				servermode = true;
				break;
			case 'c':
				servermode = false;
				strcpy(clientmode, optarg);
				break;
			case 'l':
				localmode = true;
				break;
			default:
				break;
		}
	}
	if (localmode) {
		int fd_m, fd_s;
		int retval;
		const char *pts_name;
		pid_t fpid;

		fd_m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
		retval = grantpt(fd_m);
		retval = unlockpt(fd_m);
		pts_name = (const char *)ptsname(fd_m);
		fd_s = open(pts_name, O_RDWR);
		fpid = fork();
		if (fpid == 0) {
			char *pargv[] = {"/bin/bash", NULL};
			struct termios ori_settings, new_settings;
			tcgetattr(fd_s, &ori_settings);
			new_settings = ori_settings;
			new_settings.c_lflag &= ~ICANON;
			new_settings.c_lflag |= ISIG;
			new_settings.c_cc[VMIN] = 1;
			new_settings.c_cc[VTIME] = 0;
			tcsetattr(fd_s, TCSANOW, &new_settings);
			setsid();
			dup2(fd_s, 0);
			dup2(fd_s, 1);
			dup2(fd_s, 2);
			execve("/bin/bash", pargv, NULL);
		} else {
			struct termios ori_settings, new_settings;
			int status;
			tcgetattr(fd_m, &ori_settings);
			new_settings = ori_settings;
			new_settings.c_lflag &= ~ECHO;
			new_settings.c_lflag &= ~ICANON;
			new_settings.c_lflag &= ~ISIG;
			new_settings.c_cc[VMIN] = 1;
			new_settings.c_cc[VTIME] = 0;
			tcsetattr(fd_m, TCSANOW, &new_settings);
			wait(&status);
		}
		return 0;
	}
	if (servermode) {
		int retval;
		int server_sockfd, client_sockfd;
		int server_len, client_len;
		struct sockaddr_in server_address;
		struct sockaddr_in client_address;
		pthread_t c_thread;
		int opt_val = 1;

		server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
		server_address.sin_family = AF_INET;
		if (strlen(address) == 0) {
			server_address.sin_addr.s_addr = htonl(INADDR_ANY);
		} else {
			server_address.sin_addr.s_addr = inet_addr(address);
		}
		server_address.sin_port = htons(8080);
		server_len = sizeof(server_address);
		retval = setsockopt(server_sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt_val, sizeof(opt_val));
		retval = bind(server_sockfd, (struct sockaddr *)&server_address, server_len);
		//printf("bind: %d, server_sockfd=%d\n", retval, server_sockfd);
		retval = listen(server_sockfd, 5);
		//printf("listen: %d\n", retval);
		client_len = sizeof(client_address);
		while ((client_sockfd = accept(server_sockfd, (struct sockaddr *)&client_address, (socklen_t *)&client_len)) != -1) {
			int *f;
			f = (int *)malloc(sizeof(int));
			*f = client_sockfd;
			//printf("event comming...\n");
			retval = pthread_create(&c_thread, NULL, client_thread, f);
		}
		//printf("accept error fd=%d, error=%s\n", client_sockfd, strerror(errno));
	} else {
		int retval;
		int server_sockfd;
		int server_len;
		struct sockaddr_in server_address;
		if ((strlen(address) == 0) || (strlen(clientmode) == 0) || (strlen(username) == 0)) {
			return 0;
		}
		server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
		server_address.sin_family = AF_INET;
		server_address.sin_addr.s_addr = inet_addr(address);
		server_address.sin_port = htons(8080);
		server_len = sizeof(server_address);
		retval = connect(server_sockfd, (struct sockaddr *)&server_address, server_len);
		//printf("connect to server, retval=%d\n", retval);
		if (strcmp(clientmode, "slave") == 0) {
			while (1) {
				retval = start_slave(server_sockfd);
				//printf("relink\n");
				//shutdown(server_sockfd, SHUT_RDWR);
				//close(server_sockfd);
				server_sockfd = socket(AF_INET, SOCK_STREAM, 0);
				server_address.sin_family = AF_INET;
				server_address.sin_addr.s_addr = inet_addr(address);
				server_address.sin_port = htons(8080);
				server_len = sizeof(server_address);
				retval = connect(server_sockfd, (struct sockaddr *)&server_address, server_len);
				//printf("connect server: %d\n", retval);
			}
		} else if (strcmp(clientmode, "master") == 0) {
			retval = start_master(server_sockfd);
		}
	}
	//close(f_out);
	return 0;
}


