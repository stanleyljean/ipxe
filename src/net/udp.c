#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <byteswap.h>
#include <latch.h>
#include <errno.h>
#include <gpxe/in.h>
#include <gpxe/ip.h>
#include <gpxe/ip6.h>
#include <gpxe/udp.h>
#include <gpxe/init.h>
#include <gpxe/pkbuff.h>
#include <gpxe/netdevice.h>
#include <gpxe/tcpip_if.h>

/** @file
 *
 * UDP protocol
 */

/**
 * List of registered UDP connections
 */
static LIST_HEAD ( udp_conns );

/**
 * Some utility functions
 */
static inline void copy_sockaddr ( struct sockaddr *source, struct sockaddr *dest ) {
	memcpy ( dest, source, sizeof ( *dest ) );
}

static inline uint16_t * dest_port ( struct sockaddr *sock ) {
	switch ( sock->sa_family ) {
	case AF_INET:
		return &sock->sin.sin_port;
	case AF_INET6:
		return &sock->sin6.sin6_port;
	}
	return NULL;
}

/**
 * Dump the UDP header
 *
 * @v udphdr	UDP header
 */
void udp_dump ( struct udp_header *udphdr ) {

	/* Print UDP header for debugging */
	DBG ( "UDP header at %#x + %d\n", udphdr, sizeof ( *udphdr ) );
	DBG ( "\tSource Port = %d\n", ntohs ( udphdr->source_port ) );
	DBG ( "\tDestination Port = %d\n", ntohs ( udphdr->dest_port ) );
	DBG ( "\tLength = %d\n", ntohs ( udphdr->len ) );
	DBG ( "\tChecksum = %x\n", ntohs ( udphdr->chksum ) );
	DBG ( "\tChecksum located at %#x\n", &udphdr->chksum );
}

/**
 * Open a UDP connection
 *
 * @v conn      UDP connection
 * @v peer      Destination socket address
 *
 * This function stores the socket address within the connection
 */
void udp_connect ( struct udp_connection *conn, struct sockaddr *peer ) {
	copy_sockaddr ( peer, &conn->sin );

	/* Not sure if this should add the connection to udp_conns; If it does,
	 * uncomment the following code
	 */
//	list_add ( &conn->list, &udp_conns );
}

/**
 * Initialize a UDP connection
 *
 * @v conn      UDP connection
 * @v udp_op	UDP operations
 */
void udp_init ( struct udp_connection *conn, struct udp_operations *udp_op ) {
	conn->local_port = 0;
	conn->tx_pkb = NULL;
	if ( udp_op != NULL ) {
		conn->udp_op = udp_op;
	}
}

/**
 * User request to send data via a UDP connection
 *
 * @v conn	UDP connection
 *
 * This function allocates buffer space and invokes the function's senddata()
 * callback. The callback may use the buffer space
 */
int udp_senddata ( struct udp_connection *conn ) {
	conn->tx_pkb = alloc_pkb ( UDP_MAX_TXPKB );
	if ( conn->tx_pkb == NULL ) {
		DBG ( "Error allocating packet buffer of length %d\n",
							UDP_MAX_TXPKB );
		return -ENOMEM;
	}
	pkb_reserve ( conn->tx_pkb, UDP_MAX_HLEN );
	conn->udp_op->senddata ( conn, conn->tx_pkb, 
				 pkb_available ( conn->tx_pkb ) );
	return 0;
}
		
/**
 * Transmit data via a UDP connection
 *
 * @v conn      UDP connection
 * @v data      Data to send
 * @v len       Length of data
 *
 * This function fills up the UDP headers and sends the data. Discover the
 * network protocol through the sa_family field in the destination socket
 * address.
 */
int udp_send ( struct udp_connection *conn, const void *data, size_t len ) {
       	struct udp_header *udphdr;		/* UDP header */
	struct sockaddr *sock = &conn->sin;	/* Destination sockaddr */
	uint16_t *dest;

	/* Copy payload */
	memmove ( pkb_put ( conn->tx_pkb, len ), data, len );

	/*
	 * Add the UDP header
	 *
	 * Covert all 16- and 32- bit integers into network btye order before
	 * sending it over the network
	 */
	udphdr = pkb_push ( conn->tx_pkb, sizeof ( *udphdr ) );
	if ( (dest = dest_port ( sock ) ) == NULL ) {
		DBG ( "Network family %d not supported\n", sock->sa_family );
		return -EAFNOSUPPORT;
	}
	udphdr->dest_port = *dest;
	udphdr->source_port = conn->local_port;
	udphdr->len = htons ( pkb_len ( conn->tx_pkb ) );
	udphdr->chksum = htons ( calc_chksum ( udphdr, sizeof ( *udphdr ) ) );

	udp_dump ( udphdr );

	/* Send it to the next layer for processing */
	return trans_tx ( conn->tx_pkb, &udp_protocol, sock );
}

