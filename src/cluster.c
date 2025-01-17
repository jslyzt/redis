/* Redis Cluster implementation.
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
POSIX_ONLY(#include <sys/socket.h>)
POSIX_ONLY(#include <arpa/inet.h>)
#include <fcntl.h>
POSIX_ONLY(#include <unistd.h>)
POSIX_ONLY(#include <sys/socket.h>)
#include <sys/stat.h>
POSIX_ONLY(#include <sys/file.h>)
#include <math.h>

WIN32_ONLY(extern int WSIOCP_QueueAccept(int listenfd);)

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
	clusterNode* myself = NULL;

clusterNode* createClusterNode(char* nodename, int flags);
int clusterAddNode(clusterNode* node);
void clusterAcceptHandler(aeEventLoop* el, int fd, void* privdata, int mask);
void clusterReadHandler(aeEventLoop* el, int fd, void* privdata, int mask);
void clusterSendPing(clusterLink* link, int type);
void clusterSendFail(char* nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode* node, clusterMsg* request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode* n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode* clusterLookupNode(char* name);
int clusterNodeAddSlave(clusterNode* master, clusterNode* slave);
int clusterAddSlot(clusterNode* n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode* node);
int clusterNodeSetSlotBit(clusterNode* n, int slot);
void clusterSetMaster(clusterNode* n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char* bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink* link, clusterNode* node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode* n);
void clusterDelNode(clusterNode* delnode);
sds representRedisNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

 /* Load the cluster config from 'filename'.
  *
  * If the file does not exist or is zero-length (this may happen because
  * when we lock the nodes.conf file, we create a zero-length one for the
  * sake of locking if it does not already exist), REDIS_ERR is returned.
  * If the configuration was loaded from the file, REDIS_OK is returned. */
int clusterLoadConfig(char* filename) {
	FILE* fp = fopen(filename, IF_WIN32("rb", "r"));
	struct IF_WIN32(_stat64, stat) sb;                                           // TODO: verify for 32-bit
	char* line;
	int maxline, j;

	if (fp == NULL) {
		if (errno == ENOENT) {
			return REDIS_ERR;
		}
		else {
			redisLog(REDIS_WARNING,
				"Loading the cluster node config from %s: %s",
				filename, strerror(errno));
			exit(1);
		}
	}

	/* Check if the file is zero-length: if so return REDIS_ERR to signal
	 * we have to write the config. */
	if (fstat(fileno(fp), &sb) != -1 && sb.st_size == 0) {
		fclose(fp);
		return REDIS_ERR;
	}

	/* Parse the file. Note that single lines of the cluster config file can
	 * be really long as they include all the hash slots of the node.
	 * This means in the worst possible case, half of the Redis slots will be
	 * present in a single line, possibly in importing or migrating state, so
	 * together with the node ID of the sender/receiver.
	 *
	 * To simplify we allocate 1024+REDIS_CLUSTER_SLOTS*128 bytes per line. */
	maxline = 1024 + REDIS_CLUSTER_SLOTS * 128;
	line = zmalloc(maxline);
	while (fgets(line, maxline, fp) != NULL) {
		int argc;
		sds* argv;
		clusterNode* n, * master;
		char* p, * s;

		/* Skip blank lines, they can be created either by users manually
		 * editing nodes.conf or by the config writing process if stopped
		 * before the truncate() call. */
		if (line[0] == '\n') continue;

		/* Split the line into arguments for processing. */
		argv = sdssplitargs(line, &argc);
		if (argv == NULL) goto fmterr;

		/* Handle the special "vars" line. Don't pretend it is the last
		 * line even if it actually is when generated by Redis. */
		if (strcasecmp(argv[0], "vars") == 0) {
			for (j = 1; j < argc; j += 2) {
				if (strcasecmp(argv[j], "currentEpoch") == 0) {
					server.cluster->currentEpoch =
						strtoull(argv[j + 1], NULL, 10);
				}
				else if (strcasecmp(argv[j], "lastVoteEpoch") == 0) {
					server.cluster->lastVoteEpoch =
						strtoull(argv[j + 1], NULL, 10);
				}
				else {
					redisLog(REDIS_WARNING,
						"Skipping unknown cluster config variable '%s'",
						argv[j]);
				}
			}
			sdsfreesplitres(argv, argc);
			continue;
		}

		/* Regular config lines have at least eight fields */
		if (argc < 8) goto fmterr;

		/* Create this node if it does not exist */
		n = clusterLookupNode(argv[0]);
		if (!n) {
			n = createClusterNode(argv[0], 0);
			clusterAddNode(n);
		}
		/* Address and port */
		if ((p = strrchr(argv[1], ':')) == NULL) goto fmterr;
		*p = '\0';
		memcpy(n->ip, argv[1], strlen(argv[1]) + 1);
		n->port = atoi(p + 1);

		/* Parse flags */
		p = s = argv[2];
		while (p) {
			p = strchr(s, ',');
			if (p) *p = '\0';
			if (!strcasecmp(s, "myself")) {
				redisAssert(server.cluster->myself == NULL);
				myself = server.cluster->myself = n;
				n->flags |= REDIS_NODE_MYSELF;
			}
			else if (!strcasecmp(s, "master")) {
				n->flags |= REDIS_NODE_MASTER;
			}
			else if (!strcasecmp(s, "slave")) {
				n->flags |= REDIS_NODE_SLAVE;
			}
			else if (!strcasecmp(s, "fail?")) {
				n->flags |= REDIS_NODE_PFAIL;
			}
			else if (!strcasecmp(s, "fail")) {
				n->flags |= REDIS_NODE_FAIL;
				n->fail_time = mstime();
			}
			else if (!strcasecmp(s, "handshake")) {
				n->flags |= REDIS_NODE_HANDSHAKE;
			}
			else if (!strcasecmp(s, "noaddr")) {
				n->flags |= REDIS_NODE_NOADDR;
			}
			else if (!strcasecmp(s, "noflags")) {
				/* nothing to do */
			}
			else {
				redisPanic("Unknown flag in redis cluster config file");
			}
			if (p) s = p + 1;
		}

		/* Get master if any. Set the master and populate master's
		 * slave list. */
		if (argv[3][0] != '-') {
			master = clusterLookupNode(argv[3]);
			if (!master) {
				master = createClusterNode(argv[3], 0);
				clusterAddNode(master);
			}
			n->slaveof = master;
			clusterNodeAddSlave(master, n);
		}

		/* Set ping sent / pong received timestamps */
		if (atoi(argv[4])) n->ping_sent = mstime();
		if (atoi(argv[5])) n->pong_received = mstime();

		/* Set configEpoch for this node. */
		n->configEpoch = strtoull(argv[6], NULL, 10);

		/* Populate hash slots served by this instance. */
		for (j = 8; j < argc; j++) {
			int start, stop;

			if (argv[j][0] == '[') {
				/* Here we handle migrating / importing slots */
				int slot;
				char direction;
				clusterNode* cn;

				p = strchr(argv[j], '-');
				redisAssert(p != NULL);
				*p = '\0';
				direction = p[1]; /* Either '>' or '<' */
				slot = atoi(argv[j] + 1);
				p += 3;
				cn = clusterLookupNode(p);
				if (!cn) {
					cn = createClusterNode(p, 0);
					clusterAddNode(cn);
				}
				if (direction == '>') {
					server.cluster->migrating_slots_to[slot] = cn;
				}
				else {
					server.cluster->importing_slots_from[slot] = cn;
				}
				continue;
			}
			else if ((p = strchr(argv[j], '-')) != NULL) {
				*p = '\0';
				start = atoi(argv[j]);
				stop = atoi(p + 1);
			}
			else {
				start = stop = atoi(argv[j]);
			}
			while (start <= stop) clusterAddSlot(n, start++);
		}

		sdsfreesplitres(argv, argc);
	}
	/* Config sanity check */
	if (server.cluster->myself == NULL) goto fmterr;

	zfree(line);
	fclose(fp);

	redisLog(REDIS_NOTICE, "Node configuration loaded, I'm %.40s", myself->name);

	/* Something that should never happen: currentEpoch smaller than
	 * the max epoch found in the nodes configuration. However we handle this
	 * as some form of protection against manual editing of critical files. */
	if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
		server.cluster->currentEpoch = clusterGetMaxEpoch();
	}
	return REDIS_OK;

fmterr:
	redisLog(REDIS_WARNING,
		"Unrecoverable error: corrupted cluster config file.");
	zfree(line);
	if (fp) fclose(fp);
	exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
int clusterSaveConfig(int do_fsync) {
	sds ci;
	size_t content_size;
	struct IF_WIN32(_stat64, stat) sb;                                           // TODO: verify for 32-bit
	int fd;

	server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

	/* Get the nodes description and concatenate our "vars" directive to
	 * save currentEpoch and lastVoteEpoch. */
	ci = clusterGenNodesDescription(REDIS_NODE_HANDSHAKE);
	ci = sdscatprintf(ci, "vars currentEpoch %llu lastVoteEpoch %llu\n",
		(PORT_ULONGLONG)server.cluster->currentEpoch,
		(PORT_ULONGLONG)server.cluster->lastVoteEpoch);
	content_size = sdslen(ci);

	if ((fd = FDAPI_open(server.cluster_configfile, O_WRONLY | O_CREAT, 0644))
		== -1) goto err;

	/* Pad the new payload if the existing file length is greater. */
	if (fstat(fd, &sb) != -1) {
		if (sb.st_size > (off_t)content_size) {
			ci = sdsgrowzero(ci, sb.st_size);
			memset(ci + content_size, '\n', sb.st_size - content_size);
		}
	}
	if (FDAPI_write(fd, ci, sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
	if (do_fsync) {
		server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
		FDAPI_fsync(fd);
	}

	/* Truncate the file if needed to remove the final \n padding that
	 * is just garbage. */
	if (content_size != sdslen(ci) && FDAPI_ftruncate(fd, content_size) == -1) {
		/* ftruncate() failing is not a critical error. */
	}
	close(fd);
	sdsfree(ci);
	return 0;

err:
	if (fd != -1) close(fd);
	sdsfree(ci);
	return -1;
}

void clusterSaveConfigOrDie(int do_fsync) {
	if (clusterSaveConfig(do_fsync) == -1) {
		redisLog(REDIS_WARNING, "Fatal: can't update cluster config file.");
		exit(1);
	}
}

/* Lock the cluster config using flock(), and leaks the file descritor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success REDIS_OK is returned, otherwise an error is logged and
 * the function returns REDIS_ERR to signal a lock was not acquired. */
int clusterLockConfig(char* filename) {
	/* flock() does not exist on Solaris
	 * and a fcntl-based solution won't help, as we constantly re-open that file,
	 * which will release _all_ locks anyway
	 */
#if !defined(__sun)
	 /* To lock it, we need to open the file in a way it is created if
	  * it does not exist, otherwise there is a race condition with other
	  * processes. */
	int fd = FDAPI_open(filename, O_WRONLY | O_CREAT, 0644);
	if (fd == -1) {
		redisLog(REDIS_WARNING,
			"Can't open %s in order to acquire a lock: %s",
			filename, strerror(errno));
		return REDIS_ERR;
	}

#ifndef _WIN32
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK) {
#else
	HANDLE hFile = (HANDLE)FDAPI_get_osfhandle(fd);
	OVERLAPPED ovlp;
	DWORD size_lower, size_upper;
	// start offset is 0, and also zero the remaining members of the struct
	memset(&ovlp, 0, sizeof ovlp);
	// get file size
	size_lower = GetFileSize(hFile, &size_upper);
	if (!LockFileEx(hFile, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, size_lower, size_upper, &ovlp)) {
		DWORD err = GetLastError();
		if (err == ERROR_LOCK_VIOLATION) {
#endif
			redisLog(REDIS_WARNING,
				"Sorry, the cluster configuration file %s is already used "
				"by a different Redis Cluster node. Please make sure that "
				"different nodes use different cluster configuration "
				"files.", filename);

		}
		else {
			redisLog(REDIS_WARNING,
				"Impossible to lock %s: %d", filename, err);
		}

		close(fd);
		return REDIS_ERR;
	}
	/* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
	 * lock to the file as long as the process exists. */
#endif /* __sun */

	return REDIS_OK;
}

void clusterInit(void) {
	int saveconf = 0;

	server.cluster = zmalloc(sizeof(clusterState));
	server.cluster->myself = NULL;
	server.cluster->currentEpoch = 0;
	server.cluster->state = REDIS_CLUSTER_FAIL;
	server.cluster->size = 1;
	server.cluster->todo_before_sleep = 0;
	server.cluster->nodes = dictCreate(&clusterNodesDictType, NULL);
	server.cluster->nodes_black_list =
		dictCreate(&clusterNodesBlackListDictType, NULL);
	server.cluster->failover_auth_time = 0;
	server.cluster->failover_auth_count = 0;
	server.cluster->failover_auth_rank = 0;
	server.cluster->failover_auth_epoch = 0;
	server.cluster->cant_failover_reason = REDIS_CLUSTER_CANT_FAILOVER_NONE;
	server.cluster->lastVoteEpoch = 0;
	server.cluster->stats_bus_messages_sent = 0;
	server.cluster->stats_bus_messages_received = 0;
	memset(server.cluster->slots, 0, sizeof(server.cluster->slots));
	clusterCloseAllSlots();

#ifndef WIN32   // TODO: review this to verify if we can lock the file on Windows
	/* Lock the cluster config file to make sure every node uses
	 * its own nodes.conf. */
	if (clusterLockConfig(server.cluster_configfile) == REDIS_ERR)
		exit(1);
#endif

	/* Load or create a new nodes configuration. */
	if (clusterLoadConfig(server.cluster_configfile) == REDIS_ERR) {
		/* No configuration found. We will just use the random name provided
		 * by the createClusterNode() function. */
		myself = server.cluster->myself =
			createClusterNode(NULL, REDIS_NODE_MYSELF | REDIS_NODE_MASTER);
		redisLog(REDIS_NOTICE, "No cluster configuration found, I'm %.40s",
			myself->name);
		clusterAddNode(myself);
		saveconf = 1;
	}
	if (saveconf) clusterSaveConfigOrDie(1);

	/* We need a listening TCP port for our cluster messaging needs. */
	server.cfd_count = 0;

	/* Port sanity check II
	 * The other handshake port check is triggered too late to stop
	 * us from trying to use a too-high cluster port number. */
	if (server.port > (65535 - REDIS_CLUSTER_PORT_INCR)) {
		redisLog(REDIS_WARNING, "Redis port number too high. "
			"Cluster communication port is 10,000 port "
			"numbers higher than your Redis port. "
			"Your Redis port number must be "
			"lower than 55535.");
		exit(1);
	}

	if (listenToPort(server.port + REDIS_CLUSTER_PORT_INCR,
		server.cfd, &server.cfd_count) == REDIS_ERR)
	{
		exit(1);
	}
	else {
		int j;

		for (j = 0; j < server.cfd_count; j++) {
			if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
				clusterAcceptHandler, NULL) == AE_ERR)
				redisPanic("Unrecoverable error creating Redis Cluster "
					"file event.");
		}
	}

	/* The slots -> keys map is a sorted set. Init it. */
	server.cluster->slots_to_keys = zslCreate();

	/* Set myself->port to my listening port, we'll just need to discover
	 * the IP address via MEET messages. */
	myself->port = server.port;

	server.cluster->mf_end = 0;
	resetManualFailover();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forget.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 5) Only for hard reset: a new Node ID is generated.
 * 6) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 7) The new configuration is saved and the cluster state updated.
 * 8) If the node was a slave, the whole data set is flushed away. */
