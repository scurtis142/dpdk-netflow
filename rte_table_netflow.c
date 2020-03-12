/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2014, Choonho Son choonho.som@gmail.com
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <string.h>
#include <stdio.h>
#include <sys/time.h>

#include <rte_common.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_log.h>
#include <rte_byteorder.h>
#include <rte_hash_crc.h>

#include "rte_table_netflow.h"

void *
rte_table_netflow_create(void *params, int socket_id, uint32_t entry_size)
{
    struct rte_table_netflow_params *p = 
        (struct rte_table_netflow_params *) params;

    struct rte_table_netflow *t;
    uint32_t total_cl_size, total_size;
    uint32_t i;

    if (p->n_entries > MAX_ENTRY) {
        RTE_LOG(ERR, TABLE, "Entry is large than MAX_ENTRY(%d)\n", (uint32_t)MAX_ENTRY);
        p->n_entries = MAX_ENTRY;
    }

    /* Check input parameters */
    if ((p == NULL) ||
        (p->n_entries == 0) ||
        (!rte_is_power_of_2(p->n_entries)) ) {
        return NULL;
    }

    /* Memory allocation */
    total_cl_size = (sizeof(struct rte_table_netflow) +
            RTE_CACHE_LINE_SIZE) / RTE_CACHE_LINE_SIZE;
    total_cl_size += (p->n_entries * sizeof(hashBucket_t*) + 
            RTE_CACHE_LINE_SIZE) / RTE_CACHE_LINE_SIZE;
    total_size = total_cl_size * RTE_CACHE_LINE_SIZE;
    t = rte_zmalloc_socket("TABLE", total_size, RTE_CACHE_LINE_SIZE, socket_id);
    if (t == NULL) {
        RTE_LOG(ERR, TABLE,
            "%s: Cannot allocate %u bytes for netflow table\n",
            __func__, total_size);
        return NULL;
    }

    /* Spinlock initialization */
    for (i = 0; i < p->n_entries; i++) {
        rte_spinlock_init(&t->lock[i]);
    }

    /* Memory initialzation */
    t->entry_size = entry_size;
    t->n_entries = p->n_entries;
    t->f_hash = p->f_hash;
    t->seed = p->seed;

    return t;
}

int
rte_table_netflow_entry_add(
    void *table,
    void *key,
    void *entry)
{
    struct rte_table_netflow *t = (struct rte_table_netflow *)table;
    union rte_table_netflow_key *k = key;
    struct ipv4_hdr *ip = entry;
    struct tcp_hdr *tcp;
    hashBucket_t *previous_pointer = NULL;
    hashBucket_t *bucket = NULL;
    hashBucket_t *bkt = NULL;
    uint32_t idx = 0;
    uint8_t updated = 0; 
    uint8_t notfound = 0; 
    struct timeval curr;

#if DEBUG
  	printf ("src_ip = %d\n", k->ip_src);
	printf ("dst_ip = %d\n", k->ip_dst);
	printf ("proto = %d\n", k->proto);
	printf ("src_port = %d\n", k->port_src);
	printf ("dst_port = %d\n", k->port_dst);
#endif
    /* hashing with SSE4_2 CRC32 */ 
    idx = rte_hash_crc_4byte(k->proto, idx);
    idx = rte_hash_crc_4byte(k->ip_src, idx);
    idx = rte_hash_crc_4byte(k->ip_dst, idx);
    idx = rte_hash_crc_4byte(k->port_src, idx);
    idx = rte_hash_crc_4byte(k->port_dst, idx);
    idx = idx % t->n_entries;
    
    /****************************************************************
     * Lock one entry (t->array[idx]'s lock = t->lock[idex]
     *
     * So netflow_export can use other entries 
     ****************************************************************/
    rte_spinlock_lock(&t->lock[idx]);
 
    bucket = t->array[idx];
    previous_pointer = bucket;
    
    while (bucket != NULL) {
        /* Find same flow in the bucket's list */
        if ((bucket->ip_src == k->ip_src) && (bucket->ip_dst == k->ip_dst) ) {
            /* accumulated ToS Field */
            bucket->src2dstTos |= ip->type_of_service;

            /* accumulated TCP Flags */
            if (k->proto == IPPROTO_TCP) {
                tcp = (struct tcp_hdr *)((unsigned char*)ip + sizeof(struct ipv4_hdr));
                bucket->src2dstTcpFlags |= tcp->tcp_flags;
            }

            /* accumulated Bytes */
            /* TODO: if bytesSent > 2^32, netflow v5 value is wrong
             *  since, netflow v5 dOctet is 32bit.
             */
            bucket->bytesSent += rte_cpu_to_be_16(ip->total_length);
            bucket->pktSent++;

            /* Time */
            gettimeofday(&curr, NULL);
            bucket->lastSeenSent = curr;

            updated = 1;
            break;
        }
        printf("Bucket collision\n");
        notfound = 1;
        previous_pointer = bucket;
        bucket = bucket->next;
    }

    if( !updated ) {
        /* Create New Bucket */
        //printf("First Seen : %" PRIu32 "\n", idx);
        bkt = (hashBucket_t *)rte_zmalloc("BUCKET", sizeof(hashBucket_t), RTE_CACHE_LINE_SIZE);
        bkt->magic = 1;
        bkt->vlanId     = k->vlanId;
        bkt->proto      = k->proto;
        bkt->ip_src     = k->ip_src;
        bkt->ip_dst     = k->ip_dst;
        bkt->port_src   = k->port_src;
        bkt->port_dst   = k->port_dst;
    
        /* ToS Field */
        bkt->src2dstTos = ip->type_of_service; 
        
        /* TCP Flags */
        if (k->proto == IPPROTO_TCP) {
            tcp = (struct tcp_hdr *)((unsigned char*)ip + sizeof(struct ipv4_hdr));
            bkt->src2dstTcpFlags = tcp->tcp_flags;

            /* TODO: If TCP flags is start of Flow (Syn) 
             * Save payload of DPI 
             */

            /* If Flags is FIN, check and of flow */
        }

        /* Bytes (Total number of Layer 3 bytes)  */
        bkt->bytesSent = rte_cpu_to_be_16(ip->total_length);
        bkt->pktSent++;

        /* Time */
        gettimeofday(&curr, NULL);
        bkt->firstSeenSent = bkt->lastSeenSent = curr; 
        
        /* Update contents of bucket */
        if (notfound) previous_pointer->next = bkt;
        else t->array[idx] = bkt;
    }
    
    rte_spinlock_unlock(&t->lock[idx]);
    /***********************************************************************
     * End of entry lock
     * release lock
     **********************************************************************/
    return 1;
}

