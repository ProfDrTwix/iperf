/*
 * iperf, Copyright (c) 2014-2020, The Regents of the University of
 * California, through Lawrence Berkeley National Laboratory (subject
 * to receipt of any required approvals from the U.S. Dept. of
 * Energy).  All rights reserved.
 *
 * If you have questions about your rights to use or distribute this
 * software, please contact Berkeley Lab's Technology Transfer
 * Department at TTD@lbl.gov.
 *
 * NOTICE.  This software is owned by the U.S. Department of Energy.
 * As such, the U.S. Government has been granted for itself and others
 * acting on its behalf a paid-up, nonexclusive, irrevocable,
 * worldwide license in the Software to reproduce, prepare derivative
 * works, and perform publicly and display publicly.  Beginning five
 * (5) years after the date permission to assert copyright is obtained
 * from the U.S. Department of Energy, and subject to any subsequent
 * five (5) year renewals, the U.S. Government is granted for itself
 * and others acting on its behalf a paid-up, nonexclusive,
 * irrevocable, worldwide license in the Software to reproduce,
 * prepare derivative works, distribute copies to the public, perform
 * publicly and display publicly, and to permit others to do so.
 *
 * This code is distributed under a BSD style license, see the LICENSE
 * file for complete information.
 */

#include "iperf_config.h"

#ifdef HAVE_SENDMMSG
#define _GNU_SOURCE	/* required for sendmmg() */
#endif /* HAVE_SENDMMSG */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <sys/time.h>
#include <sys/select.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <stdio.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>

#include "iperf.h"
#include "iperf_api.h"
#include "iperf_util.h"
#include "iperf_udp.h"
#include "timer.h"
#include "net.h"
#include "cjson.h"
#include "portable_endian.h"

#if defined(HAVE_INTTYPES_H)
# include <inttypes.h>
#else
# ifndef PRIu64
#  if sizeof(long) == 8
#   define PRIu64		"lu"
#  else
#   define PRIu64		"llu"
#  endif
# endif
#endif


long
perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                int cpu, int group_fd, unsigned long flags)
{
    int ret;

    ret = syscall(__NR_perf_event_open, hw_event, pid, cpu,
                    group_fd, flags);
    return ret;
}

/* iperf_udp_recv
 *
 * receives the data for UDP
 */