void clusterReset(int hard) {
	dictIterator* di;
	dictEntry* de;
	int j;

	/* Turn into master. */
	if (nodeIsSlave(myself)) {
		clusterSetNodeAsMaster(myself);
		replicationUnsetMaster();
		emptyDb(NULL);
	}

	/* Close slots, reset manual failover state. */
	clusterCloseAllSlots();
	resetManualFailover();

	/* Unassign all the slots. */
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) clusterDelSlot(j);

	/* Forget all the nodes, but myself. */
	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (node == myself) continue;
		clusterDelNode(node);
	}
	dictReleaseIterator(di);

	/* Hard reset only: set epochs to 0, change node ID. */
	if (hard) {
		sds oldname;

		server.cluster->currentEpoch = 0;
		server.cluster->lastVoteEpoch = 0;
		myself->configEpoch = 0;
		redisLog(REDIS_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

		/* To change the Node ID we need to remove the old name from the
		 * nodes table, change the ID, and re-add back with new name. */
		oldname = sdsnewlen(myself->name, REDIS_CLUSTER_NAMELEN);
		dictDelete(server.cluster->nodes, oldname);
		sdsfree(oldname);
		getRandomHexChars(myself->name, REDIS_CLUSTER_NAMELEN);
		clusterAddNode(myself);
	}

	/* Make sure to persist the new config and update the state. */
	clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
		CLUSTER_TODO_UPDATE_STATE |
		CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */

clusterLink* createClusterLink(clusterNode * node) {
	clusterLink* link = zmalloc(sizeof(*link));
	link->ctime = mstime();
	link->sndbuf = sdsempty();
	link->rcvbuf = sdsempty();
	link->node = node;
	link->fd = -1;
	return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
void freeClusterLink(clusterLink * link) {
	if (link->fd != -1) {
		aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
		aeDeleteFileEvent(server.el, link->fd, AE_READABLE);
	}
	sdsfree(link->sndbuf);
	sdsfree(link->rcvbuf);
	if (link->node)
		link->node->link = NULL;
	close(link->fd);
	zfree(link);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
void clusterAcceptHandler(aeEventLoop * el, int fd, void* privdata, int mask) {
	int cport, cfd;
	int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
	char cip[REDIS_IP_STR_LEN];
	clusterLink* link;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);
	REDIS_NOTUSED(privdata);

	/* If the server is starting up, don't accept cluster connections:
	 * UPDATE messages may interact with the database content. */
	if (server.masterhost == NULL && server.loading) {
		WIN32_ONLY(WSIOCP_QueueAccept(fd);)
			return;
	}

	while (max--) {
		cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
		if (cfd == ANET_ERR) {
			if (errno != EWOULDBLOCK)
				redisLog(REDIS_VERBOSE,
					"Error accepting cluster node: %s", server.neterr);
#ifdef _WIN32
			if (WSIOCP_QueueAccept(fd) == -1) {
				redisLog(REDIS_WARNING,
					"acceptTcpHandler: failed to queue another accept.");
			}
#endif
			return;
		}
		anetNonBlock(NULL, cfd);
		anetEnableTcpNoDelay(NULL, cfd);

		/* Use non-blocking I/O for cluster messages. */
		redisLog(REDIS_VERBOSE, "Accepted cluster node %s:%d", cip, cport);
		/* Create a link object we use to handle the connection.
		 * It gets passed to the readable handler when data is available.
		 * Initiallly the link->node pointer is set to NULL as we don't know
		 * which node is, but the right node is references once we know the
		 * node identity. */
		link = createClusterLink(NULL);
		link->fd = cfd;
		aeCreateFileEvent(server.el, cfd, AE_READABLE, clusterReadHandler, link);
	}
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

 /* We have 16384 hash slots. The hash slot of a given key is obtained
  * as the least significant 14 bits of the crc16 of the key.
  *
  * However if the key contains the {...} pattern, only the part between
  * { and } is hashed. This may be useful in the future to force certain
  * keys to be in the same node (assuming no resharding is in progress). */
unsigned int keyHashSlot(char* key, int keylen) {
	int s, e; /* start-end indexes of { and } */

	for (s = 0; s < keylen; s++)
		if (key[s] == '{') break;

	/* No '{' ? Hash the whole key. This is the base case. */
	if (s == keylen) return crc16(key, keylen) & 0x3FFF;

	/* '{' found? Check if we have the corresponding '}'. */
	for (e = s + 1; e < keylen; e++)
		if (key[e] == '}') break;

	/* No '}' or nothing betweeen {} ? Hash the whole key. */
	if (e == keylen || e == s + 1) return crc16(key, keylen) & 0x3FFF;

	/* If we are here there is both a { and a } on its right. Hash
	 * what is in the middle between { and }. */
	return crc16(key + s + 1, e - s - 1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

 /* Create a new cluster node, with the specified flags.
  * If "nodename" is NULL this is considered a first handshake and a random
  * node name is assigned to this node (it will be fixed later when we'll
  * receive the first pong).
  *
  * The node is created and returned to the user, but it is not automatically
  * added to the nodes hash table. */
clusterNode* createClusterNode(char* nodename, int flags) {
	clusterNode* node = zmalloc(sizeof(*node));

	if (nodename)
		memcpy(node->name, nodename, REDIS_CLUSTER_NAMELEN);
	else
		getRandomHexChars(node->name, REDIS_CLUSTER_NAMELEN);
	node->ctime = mstime();
	node->configEpoch = 0;
	node->flags = flags;
	memset(node->slots, 0, sizeof(node->slots));
	node->numslots = 0;
	node->numslaves = 0;
	node->slaves = NULL;
	node->slaveof = NULL;
	node->ping_sent = node->pong_received = 0;
	node->fail_time = 0;
	node->link = NULL;
	memset(node->ip, 0, sizeof(node->ip));
	node->port = 0;
	node->fail_reports = listCreate();
	node->voted_time = 0;
	node->repl_offset_time = 0;
	node->repl_offset = 0;
	listSetFreeMethod(node->fail_reports, zfree);
	return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
int clusterNodeAddFailureReport(clusterNode * failing, clusterNode * sender) {
	list* l = failing->fail_reports;
	listNode* ln;
	listIter li;
	clusterNodeFailReport* fr;

	/* If a failure report from the same sender already exists, just update
	 * the timestamp. */
	listRewind(l, &li);
	while ((ln = listNext(&li)) != NULL) {
		fr = ln->value;
		if (fr->node == sender) {
			fr->time = mstime();
			return 0;
		}
	}

	/* Otherwise create a new report. */
	fr = zmalloc(sizeof(*fr));
	fr->node = sender;
	fr->time = mstime();
	listAddNodeTail(l, fr);
	return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
void clusterNodeCleanupFailureReports(clusterNode * node) {
	list* l = node->fail_reports;
	listNode* ln;
	listIter li;
	clusterNodeFailReport* fr;
	mstime_t maxtime = server.cluster_node_timeout *
		REDIS_CLUSTER_FAIL_REPORT_VALIDITY_MULT;
	mstime_t now = mstime();

	listRewind(l, &li);
	while ((ln = listNext(&li)) != NULL) {
		fr = ln->value;
		if (now - fr->time > maxtime) listDelNode(l, ln);
	}
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
int clusterNodeDelFailureReport(clusterNode * node, clusterNode * sender) {
	list* l = node->fail_reports;
	listNode* ln;
	listIter li;
	clusterNodeFailReport* fr;

	/* Search for a failure report from this sender. */
	listRewind(l, &li);
	while ((ln = listNext(&li)) != NULL) {
		fr = ln->value;
		if (fr->node == sender) break;
	}
	if (!ln) return 0; /* No failure report from this sender. */

	/* Remove the failure report. */
	listDelNode(l, ln);
	clusterNodeCleanupFailureReports(node);
	return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
int clusterNodeFailureReportsCount(clusterNode * node) {
	clusterNodeCleanupFailureReports(node);
	return (int)listLength(node->fail_reports);                                WIN_PORT_FIX /* cast (int) */
}

int clusterNodeRemoveSlave(clusterNode * master, clusterNode * slave) {
	int j;

	for (j = 0; j < master->numslaves; j++) {
		if (master->slaves[j] == slave) {
			if ((j + 1) < master->numslaves) {
				int remaining_slaves = (master->numslaves - j) - 1;
				memmove(master->slaves + j, master->slaves + (j + 1),
					(sizeof(*master->slaves) * remaining_slaves));
			}
			master->numslaves--;
			return REDIS_OK;
		}
	}
	return REDIS_ERR;
}

int clusterNodeAddSlave(clusterNode * master, clusterNode * slave) {
	int j;

	/* If it's already a slave, don't add it again. */
	for (j = 0; j < master->numslaves; j++)
		if (master->slaves[j] == slave) return REDIS_ERR;
	master->slaves = zrealloc(master->slaves,
		sizeof(clusterNode*) * (master->numslaves + 1));
	master->slaves[master->numslaves] = slave;
	master->numslaves++;
	return REDIS_OK;
}

void clusterNodeResetSlaves(clusterNode * n) {
	zfree(n->slaves);
	n->numslaves = 0;
	n->slaves = NULL;
}

int clusterCountNonFailingSlaves(clusterNode * n) {
	int j, okslaves = 0;

	for (j = 0; j < n->numslaves; j++)
		if (!nodeFailed(n->slaves[j])) okslaves++;
	return okslaves;
}

/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
void freeClusterNode(clusterNode * n) {
	sds nodename;
	int j;

	/* If the node is a master with associated slaves, we have to set
	 * all the slaves->slaveof fields to NULL (unknown). */
	if (nodeIsMaster(n)) {
		for (j = 0; j < n->numslaves; j++)
			n->slaves[j]->slaveof = NULL;
	}

	/* Remove this node from the list of slaves of its master. */
	if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof, n);

	/* Unlink from the set of nodes. */
	nodename = sdsnewlen(n->name, REDIS_CLUSTER_NAMELEN);
	redisAssert(dictDelete(server.cluster->nodes, nodename) == DICT_OK);
	sdsfree(nodename);

	/* Release link and associated data structures. */
	if (n->link) freeClusterLink(n->link);
	listRelease(n->fail_reports);
	zfree(n->slaves);
	zfree(n);
}

/* Add a node to the nodes hash table */
int clusterAddNode(clusterNode * node) {
	int retval;

	retval = dictAdd(server.cluster->nodes,
		sdsnewlen(node->name, REDIS_CLUSTER_NAMELEN), node);
	return (retval == DICT_OK) ? REDIS_OK : REDIS_ERR;
}

/* Remove a node from the cluster. The functio performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 */
void clusterDelNode(clusterNode * delnode) {
	int j;
	dictIterator* di;
	dictEntry* de;

	/* 1) Mark slots as unassigned. */
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (server.cluster->importing_slots_from[j] == delnode)
			server.cluster->importing_slots_from[j] = NULL;
		if (server.cluster->migrating_slots_to[j] == delnode)
			server.cluster->migrating_slots_to[j] = NULL;
		if (server.cluster->slots[j] == delnode)
			clusterDelSlot(j);
	}

	/* 2) Remove failure reports. */
	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (node == delnode) continue;
		clusterNodeDelFailureReport(node, delnode);
	}
	dictReleaseIterator(di);

	/* 3) Free the node, unlinking it from the cluster. */
	freeClusterNode(delnode);
}

/* Node lookup by name */
clusterNode* clusterLookupNode(char* name) {
	sds s = sdsnewlen(name, REDIS_CLUSTER_NAMELEN);
	dictEntry* de;

	de = dictFind(server.cluster->nodes, s);
	sdsfree(s);
	if (de == NULL) return NULL;
	return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
void clusterRenameNode(clusterNode * node, char* newname) {
	int retval;
	sds s = sdsnewlen(node->name, REDIS_CLUSTER_NAMELEN);

	redisLog(REDIS_DEBUG, "Renaming node %.40s into %.40s",
		node->name, newname);
	retval = dictDelete(server.cluster->nodes, s);
	sdsfree(s);
	redisAssert(retval == DICT_OK);
	memcpy(node->name, newname, REDIS_CLUSTER_NAMELEN);
	clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling
 * -------------------------------------------------------------------------- */

 /* Return the greatest configEpoch found in the cluster, or the current
  * epoch if greater than any node configEpoch. */
uint64_t clusterGetMaxEpoch(void) {
	uint64_t max = 0;
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);
		if (node->configEpoch > max) max = node->configEpoch;
	}
	dictReleaseIterator(di);
	if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
	return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch increment the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigend, REDIS_OK is returned,
 * otherwise REDIS_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too exansive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster does not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However usign this function may violate the "last failover
 * wins" rule, so should only be used with care. */
int clusterBumpConfigEpochWithoutConsensus(void) {
	uint64_t maxEpoch = clusterGetMaxEpoch();

	if (myself->configEpoch == 0 ||
		myself->configEpoch != maxEpoch)
	{
		server.cluster->currentEpoch++;
		myself->configEpoch = server.cluster->currentEpoch;
		clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
			CLUSTER_TODO_FSYNC_CONFIG);
		redisLog(REDIS_WARNING,
			"New configEpoch set to %llu",
			(PORT_ULONGLONG)myself->configEpoch);
		return REDIS_OK;
	}
	else {
		return REDIS_ERR;
	}
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothign is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
void clusterHandleConfigEpochCollision(clusterNode * sender) {
	/* Prerequisites: nodes have the same configEpoch and are both masters. */
	if (sender->configEpoch != myself->configEpoch ||
		!nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
	/* Don't act if the colliding node has a smaller Node ID. */
	if (memcmp(sender->name, myself->name, REDIS_CLUSTER_NAMELEN) <= 0) return;
	/* Get the next ID available at the best of this node knowledge. */
	server.cluster->currentEpoch++;
	myself->configEpoch = server.cluster->currentEpoch;
	clusterSaveConfigOrDie(1);
	redisLog(REDIS_VERBOSE,
		"WARNING: configEpoch collision with node %.40s."
		" configEpoch set to %llu",
		sender->name,
		(PORT_ULONGLONG)myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in REDIS_CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the REDIS_CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * -------------------------------------------------------------------------- */

#define REDIS_CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


 /* Before of the addNode() or Exists() operations we always remove expired
  * entries from the black list. This is an O(N) operation but it is not a
  * problem since add / exists operations are called very infrequently and
  * the hash table is supposed to contain very little elements at max.
  * However without the cleanup during long uptimes and with some automated
  * node add/removal procedures, entries could accumulate. */
void clusterBlacklistCleanup(void) {
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes_black_list);
	while ((de = dictNext(di)) != NULL) {
		int64_t expire = dictGetUnsignedIntegerVal(de);

		if (expire < server.unixtime)
			dictDelete(server.cluster->nodes_black_list, dictGetKey(de));
	}
	dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
void clusterBlacklistAddNode(clusterNode * node) {
	dictEntry* de;
	sds id = sdsnewlen(node->name, REDIS_CLUSTER_NAMELEN);

	clusterBlacklistCleanup();
	if (dictAdd(server.cluster->nodes_black_list, id, NULL) == DICT_OK) {
		/* If the key was added, duplicate the sds string representation of
		 * the key for the next lookup. We'll free it at the end. */
		id = sdsdup(id);
	}
	de = dictFind(server.cluster->nodes_black_list, id);
	dictSetUnsignedIntegerVal(de, time(NULL) + REDIS_CLUSTER_BLACKLIST_TTL);
	sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
int clusterBlacklistExists(char* nodeid) {
	sds id = sdsnewlen(nodeid, REDIS_CLUSTER_NAMELEN);
	int retval;

	clusterBlacklistCleanup();
	retval = dictFind(server.cluster->nodes_black_list, id) != NULL;
	sdsfree(id);
	return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

 /* This function checks if a given node should be marked as FAIL.
  * It happens if the following conditions are met:
  *
  * 1) We received enough failure reports from other master nodes via gossip.
  *    Enough means that the majority of the masters signaled the node is
  *    down recently.
  * 2) We believe this node is in PFAIL state.
  *
  * If a failure is detected we also inform the whole cluster about this
  * event trying to force every other node to set the FAIL flag for the node.
  *
  * Note that the form of agreement used here is weak, as we collect the majority
  * of masters state during some time, and even if we force agreement by
  * propagating the FAIL message, because of partitions we may not reach every
  * node. However:
  *
  * 1) Either we reach the majority and eventually the FAIL state will propagate
  *    to all the cluster.
  * 2) Or there is no majority so no slave promotion will be authorized and the
  *    FAIL flag will be cleared after some time.
  */
void markNodeAsFailingIfNeeded(clusterNode * node) {
	int failures;
	int needed_quorum = (server.cluster->size / 2) + 1;

	if (!nodeTimedOut(node)) return; /* We can reach it. */
	if (nodeFailed(node)) return; /* Already FAILing. */

	failures = clusterNodeFailureReportsCount(node);
	/* Also count myself as a voter if I'm a master. */
	if (nodeIsMaster(myself)) failures++;
	if (failures < needed_quorum) return; /* No weak agreement from masters. */

	redisLog(REDIS_NOTICE,
		"Marking node %.40s as failing (quorum reached).", node->name);

	/* Mark the node as failing. */
	node->flags &= ~REDIS_NODE_PFAIL;
	node->flags |= REDIS_NODE_FAIL;
	node->fail_time = mstime();

	/* Broadcast the failing node name to everybody, forcing all the other
	 * reachable nodes to flag the node as FAIL. */
	if (nodeIsMaster(myself)) clusterSendFail(node->name);
	clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
void clearNodeFailureIfNeeded(clusterNode * node) {
	mstime_t now = mstime();

	redisAssert(nodeFailed(node));

	/* For slaves we always clear the FAIL flag if we can contact the
	 * node again. */
	if (nodeIsSlave(node) || node->numslots == 0) {
		redisLog(REDIS_NOTICE,
			"Clear FAIL state for node %.40s: %s is reachable again.",
			node->name,
			nodeIsSlave(node) ? "slave" : "master without slots");
		node->flags &= ~REDIS_NODE_FAIL;
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
	}

	/* If it is a master and...
	 * 1) The FAIL state is old enough.
	 * 2) It is yet serving slots from our point of view (not failed over).
	 * Apparently no one is going to fix these slots, clear the FAIL flag. */
	if (nodeIsMaster(node) && node->numslots > 0 &&
		(now - node->fail_time) >
		(server.cluster_node_timeout * REDIS_CLUSTER_FAIL_UNDO_TIME_MULT))
	{
		redisLog(REDIS_NOTICE,
			"Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
			node->name);
		node->flags &= ~REDIS_NODE_FAIL;
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
	}
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
int clusterHandshakeInProgress(char* ip, int port) {
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (!nodeInHandshake(node)) continue;
		if (!strcasecmp(node->ip, ip) && node->port == port) break;
	}
	dictReleaseIterator(di);
	return de != NULL;
}

/* Start an handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already an handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
int clusterStartHandshake(char* ip, int port) {
	clusterNode* n;
	char norm_ip[REDIS_IP_STR_LEN];
	struct sockaddr_storage sa;

	/* IP sanity check */
	if (FDAPI_inet_pton(AF_INET, ip,
		&(((struct sockaddr_in*)&sa)->sin_addr)))
	{
		sa.ss_family = AF_INET;
	}
	else if (FDAPI_inet_pton(AF_INET6, ip,
		&(((struct sockaddr_in6*)&sa)->sin6_addr)))
	{
		sa.ss_family = AF_INET6;
	}
	else {
		errno = EINVAL;
		return 0;
	}

	/* Port sanity check */
	if (port <= 0 || port > (65535 - REDIS_CLUSTER_PORT_INCR)) {
		errno = EINVAL;
		return 0;
	}

	/* Set norm_ip as the normalized string representation of the node
	 * IP address. */
	memset(norm_ip, 0, REDIS_IP_STR_LEN);
	if (sa.ss_family == AF_INET)
		FDAPI_inet_ntop(AF_INET,
			(void*)&(((struct sockaddr_in*)&sa)->sin_addr),
			norm_ip, REDIS_IP_STR_LEN);
	else
		FDAPI_inet_ntop(AF_INET6,
			(void*)&(((struct sockaddr_in6*)&sa)->sin6_addr),
			norm_ip, REDIS_IP_STR_LEN);

	if (clusterHandshakeInProgress(norm_ip, port)) {
		errno = EAGAIN;
		return 0;
	}

	/* Add the node with a random address (NULL as first argument to
	 * createClusterNode()). Everything will be fixed during the
	 * handshake. */
	n = createClusterNode(NULL, REDIS_NODE_HANDSHAKE | REDIS_NODE_MEET);
	memcpy(n->ip, norm_ip, sizeof(n->ip));
	n->port = port;
	clusterAddNode(n);
	return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
void clusterProcessGossipSection(clusterMsg * hdr, clusterLink * link) {
	uint16_t count = FDAPI_ntohs(hdr->count);
	clusterMsgDataGossip* g = (clusterMsgDataGossip*)hdr->data.ping.gossip;
	clusterNode* sender = link->node ? link->node : clusterLookupNode(hdr->sender);

	while (count--) {
		uint16_t flags = FDAPI_ntohs(g->flags);
		clusterNode* node;
		sds ci;

		ci = representRedisNodeFlags(sdsempty(), flags);
		redisLog(REDIS_DEBUG, "GOSSIP %.40s %s:%d %s",
			g->nodename,
			g->ip,
			FDAPI_ntohs(g->port),
			ci);
		sdsfree(ci);

		/* Update our state accordingly to the gossip sections */
		node = clusterLookupNode(g->nodename);
		if (node) {
			/* We already know this node.
			   Handle failure reports, only when the sender is a master. */
			if (sender && nodeIsMaster(sender) && node != myself) {
				if (flags & (REDIS_NODE_FAIL | REDIS_NODE_PFAIL)) {
					if (clusterNodeAddFailureReport(node, sender)) {
						redisLog(REDIS_VERBOSE,
							"Node %.40s reported node %.40s as not reachable.",
							sender->name, node->name);
					}
					markNodeAsFailingIfNeeded(node);
				}
				else {
					if (clusterNodeDelFailureReport(node, sender)) {
						redisLog(REDIS_VERBOSE,
							"Node %.40s reported node %.40s is back online.",
							sender->name, node->name);
					}
				}
			}

			/* If we already know this node, but it is not reachable, and
			 * we see a different address in the gossip section, start an
			 * handshake with the (possibly) new address: this will result
			 * into a node address update if the handshake will be
			 * successful. */
			if (node->flags & (REDIS_NODE_FAIL | REDIS_NODE_PFAIL) &&
				(strcasecmp(node->ip, g->ip) || node->port != FDAPI_ntohs(g->port)))
			{
				clusterStartHandshake(g->ip, FDAPI_ntohs(g->port));
			}
		}
		else {
			/* If it's not in NOADDR state and we don't have it, we
			 * start a handshake process against this IP/PORT pairs.
			 *
			 * Note that we require that the sender of this gossip message
			 * is a well known node in our cluster, otherwise we risk
			 * joining another cluster. */
			if (sender &&
				!(flags & REDIS_NODE_NOADDR) &&
				!clusterBlacklistExists(g->nodename))
			{
				clusterStartHandshake(g->ip, FDAPI_ntohs(g->port));
			}
		}

		/* Next node */
		g++;
	}
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes. */
void nodeIp2String(char* buf, clusterLink * link) {
	anetPeerToString(link->fd, buf, REDIS_IP_STR_LEN, NULL);
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, and at the specified port.
 * Also disconnect the node link so that we'll connect again to the new
 * address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
int nodeUpdateAddressIfNeeded(clusterNode * node, clusterLink * link, int port) {
	char ip[REDIS_IP_STR_LEN] = { 0 };

	/* We don't proceed if the link is the same as the sender link, as this
	 * function is designed to see if the node link is consistent with the
	 * symmetric link that is used to receive PINGs from the node.
	 *
	 * As a side effect this function never frees the passed 'link', so
	 * it is safe to call during packet processing. */
	if (link == node->link) return 0;

	nodeIp2String(ip, link);
	if (node->port == port && strcmp(ip, node->ip) == 0) return 0;

	/* IP / port is different, update it. */
	memcpy(node->ip, ip, sizeof(ip));
	node->port = port;
	if (node->link) freeClusterLink(node->link);
	node->flags &= ~REDIS_NODE_NOADDR;
	redisLog(REDIS_WARNING, "Address updated for node %.40s, now %s:%d",
		node->name, node->ip, node->port);

	/* Check if this is our master and we have to change the
	 * replication target as well. */
	if (nodeIsSlave(myself) && myself->slaveof == node)
		replicationSetMaster(node->ip, node->port);
	return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. */
void clusterSetNodeAsMaster(clusterNode * n) {
	if (nodeIsMaster(n)) return;

	if (n->slaveof) clusterNodeRemoveSlave(n->slaveof, n);
	n->flags &= ~REDIS_NODE_SLAVE;
	n->flags |= REDIS_NODE_MASTER;
	n->slaveof = NULL;

	/* Update config and state. */
	clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
		CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the case
 * we receive the info via an UPDATE packet. */
void clusterUpdateSlotsConfigWith(clusterNode * sender, uint64_t senderConfigEpoch, unsigned char* slots) {
	int j;
	clusterNode* curmaster, * newmaster = NULL;
	/* The dirty slots list is a list of slots for which we lose the ownership
	 * while having still keys inside. This usually happens after a failover
	 * or after a manual cluster reconfiguration operated by the admin.
	 *
	 * If the update message is not able to demote a master to slave (in this
	 * case we'll resync with the master updating the whole key space), we
	 * need to delete all the keys in the slots we lost ownership. */
	uint16_t dirty_slots[REDIS_CLUSTER_SLOTS];
	int dirty_slots_count = 0;

	/* Here we set curmaster to this node or the node this node
	 * replicates to if it's a slave. In the for loop we are
	 * interested to check if slots are taken away from curmaster. */
	curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;

	if (sender == myself) {
		redisLog(REDIS_WARNING, "Discarding UPDATE message about myself.");
		return;
	}

	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (bitmapTestBit(slots, j)) {
			/* The slot is already bound to the sender of this message. */
			if (server.cluster->slots[j] == sender) continue;

			/* The slot is in importing state, it should be modified only
			 * manually via redis-trib (example: a resharding is in progress
			 * and the migrating side slot was already closed and is advertising
			 * a new config. We still want the slot to be closed manually). */
			if (server.cluster->importing_slots_from[j]) continue;

			/* We rebind the slot to the new node claiming it if:
			 * 1) The slot was unassigned or the new node claims it with a
			 *    greater configEpoch.
			 * 2) We are not currently importing the slot. */
			if (server.cluster->slots[j] == NULL ||
				server.cluster->slots[j]->configEpoch < senderConfigEpoch)
			{
				/* Was this slot mine, and still contains keys? Mark it as
				 * a dirty slot. */
				if (server.cluster->slots[j] == myself &&
					countKeysInSlot(j) &&
					sender != myself)
				{
					dirty_slots[dirty_slots_count] = j;
					dirty_slots_count++;
				}

				if (server.cluster->slots[j] == curmaster)
					newmaster = sender;
				clusterDelSlot(j);
				clusterAddSlot(sender, j);
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
					CLUSTER_TODO_UPDATE_STATE |
					CLUSTER_TODO_FSYNC_CONFIG);
			}
		}
	}

	/* If at least one slot was reassigned from a node to another node
	 * with a greater configEpoch, it is possible that:
	 * 1) We are a master left without slots. This means that we were
	 *    failed over and we should turn into a replica of the new
	 *    master.
	 * 2) We are a slave and our master is left without slots. We need
	 *    to replicate to the new slots owner. */
	if (newmaster && curmaster->numslots == 0) {
		redisLog(REDIS_WARNING,
			"Configuration change detected. Reconfiguring myself "
			"as a replica of %.40s", sender->name);
		clusterSetMaster(sender);
		clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
			CLUSTER_TODO_UPDATE_STATE |
			CLUSTER_TODO_FSYNC_CONFIG);
	}
	else if (dirty_slots_count) {
		/* If we are here, we received an update message which removed
		 * ownership for certain slots we still have keys about, but still
		 * we are serving some slots, so this master node was not demoted to
		 * a slave.
		 *
		 * In order to maintain a consistent state between keys and slots
		 * we need to remove all the keys from the slots we lost. */
		for (j = 0; j < dirty_slots_count; j++)
			delKeysInSlot(dirty_slots[j]);
	}
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
int clusterProcessPacket(clusterLink * link) {
	clusterMsg* hdr = (clusterMsg*)link->rcvbuf;
	uint32_t totlen = FDAPI_ntohl(hdr->totlen);
	uint16_t type = FDAPI_ntohs(hdr->type);
	uint16_t flags = FDAPI_ntohs(hdr->flags);
	uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
	clusterNode* sender;

	server.cluster->stats_bus_messages_received++;
	redisLog(REDIS_DEBUG, "--- Processing packet of type %d, %Iu bytes", WIN_PORT_FIX /* %lu -> %Iu */
		type, (PORT_ULONG)totlen);

	/* Perform sanity checks */
	if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
	if (FDAPI_ntohs(hdr->ver) != CLUSTER_PROTO_VER)
		return 1; /* Can't handle versions other than the current one.*/
	if (totlen > sdslen(link->rcvbuf)) return 1;
	if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
		type == CLUSTERMSG_TYPE_MEET)
	{
		uint16_t count = FDAPI_ntohs(hdr->count);
		uint32_t explen; /* expected length of this packet */

		explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
		explen += (sizeof(clusterMsgDataGossip) * count);
		if (totlen != explen) return 1;
	}
	else if (type == CLUSTERMSG_TYPE_FAIL) {
		uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);

		explen += sizeof(clusterMsgDataFail);
		if (totlen != explen) return 1;
	}
	else if (type == CLUSTERMSG_TYPE_PUBLISH) {
		uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);

		explen += sizeof(clusterMsgDataPublish) -
			8 +
			FDAPI_ntohl(hdr->data.publish.msg.channel_len) +
			FDAPI_ntohl(hdr->data.publish.msg.message_len);
		if (totlen != explen) return 1;
	}
	else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
		type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
		type == CLUSTERMSG_TYPE_MFSTART)
	{
		uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);

		if (totlen != explen) return 1;
	}
	else if (type == CLUSTERMSG_TYPE_UPDATE) {
		uint32_t explen = sizeof(clusterMsg) - sizeof(union clusterMsgData);

		explen += sizeof(clusterMsgDataUpdate);
		if (totlen != explen) return 1;
	}

	/* Check if the sender is a known node. */
	sender = clusterLookupNode(hdr->sender);
	if (sender && !nodeInHandshake(sender)) {
		/* Update our curretEpoch if we see a newer epoch in the cluster. */
		senderCurrentEpoch = ntohu64(hdr->currentEpoch);
		senderConfigEpoch = ntohu64(hdr->configEpoch);
		if (senderCurrentEpoch > server.cluster->currentEpoch)
			server.cluster->currentEpoch = senderCurrentEpoch;
		/* Update the sender configEpoch if it is publishing a newer one. */
		if (senderConfigEpoch > sender->configEpoch) {
			sender->configEpoch = senderConfigEpoch;
			clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
				CLUSTER_TODO_FSYNC_CONFIG);
		}
		/* Update the replication offset info for this node. */
		sender->repl_offset = ntohu64(hdr->offset);
		sender->repl_offset_time = mstime();
		/* If we are a slave performing a manual failover and our master
		 * sent its offset while already paused, populate the MF state. */
		if (server.cluster->mf_end &&
			nodeIsSlave(myself) &&
			myself->slaveof == sender &&
			hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
			server.cluster->mf_master_offset == 0)
		{
			server.cluster->mf_master_offset = sender->repl_offset;
			redisLog(REDIS_WARNING,
				"Received replication offset for paused "
				"master manual failover: %lld",
				server.cluster->mf_master_offset);
		}
	}

	/* Initial processing of PING and MEET requests replying with a PONG. */
	if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
		redisLog(REDIS_DEBUG, "Ping packet received: %p", (void*)link->node);

		/* We use incoming MEET messages in order to set the address
		 * for 'myself', since only other cluster nodes will send us
		 * MEET messagses on handshakes, when the cluster joins, or
		 * later if we changed address, and those nodes will use our
		 * official address to connect to us. So by obtaining this address
		 * from the socket is a simple way to discover / update our own
		 * address in the cluster without it being hardcoded in the config.
		 *
		 * However if we don't have an address at all, we update the address
		 * even with a normal PING packet. If it's wrong it will be fixed
		 * by MEET later. */
		if (type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') {
			char ip[REDIS_IP_STR_LEN];

			if (anetSockName(link->fd, ip, sizeof(ip), NULL) != -1 &&
				strcmp(ip, myself->ip))
			{
				memcpy(myself->ip, ip, REDIS_IP_STR_LEN);
				redisLog(REDIS_WARNING, "IP address for this node updated to %s",
					myself->ip);
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
			}
		}

		/* Add this node if it is new for us and the msg type is MEET.
		 * In this stage we don't try to add the node with the right
		 * flags, slaveof pointer, and so forth, as this details will be
		 * resolved when we'll receive PONGs from the node. */
		if (!sender && type == CLUSTERMSG_TYPE_MEET) {
			clusterNode* node;

			node = createClusterNode(NULL, REDIS_NODE_HANDSHAKE);
			nodeIp2String(node->ip, link);
			node->port = FDAPI_ntohs(hdr->port);
			clusterAddNode(node);
			clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
		}

		/* If this is a MEET packet from an unknown node, we still process
		 * the gossip section here since we have to trust the sender because
		 * of the message type. */
		if (!sender && type == CLUSTERMSG_TYPE_MEET)
			clusterProcessGossipSection(hdr, link);

		/* Anyway reply with a PONG */
		clusterSendPing(link, CLUSTERMSG_TYPE_PONG);
	}

	/* PING, PONG, MEET: process config information. */
	if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
		type == CLUSTERMSG_TYPE_MEET)
	{
		redisLog(REDIS_DEBUG, "%s packet received: %p",
			type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
			(void*)link->node);
		if (link->node) {
			if (nodeInHandshake(link->node)) {
				/* If we already have this node, try to change the
				 * IP/port of the node with the new one. */
				if (sender) {
					redisLog(REDIS_VERBOSE,
						"Handshake: we already know node %.40s, "
						"updating the address if needed.", sender->name);
					if (nodeUpdateAddressIfNeeded(sender, link, FDAPI_ntohs(hdr->port)))
					{
						clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
							CLUSTER_TODO_UPDATE_STATE);
					}
					/* Free this node as we already have it. This will
					 * cause the link to be freed as well. */
					clusterDelNode(link->node);
					return 0;
				}

				/* First thing to do is replacing the random name with the
				 * right node name if this was a handshake stage. */
				clusterRenameNode(link->node, hdr->sender);
				redisLog(REDIS_DEBUG, "Handshake with node %.40s completed.",
					link->node->name);
				link->node->flags &= ~REDIS_NODE_HANDSHAKE;
				link->node->flags |= flags & (REDIS_NODE_MASTER | REDIS_NODE_SLAVE);
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
			}
			else if (memcmp(link->node->name, hdr->sender,
				REDIS_CLUSTER_NAMELEN) != 0)
			{
				/* If the reply has a non matching node ID we
				 * disconnect this node and set it as not having an associated
				 * address. */
				redisLog(REDIS_DEBUG, "PONG contains mismatching sender ID");
				link->node->flags |= REDIS_NODE_NOADDR;
				link->node->ip[0] = '\0';
				link->node->port = 0;
				freeClusterLink(link);
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
				return 0;
			}
		}

		/* Update the node address if it changed. */
		if (sender && type == CLUSTERMSG_TYPE_PING &&
			!nodeInHandshake(sender) &&
			nodeUpdateAddressIfNeeded(sender, link, FDAPI_ntohs(hdr->port)))
		{
			clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
				CLUSTER_TODO_UPDATE_STATE);
		}

		/* Update our info about the node */
		if (link->node && type == CLUSTERMSG_TYPE_PONG) {
			link->node->pong_received = mstime();
			link->node->ping_sent = 0;

			/* The PFAIL condition can be reversed without external
			 * help if it is momentary (that is, if it does not
			 * turn into a FAIL state).
			 *
			 * The FAIL condition is also reversible under specific
			 * conditions detected by clearNodeFailureIfNeeded(). */
			if (nodeTimedOut(link->node)) {
				link->node->flags &= ~REDIS_NODE_PFAIL;
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
					CLUSTER_TODO_UPDATE_STATE);
			}
			else if (nodeFailed(link->node)) {
				clearNodeFailureIfNeeded(link->node);
			}
		}

		/* Check for role switch: slave -> master or master -> slave. */
		if (sender) {
			if (!memcmp(hdr->slaveof, REDIS_NODE_NULL_NAME,
				sizeof(hdr->slaveof)))
			{
				/* Node is a master. */
				clusterSetNodeAsMaster(sender);
			}
			else {
				/* Node is a slave. */
				clusterNode* master = clusterLookupNode(hdr->slaveof);

				if (nodeIsMaster(sender)) {
					/* Master turned into a slave! Reconfigure the node. */
					clusterDelNodeSlots(sender);
					sender->flags &= ~REDIS_NODE_MASTER;
					sender->flags |= REDIS_NODE_SLAVE;

					/* Remove the list of slaves from the node. */
					if (sender->numslaves) clusterNodeResetSlaves(sender);

					/* Update config and state. */
					clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
						CLUSTER_TODO_UPDATE_STATE);
				}

				/* Master node changed for this slave? */
				if (master && sender->slaveof != master) {
					if (sender->slaveof)
						clusterNodeRemoveSlave(sender->slaveof, sender);
					clusterNodeAddSlave(master, sender);
					sender->slaveof = master;

					/* Update config. */
					clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
				}
			}
		}

		/* Update our info about served slots.
		 *
		 * Note: this MUST happen after we update the master/slave state
		 * so that REDIS_NODE_MASTER flag will be set. */

		 /* Many checks are only needed if the set of served slots this
		  * instance claims is different compared to the set of slots we have
		  * for it. Check this ASAP to avoid other computational expansive
		  * checks later. */
		clusterNode* sender_master = NULL; /* Sender or its master if slave. */
		int dirty_slots = 0; /* Sender claimed slots don't match my view? */

		if (sender) {
			sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
			if (sender_master) {
				dirty_slots = memcmp(sender_master->slots,
					hdr->myslots, sizeof(hdr->myslots)) != 0;
			}
		}

		/* 1) If the sender of the message is a master, and we detected that
		 *    the set of slots it claims changed, scan the slots to see if we
		 *    need to update our configuration. */
		if (sender && nodeIsMaster(sender) && dirty_slots)
			clusterUpdateSlotsConfigWith(sender, senderConfigEpoch, hdr->myslots);

		/* 2) We also check for the reverse condition, that is, the sender
		 *    claims to serve slots we know are served by a master with a
		 *    greater configEpoch. If this happens we inform the sender.
		 *
		 * This is useful because sometimes after a partition heals, a
		 * reappearing master may be the last one to claim a given set of
		 * hash slots, but with a configuration that other instances know to
		 * be deprecated. Example:
		 *
		 * A and B are master and slave for slots 1,2,3.
		 * A is partitioned away, B gets promoted.
		 * B is partitioned away, and A returns available.
		 *
		 * Usually B would PING A publishing its set of served slots and its
		 * configEpoch, but because of the partition B can't inform A of the
		 * new configuration, so other nodes that have an updated table must
		 * do it. In this way A will stop to act as a master (or can try to
		 * failover if there are the conditions to win the election). */
		if (sender && dirty_slots) {
			int j;

			for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
				if (bitmapTestBit(hdr->myslots, j)) {
					if (server.cluster->slots[j] == sender ||
						server.cluster->slots[j] == NULL) continue;
					if (server.cluster->slots[j]->configEpoch >
						senderConfigEpoch)
					{
						redisLog(REDIS_VERBOSE,
							"Node %.40s has old slots configuration, sending "
							"an UPDATE message about %.40s",
							sender->name, server.cluster->slots[j]->name);
						clusterSendUpdate(sender->link,
							server.cluster->slots[j]);

						/* TODO: instead of exiting the loop send every other
						 * UPDATE packet for other nodes that are the new owner
						 * of sender's slots. */
						break;
					}
				}
			}
		}

		/* If our config epoch collides with the sender's try to fix
		 * the problem. */
		if (sender &&
			nodeIsMaster(myself) && nodeIsMaster(sender) &&
			senderConfigEpoch == myself->configEpoch)
		{
			clusterHandleConfigEpochCollision(sender);
		}

		/* Get info from the gossip section */
		if (sender) clusterProcessGossipSection(hdr, link);
	}
	else if (type == CLUSTERMSG_TYPE_FAIL) {
		clusterNode* failing;

		if (sender) {
			failing = clusterLookupNode(hdr->data.fail.about.nodename);
			if (failing &&
				!(failing->flags & (REDIS_NODE_FAIL | REDIS_NODE_MYSELF)))
			{
				redisLog(REDIS_NOTICE,
					"FAIL message received from %.40s about %.40s",
					hdr->sender, hdr->data.fail.about.nodename);
				failing->flags |= REDIS_NODE_FAIL;
				failing->fail_time = mstime();
				failing->flags &= ~REDIS_NODE_PFAIL;
				clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
					CLUSTER_TODO_UPDATE_STATE);
			}
		}
		else {
			redisLog(REDIS_NOTICE,
				"Ignoring FAIL message from unknown node %.40s about %.40s",
				hdr->sender, hdr->data.fail.about.nodename);
		}
	}
	else if (type == CLUSTERMSG_TYPE_PUBLISH) {
		robj* channel, * message;
		uint32_t channel_len, message_len;

		/* Don't bother creating useless objects if there are no
		 * Pub/Sub subscribers. */
		if (dictSize(server.pubsub_channels) ||
			listLength(server.pubsub_patterns))
		{
			channel_len = FDAPI_ntohl(hdr->data.publish.msg.channel_len);
			message_len = FDAPI_ntohl(hdr->data.publish.msg.message_len);
			channel = createStringObject(
				(char*)hdr->data.publish.msg.bulk_data, channel_len);
			message = createStringObject(
				(char*)hdr->data.publish.msg.bulk_data + channel_len,
				message_len);
			pubsubPublishMessage(channel, message);
			decrRefCount(channel);
			decrRefCount(message);
		}
	}
	else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
		if (!sender) return 1;  /* We don't know that node. */
		clusterSendFailoverAuthIfNeeded(sender, hdr);
	}
	else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
		if (!sender) return 1;  /* We don't know that node. */
		/* We consider this vote only if the sender is a master serving
		 * a non zero number of slots, and its currentEpoch is greater or
		 * equal to epoch where this node started the election. */
		if (nodeIsMaster(sender) && sender->numslots > 0 &&
			senderCurrentEpoch >= server.cluster->failover_auth_epoch)
		{
			server.cluster->failover_auth_count++;
			/* Maybe we reached a quorum here, set a flag to make sure
			 * we check ASAP. */
			clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
		}
	}
	else if (type == CLUSTERMSG_TYPE_MFSTART) {
		/* This message is acceptable only if I'm a master and the sender
		 * is one of my slaves. */
		if (!sender || sender->slaveof != myself) return 1;
		/* Manual failover requested from slaves. Initialize the state
		 * accordingly. */
		resetManualFailover();
		server.cluster->mf_end = mstime() + REDIS_CLUSTER_MF_TIMEOUT;
		server.cluster->mf_slave = sender;
		pauseClients(mstime() + (REDIS_CLUSTER_MF_TIMEOUT * 2));
		redisLog(REDIS_WARNING, "Manual failover requested by slave %.40s.",
			sender->name);
	}
	else if (type == CLUSTERMSG_TYPE_UPDATE) {
		clusterNode* n; /* The node the update is about. */
		uint64_t reportedConfigEpoch =
			ntohu64(hdr->data.update.nodecfg.configEpoch);

		if (!sender) return 1;  /* We don't know the sender. */
		n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
		if (!n) return 1;   /* We don't know the reported node. */
		if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

		/* If in our current config the node is a slave, set it as a master. */
		if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

		/* Update the node's configEpoch. */
		n->configEpoch = reportedConfigEpoch;
		clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
			CLUSTER_TODO_FSYNC_CONFIG);

		/* Check the bitmap of served slots and update our
		 * config accordingly. */
		clusterUpdateSlotsConfigWith(n, reportedConfigEpoch,
			hdr->data.update.nodecfg.slots);
	}
	else {
		redisLog(REDIS_WARNING, "Received unknown packet type: %d", type);
	}
	return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