int
rte_table_netflow_free(void *table)
{
    struct rte_table_netflow *t = (struct rte_table_netflow *)table;
    
    /* Check input paramters */
    if (t == NULL) {
        RTE_LOG(ERR, TABLE, "%s: table parameter is NULL\n", __func__);
        return -EINVAL;
    }

    /* Free previously allocated resources */
    rte_free(t);
    return 0;
}

#if 0
struct rte_table_ops rte_table_netflow_ops = {
    .f_create = rte_table_netflow_create,
    .f_free   = rte_table_netflow_free,
    .f_add    = rte_table_netflow_entry_add,
    .f_delete = NULL,
    .f_lookup = NULL, /* rte_table_netflow_lookup, */
};
#endif

int
rte_table_print(void *table)
{
	struct rte_table_netflow *t = (struct rte_table_netflow *)table;
	hashBucket_t *bkt;

	printf ("\nprinting flow table\n");
	printf ("t->n_entries = %d\n", t->n_entries);
	
	for (unsigned int i = 0; i < t->n_entries; i++) {
		rte_spinlock_lock(&t->lock[i]);
		bkt = t->array[i];
		if (bkt != NULL) {
			printf ("src_ip = %d\ndst_ip = %d\nsrc_port = %d\ndst_port = %d\nproto = %d\n",
					bkt->ip_src, bkt->ip_dst, bkt->port_src, bkt->port_dst, bkt->proto);
			printf ("bytes_sent = %ld\nbytes_recv = %ld\npackets_sent = %ld\npackets_recv = %ld\n\n",
					bkt->bytesSent, bkt->bytesRcvd, bkt->pktSent, bkt->pktRcvd);
		}
        rte_spinlock_unlock(&t->lock[i]);
	}

	return 0;
}


int
rte_table_print_stats(void *table)
{
	struct rte_table_netflow *t = (struct rte_table_netflow *)table;
	hashBucket_t *bkt;

   uint64_t total_bytes = 0;
   uint64_t total_pkts  = 0;

   printf ("\nprinting flow table statistics\n");
   printf ("t->n_entries = %d\n", t->n_entries);

   for (unsigned int i = 0; i < t->n_entries; i++) {
      rte_spinlock_lock(&t->lock[i]);
		bkt = t->array[i];
		if (bkt != NULL) {
         total_bytes += bkt->bytesSent;
         total_pkts  += bkt->pktSent;
		}
      rte_spinlock_unlock(&t->lock[i]);
	}

   printf ("total bytes = %lu\n", total_bytes);
   printf ("total pkts  = %lu\n", total_pkts);


	return 0;
}