int
iperf_udp_recv(struct iperf_stream *sp)
{
    uint32_t  sec, usec;
    uint64_t  pcount;
    int       r;
    int       size = sp->settings->blksize;
    int       first_packet = 0;
    double    transit = 0, d = 0;
    struct iperf_time sent_time, arrival_time, temp_time;

/*
 * Tests under WSL Ubunto 20 shows that using `mmsgrecv()` has no prformance advantage
 * over `Nread()`. It may be that is is because of extra "read" required to find that
 * there are is more data to read.
 * In addition, `mmsgrecv` requires the use of timeout parameter,
 * as otherwise it can get stack after receiving the last message send.
 * However, adding the timeout may have major impact on the performance.
 * Therefore, using `mmsgrecv` is commented out.
 * To fully support `mmsgrecv`, in addition to uncommenting (here and in other parts
 * of `iperf_udp_recv`), `sp->buffer` should be changed to `pbuf`.
 */
    int msgs_recvd;
    int i;
    char *pbuf;

    // Select message reading method
#ifdef HAVE_SEND_RECVMMSG

    struct timespec tmo;

    if (sp->settings->send_recvmmsg == 1) { // Use recvmmsg()
    	// Set read timeout
	tmo.tv_sec = sp->settings->rcv_timeout.secs;
	tmo.tv_nsec = sp->settings->rcv_timeout.usecs;

        // Receive at least one message
        do {
	    msgs_recvd = recvmmsg(sp->socket, sp->msg, sp->settings->burst, MSG_WAITFORONE, &tmo);
	} while (msgs_recvd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        if (msgs_recvd <= 0) {
            r = msgs_recvd;
        } else {
            for (i = 0, r = 0; i < msgs_recvd; i++) {
                r += sp->msg[i].msg_hdr.msg_iov->iov_len;
            }
        } 
    } // recvmmsg
    else
#endif // HAVE_SEND_RECVMMSG
    { // Nread
        r = Nread(sp->socket, sp->buffer, size, Pudp);
        msgs_recvd = 1;
        // `sp->msg[0].msg_hdr.msg_iov->iov_base = sp->buffer;` - not needed as initialized as part of Stream init
    }

    r = Nread(sp->socket, sp->buffer, size, Pudp);

    /*
     * If we got an error in the read, or if we didn't read anything
     * because the underlying read(2) got a EAGAIN, then skip packet
     * processing.
     */
    if (r <= 0)
        return r;

    /* Only count bytes received while we're in the correct state. */
    if (sp->test->state == TEST_RUNNING) {

	/*
	 * For jitter computation below, it's important to know if this
	 * packet is the first packet received.
	 */
	if (sp->result->bytes_received == 0) {
	    first_packet = 1;
	}

	sp->result->bytes_received += r;
	sp->result->bytes_received_this_interval += r;

	// Go over all nessages received to evalauet packet count and timings
	for (i = 0; i < msgs_recvd; i++) {
            pbuf = sp->msg[i].msg_hdr.msg_iov->iov_base; // current msg buffer

	/* Dig the various counters out of the incoming UDP packet */
	if (sp->test->udp_counters_64bit) {
	    memcpy(&sec, sp->buffer, sizeof(sec));
	    memcpy(&usec, sp->buffer+4, sizeof(usec));
	    memcpy(&pcount, sp->buffer+8, sizeof(pcount));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = be64toh(pcount);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}
	else {
	    uint32_t pc;
	    memcpy(&sec, sp->buffer, sizeof(sec));
	    memcpy(&usec, sp->buffer+4, sizeof(usec));
	    memcpy(&pc, sp->buffer+8, sizeof(pc));
	    sec = ntohl(sec);
	    usec = ntohl(usec);
	    pcount = ntohl(pc);
	    sent_time.secs = sec;
	    sent_time.usecs = usec;
	}

	if (sp->test->debug)
	    fprintf(stderr, "pcount %" PRIu64 " packet_count %d\n", pcount, sp->packet_count);

	/*
	 * Try to handle out of order packets.  The way we do this
	 * uses a constant amount of storage but might not be
	 * correct in all cases.  In particular we seem to have the
	 * assumption that packets can't be duplicated in the network,
	 * because duplicate packets will possibly cause some problems here.
	 *
	 * First figure out if the sequence numbers are going forward.
	 * Note that pcount is the sequence number read from the packet,
	 * and sp->packet_count is the highest sequence number seen so
	 * far (so we're expecting to see the packet with sequence number
	 * sp->packet_count + 1 arrive next).
	 */
	if (pcount >= sp->packet_count + 1) {

	    /* Forward, but is there a gap in sequence numbers? */
	    if (pcount > sp->packet_count + 1) {
		/* There's a gap so count that as a loss. */
		sp->cnt_error += (pcount - 1) - sp->packet_count;
	    }
	    /* Update the highest sequence number seen so far. */
	    sp->packet_count = pcount;
	} else {

            /*
             * Sequence number went backward (or was stationary?!?).
             * This counts as an out-of-order packet.
             */
            sp->outoforder_packets++;

	    /*
	     * If we have lost packets, then the fact that we are now
	     * seeing an out-of-order packet offsets a prior sequence
	     * number gap that was counted as a loss.  So we can take
	     * away a loss.
	     */
	    if (sp->cnt_error > 0)
		sp->cnt_error--;

	    /* Log the out-of-order packet */
            if (sp->test->debug) 
                fprintf(stderr, "OUT OF ORDER - incoming packet sequence %" PRIu64 " but expected sequence %d on stream %d\n", pcount, sp->packet_count + 1, sp->socket);
        }

	/*
	 * jitter measurement
	 *
	 * This computation is based on RFC 1889 (specifically
	 * sections 6.3.1 and A.8).
	 *
	 * Note that synchronized clocks are not required since
	 * the source packet delta times are known.  Also this
	 * computation does not require knowing the round-trip
	 * time.
	 */
	iperf_time_now(&arrival_time);

	iperf_time_diff(&arrival_time, &sent_time, &temp_time);
	transit = iperf_time_in_secs(&temp_time);

        /* Hack to handle the first packet by initializing prev_transit. */
        if (first_packet) {
            sp->prev_transit = transit;
            first_packet = 0;
        }

	d = transit - sp->prev_transit;
	if (d < 0)
	    d = -d;
	sp->prev_transit = transit;
	sp->jitter += (d - sp->jitter) / 16.0;

	} // for over all received messages

    } /* if state is TEST_RUNNING */
    else {
	if (sp->test->debug)
	    printf("Late receive, state = %d\n", sp->test->state);
    }

    return r;
}


/* iperf_udp_send
 *
 * sends the data for UDP
 */
int
iperf_udp_send(struct iperf_stream *sp)
{
    int r = 0;
    int size = sp->settings->blksize;
    struct iperf_time before;
    char *buf = sp->buffer;
    uint32_t  sec, usec;


    long long count;
    int fd;
    struct perf_event_attr pe;
    
    memset(&pe, 0, sizeof(struct perf_event_attr));
    pe.type = PERF_TYPE_HARDWARE;
    pe.size = sizeof(struct perf_event_attr);
    pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    // Don't count hypervisor events.
    pe.exclude_hv = 1;  

    fd = perf_event_open(&pe, 0, getpid(),-1,0);
    if (fd == -1) {
        printf("Error opening leader %llx\n", pe.config);
        exit(EXIT_FAILURE);
    }

#ifdef HAVE_SENDMMSG
    int i, j, k;
    char *b;
#endif /* HAVE_SENDMMSG */

#ifdef HAVE_SENDMMSG
    /* if sendmmsg is used - set buffer pointer to next buffer */
    if (sp->settings->send_recvmmsg == 1) {
        i = sp->sendmmsg_buffered_packets_count++;
        sp->msg[i].msg_hdr.msg_iovlen = 1;
        buf = sp->pbuf;
        sp->msg[i].msg_hdr.msg_iov->iov_base = buf;
        sp->pbuf += sp->settings->blksize;
        sp->msg[i].msg_hdr.msg_iov->iov_len = size;
    }
#endif /* HAVE_SENDMMSG */

    /* Set message packet count */
    ++sp->packet_count;
    if (sp->test->udp_counters_64bit) {
	uint64_t  pcount;
	pcount = htobe64(sp->packet_count);
	memcpy(buf+8, &pcount, sizeof(pcount));
    } else {
	uint32_t  pcount;
	pcount = htonl(sp->packet_count);
	memcpy(buf+8, &pcount, sizeof(pcount));	
    }

/* Use sendmmsg when approriate to send the packet, else use Nwrite */
#ifdef HAVE_SENDMMSG
    if (sp->settings->send_recvmmsg == 1) {
	/* When enough "burst" packets were buffered - send them */
	if (sp->sendmmsg_buffered_packets_count >= sp->settings->burst) {
            /* Set actual sending time to all packets*/
            iperf_time_now(&before);
            sec = htonl(before.secs);
	    usec = htonl(before.usecs);
            for (i = 0; i < sp->sendmmsg_buffered_packets_count; i++) {
                b = sp->msg[i].msg_hdr.msg_iov->iov_base;
                memcpy(b, &sec, sizeof(sec));
		memcpy(b+4, &usec, sizeof(usec));	
	    }

	    /* Sending messages and making sure all packets are sent */
	    i = 0;	/* count of messages sent */
	    r = 0;	/* total bytes sent */

        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

	    while (i < sp->sendmmsg_buffered_packets_count) {
	        j = sendmmsg(sp->socket, &sp->msg[i], sp->sendmmsg_buffered_packets_count - i, MSG_DONTWAIT);
		if (j < 0) {
		    r = j;
		    break;
		}

        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        read(fd, &count, sizeof(long long));

        printf("Used %lld instructions\n", count);

		if (sp->test->debug && i+j < sp->sendmmsg_buffered_packets_count)
                    printf("sendmmsg() sent only %d messges out of %d still bufferred\n",
		           j, sp->sendmmsg_buffered_packets_count-i);

                for (k = i; k < i+j; k++) { /* accumulate number of bytes sent */
                    r += sp->msg[k].msg_len;
                }
                i += j; /* accumulate number of messages received */
	    }

	    if (sp->test->debug)
                printf("sendmmsg() %s. Sent %d messges out of %d bufferred. %d bytes sent. (errno=%d: %s)\n",
		       ((r > 0)? "succesful":"FAILED"), i, sp->sendmmsg_buffered_packets_count, r, errno, strerror(errno));
	    
	    sp->sendmmsg_buffered_packets_count = 0;
            sp->pbuf = sp->buffer;
	}
    }
    else
#endif	/* HAVE_SENDMMSG */
    {
        /* Set sending time */
        iperf_time_now(&before);
        sec = htonl(before.secs);
	usec = htonl(before.usecs);
        memcpy(buf, &sec, sizeof(sec));
	memcpy(buf+4, &usec, sizeof(usec));

        /* Write singe message */
        r = Nwrite(sp->socket, buf, size, Pudp);
    }

    if (r < 0) {
	if (sp->test->debug)
	    printf("Write failed with errno %d: %s\n", errno, strerror(errno));
	return r;
    }

    sp->result->bytes_sent += r;
    sp->result->bytes_sent_this_interval += r;

    if (sp->test->debug) {
	if (sp->settings->send_recvmmsg == 0 || r > 0)
	    printf("sent %d bytes of %d bytes buffers, total %" PRIu64 "\n", r, size, sp->result->bytes_sent);
    }

    return r;
}


/**************************************************************************/

/*
 * The following functions all have to do with managing UDP data sockets.
 * UDP of course is connectionless, so there isn't really a concept of
 * setting up a connection, although connect(2) can (and is) used to
 * bind the remote end of sockets.  We need to simulate some of the
 * connection management that is built-in to TCP so that each side of the
 * connection knows about each other before the real data transfers begin.
 */

/*
 * Set and verify socket buffer sizes.
 * Return 0 if no error, -1 if an error, +1 if socket buffers are
 * potentially too small to hold a message.
 */
int
iperf_udp_buffercheck(struct iperf_test *test, int s)
{
    int rc = 0;
    int sndbuf_actual, rcvbuf_actual;

    /*
     * Set socket buffer size if requested.  Do this for both sending and
     * receiving so that we can cover both normal and --reverse operation.
     */
    int opt;
    socklen_t optlen;

    if ((opt = test->settings->socket_bufsize)) {
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
        if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt)) < 0) {
            i_errno = IESETBUF;
            return -1;
        }
    }

    /* Read back and verify the sender socket buffer size */
    optlen = sizeof(sndbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf_actual, &optlen) < 0) {
	i_errno = IESETBUF;
	return -1;
    }
    if (test->debug) {
	printf("SNDBUF is %u, expecting %u\n", sndbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > sndbuf_actual) {
	i_errno = IESETBUF2;
	return -1;
    }
    if (STREAM_BUFSIZE(test) > sndbuf_actual) {
	char str[80];
	snprintf(str, sizeof(str),
		 "Block size %d > sending socket buffer size %d",
		 STREAM_BUFSIZE(test), sndbuf_actual);
	warning(str);
	rc = 1;
    }

    /* Read back and verify the receiver socket buffer size */
    optlen = sizeof(rcvbuf_actual);
    if (getsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf_actual, &optlen) < 0) {
	i_errno = IESETBUF;
	return -1;
    }
    if (test->debug) {
	printf("RCVBUF is %u, expecting %u\n", rcvbuf_actual, test->settings->socket_bufsize);
    }
    if (test->settings->socket_bufsize && test->settings->socket_bufsize > rcvbuf_actual) {
	i_errno = IESETBUF2;
	return -1;
    }
    if (STREAM_BUFSIZE(test) > rcvbuf_actual) {
	char str[80];
	snprintf(str, sizeof(str),
		 "Block size %d > receiving socket buffer size %d",
		 STREAM_BUFSIZE(test), rcvbuf_actual);
	warning(str);
	rc = 1;
    }

    if (test->json_output) {
	cJSON_AddNumberToObject(test->json_start, "sock_bufsize", test->settings->socket_bufsize);
	cJSON_AddNumberToObject(test->json_start, "sndbuf_actual", sndbuf_actual);
	cJSON_AddNumberToObject(test->json_start, "rcvbuf_actual", rcvbuf_actual);
    }

    return rc;
}

