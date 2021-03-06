/*-
 * xnumon - monitor macOS for malicious activity
 * https://www.roe.ch/xnumon
 *
 * Copyright (c) 2017-2018, Daniel Roethlisberger <daniel@roe.ch>.
 * All rights reserved.
 *
 * Licensed under the Open Software License version 3.0.
 */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include "getpath.h"

#define SOCKADDR4 "0.0.0.0"
#define SOCKPORT 54345

int
main(int argc, char *argv[]) {
	printf("spec:testcase returncode=0\n");
	printf("spec:socket-listen subject.pid=%i subject.image.path=%s "
	       "sockaddr="SOCKADDR4" sockport=%i proto=tcp\n",
	       getpid(), getpath(), SOCKPORT);
	fflush(stdout);

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		return 1;
	}

	struct sockaddr_in sai;
	bzero(&sai, sizeof(sai));
	sai.sin_family = AF_INET;
	sai.sin_port = htons(SOCKPORT);
	if (inet_pton(AF_INET, SOCKADDR4, &sai.sin_addr) != 1) {
		perror("inet_pton");
		return 1;
	}
	if (bind(fd, (struct sockaddr *)&sai, sizeof(sai)) == -1) {
		perror("bind");
		return 1;
	}
	if (listen(fd, 5) == -1) {
		perror("listen");
		return 1;
	}

	/* sleep(1); */
	close(fd);
	return 0;
}

