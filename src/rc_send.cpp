#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>
#include <infiniband/verbs.h>

enum {
	RECV_WRID = 1,
	SEND_WRID = 2,
};
enum bench_mode{
	SINGLE = 1,
	MULTIPLE = 2,
};

static int page_size;

struct context {
	struct ibv_context	*context;
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_dm		*dm;
	union {
		struct ibv_cq		*cq;
		struct ibv_cq_ex	*cq_ex;
	} cq_s;
	struct ibv_qp		*qp;
	struct ibv_qp_ex	*qpx;
	char			*buf;
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	uint64_t		 completion_timestamp_mask;
};

struct dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	__be32 v32;
	int i;
	uint32_t tmp_gid[4];

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		tmp_gid[i] = be32toh(v32);
	}
	memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

enum ibv_mtu mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256:  return IBV_MTU_256;
	case 512:  return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default:   return IBV_MTU_1024;
	}
}
int enum_to_mtu(enum ibv_mtu mtu)
{
	switch (mtu) {
	case IBV_MTU_256:  return 256;
	case IBV_MTU_512:  return 512;
	case IBV_MTU_1024 : return 1024;
	case IBV_MTU_2048 : return 2048;
	case IBV_MTU_4096 : return 4096;
	default:   return 1024;
	}
}
static int connect_ctx(struct context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct dest *dest, int sgid_idx)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = mtu;
    attr.dest_qp_num = dest->qpn;
    attr.rq_psn = dest->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = dest->lid;
    attr.ah_attr.sl = sl;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = port;

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(ctx->qp, &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct dest *client_exch_dest(const char *servername, int port,
						 const struct dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	char service[6];
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct dest *rem_dest = NULL;
	char gid[33];

	if (sprintf(service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg ||
	    write(sockfd, "done", sizeof "done") != sizeof "done") {
		perror("client read/write");
		fprintf(stderr, "Couldn't read/write remote address\n");
		goto out;
	}

	rem_dest = (dest *)malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
						&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

out:
	close(sockfd);
	return rem_dest;
}

static struct dest *server_exch_dest(struct context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct dest *my_dest,
						 int sgid_idx)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char service[6];
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct dest *rem_dest = NULL;
	char gid[33];

	if (sprintf(service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, NULL);
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = (dest *)malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	if (connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}


	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	if (write(connfd, msg, sizeof msg) != sizeof msg ||
	    read(connfd, msg, sizeof msg) != sizeof "done") {
		fprintf(stderr, "Couldn't send/recv local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}


out:
	close(connfd);
	return rem_dest;
}

static struct context *init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port)
{
	struct context *ctx;
	int access_flags = IBV_ACCESS_LOCAL_WRITE;

	ctx = (context *)calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size       = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth   = rx_depth;

	ctx->buf = (char *)memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}

	/* FIXME memset(ctx->buf, 0, size); */
	memset(ctx->buf, 0x7b, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);

	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_dm;
	}

	ctx->cq_s.cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
					ctx->channel, 0);

	if (!ctx->cq_s.cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}

	{
		struct ibv_qp_attr attr;
		memset(&attr, 0, sizeof(attr));
		struct ibv_qp_init_attr init_attr;
		memset(&init_attr, 0, sizeof(init_attr));
		init_attr.send_cq = ctx->cq_s.cq;
		init_attr.recv_cq = ctx->cq_s.cq;
		init_attr.cap.max_send_wr  = rx_depth;
		init_attr.cap.max_recv_wr  = rx_depth;
		init_attr.cap.max_send_sge = 1;
		init_attr.cap.max_recv_sge = 1;
		init_attr.qp_type = IBV_QPT_RC;

		ctx->qp = ibv_create_qp(ctx->pd, &init_attr);

		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}

		ibv_query_qp(ctx->qp, &attr, IBV_QP_CAP, &init_attr);
		if (init_attr.cap.max_inline_data >= size)
			ctx->send_flags |= IBV_SEND_INLINE;
	}

	{
		struct ibv_qp_attr attr;
		memset(&attr, 0, sizeof(attr));
		attr.qp_state        = IBV_QPS_INIT;
		attr.pkey_index      = 0;
		attr.port_num        = port;
		attr.qp_access_flags = 0;

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}

	return ctx;

clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_cq:
	ibv_destroy_cq(ctx->cq_s.cq);

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_dm:
	if (ctx->dm)
		ibv_free_dm(ctx->dm);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;
}