/*
 * iperf_udp_accept
 *
 * Accepts a new UDP "connection"
 */
int
iperf_udp_accept(struct iperf_test *test)
{
    struct sockaddr_storage sa_peer;
    int       buf;
    socklen_t len;
    int       sz, s;
    int	      rc;

    /*
     * Get the current outstanding socket.  This socket will be used to handle
     * data transfers and a new "listening" socket will be created.
     */
    s = test->prot_listener;

    /*
     * Grab the UDP packet sent by the client.  From that we can extract the
     * client's address, and then use that information to bind the remote side
     * of the socket to the client.
     */
    len = sizeof(sa_peer);
    if ((sz = recvfrom(test->prot_listener, &buf, sizeof(buf), 0, (struct sockaddr *) &sa_peer, &len)) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    if (connect(s, (struct sockaddr *) &sa_peer, len) < 0) {
        i_errno = IESTREAMACCEPT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_udp_buffercheck(test, s);
    if (rc < 0)
	/* error */
	return rc;
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
	if (test->settings->socket_bufsize == 0) {
	    int bufsize = STREAM_BUFSIZE(test) + UDP_BUFFER_EXTRA;
	    printf("Increasing socket buffer size to %d\n",
		bufsize);
	    test->settings->socket_bufsize = bufsize;
	    rc = iperf_udp_buffercheck(test, s);
	    if (rc < 0)
		return rc;
	}
    }

#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is specified, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	unsigned int fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		printf("Setting fair-queue socket pacing to %u\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		warning("Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

    /*
     * Create a new "listening" socket to replace the one we were using before.
     */
    test->prot_listener = netannounce(test->settings->domain, Pudp, test->bind_address, test->bind_dev, test->server_port);
    if (test->prot_listener < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    FD_SET(test->prot_listener, &test->read_set);
    test->max_fd = (test->max_fd < test->prot_listener) ? test->prot_listener : test->max_fd;

    /* Let the client know we're ready "accept" another UDP "stream" */
    buf = 987654321;		/* any content will work here */
    if (write(s, &buf, sizeof(buf)) < 0) {
        i_errno = IESTREAMWRITE;
        return -1;
    }

    return s;
}


/*
 * iperf_udp_listen
 *
 * Start up a listener for UDP stream connections.  Unlike for TCP,
 * there is no listen(2) for UDP.  This socket will however accept
 * a UDP datagram from a client (indicating the client's presence).
 */
int
iperf_udp_listen(struct iperf_test *test)
{
    int s;

    if ((s = netannounce(test->settings->domain, Pudp, test->bind_address, test->bind_dev, test->server_port)) < 0) {
        i_errno = IESTREAMLISTEN;
        return -1;
    }

    /*
     * The caller will put this value into test->prot_listener.
     */
    return s;
}


/*
 * iperf_udp_connect
 *
 * "Connect" to a UDP stream listener.
 */
int
iperf_udp_connect(struct iperf_test *test)
{
    int s, buf, sz;
#ifdef SO_RCVTIMEO
    struct timeval tv;
#endif
    int rc;

    /* Create and bind our local socket. */
    if ((s = netdial(test->settings->domain, Pudp, test->bind_address, test->bind_dev, test->bind_port, test->server_hostname, test->server_port, -1)) < 0) {
        i_errno = IESTREAMCONNECT;
        return -1;
    }

    /* Check and set socket buffer sizes */
    rc = iperf_udp_buffercheck(test, s);
    if (rc < 0)
	/* error */
	return rc;
    /*
     * If the socket buffer was too small, but it was the default
     * size, then try explicitly setting it to something larger.
     */
    if (rc > 0) {
	if (test->settings->socket_bufsize == 0) {
	    int bufsize = STREAM_BUFSIZE(test) + UDP_BUFFER_EXTRA;
	    printf("Increasing socket buffer size to %d\n",
		bufsize);
	    test->settings->socket_bufsize = bufsize;
	    rc = iperf_udp_buffercheck(test, s);
	    if (rc < 0)
		return rc;
	}
    }

#if defined(HAVE_SO_MAX_PACING_RATE)
    /* If socket pacing is available and not disabled, try it. */
    if (test->settings->fqrate) {
	/* Convert bits per second to bytes per second */
	unsigned int fqrate = test->settings->fqrate / 8;
	if (fqrate > 0) {
	    if (test->debug) {
		printf("Setting fair-queue socket pacing to %u\n", fqrate);
	    }
	    if (setsockopt(s, SOL_SOCKET, SO_MAX_PACING_RATE, &fqrate, sizeof(fqrate)) < 0) {
		warning("Unable to set socket pacing");
	    }
	}
    }
#endif /* HAVE_SO_MAX_PACING_RATE */
    {
	unsigned int rate = test->settings->rate / 8;
	if (rate > 0) {
	    if (test->debug) {
		printf("Setting application pacing to %u\n", rate);
	    }
	}
    }

#ifdef SO_RCVTIMEO
    /* 30 sec timeout for a case when there is a network problem. */
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *)&tv, sizeof(struct timeval));
#endif

    /*
     * Write a datagram to the UDP stream to let the server know we're here.
     * The server learns our address by obtaining its peer's address.
     */
    buf = 123456789;		/* this can be pretty much anything */
    if (write(s, &buf, sizeof(buf)) < 0) {
        // XXX: Should this be changed to IESTREAMCONNECT?
        i_errno = IESTREAMWRITE;
        return -1;
    }

    /*
     * Wait until the server replies back to us.
     */
    if ((sz = recv(s, &buf, sizeof(buf), 0)) < 0) {
        i_errno = IESTREAMREAD;
        return -1;
    }

    return s;
}


/* iperf_udp_init
 *
 * initializer for UDP streams in TEST_START
 */
int
iperf_udp_init(struct iperf_test *test)
{
    return 0;
}
