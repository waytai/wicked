/*
 * Network socket related functionality for wicked.
 *
 * Copyright (C) 2009-2010 Olaf Kirch <okir@suse.de>
 */

#ifndef __WICKED_SOCKET_PRIV_H__
#define __WICKED_SOCKET_PRIV_H__

#include <stdio.h>

#include <wicked/types.h>
#include <wicked/socket.h>
#include "buffer.h"

struct ni_socket_ops {
	int		(*begin_buffering)(ni_socket_t *);
	int		(*push)(ni_socket_t *);
};

struct ni_socket {
	unsigned int	refcount;

	int		__fd;
	FILE *		wfile;
	FILE *		rfile;
	unsigned int	stream : 1,
			active : 1,
			error : 1,
			shutdown_after_send : 1;
	int		poll_flags;

	ni_buffer_t	rbuf;
	ni_buffer_t	wbuf;

	const struct ni_socket_ops *iops;

	int		(*get_timeout)(const ni_socket_t *, struct timeval *);
	void		(*receive)(ni_socket_t *);
	void		(*transmit)(ni_socket_t *);
	int		(*process_request)(ni_socket_t *);
	int		(*accept)(ni_socket_t *, uid_t, gid_t);
	void		(*check_timeout)(ni_socket_t *, const struct timeval *);

	void *		user_data;
};


#endif /* __WICKED_SOCKET_PRIV_H__ */