static int close_ctx(struct context *ctx)
{
	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->cq_s.cq)) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ctx->dm) {
		if (ibv_free_dm(ctx->dm)) {
			fprintf(stderr, "Couldn't free DM\n");
			return 1;
		}
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int post_recv(struct context *ctx, int n)
{
	struct ibv_sge list;
	memset(&list, 0, sizeof(list));
	list.addr	= (uintptr_t) ctx->buf;
	list.length = ctx->size;
	list.lkey	= ctx->mr->lkey;

	struct ibv_recv_wr wr;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id	    = RECV_WRID;
	wr.sg_list    = &list;
	wr.num_sge    = 1;

	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int post_send(struct context *ctx,int size)
{
	struct ibv_sge list;
	memset(&list, 0, sizeof(list));
	list.addr	= (uintptr_t) ctx->buf;
	list.length = size;
	list.lkey	= ctx->mr->lkey;

	struct ibv_send_wr wr;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id	    = SEND_WRID;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = ctx->send_flags;

	struct ibv_send_wr *bad_wr;

	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>      listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>     use IB device <dev> (default first device found)\n");
	printf("  -i, --ib-port=<port>   use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>      size of message to exchange (default 0)\n");
	printf("  -m, --mtu=<size>       path MTU (default 1024)\n");
	printf("  -r, --rx-depth=<dep>   number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>    number of exchanges (default 1000)\n");
	printf("  -l, --sl=<sl>          service level value\n");
	printf("  -g, --gid-idx=<gid index> local port gid index\n");
	printf("  -u, --max-size=<size>  max size of message to exchange (default 4096)\n");
	printf("  -b, --batch=<size>   	 batch size of message to write (default 5)\n");
	printf("  -w, --warm-up=<warm up>  number of warm-up iterations (default 50)\n");


}

int post_send_poll(struct context *ctx, int count,int size)
{
	int send = 0, poll = 0;
	int wc_count = 50;

	for (send = 0; send < count; send++) {
		if (post_send(ctx,size)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
	}
	for (poll = 0; poll < count;) {
		int ne = 0;
		struct ibv_wc wc[wc_count];
		do {
			ne = ibv_poll_cq(ctx->cq_s.cq, wc_count, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (ne < 1);
		poll += ne;
	
	}
	//printf("send = %d, poll = %d\n", send, poll);
	return 0;
}

int post_poll_recv(struct context *ctx, int *routs, int iters)
{
	int rcnt = 0;
	while (rcnt < iters) {
		int ret;
		int ne, i;
		int wc_count = 50;
		struct ibv_wc wc[wc_count];

		do {
			ne = ibv_poll_cq(ctx->cq_s.cq, wc_count, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (ne < 1);
		rcnt += ne;

		*routs -= ne;
		if (*routs < ctx->rx_depth) {
			*routs += post_recv(ctx, ctx->rx_depth - *routs);
			if (*routs < ctx->rx_depth) {
				fprintf(stderr, "Couldn't post receive (%d)\n", *routs);
				return 1;
			}
		}
	}
	//printf("poll = %d\n", rcnt);
	return 0;
}

int rdma_send_recv_ops(struct context *ctx, char *servername, int size, int iters, int *routs,int poll_batch)
{
	//int rx_depth = ctx->rx_depth;
	if (servername) {
		for (int i = 0; i < iters / poll_batch; i++) {
			if (post_send_poll(ctx, poll_batch,size)) {
				fprintf(stderr, "Couldn't post_send_poll1\n");
				return 1;
			}
		}
		if (post_send_poll(ctx, iters % poll_batch,size)) {
			fprintf(stderr, "Couldn't post_send_poll2\n");
			return 1;
		}
	} else {
		if (post_poll_recv(ctx, routs, iters)) {
			fprintf(stderr, "Couldn't post_poll_recv\n");
			return 1;
		}
	}
	return 0;
}
int rdma_send_recv_benchmark(struct context *ctx, char *servername, int max_size, int iters, int *routs,enum bench_mode mode,enum ibv_mtu mtu,int poll_batch){

	int rt = 0;
	struct timeval start, end;
	int mtu_size = enum_to_mtu(mtu);
	int header_size = 58;
	int size = 2;
	printf("RDMA Send_Recv Benchmark  \n");
	printf("Connection type : %s\n","RC");
	printf("%-20s %-20s %-20s %-20s\n", "Message size(byte) ", "Iterations", "Effective BW(Gbps)","Total BW(Gbps)");
	if(mode == SINGLE){
		size = max_size;
	}
	while(size <= max_size ){
		//start  clock
		if (gettimeofday(&start, NULL)) {
			perror("gettimeofday");
			return -1;
		}
		rt = rdma_send_recv_ops(ctx,servername,size,iters,routs,poll_batch);
		//end  clock
		if (gettimeofday(&end, NULL)) {
			perror("gettimeofday");
			return -1;
		}
		if(rt){
			printf("rdma_send_recv_benchmark error size %d rt %d \n",size,rt);
			return rt;
		}

		{
			float usec = (end.tv_sec - start.tv_sec) * 1000000 +
				(end.tv_usec - start.tv_usec);
			long long bytes = (long long) size * iters ;
			double  e_bw = bytes*8.0/(usec)/1000;
			bytes += ((size+mtu_size-1)/mtu_size)*iters*header_size;
			double  t_bw = bytes*8.0/(usec)/1000;
			printf("%-20d  %-20d    %-20.3lf %-20.3lf\n",size,iters,e_bw,t_bw);
		}
		if(size < max_size && size*2 > max_size){
			size = max_size;
		}else{
			size *= 2;
		}
	}
	return rt;
}

int main(int argc, char *argv[])
{
	struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct context *ctx;
	struct dest     my_dest;
	struct dest    *rem_dest;
	char                    *ib_devname = NULL;
	char                    *servername = NULL;
	unsigned int             port = 18515;
	int                      ib_port = 1;
	unsigned int             size = 0;
	unsigned int             max_size = 4096;

	enum ibv_mtu		 mtu = IBV_MTU_1024;
	unsigned int             rx_depth = 512;
	unsigned int             iters = 1000;
	int                      routs = 0;
	int                      rcnt, scnt;
	int                      num_cq_events = 0;
	int                      sl = 0;
	int			 			 gidx = -1;
	char			 		 gid[33];
	int 					 warm_up = 50;
	int 					 poll_batch = 5;

	srand48(getpid() * time(NULL));

	while (1) {
		int c;

		static struct option long_options[] = {
			{ "port",     1, NULL, 'p' },
			{ "ib-dev",   1, NULL, 'd' },
			{ "ib-port",  1, NULL, 'i' },
			{ "size",     1, NULL, 's' },
			{ "mtu",      1, NULL, 'm' },
			{ "rx-depth", 1, NULL, 'r' },
			{ "iters",    1, NULL, 'n' },
			{ "sl",       1, NULL, 'l' },
			{ "max-size", 1, NULL, 'u' },
			{ "gid-idx",  1, NULL, 'g' },
			{ "warm-up",  1, NULL, 'w' },
			{ "batch", 	  1, NULL, 'b' },
			{ NULL,		  0, NULL, 0 }  // 结尾元素，必要以表示数组结束
		};

		c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:u:w:b:eg:oOPtcjN",
				long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case 'p':
			port = strtoul(optarg, NULL, 0);
			if (port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			ib_devname = strdup(optarg);
			break;

		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 1) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			size = strtoul(optarg, NULL, 0);
			break;

		case 'm':
			mtu = mtu_to_enum(strtol(optarg, NULL, 0));
			if (mtu == 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'r':
			rx_depth = strtoul(optarg, NULL, 0);
			break;

		case 'n':
			iters = strtoul(optarg, NULL, 0);
			break;

		case 'l':
			sl = strtol(optarg, NULL, 0);
			break;

		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;

		case 'u':
			max_size = strtol(optarg, NULL, 0);
			break;

		case 'w':
			warm_up = strtol(optarg, NULL, 0);
			break;

		case 'b':
			poll_batch = strtol(optarg, NULL, 0);
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1)
		servername = strdup(argv[optind]);
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	page_size = sysconf(_SC_PAGESIZE);

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return 1;
	}

	if (!ib_devname) {
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	ctx = init_ctx(ib_dev, size?size:max_size, rx_depth, ib_port);
	if (!ctx)
		return 1;

	if (!servername) {
		routs = post_recv(ctx, ctx->rx_depth);
		if (routs < ctx->rx_depth) {
			fprintf(stderr, "Couldn't post receive (%d)\n", routs);
			return 1;
		}
	}

	if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}

	my_dest.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
							!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}

	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "can't read sgid of index %d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);

	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       my_dest.lid, my_dest.qpn, my_dest.psn, gid);

	if (servername)
		rem_dest = client_exch_dest(servername, port, &my_dest);
	else
		rem_dest = server_exch_dest(ctx, ib_port, mtu, port, sl,
								&my_dest, gidx);

	if (!rem_dest)
		return 1;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
	       rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	if (servername)
		if (connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest,
					gidx))
			return 1;

	ctx->pending = RECV_WRID;
	//warm up
	rdma_send_recv_ops(ctx,servername,(size == 0? max_size:size)/4,poll_batch,&routs,poll_batch);
	//start bench mark
	if(size){
		if (rdma_send_recv_benchmark(ctx, servername, size, iters, &routs,SINGLE,mtu,poll_batch)) {
			fprintf(stderr, "rdma_send_recv_benchmark error\n");
			return 1;
		}
	}else{
		if (rdma_send_recv_benchmark(ctx, servername, max_size, iters, &routs,MULTIPLE,mtu,poll_batch)) {
			fprintf(stderr, "rdma_send_recv_benchmark error\n");
			return 1;
		}
	}



	ibv_ack_cq_events(ctx->cq_s.cq, num_cq_events);

	if (close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	free(rem_dest);

	return 0;
}