void handleLinkIOError(clusterLink * link) {
	freeClusterLink(link);
}

#ifdef _WIN32
void clusterWriteDone(aeEventLoop * el, int fd, void* privdata, int written) {
	WSIOCP_Request* req = (WSIOCP_Request*)privdata;
	clusterLink* link = (clusterLink*)req->client;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(fd);

	if (sdslen(link->sndbuf) == written) {
		sdsrange(link->sndbuf, written, -1);
		aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
		redisLog(REDIS_WARNING, "clusterWriteDone written %d fd %d", written, link->fd);
	}
}

void clusterWriteHandler(aeEventLoop * el, int fd, void* privdata, int mask) {
	clusterLink* link = (clusterLink*)privdata;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	int result = WSIOCP_SocketSend(fd,
		(char*)link->sndbuf,
		(int)(sdslen(link->sndbuf)),
		el,
		link,
		NULL,
		clusterWriteDone);
	if (errno == WSA_IO_PENDING)
		redisLog(REDIS_WARNING, "WSA_IO_PENDING writing to socket fd %d", link->fd);

	if (result == SOCKET_ERROR && errno != WSA_IO_PENDING) {
		redisLog(REDIS_WARNING, "Error writing to socket fd", link->fd);
		handleLinkIOError(link);
		return;
	}
}
#else
/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
void clusterWriteHandler(aeEventLoop * el, int fd, void* privdata, int mask) {
	clusterLink* link = (clusterLink*)privdata;
	ssize_t nwritten;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));
	if (nwritten <= 0) {
		redisLog(REDIS_DEBUG, "I/O error writing to node link: %s",
			strerror(errno));
		handleLinkIOError(link);
		return;
	}
	sdsrange(link->sndbuf, nwritten, -1);
	if (sdslen(link->sndbuf) == 0)
		aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}