/**
 * Send data to a specified address
 *
 * @v conn      UDP connection
 * @v peer      Destination address
 * @v data      Data to send
 * @v len       Length of data
 */
int udp_sendto ( struct udp_connection *conn, struct sockaddr *peer,
		 const void *data, size_t len ) {
	struct sockaddr tempsock;
	copy_sockaddr ( &conn->sin, &tempsock );
	copy_sockaddr ( peer, &conn->sin );
	int rc = udp_send ( conn, data, len );
	copy_sockaddr ( &tempsock, &conn->sin );
	return rc;
}

/**
 * Close a UDP connection
 *
 * @v conn      UDP connection
 */
void udp_close ( struct udp_connection *conn ) {
	list_del ( &conn->list );
}

/**
 * Open a local port
 *
 * @v conn		UDP connection
 * @v local_port	Local port on which to open connection
 *
 * This does not support the 0 port option correctly yet
 */
int udp_open ( struct udp_connection *conn, uint16_t local_port ) {
	struct udp_connection *connr;
	uint16_t min_port = 0xffff;

	/* Iterate through udp_conns to see if local_port is available */
	list_for_each_entry ( connr, &udp_conns, list ) {
		if ( connr->local_port == local_port ) {
			return -EISCONN;
		}
		if ( min_port > connr->local_port ) {
			min_port = connr->local_port;
		}
	}
	/* This code is buggy. I will update it soon :) */
	conn->local_port = local_port == 0 ? min_port > 1024 ? 1024 :
						min_port + 1 : local_port;

	/* Add the connection to the list of listening connections */
	list_add ( &conn->list, &udp_conns );
	return 0;
}

/**
 * Process a received packet
 *
 * @v pkb	       Packet buffer
 * @v src_net_addr      Source network address
 * @v dest_net_addr     Destination network address
 */
void udp_rx ( struct pk_buff *pkb, struct in_addr *src_net_addr __unused,
			struct in_addr *dest_net_addr __unused ) {
	struct udp_header *udphdr = pkb->data;
	struct udp_connection *conn;
	uint16_t ulen;
	uint16_t chksum;

	udp_dump ( udphdr );

	/* Validate the packet and the UDP length */
	if ( pkb_len ( pkb ) < sizeof ( *udphdr ) ) {
		DBG ( "UDP packet too short (%d bytes)\n",
		      pkb_len ( pkb ) );
		return;
	}

	ulen = ntohs ( udphdr->len );
	if ( ulen != pkb_len ( pkb ) ) {
		DBG ( "Inconsistent UDP packet length (%d bytes)\n",
		      pkb_len ( pkb ) );
		return;
	}

	/* Verify the checksum */
	chksum = calc_chksum ( pkb->data, pkb_len ( pkb ) );
	if ( chksum != 0xffff ) {
		DBG ( "Bad checksum %d\n", chksum );
		return;
	}

	/* Todo: Check if it is a broadcast or multicast address */

	/* Demux the connection */
	list_for_each_entry ( conn, &udp_conns, list ) {
		if ( conn->local_port == ntohs ( udphdr->dest_port ) ) {
			goto conn;
		}
	}
	return;

	conn:
	/** Strip off the UDP header */
	pkb_pull ( pkb, sizeof ( *udphdr ) );

	/** Allocate max possible buffer space to the tx buffer */
	conn->tx_pkb = alloc_pkb ( UDP_MAX_TXPKB );
	pkb_reserve ( conn->tx_pkb, UDP_MAX_HLEN );

	/** Call the application's callback */
	conn->udp_op->newdata ( conn, pkb->data, ulen - sizeof ( *udphdr ) );
}

struct tcpip_protocol udp_protocol  = {
	.name = "UDP",
	.rx = udp_rx,
	.trans_proto = IP_UDP,
	.csum_offset = 6,
};

TCPIP_PROTOCOL ( udp_protocol );