#endif

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
void clusterReadHandler(aeEventLoop * el, int fd, void* privdata, int mask) {
	char buf[sizeof(clusterMsg)];
	ssize_t nread;
	clusterMsg* hdr;
	clusterLink* link = (clusterLink*)privdata;
	unsigned int readlen, rcvbuflen;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	while (1) { /* Read as long as there is data to read. */
		rcvbuflen = (unsigned int)sdslen(link->rcvbuf);                         WIN_PORT_FIX /* cast (unsigned int) */
			if (rcvbuflen < 8) {
				/* First, obtain the first 8 bytes to get the full message
				 * length. */
				readlen = 8 - rcvbuflen;
			}
			else {
				/* Finally read the full message. */
				hdr = (clusterMsg*)link->rcvbuf;
				if (rcvbuflen == 8) {
					/* Perform some sanity check on the message signature
					 * and length. */
					if (memcmp(hdr->sig, "RCmb", 4) != 0 ||
						FDAPI_ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
					{
						redisLog(REDIS_WARNING,
							"Bad message length or signature received "
							"from Cluster bus.");
						handleLinkIOError(link);
						return;
					}
				}
				readlen = FDAPI_ntohl(hdr->totlen) - rcvbuflen;
				if (readlen > sizeof(buf)) readlen = sizeof(buf);
			}

		nread = FDAPI_read(fd, buf, readlen);
		if (nread == -1 && errno == EAGAIN) { WIN32_ONLY(WSIOCP_QueueNextRead(fd);) return; } /* No more data ready. */

		if (nread <= 0) {
			/* I/O error... */
			redisLog(REDIS_DEBUG, "I/O error reading from node link: %s",
				(nread == 0) ? "connection closed" : strerror(errno));
			handleLinkIOError(link);
			return;
		}
		else {
			/* Read data and recast the pointer to the new buffer. */
			link->rcvbuf = sdscatlen(link->rcvbuf, buf, nread);
			hdr = (clusterMsg*)link->rcvbuf;
			rcvbuflen += (unsigned int)nread;                                   WIN_PORT_FIX /* cast (unsigned int) */
		}

		/* Total length obtained? Process this packet. */
		if (rcvbuflen >= 8 && rcvbuflen == FDAPI_ntohl(hdr->totlen)) {
			if (clusterProcessPacket(link)) {
				sdsfree(link->rcvbuf);
				link->rcvbuf = sdsempty();
			}
			else {
				return; /* Link no longer valid. */
			}
		}
	}
	WIN32_ONLY(WSIOCP_QueueNextRead(fd);)
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
void clusterSendMessage(clusterLink * link, unsigned char* msg, size_t msglen) {
	if (sdslen(link->sndbuf) == 0 && msglen != 0)
		aeCreateFileEvent(server.el, link->fd, AE_WRITABLE,
			clusterWriteHandler, link);

	link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);
	server.cluster->stats_bus_messages_sent++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
void clusterBroadcastMessage(void* buf, size_t len) {
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (!node->link) continue;
		if (node->flags & (REDIS_NODE_MYSELF | REDIS_NODE_HANDSHAKE))
			continue;
		clusterSendMessage(node->link, buf, len);
	}
	dictReleaseIterator(di);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
void clusterBuildMessageHdr(clusterMsg * hdr, int type) {
	int totlen = 0;
	uint64_t offset;
	clusterNode* master;

	/* If this node is a master, we send its slots bitmap and configEpoch.
	 * If this node is a slave we send the master's information instead (the
	 * node is flagged as slave so the receiver knows that it is NOT really
	 * in charge for this slots. */
	master = (nodeIsSlave(myself) && myself->slaveof) ?
		myself->slaveof : myself;

	memset(hdr, 0, sizeof(*hdr));
	hdr->ver = FDAPI_htons(CLUSTER_PROTO_VER);
	hdr->sig[0] = 'R';
	hdr->sig[1] = 'C';
	hdr->sig[2] = 'm';
	hdr->sig[3] = 'b';
	hdr->type = FDAPI_htons(type);
	memcpy(hdr->sender, myself->name, REDIS_CLUSTER_NAMELEN);

	memcpy(hdr->myslots, master->slots, sizeof(hdr->myslots));
	memset(hdr->slaveof, 0, REDIS_CLUSTER_NAMELEN);
	if (myself->slaveof != NULL)
		memcpy(hdr->slaveof, myself->slaveof->name, REDIS_CLUSTER_NAMELEN);
	hdr->port = FDAPI_htons(server.port);
	hdr->flags = FDAPI_htons(myself->flags);
	hdr->state = server.cluster->state;

	/* Set the currentEpoch and configEpochs. */
	hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
	hdr->configEpoch = htonu64(master->configEpoch);

	/* Set the replication offset. */
	if (nodeIsSlave(myself))
		offset = replicationGetSlaveOffset();
	else
		offset = server.master_repl_offset;
	hdr->offset = htonu64(offset);

	/* Set the message flags. */
	if (nodeIsMaster(myself) && server.cluster->mf_end)
		hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

	/* Compute the message length for certain messages. For other messages
	 * this is up to the caller. */
	if (type == CLUSTERMSG_TYPE_FAIL) {
		totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
		totlen += sizeof(clusterMsgDataFail);
	}
	else if (type == CLUSTERMSG_TYPE_UPDATE) {
		totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
		totlen += sizeof(clusterMsgDataUpdate);
	}
	hdr->totlen = FDAPI_htonl(totlen);
	/* For PING, PONG, and MEET, fixing the totlen field is up to the caller. */
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
void clusterSendPing(clusterLink * link, int type) {
	unsigned char* buf;
	clusterMsg* hdr;
	int gossipcount = 0; /* Number of gossip sections added so far. */
	int wanted; /* Number of gossip sections we want to append if possible. */
	int totlen; /* Total packet length. */
	/* freshnodes is the max number of nodes we can hope to append at all:
	 * nodes available minus two (ourself and the node we are sending the
	 * message to). However practically there may be less valid nodes since
	 * nodes in handshake state, disconnected, are not considered. */
	int freshnodes = (int)dictSize(server.cluster->nodes) - 2;                    WIN_PORT_FIX /* cast (int) */

	/* How many gossip sections we want to add? 1/10 of the number of nodes
	 * and anyway at least 3. Why 1/10?
	 *
	 * If we have N masters, with N/10 entries, and we consider that in
	 * node_timeout we exchange with each other node at least 4 packets
	 * (we ping in the worst case in node_timeout/2 time, and we also
	 * receive two pings from the host), we have a total of 8 packets
	 * in the node_timeout*2 falure reports validity time. So we have
	 * that, for a single PFAIL node, we can expect to receive the following
	 * number of failure reports (in the specified window of time):
	 *
	 * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
	 *
	 * PROB = probability of being featured in a single gossip entry,
	 *        which is 1 / NUM_OF_NODES.
	 * ENTRIES = 10.
	 * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
	 *
	 * If we assume we have just masters (so num of nodes and num of masters
	 * is the same), with 1/10 we always get over the majority, and specifically
	 * 80% of the number of nodes, to account for many masters failing at the
	 * same time.
	 *
	 * Since we have non-voting slaves that lower the probability of an entry
	 * to feature our node, we set the number of entires per packet as
	 * 10% of the total nodes we have. */
		wanted = floor(dictSize(server.cluster->nodes) / 10);
	if (wanted < 3) wanted = 3;
	if (wanted > freshnodes) wanted = freshnodes;

	/* Compute the maxium totlen to allocate our buffer. We'll fix the totlen
	 * later according to the number of gossip sections we really were able
	 * to put inside the packet. */
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	totlen += (sizeof(clusterMsgDataGossip) * wanted);
	/* Note: clusterBuildMessageHdr() expects the buffer to be always at least
	 * sizeof(clusterMsg) or more. */
	if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
	buf = zcalloc(totlen);
	hdr = (clusterMsg*)buf;

	/* Populate the header. */
	if (link->node && type == CLUSTERMSG_TYPE_PING)
		link->node->ping_sent = mstime();
	clusterBuildMessageHdr(hdr, type);

	/* Populate the gossip fields */
	int maxiterations = wanted * 3;
	while (freshnodes > 0 && gossipcount < wanted && maxiterations--) {
		dictEntry* de = dictGetRandomKey(server.cluster->nodes);
		clusterNode* this = dictGetVal(de);
		clusterMsgDataGossip* gossip;
		int j;

		/* Don't include this node: the whole packet header is about us
		 * already, so we just gossip about other nodes. */
		if (this == myself) continue;

		/* Give a bias to FAIL/PFAIL nodes. */
		if (maxiterations > wanted * 2 &&
			!(this->flags & (REDIS_NODE_PFAIL | REDIS_NODE_FAIL)))
			continue;

		/* In the gossip section don't include:
		 * 1) Nodes in HANDSHAKE state.
		 * 3) Nodes with the NOADDR flag set.
		 * 4) Disconnected nodes if they don't have configured slots.
		 */
		if (this->flags & (REDIS_NODE_HANDSHAKE | REDIS_NODE_NOADDR) ||
			(this->link == NULL && this->numslots == 0))
		{
			freshnodes--; /* Tecnically not correct, but saves CPU. */
			continue;
		}

		/* Check if we already added this node */
		for (j = 0; j < gossipcount; j++) {
			if (memcmp(hdr->data.ping.gossip[j].nodename, this->name,
				REDIS_CLUSTER_NAMELEN) == 0) break;
		}
		if (j != gossipcount) continue;

		/* Add it */
		freshnodes--;
		gossip = &(hdr->data.ping.gossip[gossipcount]);
		memcpy(gossip->nodename, this->name, REDIS_CLUSTER_NAMELEN);
		gossip->ping_sent = FDAPI_htonl((u_long)this->ping_sent);                     WIN_PORT_FIX /* cast (u_long) */
			gossip->pong_received = FDAPI_htonl((u_long)this->pong_received);             WIN_PORT_FIX /* cast (u_long) */
			memcpy(gossip->ip, this->ip, sizeof(this->ip));
		gossip->port = FDAPI_htons(this->port);
		gossip->flags = FDAPI_htons(this->flags);
		gossip->notused1 = 0;
		gossip->notused2 = 0;
		gossipcount++;
	}

	/* Ready to send... fix the totlen fiend and queue the message in the
	 * output buffer. */
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	totlen += (sizeof(clusterMsgDataGossip) * gossipcount);
	hdr->count = FDAPI_htons(gossipcount);
	hdr->totlen = FDAPI_htonl(totlen);
	clusterSendMessage(link, buf, totlen);
	zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (!node->link) continue;
		if (node == myself || nodeInHandshake(node)) continue;
		if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
			int local_slave =
				nodeIsSlave(node) && node->slaveof &&
				(node->slaveof == myself || node->slaveof == myself->slaveof);
			if (!local_slave) continue;
		}
		clusterSendPing(node->link, CLUSTERMSG_TYPE_PONG);
	}
	dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
void clusterSendPublish(clusterLink * link, robj * channel, robj * message) {
	unsigned char buf[sizeof(clusterMsg)], * payload;
	clusterMsg* hdr = (clusterMsg*)buf;
	uint32_t totlen;
	uint32_t channel_len, message_len;

	channel = getDecodedObject(channel);
	message = getDecodedObject(message);
	channel_len = (uint32_t)sdslen(channel->ptr);                               WIN_PORT_FIX /* cast (uint32_t) */
		message_len = (uint32_t)sdslen(message->ptr);                               WIN_PORT_FIX /* cast (uint32_t) */

		clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_PUBLISH);
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;

	hdr->data.publish.msg.channel_len = FDAPI_htonl(channel_len);
	hdr->data.publish.msg.message_len = FDAPI_htonl(message_len);
	hdr->totlen = FDAPI_htonl(totlen);

	/* Try to use the local buffer if possible */
	if (totlen < sizeof(buf)) {
		payload = buf;
	}
	else {
		payload = zmalloc(totlen);
		memcpy(payload, hdr, sizeof(*hdr));
		hdr = (clusterMsg*)payload;
	}
	memcpy(hdr->data.publish.msg.bulk_data, channel->ptr, sdslen(channel->ptr));
	memcpy(hdr->data.publish.msg.bulk_data + sdslen(channel->ptr),
		message->ptr, sdslen(message->ptr));

	if (link)
		clusterSendMessage(link, payload, totlen);
	else
		clusterBroadcastMessage(payload, totlen);

	decrRefCount(channel);
	decrRefCount(message);
	if (payload != buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (REDIS_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to REDIS_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
void clusterSendFail(char* nodename) {
	unsigned char buf[sizeof(clusterMsg)];
	clusterMsg* hdr = (clusterMsg*)buf;

	clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAIL);
	memcpy(hdr->data.fail.about.nodename, nodename, REDIS_CLUSTER_NAMELEN);
	clusterBroadcastMessage(buf, FDAPI_ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
void clusterSendUpdate(clusterLink * link, clusterNode * node) {
	unsigned char buf[sizeof(clusterMsg)];
	clusterMsg* hdr = (clusterMsg*)buf;

	if (link == NULL) return;
	clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_UPDATE);
	memcpy(hdr->data.update.nodecfg.nodename, node->name, REDIS_CLUSTER_NAMELEN);
	hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
	memcpy(hdr->data.update.nodecfg.slots, node->slots, sizeof(node->slots));
	clusterSendMessage(link, buf, FDAPI_ntohl(hdr->totlen));
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * -------------------------------------------------------------------------- */
void clusterPropagatePublish(robj * channel, robj * message) {
	clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

 /* This function sends a FAILOVE_AUTH_REQUEST message to every node in order to
  * see if there is the quorum for this slave instance to failover its failing
  * master.
  *
  * Note that we send the failover request to everybody, master and slave nodes,
  * but only the masters are supposed to reply to our query. */
void clusterRequestFailoverAuth(void) {
	unsigned char buf[sizeof(clusterMsg)];
	clusterMsg* hdr = (clusterMsg*)buf;
	uint32_t totlen;

	clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
	/* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
	 * in the header to communicate the nodes receiving the message that
	 * they should authorized the failover even if the master is working. */
	if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	hdr->totlen = FDAPI_htonl(totlen);
	clusterBroadcastMessage(buf, totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
void clusterSendFailoverAuth(clusterNode * node) {
	unsigned char buf[sizeof(clusterMsg)];
	clusterMsg* hdr = (clusterMsg*)buf;
	uint32_t totlen;

	if (!node->link) return;
	clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	hdr->totlen = FDAPI_htonl(totlen);
	clusterSendMessage(node->link, buf, totlen);
}

/* Send a MFSTART message to the specified node. */
void clusterSendMFStart(clusterNode * node) {
	unsigned char buf[sizeof(clusterMsg)];
	clusterMsg* hdr = (clusterMsg*)buf;
	uint32_t totlen;

	if (!node->link) return;
	clusterBuildMessageHdr(hdr, CLUSTERMSG_TYPE_MFSTART);
	totlen = sizeof(clusterMsg) - sizeof(union clusterMsgData);
	hdr->totlen = FDAPI_htonl(totlen);
	clusterSendMessage(node->link, buf, totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
void clusterSendFailoverAuthIfNeeded(clusterNode * node, clusterMsg * request) {
	clusterNode* master = node->slaveof;
	uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
	uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
	unsigned char* claimed_slots = request->myslots;
	int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
	int j;

	/* IF we are not a master serving at least 1 slot, we don't have the
	 * right to vote, as the cluster size in Redis Cluster is the number
	 * of masters serving at least one slot, and quorum is the cluster
	 * size + 1 */
	if (nodeIsSlave(myself) || myself->numslots == 0) return;

	/* Request epoch must be >= our currentEpoch.
	 * Note that it is impossible for it to actually be greater since
	 * our currentEpoch was updated as a side effect of receiving this
	 * request, if the request epoch was greater. */
	if (requestCurrentEpoch < server.cluster->currentEpoch) {
		redisLog(REDIS_WARNING,
			"Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
			node->name,
			(PORT_ULONGLONG)requestCurrentEpoch,
			(PORT_ULONGLONG)server.cluster->currentEpoch);
		return;
	}

	/* I already voted for this epoch? Return ASAP. */
	if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
		redisLog(REDIS_WARNING,
			"Failover auth denied to %.40s: already voted for epoch %llu",
			node->name,
			(PORT_ULONGLONG)server.cluster->currentEpoch);
		return;
	}

	/* Node must be a slave and its master down.
	 * The master can be non failing if the request is flagged
	 * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
	if (nodeIsMaster(node) || master == NULL ||
		(!nodeFailed(master) && !force_ack))
	{
		if (nodeIsMaster(node)) {
			redisLog(REDIS_WARNING,
				"Failover auth denied to %.40s: it is a master node",
				node->name);
		}
		else if (master == NULL) {
			redisLog(REDIS_WARNING,
				"Failover auth denied to %.40s: I don't know its master",
				node->name);
		}
		else if (!nodeFailed(master)) {
			redisLog(REDIS_WARNING,
				"Failover auth denied to %.40s: its master is up",
				node->name);
		}
		return;
	}

	/* We did not voted for a slave about this master for two
	 * times the node timeout. This is not strictly needed for correctness
	 * of the algorithm but makes the base case more linear. */
	if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
	{
		redisLog(REDIS_WARNING,
			"Failover auth denied to %.40s: "
			"can't vote about this master before %lld milliseconds",
			node->name,
			(PORT_LONGLONG)((server.cluster_node_timeout * 2) -
				(mstime() - node->slaveof->voted_time)));
		return;
	}

	/* The slave requesting the vote must have a configEpoch for the claimed
	 * slots that is >= the one of the masters currently serving the same
	 * slots in the current configuration. */
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (bitmapTestBit(claimed_slots, j) == 0) continue;
		if (server.cluster->slots[j] == NULL ||
			server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
		{
			continue;
		}
		/* If we reached this point we found a slot that in our current slots
		 * is served by a master with a greater configEpoch than the one claimed
		 * by the slave requesting our vote. Refuse to vote for this slave. */
		redisLog(REDIS_WARNING,
			"Failover auth denied to %.40s: "
			"slot %d epoch (%llu) > reqEpoch (%llu)",
			node->name, j,
			(PORT_ULONGLONG)server.cluster->slots[j]->configEpoch,
			(PORT_ULONGLONG)requestConfigEpoch);
		return;
	}

	/* We can vote for this slave. */
	clusterSendFailoverAuth(node);
	server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
	node->slaveof->voted_time = mstime();
	redisLog(REDIS_WARNING, "Failover auth granted to %.40s for epoch %llu",
		node->name, (PORT_ULONGLONG)server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
int clusterGetSlaveRank(void) {
	PORT_LONGLONG myoffset;
	int j, rank = 0;
	clusterNode* master;

	redisAssert(nodeIsSlave(myself));
	master = myself->slaveof;
	if (master == NULL) return 0; /* Never called by slaves without master. */

	myoffset = replicationGetSlaveOffset();
	for (j = 0; j < master->numslaves; j++)
		if (master->slaves[j] != myself &&
			master->slaves[j]->repl_offset > myoffset) rank++;
	return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    REDIS_CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros REDIS_CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. */
void clusterLogCantFailover(int reason) {
	char* msg;
	static time_t lastlog_time = 0;
	mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

	/* Don't log if we have the same reason for some time. */
	if (reason == server.cluster->cant_failover_reason &&
		time(NULL) - lastlog_time < REDIS_CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
		return;

	server.cluster->cant_failover_reason = reason;

	/* We also don't emit any log if the master failed no long ago, the
	 * goal of this function is to log slaves in a stalled condition for
	 * a long time. */
	if (myself->slaveof &&
		nodeFailed(myself->slaveof) &&
		(mstime() - myself->slaveof->fail_time) < nolog_fail_time) return;

	switch (reason) {
	case REDIS_CLUSTER_CANT_FAILOVER_DATA_AGE:
		msg = "Disconnected from master for longer than allowed.";
		break;
	case REDIS_CLUSTER_CANT_FAILOVER_WAITING_DELAY:
		msg = "Waiting the delay before I can start a new failover.";
		break;
	case REDIS_CLUSTER_CANT_FAILOVER_EXPIRED:
		msg = "Failover attempt expired.";
		break;
	case REDIS_CLUSTER_CANT_FAILOVER_WAITING_VOTES:
		msg = "Waiting for votes, but majority still not reached.";
		break;
	default:
		msg = "Unknown reason code.";
		break;
	}
	lastlog_time = time(NULL);
	redisLog(REDIS_WARNING, "Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
void clusterFailoverReplaceYourMaster(void) {
	int j;
	clusterNode* oldmaster = myself->slaveof;

	if (nodeIsMaster(myself) || oldmaster == NULL) return;

	/* 1) Turn this node into a master. */
	clusterSetNodeAsMaster(myself);
	replicationUnsetMaster();

	/* 2) Claim all the slots assigned to our master. */
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (clusterNodeGetSlotBit(oldmaster, j)) {
			clusterDelSlot(j);
			clusterAddSlot(myself, j);
		}
	}

	/* 3) Update state and save config. */
	clusterUpdateState();
	clusterSaveConfigOrDie(1);

	/* 4) Pong all the other nodes so that they can update the state
	 *    accordingly and detect that we switched to master role. */
	clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

	/* 5) If there was a manual failover in progress, clear the state. */
	resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The gaol of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 */
void clusterHandleSlaveFailover(void) {
	mstime_t data_age;
	mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
	int needed_quorum = (server.cluster->size / 2) + 1;
	int manual_failover = server.cluster->mf_end != 0 &&
		server.cluster->mf_can_start;
	mstime_t auth_timeout, auth_retry_time;

	server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

	/* Compute the failover timeout (the max time we have to send votes
	 * and wait for replies), and the failover retry time (the time to wait
	 * before trying to get voted again).
	 *
	 * Timeout is MIN(NODE_TIMEOUT*2,2000) milliseconds.
	 * Retry is two times the Timeout.
	 */
	auth_timeout = server.cluster_node_timeout * 2;
	if (auth_timeout < 2000) auth_timeout = 2000;
	auth_retry_time = auth_timeout * 2;

	/* Pre conditions to run the function, that must be met both in case
	 * of an automatic or manual failover:
	 * 1) We are a slave.
	 * 2) Our master is flagged as FAIL, or this is a manual failover.
	 * 3) It is serving slots. */
	if (nodeIsMaster(myself) ||
		myself->slaveof == NULL ||
		(!nodeFailed(myself->slaveof) && !manual_failover) ||
		myself->slaveof->numslots == 0)
	{
		/* There are no reasons to failover, so we set the reason why we
		 * are returning without failing over to NONE. */
		server.cluster->cant_failover_reason = REDIS_CLUSTER_CANT_FAILOVER_NONE;
		return;
	}

	/* Set data_age to the number of seconds we are disconnected from
	 * the master. */
	if (server.repl_state == REDIS_REPL_CONNECTED) {
		data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
			* 1000;
	}
	else {
		data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
	}

	/* Remove the node timeout from the data age as it is fine that we are
	 * disconnected from our master at least for the time it was down to be
	 * flagged as FAIL, that's the baseline. */
	if (data_age > server.cluster_node_timeout)
		data_age -= server.cluster_node_timeout;

	/* Check if our data is recent enough according to the slave validity
	 * factor configured by the user.
	 *
	 * Check bypassed for manual failovers. */
	if (server.cluster_slave_validity_factor &&
		data_age >
		(((mstime_t)server.repl_ping_slave_period * 1000) +
			(server.cluster_node_timeout * server.cluster_slave_validity_factor)))
	{
		if (!manual_failover) {
			clusterLogCantFailover(REDIS_CLUSTER_CANT_FAILOVER_DATA_AGE);
			return;
		}
	}

	/* If the previous failover attempt timedout and the retry time has
	 * elapsed, we can setup a new one. */
	if (auth_age > auth_retry_time) {
		server.cluster->failover_auth_time = mstime() +
			500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
			random() % 500; /* Random delay between 0 and 500 milliseconds. */
		server.cluster->failover_auth_count = 0;
		server.cluster->failover_auth_sent = 0;
		server.cluster->failover_auth_rank = clusterGetSlaveRank();
		/* We add another delay that is proportional to the slave rank.
		 * Specifically 1 second * rank. This way slaves that have a probably
		 * less updated replication offset, are penalized. */
		server.cluster->failover_auth_time +=
			server.cluster->failover_auth_rank * 1000;
		/* However if this is a manual failover, no delay is needed. */
		if (server.cluster->mf_end) {
			server.cluster->failover_auth_time = mstime();
			server.cluster->failover_auth_rank = 0;
		}
		redisLog(REDIS_WARNING,
			"Start of election delayed for %lld milliseconds "
			"(rank #%d, offset %lld).",
			server.cluster->failover_auth_time - mstime(),
			server.cluster->failover_auth_rank,
			replicationGetSlaveOffset());
		/* Now that we have a scheduled election, broadcast our offset
		 * to all the other slaves so that they'll updated their offsets
		 * if our offset is better. */
		clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
		return;
	}

	/* It is possible that we received more updated offsets from other
	 * slaves for the same master since we computed our election delay.
	 * Update the delay if our rank changed.
	 *
	 * Not performed if this is a manual failover. */
	if (server.cluster->failover_auth_sent == 0 &&
		server.cluster->mf_end == 0)
	{
		int newrank = clusterGetSlaveRank();
		if (newrank > server.cluster->failover_auth_rank) {
			PORT_LONGLONG added_delay =
				(newrank - server.cluster->failover_auth_rank) * 1000;
			server.cluster->failover_auth_time += added_delay;
			server.cluster->failover_auth_rank = newrank;
			redisLog(REDIS_WARNING,
				"Slave rank updated to #%d, added %lld milliseconds of delay.",
				newrank, added_delay);
		}
	}

	/* Return ASAP if we can't still start the election. */
	if (mstime() < server.cluster->failover_auth_time) {
		clusterLogCantFailover(REDIS_CLUSTER_CANT_FAILOVER_WAITING_DELAY);
		return;
	}

	/* Return ASAP if the election is too old to be valid. */
	if (auth_age > auth_timeout) {
		clusterLogCantFailover(REDIS_CLUSTER_CANT_FAILOVER_EXPIRED);
		return;
	}

	/* Ask for votes if needed. */
	if (server.cluster->failover_auth_sent == 0) {
		server.cluster->currentEpoch++;
		server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
		redisLog(REDIS_WARNING, "Starting a failover election for epoch %llu.",
			(PORT_ULONGLONG)server.cluster->currentEpoch);
		clusterRequestFailoverAuth();
		server.cluster->failover_auth_sent = 1;
		clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG |
			CLUSTER_TODO_UPDATE_STATE |
			CLUSTER_TODO_FSYNC_CONFIG);
		return; /* Wait for replies. */
	}

	/* Check if we reached the quorum. */
	if (server.cluster->failover_auth_count >= needed_quorum) {
		/* We have the quorum, we can finally failover the master. */

		redisLog(REDIS_WARNING,
			"Failover election won: I'm the new master.");

		/* Update my configEpoch to the epoch of the election. */
		if (myself->configEpoch < server.cluster->failover_auth_epoch) {
			myself->configEpoch = server.cluster->failover_auth_epoch;
			redisLog(REDIS_WARNING,
				"configEpoch set to %llu after successful failover",
				(PORT_ULONGLONG)myself->configEpoch);
		}

		/* Take responsability for the cluster slots. */
		clusterFailoverReplaceYourMaster();
	}
	else {
		clusterLogCantFailover(REDIS_CLUSTER_CANT_FAILOVER_WAITING_VOTES);
	}
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orpaned, that is, left with no working slaves.
 * -------------------------------------------------------------------------- */

 /* This function is responsible to decide if this replica should be migrated
  * to a different (orphaned) master. It is called by the clusterCron() function
  * only if:
  *
  * 1) We are a slave node.
  * 2) It was detected that there is at least one orphaned master in
  *    the cluster.
  * 3) We are a slave of one of the masters with the greatest number of
  *    slaves.
  *
  * This checks are performed by the caller since it requires to iterate
  * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
  * if definitely needed.
  *
  * The fuction is called with a pre-computed max_slaves, that is the max
  * number of working (not in FAIL state) slaves for a single master.
  *
  * Additional conditions for migration are examined inside the function.
  */
void clusterHandleSlaveMigration(int max_slaves) {
	int j, okslaves = 0;
	clusterNode* mymaster = myself->slaveof, * target = NULL, * candidate = NULL;
	dictIterator* di;
	dictEntry* de;

	/* Step 1: Don't migrate if the cluster state is not ok. */
	if (server.cluster->state != REDIS_CLUSTER_OK) return;

	/* Step 2: Don't migrate if my master will not be left with at least
	 *         'migration-barrier' slaves after my migration. */
	if (mymaster == NULL) return;
	for (j = 0; j < mymaster->numslaves; j++)
		if (!nodeFailed(mymaster->slaves[j]) &&
			!nodeTimedOut(mymaster->slaves[j])) okslaves++;
	if (okslaves <= server.cluster_migration_barrier) return;

	/* Step 3: Idenitfy a candidate for migration, and check if among the
	 * masters with the greatest number of ok slaves, I'm the one with the
	 * smaller node ID.
	 *
	 * Note that this means that eventually a replica migration will occurr
	 * since slaves that are reachable again always have their FAIL flag
	 * cleared. At the same time this does not mean that there are no
	 * race conditions possible (two slaves migrating at the same time), but
	 * this is extremely unlikely to happen, and harmless. */
	candidate = myself;
	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);
		int okslaves;

		/* Only iterate over working masters. */
		if (nodeIsSlave(node) || nodeFailed(node)) continue;
		/* If this master never had slaves so far, don't migrate. We want
		 * to migrate to a master that remained orphaned, not masters that
		 * were never configured to have slaves. */
		if (node->numslaves == 0) continue;
		okslaves = clusterCountNonFailingSlaves(node);

		if (okslaves == 0 && target == NULL && node->numslots > 0)
			target = node;

		if (okslaves == max_slaves) {
			for (j = 0; j < node->numslaves; j++) {
				if (memcmp(node->slaves[j]->name,
					candidate->name,
					REDIS_CLUSTER_NAMELEN) < 0)
				{
					candidate = node->slaves[j];
				}
			}
		}
	}
	dictReleaseIterator(di);

	/* Step 4: perform the migration if there is a target, and if I'm the
	 * candidate. */
	if (target && candidate == myself) {
		redisLog(REDIS_WARNING, "Migrating to orphaned master %.40s",
			target->name);
		clusterSetMaster(target);
	}
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout REDIS_CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- */

 /* Reset the manual failover state. This works for both masters and slavesa
  * as all the state about manual failover is cleared.
  *
  * The function can be used both to initialize the manual failover state at
  * startup or to abort a manual failover in progress. */
void resetManualFailover(void) {
	if (server.cluster->mf_end && clientsArePaused()) {
		server.clients_pause_end_time = 0;
		clientsArePaused(); /* Just use the side effect of the function. */
	}
	server.cluster->mf_end = 0; /* No manual failover in progress. */
	server.cluster->mf_can_start = 0;
	server.cluster->mf_slave = NULL;
	server.cluster->mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. */
void manualFailoverCheckTimeout(void) {
	if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
		redisLog(REDIS_WARNING, "Manual failover timed out.");
		resetManualFailover();
	}
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
void clusterHandleManualFailover(void) {
	/* Return ASAP if no manual failover is in progress. */
	if (server.cluster->mf_end == 0) return;

	/* If mf_can_start is non-zero, the failover was already triggered so the
	 * next steps are performed by clusterHandleSlaveFailover(). */
	if (server.cluster->mf_can_start) return;

	if (server.cluster->mf_master_offset == 0) return; /* Wait for offset... */

	if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
		/* Our replication offset matches the master replication offset
		 * announced after clients were paused. We can start the failover. */
		server.cluster->mf_can_start = 1;
		redisLog(REDIS_WARNING,
			"All master replication stream processed, "
			"manual failover can start.");
	}
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

 /* This is executed 10 times every second */
void clusterCron(void) {
	dictIterator* di;
	dictEntry* de;
	int update_state = 0;
	int orphaned_masters; /* How many masters there are without ok slaves. */
	int max_slaves; /* Max number of ok slaves for a single master. */
	int this_slaves; /* Number of ok slaves for our master (if we are slave). */
	mstime_t min_pong = 0, now = mstime();
	clusterNode* min_pong_node = NULL;
	static PORT_ULONGLONG iteration = 0;
	mstime_t handshake_timeout;

	iteration++; /* Number of times this function was called so far. */

	/* The handshake timeout is the time after which a handshake node that was
	 * not turned into a normal node is removed from the nodes. Usually it is
	 * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
	 * the value of 1 second. */
	handshake_timeout = server.cluster_node_timeout;
	if (handshake_timeout < 1000) handshake_timeout = 1000;

	/* Check if we have disconnected nodes and re-establish the connection. */
	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (node->flags & (REDIS_NODE_MYSELF | REDIS_NODE_NOADDR)) continue;

		/* A Node in HANDSHAKE state has a limited lifespan equal to the
		 * configured node timeout. */
		if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
			clusterDelNode(node);
			continue;
		}

		if (node->link == NULL) {
			int fd;
			mstime_t old_ping_sent;
			clusterLink* link;

			fd = anetTcpNonBlockBindConnect(server.neterr, node->ip,
				node->port + REDIS_CLUSTER_PORT_INCR, REDIS_BIND_ADDR);
			if (fd == -1) {
				/* We got a synchronous error from connect before
				 * clusterSendPing() had a chance to be called.
				 * If node->ping_sent is zero, failure detection can't work,
				 * so we claim we actually sent a ping now (that will
				 * be really sent as soon as the link is obtained). */
				if (node->ping_sent == 0) node->ping_sent = mstime();
				redisLog(REDIS_DEBUG, "Unable to connect to "
					"Cluster Node [%s]:%d -> %s", node->ip,
					node->port + REDIS_CLUSTER_PORT_INCR,
					server.neterr);
				continue;
			}
			link = createClusterLink(node);
			link->fd = fd;
			node->link = link;
			aeCreateFileEvent(server.el, link->fd, AE_READABLE,
				clusterReadHandler, link);
			/* Queue a PING in the new connection ASAP: this is crucial
			 * to avoid false positives in failure detection.
			 *
			 * If the node is flagged as MEET, we send a MEET message instead
			 * of a PING one, to force the receiver to add us in its node
			 * table. */
			old_ping_sent = node->ping_sent;
			clusterSendPing(link, node->flags & REDIS_NODE_MEET ?
				CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
			if (old_ping_sent) {
				/* If there was an active ping before the link was
				 * disconnected, we want to restore the ping time, otherwise
				 * replaced by the clusterSendPing() call. */
				node->ping_sent = old_ping_sent;
			}
			/* We can clear the flag after the first packet is sent.
			 * If we'll never receive a PONG, we'll never send new packets
			 * to this node. Instead after the PONG is received and we
			 * are no longer in meet/handshake status, we want to send
			 * normal PING packets. */
			node->flags &= ~REDIS_NODE_MEET;

			redisLog(REDIS_DEBUG, "Connecting with Node %.40s at %s:%d",
				node->name, node->ip, node->port + REDIS_CLUSTER_PORT_INCR);
		}
	}
	dictReleaseIterator(di);

	/* Ping some random node 1 time every 10 iterations, so that we usually ping
	 * one random node every second. */
	if (!(iteration % 10)) {
		int j;

		/* Check a few random nodes and ping the one with the oldest
		 * pong_received time. */
		for (j = 0; j < 5; j++) {
			de = dictGetRandomKey(server.cluster->nodes);
			clusterNode* this = dictGetVal(de);

			/* Don't ping nodes disconnected or with a ping currently active. */
			if (this->link == NULL || this->ping_sent != 0) continue;
			if (this->flags & (REDIS_NODE_MYSELF | REDIS_NODE_HANDSHAKE))
				continue;
			if (min_pong_node == NULL || min_pong > this->pong_received) {
				min_pong_node = this;
				min_pong = this->pong_received;
			}
		}
		if (min_pong_node) {
			redisLog(REDIS_DEBUG, "Pinging node %.40s", min_pong_node->name);
			clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
		}
	}

	/* Iterate nodes to check if we need to flag something as failing.
	 * This loop is also responsible to:
	 * 1) Check if there are orphaned masters (masters without non failing
	 *    slaves).
	 * 2) Count the max number of non failing slaves for a single master.
	 * 3) Count the number of slaves for our master, if we are a slave. */
	orphaned_masters = 0;
	max_slaves = 0;
	this_slaves = 0;
	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);
		now = mstime(); /* Use an updated time at every iteration. */
		mstime_t delay;

		if (node->flags &
			(REDIS_NODE_MYSELF | REDIS_NODE_NOADDR | REDIS_NODE_HANDSHAKE))
			continue;

		/* Orphaned master check, useful only if the current instance
		 * is a slave that may migrate to another master. */
		if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
			int okslaves = clusterCountNonFailingSlaves(node);

			/* A master is orphaned if it is serving a non-zero number of
			 * slots, have no working slaves, but used to have at least one
			 * slave. */
			if (okslaves == 0 && node->numslots > 0 && node->numslaves)
				orphaned_masters++;
			if (okslaves > max_slaves) max_slaves = okslaves;
			if (nodeIsSlave(myself) && myself->slaveof == node)
				this_slaves = okslaves;
		}

		/* If we are waiting for the PONG more than half the cluster
		 * timeout, reconnect the link: maybe there is a connection
		 * issue even if the node is alive. */
		if (node->link && /* is connected */
			now - node->link->ctime >
			server.cluster_node_timeout && /* was not already reconnected */
			node->ping_sent && /* we already sent a ping */
			node->pong_received < node->ping_sent && /* still waiting pong */
			/* and we are waiting for the pong more than timeout/2 */
			now - node->ping_sent > server.cluster_node_timeout / 2)
		{
			/* Disconnect the link, it will be reconnected automatically. */
			freeClusterLink(node->link);
		}

		/* If we have currently no active ping in this instance, and the
		 * received PONG is older than half the cluster timeout, send
		 * a new ping now, to ensure all the nodes are pinged without
		 * a too big delay. */
		if (node->link &&
			node->ping_sent == 0 &&
			(now - node->pong_received) > server.cluster_node_timeout / 2)
		{
			clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
			continue;
		}

		/* If we are a master and one of the slaves requested a manual
		 * failover, ping it continuously. */
		if (server.cluster->mf_end &&
			nodeIsMaster(myself) &&
			server.cluster->mf_slave == node &&
			node->link)
		{
			clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
			continue;
		}

		/* Check only if we have an active ping for this instance. */
		if (node->ping_sent == 0) continue;

		/* Compute the delay of the PONG. Note that if we already received
		 * the PONG, then node->ping_sent is zero, so can't reach this
		 * code at all. */
		delay = now - node->ping_sent;

		if (delay > server.cluster_node_timeout) {
			/* Timeout reached. Set the node as possibly failing if it is
			 * not already in this state. */
			if (!(node->flags & (REDIS_NODE_PFAIL | REDIS_NODE_FAIL))) {
				redisLog(REDIS_DEBUG, "*** NODE %.40s possibly failing",
					node->name);
				node->flags |= REDIS_NODE_PFAIL;
				update_state = 1;
			}
		}
	}
	dictReleaseIterator(di);

	/* If we are a slave node but the replication is still turned off,
	 * enable it if we know the address of our master and it appears to
	 * be up. */
	if (nodeIsSlave(myself) &&
		server.masterhost == NULL &&
		myself->slaveof &&
		nodeHasAddr(myself->slaveof))
	{
		replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
	}

	/* Abourt a manual failover if the timeout is reached. */
	manualFailoverCheckTimeout();

	if (nodeIsSlave(myself)) {
		clusterHandleManualFailover();
		clusterHandleSlaveFailover();
		/* If there are orphaned slaves, and we are a slave among the masters
		 * with the max number of non-failing slaves, consider migrating to
		 * the orphaned masters. Note that it does not make sense to try
		 * a migration if there is no master with at least *two* working
		 * slaves. */
		if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
			clusterHandleSlaveMigration(max_slaves);
	}

	if (update_state || server.cluster->state == REDIS_CLUSTER_FAIL)
		clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
void clusterBeforeSleep(void) {
	/* Handle failover, this is needed when it is likely that there is already
	 * the quorum from masters in order to react fast. */
	if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
		clusterHandleSlaveFailover();

	/* Update the cluster state. */
	if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
		clusterUpdateState();

	/* Save the config, possibly using fsync. */
	if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
		int fsync = server.cluster->todo_before_sleep &
			CLUSTER_TODO_FSYNC_CONFIG;
		clusterSaveConfigOrDie(fsync);
	}

	/* Reset our flags (not strictly needed since every single function
	 * called for flags set should be able to clear its flag). */
	server.cluster->todo_before_sleep = 0;
}

void clusterDoBeforeSleep(int flags) {
	server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

 /* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
  * otherwise 0. */
int bitmapTestBit(unsigned char* bitmap, int pos) {
	off_t byte = pos / 8;
	int bit = pos & 7;
	return (bitmap[byte] & (1 << bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
void bitmapSetBit(unsigned char* bitmap, int pos) {
	off_t byte = pos / 8;
	int bit = pos & 7;
	bitmap[byte] |= 1 << bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
void bitmapClearBit(unsigned char* bitmap, int pos) {
	off_t byte = pos / 8;
	int bit = pos & 7;
	bitmap[byte] &= ~(1 << bit);
}

/* Set the slot bit and return the old value. */
int clusterNodeSetSlotBit(clusterNode * n, int slot) {
	int old = bitmapTestBit(n->slots, slot);
	bitmapSetBit(n->slots, slot);
	if (!old) n->numslots++;
	return old;
}

/* Clear the slot bit and return the old value. */
int clusterNodeClearSlotBit(clusterNode * n, int slot) {
	int old = bitmapTestBit(n->slots, slot);
	bitmapClearBit(n->slots, slot);
	if (old) n->numslots--;
	return old;
}

/* Return the slot bit from the cluster node structure. */
int clusterNodeGetSlotBit(clusterNode * n, int slot) {
	return bitmapTestBit(n->slots, slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return REDIS_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and REDIS_ERR is returned. */
int clusterAddSlot(clusterNode * n, int slot) {
	if (server.cluster->slots[slot]) return REDIS_ERR;
	clusterNodeSetSlotBit(n, slot);
	server.cluster->slots[slot] = n;
	return REDIS_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns REDIS_OK if the slot was assigned, otherwise if the slot was
 * already unassigned REDIS_ERR is returned. */
int clusterDelSlot(int slot) {
	clusterNode* n = server.cluster->slots[slot];

	if (!n) return REDIS_ERR;
	redisAssert(clusterNodeClearSlotBit(n, slot) == 1);
	server.cluster->slots[slot] = NULL;
	return REDIS_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
int clusterDelNodeSlots(clusterNode * node) {
	int deleted = 0, j;

	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (clusterNodeGetSlotBit(node, j)) clusterDelSlot(j);
		deleted++;
	}
	return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
void clusterCloseAllSlots(void) {
	memset(server.cluster->migrating_slots_to, 0,
		sizeof(server.cluster->migrating_slots_to));
	memset(server.cluster->importing_slots_from, 0,
		sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */

 /* The following are defines that are only used in the evaluation function
  * and are based on heuristics. Actaully the main point about the rejoin and
  * writable delay is that they should be a few orders of magnitude larger
  * than the network latency. */
#define REDIS_CLUSTER_MAX_REJOIN_DELAY 5000
#define REDIS_CLUSTER_MIN_REJOIN_DELAY 500
#define REDIS_CLUSTER_WRITABLE_DELAY 2000

void clusterUpdateState(void) {
	int j, new_state;
	int reachable_masters = 0;
	static mstime_t among_minority_time;
	static mstime_t first_call_time = 0;

	server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

	/* If this is a master node, wait some time before turning the state
	 * into OK, since it is not a good idea to rejoin the cluster as a writable
	 * master, after a reboot, without giving the cluster a chance to
	 * reconfigure this node. Note that the delay is calculated starting from
	 * the first call to this function and not since the server start, in order
	 * to don't count the DB loading time. */
	if (first_call_time == 0) first_call_time = mstime();
	if (nodeIsMaster(myself) &&
		server.cluster->state == REDIS_CLUSTER_FAIL &&
		mstime() - first_call_time < REDIS_CLUSTER_WRITABLE_DELAY) return;

	/* Start assuming the state is OK. We'll turn it into FAIL if there
	 * are the right conditions. */
	new_state = REDIS_CLUSTER_OK;

	/* Check if all the slots are covered. */
	if (server.cluster_require_full_coverage) {
		for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
			if (server.cluster->slots[j] == NULL ||
				server.cluster->slots[j]->flags & (REDIS_NODE_FAIL))
			{
				new_state = REDIS_CLUSTER_FAIL;
				break;
			}
		}
	}

	/* Compute the cluster size, that is the number of master nodes
	 * serving at least a single slot.
	 *
	 * At the same time count the number of reachable masters having
	 * at least one slot. */
	{
		dictIterator* di;
		dictEntry* de;

		server.cluster->size = 0;
		di = dictGetSafeIterator(server.cluster->nodes);
		while ((de = dictNext(di)) != NULL) {
			clusterNode* node = dictGetVal(de);

			if (nodeIsMaster(node) && node->numslots) {
				server.cluster->size++;
				if ((node->flags & (REDIS_NODE_FAIL | REDIS_NODE_PFAIL)) == 0)
					reachable_masters++;
			}
		}
		dictReleaseIterator(di);
	}

	/* If we are in a minority partition, change the cluster state
	 * to FAIL. */
	{
		int needed_quorum = (server.cluster->size / 2) + 1;

		if (reachable_masters < needed_quorum) {
			new_state = REDIS_CLUSTER_FAIL;
			among_minority_time = mstime();
		}
	}

	/* Log a state change */
	if (new_state != server.cluster->state) {
		mstime_t rejoin_delay = server.cluster_node_timeout;

		/* If the instance is a master and was partitioned away with the
		 * minority, don't let it accept queries for some time after the
		 * partition heals, to make sure there is enough time to receive
		 * a configuration update. */
		if (rejoin_delay > REDIS_CLUSTER_MAX_REJOIN_DELAY)
			rejoin_delay = REDIS_CLUSTER_MAX_REJOIN_DELAY;
		if (rejoin_delay < REDIS_CLUSTER_MIN_REJOIN_DELAY)
			rejoin_delay = REDIS_CLUSTER_MIN_REJOIN_DELAY;

		if (new_state == REDIS_CLUSTER_OK &&
			nodeIsMaster(myself) &&
			mstime() - among_minority_time < rejoin_delay)
		{
			return;
		}

		/* Change the state and log the event. */
		redisLog(REDIS_WARNING, "Cluster state changed: %s",
			new_state == REDIS_CLUSTER_OK ? "ok" : "fail");
		server.cluster->state = new_state;
	}
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this lots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return REDIS_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns REDIS_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, REDIS_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
int verifyClusterConfigWithData(void) {
	int j;
	int update_config = 0;

	/* If this node is a slave, don't perform the check at all as we
	 * completely depend on the replication stream. */
	if (nodeIsSlave(myself)) return REDIS_OK;

	/* Make sure we only have keys in DB0. */
	for (j = 1; j < server.dbnum; j++) {
		if (dictSize(server.db[j].dict)) return REDIS_ERR;
	}

	/* Check that all the slots we see populated memory have a corresponding
	 * entry in the cluster table. Otherwise fix the table. */
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
		/* Check if we are assigned to this slot or if we are importing it.
		 * In both cases check the next slot as the configuration makes
		 * sense. */
		if (server.cluster->slots[j] == myself ||
			server.cluster->importing_slots_from[j] != NULL) continue;

		/* If we are here data and cluster config don't agree, and we have
		 * slot 'j' populated even if we are not importing it, nor we are
		 * assigned to this slot. Fix this condition. */

		update_config++;
		/* Case A: slot is unassigned. Take responsibility for it. */
		if (server.cluster->slots[j] == NULL) {
			redisLog(REDIS_WARNING, "I have keys for unassigned slot %d. "
				"Taking responsibility for it.", j);
			clusterAddSlot(myself, j);
		}
		else {
			redisLog(REDIS_WARNING, "I have keys for slot %d, but the slot is "
				"assigned to another node. "
				"Setting it to importing state.", j);
			server.cluster->importing_slots_from[j] = server.cluster->slots[j];
		}
	}
	if (update_config) clusterSaveConfigOrDie(1);
	return REDIS_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

 /* Set the specified node 'n' as master for this node.
  * If this node is currently a master, it is turned into a slave. */
void clusterSetMaster(clusterNode * n) {
	redisAssert(n != myself);
	redisAssert(myself->numslots == 0);

	if (nodeIsMaster(myself)) {
		myself->flags &= ~REDIS_NODE_MASTER;
		myself->flags |= REDIS_NODE_SLAVE;
		clusterCloseAllSlots();
	}
	else {
		if (myself->slaveof)
			clusterNodeRemoveSlave(myself->slaveof, myself);
	}
	myself->slaveof = n;
	clusterNodeAddSlave(n, myself);
	replicationSetMaster(n->ip, n->port);
	resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
	uint16_t flag;
	char* name;
};

static struct redisNodeFlags redisNodeFlagsTable[] = {
	{REDIS_NODE_MYSELF,    "myself,"},
	{REDIS_NODE_MASTER,    "master,"},
	{REDIS_NODE_SLAVE,     "slave,"},
	{REDIS_NODE_PFAIL,     "fail?,"},
	{REDIS_NODE_FAIL,      "fail,"},
	{REDIS_NODE_HANDSHAKE, "handshake,"},
	{REDIS_NODE_NOADDR,    "noaddr,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
sds representRedisNodeFlags(sds ci, uint16_t flags) {
	if (flags == 0) {
		ci = sdscat(ci, "noflags,");
	}
	else {
		int i, size = sizeof(redisNodeFlagsTable) / sizeof(struct redisNodeFlags);
		for (i = 0; i < size; i++) {
			struct redisNodeFlags* nodeflag = redisNodeFlagsTable + i;
			if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
		}
	}
	sdsIncrLen(ci, -1); /* Remove trailing comma. */
	return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
sds clusterGenNodeDescription(clusterNode * node) {
	int j, start;
	sds ci;

	/* Node coordinates */
	ci = sdscatprintf(sdsempty(), "%.40s %s:%d ",
		node->name,
		node->ip,
		node->port);

	/* Flags */
	ci = representRedisNodeFlags(ci, node->flags);

	/* Slave of... or just "-" */
	if (node->slaveof)
		ci = sdscatprintf(ci, " %.40s ", node->slaveof->name);
	else
		ci = sdscatlen(ci, " - ", 3);

	/* Latency from the POV of this node, config epoch, link status */
	ci = sdscatprintf(ci, "%lld %lld %llu %s",
		(PORT_LONGLONG)node->ping_sent,
		(PORT_LONGLONG)node->pong_received,
		(PORT_ULONGLONG)node->configEpoch,
		(node->link || node->flags & REDIS_NODE_MYSELF) ?
		"connected" : "disconnected");

	/* Slots served by this instance */
	start = -1;
	for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
		int bit;

		if ((bit = clusterNodeGetSlotBit(node, j)) != 0) {
			if (start == -1) start = j;
		}
		if (start != -1 && (!bit || j == REDIS_CLUSTER_SLOTS - 1)) {
			if (bit && j == REDIS_CLUSTER_SLOTS - 1) j++;

			if (start == j - 1) {
				ci = sdscatprintf(ci, " %d", start);
			}
			else {
				ci = sdscatprintf(ci, " %d-%d", start, j - 1);
			}
			start = -1;
		}
	}

	/* Just for MYSELF node we also dump info about slots that
	 * we are migrating to other instances or importing from other
	 * instances. */
	if (node->flags & REDIS_NODE_MYSELF) {
		for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
			if (server.cluster->migrating_slots_to[j]) {
				ci = sdscatprintf(ci, " [%d->-%.40s]", j,
					server.cluster->migrating_slots_to[j]->name);
			}
			else if (server.cluster->importing_slots_from[j]) {
				ci = sdscatprintf(ci, " [%d-<-%.40s]", j,
					server.cluster->importing_slots_from[j]->name);
			}
		}
	}
	return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
sds clusterGenNodesDescription(int filter) {
	sds ci = sdsempty(), ni;
	dictIterator* di;
	dictEntry* de;

	di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);

		if (node->flags & filter) continue;
		ni = clusterGenNodeDescription(node);
		ci = sdscatsds(ci, ni);
		sdsfree(ni);
		ci = sdscatlen(ci, "\n", 1);
	}
	dictReleaseIterator(di);
	return ci;
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */

int getSlotOrReply(redisClient * c, robj * o) {
	PORT_LONGLONG slot;

	if (getLongLongFromObject(o, &slot) != REDIS_OK ||
		slot < 0 || slot >= REDIS_CLUSTER_SLOTS)
	{
		addReplyError(c, "Invalid or out of range slot");
		return -1;
	}
	return (int)slot;
}

void clusterReplyMultiBulkSlots(redisClient * c) {
	/* Format: 1) 1) start slot
	 *            2) end slot
	 *            3) 1) master IP
	 *               2) master port
	 *            4) 1) replica IP
	 *               2) replica port
	 *           ... continued until done
	 */

	int num_masters = 0;
	void* slot_replylen = addDeferredMultiBulkLength(c);

	dictEntry* de;
	dictIterator* di = dictGetSafeIterator(server.cluster->nodes);
	while ((de = dictNext(di)) != NULL) {
		clusterNode* node = dictGetVal(de);
		int j = 0, start = -1;

		/* Skip slaves (that are iterated when producing the output of their
		 * master) and  masters not serving any slot. */
		if (!nodeIsMaster(node) || node->numslots == 0) continue;

		for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
			int bit, i;

			if ((bit = clusterNodeGetSlotBit(node, j)) != 0) {
				if (start == -1) start = j;
			}
			if (start != -1 && (!bit || j == REDIS_CLUSTER_SLOTS - 1)) {
				int nested_elements = 3; /* slots (2) + master addr (1). */
				void* nested_replylen = addDeferredMultiBulkLength(c);

				if (bit && j == REDIS_CLUSTER_SLOTS - 1) j++;

				/* If slot exists in output map, add to it's list.
				 * else, create a new output map for this slot */
				if (start == j - 1) {
					addReplyLongLong(c, start); /* only one slot; low==high */
					addReplyLongLong(c, start);
				}
				else {
					addReplyLongLong(c, start); /* low */
					addReplyLongLong(c, j - 1);   /* high */
				}
				start = -1;

				/* First node reply position is always the master */
				addReplyMultiBulkLen(c, 2);
				addReplyBulkCString(c, node->ip);
				addReplyLongLong(c, node->port);

				/* Remaining nodes in reply are replicas for slot range */
				for (i = 0; i < node->numslaves; i++) {
					/* This loop is copy/pasted from clusterGenNodeDescription()
					 * with modifications for per-slot node aggregation */
					if (nodeFailed(node->slaves[i])) continue;
					addReplyMultiBulkLen(c, 2);
					addReplyBulkCString(c, node->slaves[i]->ip);
					addReplyLongLong(c, node->slaves[i]->port);
					nested_elements++;
				}
				setDeferredMultiBulkLength(c, nested_replylen, nested_elements);
				num_masters++;
			}
		}
	}
	dictReleaseIterator(di);
	setDeferredMultiBulkLength(c, slot_replylen, num_masters);
}

void clusterCommand(redisClient * c) {
	if (server.cluster_enabled == 0) {
		addReplyError(c, "This instance has cluster support disabled");
		return;
	}

	if (!strcasecmp(c->argv[1]->ptr, "meet") && c->argc == 4) {
		PORT_LONGLONG port;

		if (getLongLongFromObject(c->argv[3], &port) != REDIS_OK) {
			addReplyErrorFormat(c, "Invalid TCP port specified: %s",
				(char*)c->argv[3]->ptr);
			return;
		}

		if (clusterStartHandshake(c->argv[2]->ptr, (int)port) == 0 && WIN_PORT_FIX /* cast (int) */
			errno == EINVAL)
		{
			addReplyErrorFormat(c, "Invalid node address specified: %s:%s",
				(char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
		}
		else {
			addReply(c, shared.ok);
		}
	}
	else if (!strcasecmp(c->argv[1]->ptr, "nodes") && c->argc == 2) {
		/* CLUSTER NODES */
		robj* o;
		sds ci = clusterGenNodesDescription(0);

		o = createObject(REDIS_STRING, ci);
		addReplyBulk(c, o);
		decrRefCount(o);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "myid") && c->argc == 2) {
		/* CLUSTER MYID */
		addReplyBulkCBuffer(c, myself->name, REDIS_CLUSTER_NAMELEN);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "slots") && c->argc == 2) {
		/* CLUSTER SLOTS */
		clusterReplyMultiBulkSlots(c);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "flushslots") && c->argc == 2) {
		/* CLUSTER FLUSHSLOTS */
		if (dictSize(server.db[0].dict) != 0) {
			addReplyError(c, "DB must be empty to perform CLUSTER FLUSHSLOTS.");
			return;
		}
		clusterDelNodeSlots(myself);
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
		addReply(c, shared.ok);
	}
	else if ((!strcasecmp(c->argv[1]->ptr, "addslots") ||
		!strcasecmp(c->argv[1]->ptr, "delslots")) && c->argc >= 3)
	{
		/* CLUSTER ADDSLOTS <slot> [slot] ... */
		/* CLUSTER DELSLOTS <slot> [slot] ... */
		int j, slot;
		unsigned char* slots = zmalloc(REDIS_CLUSTER_SLOTS);
		int del = !strcasecmp(c->argv[1]->ptr, "delslots");

		memset(slots, 0, REDIS_CLUSTER_SLOTS);
		/* Check that all the arguments are parseable and that all the
		 * slots are not already busy. */
		for (j = 2; j < c->argc; j++) {
			if ((slot = getSlotOrReply(c, c->argv[j])) == -1) {
				zfree(slots);
				return;
			}
			if (del && server.cluster->slots[slot] == NULL) {
				addReplyErrorFormat(c, "Slot %d is already unassigned", slot);
				zfree(slots);
				return;
			}
			else if (!del && server.cluster->slots[slot]) {
				addReplyErrorFormat(c, "Slot %d is already busy", slot);
				zfree(slots);
				return;
			}
			if (slots[slot]++ == 1) {
				addReplyErrorFormat(c, "Slot %d specified multiple times",
					(int)slot);
				zfree(slots);
				return;
			}
		}
		for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
			if (slots[j]) {
				int retval;

				/* If this slot was set as importing we can clear this
				 * state as now we are the real owner of the slot. */
				if (server.cluster->importing_slots_from[j])
					server.cluster->importing_slots_from[j] = NULL;

				retval = del ? clusterDelSlot(j) :
					clusterAddSlot(myself, j);
				redisAssertWithInfo(c, NULL, retval == REDIS_OK);
			}
		}
		zfree(slots);
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
		addReply(c, shared.ok);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "setslot") && c->argc >= 4) {
		/* SETSLOT 10 MIGRATING <node ID> */
		/* SETSLOT 10 IMPORTING <node ID> */
		/* SETSLOT 10 STABLE */
		/* SETSLOT 10 NODE <node ID> */
		int slot;
		clusterNode* n;

		if ((slot = getSlotOrReply(c, c->argv[2])) == -1) return;

		if (!strcasecmp(c->argv[3]->ptr, "migrating") && c->argc == 5) {
			if (server.cluster->slots[slot] != myself) {
				addReplyErrorFormat(c, "I'm not the owner of hash slot %u", slot);
				return;
			}
			if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
				addReplyErrorFormat(c, "I don't know about node %s",
					(char*)c->argv[4]->ptr);
				return;
			}
			server.cluster->migrating_slots_to[slot] = n;
		}
		else if (!strcasecmp(c->argv[3]->ptr, "importing") && c->argc == 5) {
			if (server.cluster->slots[slot] == myself) {
				addReplyErrorFormat(c,
					"I'm already the owner of hash slot %u", slot);
				return;
			}
			if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
				addReplyErrorFormat(c, "I don't know about node %s",
					(char*)c->argv[3]->ptr);
				return;
			}
			server.cluster->importing_slots_from[slot] = n;
		}
		else if (!strcasecmp(c->argv[3]->ptr, "stable") && c->argc == 4) {
			/* CLUSTER SETSLOT <SLOT> STABLE */
			server.cluster->importing_slots_from[slot] = NULL;
			server.cluster->migrating_slots_to[slot] = NULL;
		}
		else if (!strcasecmp(c->argv[3]->ptr, "node") && c->argc == 5) {
			/* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
			clusterNode* n = clusterLookupNode(c->argv[4]->ptr);

			if (!n) {
				addReplyErrorFormat(c, "Unknown node %s",
					(char*)c->argv[4]->ptr);
				return;
			}
			/* If this hash slot was served by 'myself' before to switch
			 * make sure there are no longer local keys for this hash slot. */
			if (server.cluster->slots[slot] == myself && n != myself) {
				if (countKeysInSlot(slot) != 0) {
					addReplyErrorFormat(c,
						"Can't assign hashslot %d to a different node "
						"while I still hold keys for this hash slot.", slot);
					return;
				}
			}
			/* If this slot is in migrating status but we have no keys
			 * for it assigning the slot to another node will clear
			 * the migratig status. */
			if (countKeysInSlot(slot) == 0 &&
				server.cluster->migrating_slots_to[slot])
				server.cluster->migrating_slots_to[slot] = NULL;

			/* If this node was importing this slot, assigning the slot to
			 * itself also clears the importing status. */
			if (n == myself &&
				server.cluster->importing_slots_from[slot])
			{
				/* This slot was manually migrated, set this node configEpoch
				 * to a new epoch so that the new version can be propagated
				 * by the cluster.
				 *
				 * Note that if this ever results in a collision with another
				 * node getting the same configEpoch, for example because a
				 * failover happens at the same time we close the slot, the
				 * configEpoch collision resolution will fix it assigning
				 * a different epoch to each node. */
				if (clusterBumpConfigEpochWithoutConsensus() == REDIS_OK) {
					redisLog(REDIS_WARNING,
						"configEpoch updated after importing slot %d", slot);
				}
				server.cluster->importing_slots_from[slot] = NULL;
			}
			clusterDelSlot(slot);
			clusterAddSlot(n, slot);
		}
		else {
			addReplyError(c,
				"Invalid CLUSTER SETSLOT action or number of arguments");
			return;
		}
		clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG | CLUSTER_TODO_UPDATE_STATE);
		addReply(c, shared.ok);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "info") && c->argc == 2) {
		/* CLUSTER INFO */
		char* statestr[] = { "ok","fail","needhelp" };
		int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
		uint64_t myepoch;
		int j;

		for (j = 0; j < REDIS_CLUSTER_SLOTS; j++) {
			clusterNode* n = server.cluster->slots[j];

			if (n == NULL) continue;
			slots_assigned++;
			if (nodeFailed(n)) {
				slots_fail++;
			}
			else if (nodeTimedOut(n)) {
				slots_pfail++;
			}
			else {
				slots_ok++;
			}
		}

		myepoch = (nodeIsSlave(myself) && myself->slaveof) ?
			myself->slaveof->configEpoch : myself->configEpoch;

		sds info = sdscatprintf(sdsempty(),
			"cluster_state:%s\r\n"
			"cluster_slots_assigned:%d\r\n"
			"cluster_slots_ok:%d\r\n"
			"cluster_slots_pfail:%d\r\n"
			"cluster_slots_fail:%d\r\n"
			"cluster_known_nodes:%Iu\r\n"                                       WIN_PORT_FIX /* %lu -> %Iu */
			"cluster_size:%d\r\n"
			"cluster_current_epoch:%llu\r\n"
			"cluster_my_epoch:%llu\r\n"
			"cluster_stats_messages_sent:%lld\r\n"
			"cluster_stats_messages_received:%lld\r\n"
			, statestr[server.cluster->state],
			slots_assigned,
			slots_ok,
			slots_pfail,
			slots_fail,
			dictSize(server.cluster->nodes),
			server.cluster->size,
			(PORT_ULONGLONG)server.cluster->currentEpoch,
			(PORT_ULONGLONG)myepoch,
			server.cluster->stats_bus_messages_sent,
			server.cluster->stats_bus_messages_received
		);
		addReplySds(c, sdscatprintf(sdsempty(), "$%Iu\r\n", WIN_PORT_FIX /* %lu -> %Iu */
		(PORT_ULONG)sdslen(info)));
		addReplySds(c, info);
		addReply(c, shared.crlf);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "saveconfig") && c->argc == 2) {
		int retval = clusterSaveConfig(1);

		if (retval == 0)
			addReply(c, shared.ok);
		else
			addReplyErrorFormat(c, "error saving the cluster node config: %s",
				strerror(errno));
	}
	else if (!strcasecmp(c->argv[1]->ptr, "keyslot") && c->argc == 3) {
		/* CLUSTER KEYSLOT <key> */
		sds key = c->argv[2]->ptr;

		addReplyLongLong(c, keyHashSlot(key, (int)sdslen(key)));                  WIN_PORT_FIX /* cast (int) */
	}
	else if (!strcasecmp(c->argv[1]->ptr, "countkeysinslot") && c->argc == 3) {
		/* CLUSTER COUNTKEYSINSLOT <slot> */
		PORT_LONGLONG slot;

		if (getLongLongFromObjectOrReply(c, c->argv[2], &slot, NULL) != REDIS_OK)
			return;
		if (slot < 0 || slot >= REDIS_CLUSTER_SLOTS) {
			addReplyError(c, "Invalid slot");
			return;
		}
		addReplyLongLong(c, countKeysInSlot((unsigned int)slot));                WIN_PORT_FIX /* cast (unsigned int) */
	}
	else if (!strcasecmp(c->argv[1]->ptr, "getkeysinslot") && c->argc == 4) {
		/* CLUSTER GETKEYSINSLOT <slot> <count> */
		PORT_LONGLONG maxkeys, slot;
		unsigned int numkeys, j;
		robj** keys;

		if (getLongLongFromObjectOrReply(c, c->argv[2], &slot, NULL) != REDIS_OK)
			return;
		if (getLongLongFromObjectOrReply(c, c->argv[3], &maxkeys, NULL)
			!= REDIS_OK)
			return;
		if (slot < 0 || slot >= REDIS_CLUSTER_SLOTS || maxkeys < 0) {
			addReplyError(c, "Invalid slot or number of keys");
			return;
		}

		keys = zmalloc(sizeof(robj*) * maxkeys);
		numkeys = getKeysInSlot((unsigned int)slot, keys, (unsigned int)maxkeys); WIN_PORT_FIX /* cast (unsigned int) */
			addReplyMultiBulkLen(c, numkeys);
		for (j = 0; j < numkeys; j++) addReplyBulk(c, keys[j]);
		zfree(keys);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "forget") && c->argc == 3) {
		/* CLUSTER FORGET <NODE ID> */
		clusterNode* n = clusterLookupNode(c->argv[2]->ptr);

		if (!n) {
			addReplyErrorFormat(c, "Unknown node %s", (char*)c->argv[2]->ptr);
			return;
		}
		else if (n == myself) {
			addReplyError(c, "I tried hard but I can't forget myself...");
			return;
		}
		else if (nodeIsSlave(myself) && myself->slaveof == n) {
			addReplyError(c, "Can't forget my master!");
			return;
		}
		clusterBlacklistAddNode(n);
		clusterDelNode(n);
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE |
			CLUSTER_TODO_SAVE_CONFIG);
		addReply(c, shared.ok);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "replicate") && c->argc == 3) {
		/* CLUSTER REPLICATE <NODE ID> */
		clusterNode* n = clusterLookupNode(c->argv[2]->ptr);

		/* Lookup the specified node in our table. */
		if (!n) {
			addReplyErrorFormat(c, "Unknown node %s", (char*)c->argv[2]->ptr);
			return;
		}

		/* I can't replicate myself. */
		if (n == myself) {
			addReplyError(c, "Can't replicate myself");
			return;
		}

		/* Can't replicate a slave. */
		if (nodeIsSlave(n)) {
			addReplyError(c, "I can only replicate a master, not a slave.");
			return;
		}

		/* If the instance is currently a master, it should have no assigned
		 * slots nor keys to accept to replicate some other node.
		 * Slaves can switch to another master without issues. */
		if (nodeIsMaster(myself) &&
			(myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
			addReplyError(c,
				"To set a master the node must be empty and "
				"without assigned slots.");
			return;
		}

		/* Set the master. */
		clusterSetMaster(n);
		clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE | CLUSTER_TODO_SAVE_CONFIG);
		addReply(c, shared.ok);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "slaves") && c->argc == 3) {
		/* CLUSTER SLAVES <NODE ID> */
		clusterNode* n = clusterLookupNode(c->argv[2]->ptr);
		int j;

		/* Lookup the specified node in our table. */
		if (!n) {
			addReplyErrorFormat(c, "Unknown node %s", (char*)c->argv[2]->ptr);
			return;
		}

		if (nodeIsSlave(n)) {
			addReplyError(c, "The specified node is not a master");
			return;
		}

		addReplyMultiBulkLen(c, n->numslaves);
		for (j = 0; j < n->numslaves; j++) {
			sds ni = clusterGenNodeDescription(n->slaves[j]);
			addReplyBulkCString(c, ni);
			sdsfree(ni);
		}
	}
	else if (!strcasecmp(c->argv[1]->ptr, "count-failure-reports") &&
		c->argc == 3)
	{
		/* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
		clusterNode* n = clusterLookupNode(c->argv[2]->ptr);

		if (!n) {
			addReplyErrorFormat(c, "Unknown node %s", (char*)c->argv[2]->ptr);
			return;
		}
		else {
			addReplyLongLong(c, clusterNodeFailureReportsCount(n));
		}
	}
	else if (!strcasecmp(c->argv[1]->ptr, "failover") &&
		(c->argc == 2 || c->argc == 3))
	{
		/* CLUSTER FAILOVER [FORCE|TAKEOVER] */
		int force = 0, takeover = 0;

		if (c->argc == 3) {
			if (!strcasecmp(c->argv[2]->ptr, "force")) {
				force = 1;
			}
			else if (!strcasecmp(c->argv[2]->ptr, "takeover")) {
				takeover = 1;
				force = 1; /* Takeover also implies force. */
			}
			else {
				addReply(c, shared.syntaxerr);
				return;
			}
		}

		/* Check preconditions. */
		if (nodeIsMaster(myself)) {
			addReplyError(c, "You should send CLUSTER FAILOVER to a slave");
			return;
		}
		else if (myself->slaveof == NULL) {
			addReplyError(c, "I'm a slave but my master is unknown to me");
			return;
		}
		else if (!force &&
			(nodeFailed(myself->slaveof) ||
				myself->slaveof->link == NULL))
		{
			addReplyError(c, "Master is down or failed, "
				"please use CLUSTER FAILOVER FORCE");
			return;
		}
		resetManualFailover();
		server.cluster->mf_end = mstime() + REDIS_CLUSTER_MF_TIMEOUT;

		if (takeover) {
			/* A takeover does not perform any initial check. It just
			 * generates a new configuration epoch for this node without
			 * consensus, claims the master's slots, and broadcast the new
			 * configuration. */
			redisLog(REDIS_WARNING, "Taking over the master (user request).");
			clusterBumpConfigEpochWithoutConsensus();
			clusterFailoverReplaceYourMaster();
		}
		else if (force) {
			/* If this is a forced failover, we don't need to talk with our
			 * master to agree about the offset. We just failover taking over
			 * it without coordination. */
			redisLog(REDIS_WARNING, "Forced failover user request accepted.");
			server.cluster->mf_can_start = 1;
		}
		else {
			redisLog(REDIS_WARNING, "Manual failover user request accepted.");
			clusterSendMFStart(myself->slaveof);
		}
		addReply(c, shared.ok);
	}
	else if (!strcasecmp(c->argv[1]->ptr, "set-config-epoch") && c->argc == 3)
	{
		/* CLUSTER SET-CONFIG-EPOCH <epoch>
		 *
		 * The user is allowed to set the config epoch only when a node is
		 * totally fresh: no config epoch, no other known node, and so forth.
		 * This happens at cluster creation time to start with a cluster where
		 * every node has a different node ID, without to rely on the conflicts
		 * resolution system which is too slow when a big cluster is created. */
		PORT_LONGLONG epoch;

		if (getLongLongFromObjectOrReply(c, c->argv[2], &epoch, NULL) != REDIS_OK)
			return;

		if (epoch < 0) {
			addReplyErrorFormat(c, "Invalid config epoch specified: %lld", epoch);
		}
		else if (dictSize(server.cluster->nodes) > 1) {
			addReplyError(c, "The user can assign a config epoch only when the "
				"node does not know any other node.");
		}
		else if (myself->configEpoch != 0) {
			addReplyError(c, "Node config epoch is already non-zero");
		}
		else {
			myself->configEpoch = epoch;
			redisLog(REDIS_WARNING,
				"configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
				(PORT_ULONGLONG)myself->configEpoch);

			if (server.cluster->currentEpoch < (uint64_t)epoch)
				server.cluster->currentEpoch = epoch;
			/* No need to fsync the config here since in the unlucky event
			 * of a failure to persist the config, the conflict resolution code
			 * will assign an unique config to this node. */
			clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE |
				CLUSTER_TODO_SAVE_CONFIG);
			addReply(c, shared.ok);
		}
	}
	else if (!strcasecmp(c->argv[1]->ptr, "reset") &&
		(c->argc == 2 || c->argc == 3))
	{
		/* CLUSTER RESET [SOFT|HARD] */
		int hard = 0;

		/* Parse soft/hard argument. Default is soft. */
		if (c->argc == 3) {
			if (!strcasecmp(c->argv[2]->ptr, "hard")) {
				hard = 1;
			}
			else if (!strcasecmp(c->argv[2]->ptr, "soft")) {
				hard = 0;
			}
			else {
				addReply(c, shared.syntaxerr);
				return;
			}
		}

		/* Slaves can be reset while containing data, but not master nodes
		 * that must be empty. */
		if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
			addReplyError(c, "CLUSTER RESET can't be called with "
				"master nodes containing keys");
			return;
		}
		clusterReset(hard);
		addReply(c, shared.ok);
	}
	else {
		addReplyError(c, "Wrong CLUSTER subcommand or number of arguments");
	}
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

 /* Generates a DUMP-format representation of the object 'o', adding it to the
  * io stream pointed by 'rio'. This function can't fail. */
void createDumpPayload(rio * payload, robj * o) {
	unsigned char buf[2];
	uint64_t crc;

	/* Serialize the object in a RDB-like format. It consist of an object type
	 * byte followed by the serialized object. This is understood by RESTORE. */
	rioInitWithBuffer(payload, sdsempty());
	redisAssert(rdbSaveObjectType(payload, o));
	redisAssert(rdbSaveObject(payload, o));

	/* Write the footer, this is how it looks like:
	 * ----------------+---------------------+---------------+
	 * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
	 * ----------------+---------------------+---------------+
	 * RDB version and CRC are both in little endian.
	 */

	 /* RDB version */
	buf[0] = REDIS_RDB_VERSION & 0xff;
	buf[1] = (REDIS_RDB_VERSION >> 8) & 0xff;
	payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr, buf, 2);

	/* CRC64 */
	crc = crc64(0, (unsigned char*)payload->io.buffer.ptr,
		sdslen(payload->io.buffer.ptr));
	memrev64ifbe(&crc);
	payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr, &crc, 8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid REDIS_OK is returned, otherwise REDIS_ERR
 * is returned. */
int verifyDumpPayload(unsigned char* p, size_t len) {
	unsigned char* footer;
	uint16_t rdbver;
	uint64_t crc;

	/* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
	if (len < 10) return REDIS_ERR;
	footer = p + (len - 10);

	/* Verify RDB version */
	rdbver = (footer[1] << 8) | footer[0];
	if (rdbver != REDIS_RDB_VERSION) return REDIS_ERR;

	/* Verify CRC64 */
	crc = crc64(0, p, len - 8);
	memrev64ifbe(&crc);
	return (memcmp(&crc, footer + 2, 8) == 0) ? REDIS_OK : REDIS_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
void dumpCommand(redisClient * c) {
	robj* o, * dumpobj;
	rio payload;

	/* Check if the key is here. */
	if ((o = lookupKeyRead(c->db, c->argv[1])) == NULL) {
		addReply(c, shared.nullbulk);
		return;
	}

	/* Create the DUMP encoded representation. */
	createDumpPayload(&payload, o);

	/* Transfer to the client */
	dumpobj = createObject(REDIS_STRING, payload.io.buffer.ptr);
	addReplyBulk(c, dumpobj);
	decrRefCount(dumpobj);
	return;
}

/* RESTORE key ttl serialized-value [REPLACE] */
void restoreCommand(redisClient * c) {
	PORT_LONGLONG ttl;
	rio payload;
	int j, type, replace = 0;
	robj* obj;

	/* Parse additional options */
	for (j = 4; j < c->argc; j++) {
		if (!strcasecmp(c->argv[j]->ptr, "replace")) {
			replace = 1;
		}
		else {
			addReply(c, shared.syntaxerr);
			return;
		}
	}

	/* Make sure this key does not already exist here... */
	if (!replace && lookupKeyWrite(c->db, c->argv[1]) != NULL) {
		addReply(c, shared.busykeyerr);
		return;
	}

	/* Check if the TTL value makes sense */
	if (getLongLongFromObjectOrReply(c, c->argv[2], &ttl, NULL) != REDIS_OK) {
		return;
	}
	else if (ttl < 0) {
		addReplyError(c, "Invalid TTL value, must be >= 0");
		return;
	}

	/* Verify RDB version and data checksum. */
	if (verifyDumpPayload(c->argv[3]->ptr, sdslen(c->argv[3]->ptr)) == REDIS_ERR)
	{
		addReplyError(c, "DUMP payload version or checksum are wrong");
		return;
	}

	rioInitWithBuffer(&payload, c->argv[3]->ptr);
	if (((type = rdbLoadObjectType(&payload)) == -1) ||
		((obj = rdbLoadObject(type, &payload)) == NULL))
	{
		addReplyError(c, "Bad data format");
		return;
	}

	/* Remove the old key if needed. */
	if (replace) dbDelete(c->db, c->argv[1]);

	/* Create the key and set the TTL if any */
	dbAdd(c->db, c->argv[1], obj);
	if (ttl) setExpire(c->db, c->argv[1], mstime() + ttl);
	signalModifiedKey(c->db, c->argv[1]);
	addReply(c, shared.ok);
	server.dirty++;
}

/* MIGRATE socket cache implementation.
 *
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. */

typedef struct migrateCachedSocket {
	int fd;
	PORT_LONG last_dbid;
	time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
migrateCachedSocket* migrateGetSocket(redisClient * c, robj * host, robj * port, PORT_LONG timeout) {
	int fd;
	sds name = sdsempty();
	migrateCachedSocket* cs;

	/* Check if we have an already cached socket for this ip:port pair. */
	name = sdscatlen(name, host->ptr, sdslen(host->ptr));
	name = sdscatlen(name, ":", 1);
	name = sdscatlen(name, port->ptr, sdslen(port->ptr));
	cs = dictFetchValue(server.migrate_cached_sockets, name);
	if (cs) {
		sdsfree(name);
		cs->last_use_time = server.unixtime;
		return cs;
	}

	/* No cached socket, create one. */
	if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
		/* Too many items, drop one at random. */
		dictEntry* de = dictGetRandomKey(server.migrate_cached_sockets);
		cs = dictGetVal(de);
		close(cs->fd);
		zfree(cs);
		dictDelete(server.migrate_cached_sockets, dictGetKey(de));
	}

	/* Create the socket */
	fd = anetTcpNonBlockConnect(server.neterr, c->argv[1]->ptr,
		atoi(c->argv[2]->ptr));
	if (fd == -1) {
		sdsfree(name);
		addReplyErrorFormat(c, "Can't connect to target node: %s",
			server.neterr);
		return NULL;
	}
	anetEnableTcpNoDelay(server.neterr, fd);

	/* Check if it connects within the specified timeout. */
	if ((aeWait(fd, AE_WRITABLE, timeout) & AE_WRITABLE) == 0) {
		sdsfree(name);
		addReplySds(c,
			sdsnew("-IOERR error or timeout connecting to the client\r\n"));
		close(fd);
		return NULL;
	}

	/* Add to the cache and return it to the caller. */
	cs = zmalloc(sizeof(*cs));
	cs->fd = fd;
	cs->last_dbid = -1;
	cs->last_use_time = server.unixtime;
	dictAdd(server.migrate_cached_sockets, name, cs);
	return cs;
}

/* Free a migrate cached connection. */
void migrateCloseSocket(robj * host, robj * port) {
	sds name = sdsempty();
	migrateCachedSocket* cs;

	name = sdscatlen(name, host->ptr, sdslen(host->ptr));
	name = sdscatlen(name, ":", 1);
	name = sdscatlen(name, port->ptr, sdslen(port->ptr));
	cs = dictFetchValue(server.migrate_cached_sockets, name);
	if (!cs) {
		sdsfree(name);
		return;
	}

	close(cs->fd);
	zfree(cs);
	dictDelete(server.migrate_cached_sockets, name);
	sdsfree(name);
}

void migrateCloseTimedoutSockets(void) {
	dictIterator* di = dictGetSafeIterator(server.migrate_cached_sockets);
	dictEntry* de;

	while ((de = dictNext(di)) != NULL) {
		migrateCachedSocket* cs = dictGetVal(de);

		if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
			close(cs->fd);
			zfree(cs);
			dictDelete(server.migrate_cached_sockets, dictGetKey(de));
		}
	}
	dictReleaseIterator(di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE] */
void migrateCommand(redisClient * c) {
	migrateCachedSocket* cs;
	int copy, replace, j;
	PORT_LONG timeout;
	PORT_LONG dbid;
	PORT_LONGLONG ttl, expireat;
	robj* o;
	rio cmd, payload;
	int retry_num = 0;

try_again:
	/* Initialization */
	copy = 0;
	replace = 0;
	ttl = 0;

	/* Parse additional options */
	for (j = 6; j < c->argc; j++) {
		if (!strcasecmp(c->argv[j]->ptr, "copy")) {
			copy = 1;
		}
		else if (!strcasecmp(c->argv[j]->ptr, "replace")) {
			replace = 1;
		}
		else {
			addReply(c, shared.syntaxerr);
			return;
		}
	}

	/* Sanity check */
	if (getLongFromObjectOrReply(c, c->argv[5], &timeout, NULL) != REDIS_OK)
		return;
	if (getLongFromObjectOrReply(c, c->argv[4], &dbid, NULL) != REDIS_OK)
		return;
	if (timeout <= 0) timeout = 1000;

	/* Check if the key is here. If not we reply with success as there is
	 * nothing to migrate (for instance the key expired in the meantime), but
	 * we include such information in the reply string. */
	if ((o = lookupKeyRead(c->db, c->argv[3])) == NULL) {
		addReplySds(c, sdsnew("+NOKEY\r\n"));
		return;
	}

	/* Connect */
	cs = migrateGetSocket(c, c->argv[1], c->argv[2], timeout);
	if (cs == NULL) return; /* error sent to the client by migrateGetSocket() */

	rioInitWithBuffer(&cmd, sdsempty());

	/* Send the SELECT command if the current DB is not already selected. */
	int select = cs->last_dbid != dbid; /* Should we emit SELECT? */
	if (select) {
		redisAssertWithInfo(c, NULL, rioWriteBulkCount(&cmd, '*', 2));
		redisAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "SELECT", 6));
		redisAssertWithInfo(c, NULL, rioWriteBulkLongLong(&cmd, dbid));
	}

	/* Create RESTORE payload and generate the protocol to call the command. */
	expireat = getExpire(c->db, c->argv[3]);
	if (expireat != -1) {
		ttl = expireat - mstime();
		if (ttl < 1) ttl = 1;
	}
	redisAssertWithInfo(c, NULL, rioWriteBulkCount(&cmd, '*', replace ? 5 : 4));
	if (server.cluster_enabled)
		redisAssertWithInfo(c, NULL,
			rioWriteBulkString(&cmd, "RESTORE-ASKING", 14));
	else
		redisAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "RESTORE", 7));
	redisAssertWithInfo(c, NULL, sdsEncodedObject(c->argv[3]));
	redisAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, c->argv[3]->ptr,
		sdslen(c->argv[3]->ptr)));
	redisAssertWithInfo(c, NULL, rioWriteBulkLongLong(&cmd, ttl));

	/* Emit the payload argument, that is the serialized object using
	 * the DUMP format. */
	createDumpPayload(&payload, o);
	redisAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, payload.io.buffer.ptr,
		sdslen(payload.io.buffer.ptr)));
	sdsfree(payload.io.buffer.ptr);

	/* Add the REPLACE option to the RESTORE command if it was specified
	 * as a MIGRATE option. */
	if (replace)
		redisAssertWithInfo(c, NULL, rioWriteBulkString(&cmd, "REPLACE", 7));

	/* Transfer the query to the other node in 64K chunks. */
	errno = 0;
	{
		sds buf = cmd.io.buffer.ptr;
		size_t pos = 0, towrite;
		int nwritten = 0;

#ifdef _WIN32
		while ((towrite = sdslen(buf) - pos) > 0) {
			towrite = (towrite > (64 * 1024) ? (64 * 1024) : towrite);
			while (nwritten != (signed)towrite) {
				nwritten = (int)syncWrite(cs->fd, buf + pos, (ssize_t)towrite, timeout);
				if (nwritten != (signed)towrite) {
					DWORD err = GetLastError();
					if (err == WSAEWOULDBLOCK) {
						// Likely send buffer is full. A short delay or two is sufficient to allow this to work.
						redisLog(REDIS_VERBOSE, "In migrate. WSAEWOULDBLOCK with synchronous socket: sleeping for 0.1s");
						Sleep(100);
					}
					else {
						redisLog(REDIS_WARNING, "SyncWrite failure toWrite=%d  written=%d err=%d timeout=%d ", towrite, nwritten, GetLastError(), timeout);
						goto socket_wr_err;
					}
				}
			}
			pos += nwritten;
			nwritten = 0;
		}
#else
		while ((towrite = sdslen(buf) - pos) > 0) {
			towrite = (towrite > (64 * 1024) ? (64 * 1024) : towrite);
			nwritten = syncWrite(cs->fd, buf + pos, towrite, timeout);
			if (nwritten != (signed)towrite) goto socket_wr_err;
			pos += nwritten;
		}
#endif
	}

	/* Read back the reply. */
	{
		char buf1[1024];
		char buf2[1024];

		/* Read the two replies */
		if (select && syncReadLine(cs->fd, buf1, sizeof(buf1), timeout) <= 0)
			goto socket_rd_err;
		if (syncReadLine(cs->fd, buf2, sizeof(buf2), timeout) <= 0)
			goto socket_rd_err;
		if ((select && buf1[0] == '-') || buf2[0] == '-') {
			/* On error assume that last_dbid is no longer valid. */
			cs->last_dbid = -1;
			addReplyErrorFormat(c, "Target instance replied with error: %s",
				(select && buf1[0] == '-') ? buf1 + 1 : buf2 + 1);
		}
		else {
			/* Update the last_dbid in migrateCachedSocket */
			cs->last_dbid = dbid;
			robj* aux;

			addReply(c, shared.ok);

			if (!copy) {
				/* No COPY option: remove the local key, signal the change. */
				dbDelete(c->db, c->argv[3]);
				signalModifiedKey(c->db, c->argv[3]);
				server.dirty++;

				/* Translate MIGRATE as DEL for replication/AOF. */
				aux = createStringObject("DEL", 3);
				rewriteClientCommandVector(c, 2, aux, c->argv[3]);
				decrRefCount(aux);
			}
		}
	}

	sdsfree(cmd.io.buffer.ptr);
	return;

socket_wr_err:
	sdsfree(cmd.io.buffer.ptr);
	migrateCloseSocket(c->argv[1], c->argv[2]);
	if (errno != ETIMEDOUT && retry_num++ == 0) goto try_again;
	addReplySds(c,
		sdsnew("-IOERR error or timeout writing to target instance\r\n"));
	return;

socket_rd_err:
#ifdef _WIN32
	redisLog(REDIS_WARNING, "syncReadLine failure err=%d timeout=%d ", GetLastError(), timeout);
#endif
	sdsfree(cmd.io.buffer.ptr);
	migrateCloseSocket(c->argv[1], c->argv[2]);
	if (errno != ETIMEDOUT && retry_num++ == 0) goto try_again;
	addReplySds(c,
		sdsnew("-IOERR error or timeout reading from target node\r\n"));
	return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

 /* The ASKING command is required after a -ASK redirection.
  * The client should issue ASKING before to actually send the command to
  * the target instance. See the Redis Cluster specification for more
  * information. */
void askingCommand(redisClient * c) {
	if (server.cluster_enabled == 0) {
		addReplyError(c, "This instance has cluster support disabled");
		return;
	}
	c->flags |= REDIS_ASKING;
	addReply(c, shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
void readonlyCommand(redisClient * c) {
	if (server.cluster_enabled == 0) {
		addReplyError(c, "This instance has cluster support disabled");
		return;
	}
	c->flags |= REDIS_READONLY;
	addReply(c, shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
void readwriteCommand(redisClient * c) {
	c->flags &= ~REDIS_READONLY;
	addReply(c, shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be perfomed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to REDIS_CLUSTER_REDIR_ASK or
 * REDIS_CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to REDIS_CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * REDIS_CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * REDIS_CLUSTER_REDIR_UNSTABLE if the request contains mutliple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * REDIS_CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here. */
clusterNode* getNodeByQuery(redisClient * c, struct redisCommand* cmd, robj * *argv, int argc, int* hashslot, int* error_code) {
	clusterNode* n = NULL;
	robj* firstkey = NULL;
	int multiple_keys = 0;
	multiState* ms, _ms;
	multiCmd mc;
	int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

	/* Set error code optimistically for the base case. */
	if (error_code) *error_code = REDIS_CLUSTER_REDIR_NONE;

	/* We handle all the cases as if they were EXEC commands, so we have
	 * a common code path for everything */
	if (cmd->proc == execCommand) {
		/* If REDIS_MULTI flag is not set EXEC is just going to return an
		 * error. */
		if (!(c->flags & REDIS_MULTI)) return myself;
		ms = &c->mstate;
	}
	else {
		/* In order to have a single codepath create a fake Multi State
		 * structure if the client is not in MULTI/EXEC state, this way
		 * we have a single codepath below. */
		ms = &_ms;
		_ms.commands = &mc;
		_ms.count = 1;
		mc.argv = argv;
		mc.argc = argc;
		mc.cmd = cmd;
	}

	/* Check that all the keys are in the same hash slot, and obtain this
	 * slot and the node associated. */
	for (i = 0; i < ms->count; i++) {
		struct redisCommand* mcmd;
		robj** margv;
		int margc, * keyindex, numkeys, j;

		mcmd = ms->commands[i].cmd;
		margc = ms->commands[i].argc;
		margv = ms->commands[i].argv;

		keyindex = getKeysFromCommand(mcmd, margv, margc, &numkeys);
		for (j = 0; j < numkeys; j++) {
			robj* thiskey = margv[keyindex[j]];
			int thisslot = keyHashSlot((char*)thiskey->ptr,
				(int)sdslen(thiskey->ptr));              WIN_PORT_FIX /* cast (int) */

				if (firstkey == NULL) {
					/* This is the first key we see. Check what is the slot
					 * and node. */
					firstkey = thiskey;
					slot = thisslot;
					n = server.cluster->slots[slot];

					/* Error: If a slot is not served, we are in "cluster down"
					 * state. However the state is yet to be updated, so this was
					 * not trapped earlier in processCommand(). Report the same
					 * error to the client. */
					if (n == NULL) {
						getKeysFreeResult(keyindex);
						if (error_code)
							*error_code = REDIS_CLUSTER_REDIR_DOWN_UNBOUND;
						return NULL;
					}

					/* If we are migrating or importing this slot, we need to check
					 * if we have all the keys in the request (the only way we
					 * can safely serve the request, otherwise we return a TRYAGAIN
					 * error). To do so we set the importing/migrating state and
					 * increment a counter for every missing key. */
					if (n == myself &&
						server.cluster->migrating_slots_to[slot] != NULL)
					{
						migrating_slot = 1;
					}
					else if (server.cluster->importing_slots_from[slot] != NULL) {
						importing_slot = 1;
					}
				}
				else {
					/* If it is not the first key, make sure it is exactly
					 * the same key as the first we saw. */
					if (!equalStringObjects(firstkey, thiskey)) {
						if (slot != thisslot) {
							/* Error: multiple keys from different slots. */
							getKeysFreeResult(keyindex);
							if (error_code)
								*error_code = REDIS_CLUSTER_REDIR_CROSS_SLOT;
							return NULL;
						}
						else {
							/* Flag this request as one with multiple different
							 * keys. */
							multiple_keys = 1;
						}
					}
				}

			/* Migarting / Improrting slot? Count keys we don't have. */
			if ((migrating_slot || importing_slot) &&
				lookupKeyRead(&server.db[0], thiskey) == NULL)
			{
				missing_keys++;
			}
		}
		getKeysFreeResult(keyindex);
	}

	/* No key at all in command? then we can serve the request
	 * without redirections or errors. */
	if (n == NULL) return myself;

	/* Return the hashslot by reference. */
	if (hashslot) *hashslot = slot;

	/* This request is about a slot we are migrating into another instance?
	 * Then if we have all the keys. */

	 /* If we don't have all the keys and we are migrating the slot, send
	  * an ASK redirection. */
	if (migrating_slot && missing_keys) {
		if (error_code) *error_code = REDIS_CLUSTER_REDIR_ASK;
		return server.cluster->migrating_slots_to[slot];
	}

	/* If we are receiving the slot, and the client correctly flagged the
	 * request as "ASKING", we can serve the request. However if the request
	 * involves multiple keys and we don't have them all, the only option is
	 * to send a TRYAGAIN error. */
	if (importing_slot &&
		(c->flags & REDIS_ASKING || cmd->flags & REDIS_CMD_ASKING))
	{
		if (multiple_keys && missing_keys) {
			if (error_code) *error_code = REDIS_CLUSTER_REDIR_UNSTABLE;
			return NULL;
		}
		else {
			return myself;
		}
	}

	/* Handle the read-only client case reading from a slave: if this
	 * node is a slave and the request is about an hash slot our master
	 * is serving, we can reply without redirection. */
	if (c->flags & REDIS_READONLY &&
		cmd->flags & REDIS_CMD_READONLY &&
		nodeIsSlave(myself) &&
		myself->slaveof == n)
	{
		return myself;
	}

	/* Base case: just return the right node. However if this node is not
	 * myself, set error_code to MOVED since we need to issue a rediretion. */
	if (n != myself && error_code) *error_code = REDIS_CLUSTER_REDIR_MOVED;
	return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of REDIS_CLUSTER_REDIR_* macros.
 *
 * If REDIS_CLUSTER_REDIR_ASK or REDIS_CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
void clusterRedirectClient(redisClient * c, clusterNode * n, int hashslot, int error_code) {
	if (error_code == REDIS_CLUSTER_REDIR_CROSS_SLOT) {
		addReplySds(c, sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
	}
	else if (error_code == REDIS_CLUSTER_REDIR_UNSTABLE) {
		/* The request spawns mutliple keys in the same slot,
		 * but the slot is not "stable" currently as there is
		 * a migration or import in progress. */
		addReplySds(c, sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
	}
	else if (error_code == REDIS_CLUSTER_REDIR_DOWN_STATE) {
		addReplySds(c, sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
	}
	else if (error_code == REDIS_CLUSTER_REDIR_DOWN_UNBOUND) {
		addReplySds(c, sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
	}
	else if (error_code == REDIS_CLUSTER_REDIR_MOVED ||
		error_code == REDIS_CLUSTER_REDIR_ASK)
	{
		addReplySds(c, sdscatprintf(sdsempty(),
			"-%s %d %s:%d\r\n",
			(error_code == REDIS_CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
			hashslot, n->ip, n->port));
	}
	else {
		redisPanic("getNodeByQuery() unknown error.");
	}
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into an hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
int clusterRedirectBlockedClientIfNeeded(redisClient * c) {
	if (c->flags & REDIS_BLOCKED && c->btype == REDIS_BLOCKED_LIST) {
		dictEntry* de;
		dictIterator* di;

		/* If the cluster is down, unblock the client with the right error. */
		if (server.cluster->state == REDIS_CLUSTER_FAIL) {
			clusterRedirectClient(c, NULL, 0, REDIS_CLUSTER_REDIR_DOWN_STATE);
			return 1;
		}

		di = dictGetIterator(c->bpop.keys);
		while ((de = dictNext(di)) != NULL) {
			robj* key = dictGetKey(de);
			int slot = keyHashSlot((char*)key->ptr, (int)sdslen(key->ptr));     WIN_PORT_FIX /* cast (int) */
				clusterNode* node = server.cluster->slots[slot];

			/* We send an error and unblock the client if:
			 * 1) The slot is unassigned, emitting a cluster down error.
			 * 2) The slot is not handled by this node, nor being imported. */
			if (node != myself &&
				server.cluster->importing_slots_from[slot] == NULL)
			{
				if (node == NULL) {
					clusterRedirectClient(c, NULL, 0,
						REDIS_CLUSTER_REDIR_DOWN_UNBOUND);
				}
				else {
					clusterRedirectClient(c, node, slot,
						REDIS_CLUSTER_REDIR_MOVED);
				}
				return 1;
			}
		}
		dictReleaseIterator(di);
	}
	return 0;
}
