#define LOG_CLASS "SCTP"
#include "kvs_instrumentation.h"
#include "../Include_i.h"
#include "esp_log.h"

STATUS initSctpAddrConn(PSctpSession pSctpSession, struct sockaddr_conn* sconn)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    sconn->sconn_family = AF_CONN;
    putInt16((PINT16) &sconn->sconn_port, SCTP_ASSOCIATION_DEFAULT_PORT);
    sconn->sconn_addr = pSctpSession;

    LEAVES();
    return retStatus;
}

STATUS configureSctpSocket(struct socket* socket)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    struct linger linger_opt;
    struct sctp_event event;
    UINT32 i;
    UINT32 valueOn = 1;
    UINT16 event_types[] = {SCTP_ASSOC_CHANGE,   SCTP_PEER_ADDR_CHANGE,      SCTP_REMOTE_ERROR,
                            SCTP_SHUTDOWN_EVENT, SCTP_ADAPTATION_INDICATION, SCTP_PARTIAL_DELIVERY_EVENT};

    CHK(usrsctp_set_non_blocking(socket, 1) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);

    // onSctpOutboundPacket must not be called after close
    linger_opt.l_onoff = 1;
    linger_opt.l_linger = 0;
    CHK(usrsctp_setsockopt(socket, SOL_SOCKET, SO_LINGER, &linger_opt, SIZEOF(linger_opt)) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);

    // packets are generally sent as soon as possible and no unnecessary
    // delays are introduced, at the cost of more packets in the network.
    CHK(usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_NODELAY, &valueOn, SIZEOF(valueOn)) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);

    /* Set the path MTU on the endpoint, before there is an association.
     *
     * There is a second SCTP_PEER_ADDR_PARAMS call further down, after
     * usrsctp_connect. That one cannot work: an AF_CONN destination starts at
     * an MTU of 1280, the association's smallest_mtu is initialized from it,
     * and the post-connect path only ever lowers smallest_mtu
     * (sctp_usrreq.c, "if (net->mtu < stcb->asoc.smallest_mtu)"). Raising it
     * to SCTP_MTU there is silently a no-op.
     *
     * With no association the same option lands on inp->sctp_ep.default_mtu,
     * which sctp_init_asoc copies into asoc->default_mtu, which is what the
     * association's MTU is built from. Fragmentation then happens at
     * SCTP_MTU - 28 rather than 1280 - 28, so every full packet carries 80
     * more bytes -- worth about 6% of the wire on a link where the packet
     * rate, not the byte rate, is what saturates. */
    {
        struct sctp_paddrparams mtuParams;
        MEMSET(&mtuParams, 0x00, SIZEOF(mtuParams));
        mtuParams.spp_assoc_id = SCTP_FUTURE_ASSOC;
        mtuParams.spp_flags = SPP_PMTUD_DISABLE;
        mtuParams.spp_pathmtu = SCTP_MTU;
        CHK(usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &mtuParams, SIZEOF(mtuParams)) == 0,
            STATUS_SCTP_SESSION_SETUP_FAILED);
    }

    /* usrsctp defaults this to SB_MAX, 256 KB, which is a desktop number. At
     * the rate this link carries that is most of a second of data buffered
     * ahead of the wire, and for live video buffered means stale.
     *
     * 64 KB is about a fifth of a second. Two floors it has to clear: a
     * message larger than the buffer is refused outright with EMSGSIZE rather
     * than queued (sctp_output.c, "It will NEVER fit"), and the buffer must
     * exceed the bandwidth-delay product or it caps throughput instead of just
     * latency -- roughly 33 KB on a 200 ms path at this rate. 64 KB clears
     * both with room. */
    {
        INT32 sndBuf = SCTP_SESSION_SNDBUF_BYTES;
        if (usrsctp_setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &sndBuf, SIZEOF(sndBuf)) != 0) {
            DLOGW("could not set SO_SNDBUF");
        }
    }

    MEMSET(&event, 0, SIZEOF(event));
    event.se_assoc_id = SCTP_FUTURE_ASSOC;
    event.se_on = 1;
    for (i = 0; i < (UINT32) (SIZEOF(event_types) / SIZEOF(UINT16)); i++) {
        event.se_type = event_types[i];
        CHK(usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_EVENT, &event, SIZEOF(struct sctp_event)) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);
    }

    struct sctp_initmsg initmsg;
    MEMSET(&initmsg, 0, SIZEOF(struct sctp_initmsg));
    initmsg.sinit_num_ostreams = 300;
    initmsg.sinit_max_instreams = 300;
    CHK(usrsctp_setsockopt(socket, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, SIZEOF(struct sctp_initmsg)) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);

CleanUp:
    LEAVES();
    return retStatus;
}

/* usrsctp_init_nothreads suppresses usrsctp's own timer thread, and in that
 * mode the application has to drive usrsctp_handle_timers itself. Nothing
 * did, so the association ran with no timers at all: no T3-rtx, no delayed
 * SACK, no heartbeats.
 *
 * With no retransmission a lost packet is lost for good. A message that
 * fits in one SCTP packet survives that -- it either arrives or the peer
 * asks again -- which is why small messages looked fine. A message large
 * enough to fragment needs every fragment, so a single loss stalls that
 * stream permanently while every send still reports success and the
 * association still looks healthy.
 *
 * SCTP_TIMER_INTERVAL_MS is the granularity usrsctp's timers are quantised
 * to; 10ms matches what its own timer thread uses. */
static volatile BOOL gSctpTimerRunning = FALSE;
static TID gSctpTimerTid = INVALID_TID_VALUE;

static PVOID sctpTimerRoutine(PVOID arg)
{
    UNUSED_PARAM(arg);
    while (gSctpTimerRunning) {
        THREAD_SLEEP(SCTP_TIMER_INTERVAL_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        usrsctp_handle_timers(SCTP_TIMER_INTERVAL_MS);
    }
    return NULL;
}

STATUS initSctpSession()
{
    STATUS retStatus = STATUS_SUCCESS;

    usrsctp_init_nothreads(0, (int (*)(void *, void *, size_t, uint8_t, uint8_t))(void*)onSctpOutboundPacket, NULL);

    // Disable Explicit Congestion Notification
    usrsctp_sysctl_set_sctp_ecn_enable(0);

    gSctpTimerRunning = TRUE;
    if (STATUS_FAILED(THREAD_CREATE_EX_EXT(&gSctpTimerTid, "sctpTimer", SCTP_TIMER_THREAD_STACK_SIZE, TRUE, sctpTimerRoutine, NULL))) {
        DLOGE("failed to start the sctp timer thread; retransmission will not happen");
        gSctpTimerRunning = FALSE;
        gSctpTimerTid = INVALID_TID_VALUE;
    }

    return retStatus;
}

VOID deinitSctpSession()
{
    /* Stop driving timers before usrsctp_finish, or the timer thread walks
     * structures it is freeing. */
    if (gSctpTimerRunning) {
        gSctpTimerRunning = FALSE;
        if (IS_VALID_TID_VALUE(gSctpTimerTid)) {
            THREAD_JOIN(gSctpTimerTid, NULL);
            gSctpTimerTid = INVALID_TID_VALUE;
        }
    }

    // need to block until usrsctp_finish or sctp thread could be calling free objects and cause segfault
    while (usrsctp_finish() != 0) {
        THREAD_SLEEP(DEFAULT_USRSCTP_TEARDOWN_POLLING_INTERVAL);
    }
}

STATUS createSctpSession(PSctpSessionCallbacks pSctpSessionCallbacks, PSctpSession* ppSctpSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSctpSession pSctpSession = NULL;
    struct sockaddr_conn localConn, remoteConn;
    struct sctp_paddrparams params;
    INT32 connectStatus = 0;

    CHK(ppSctpSession != NULL && pSctpSessionCallbacks != NULL, STATUS_NULL_ARG);

    pSctpSession = (PSctpSession) MEMCALLOC(1, SIZEOF(SctpSession));
    CHK(pSctpSession != NULL, STATUS_NOT_ENOUGH_MEMORY);

    MEMSET(&params, 0x00, SIZEOF(struct sctp_paddrparams));
    MEMSET(&localConn, 0x00, SIZEOF(struct sockaddr_conn));
    MEMSET(&remoteConn, 0x00, SIZEOF(struct sockaddr_conn));

    ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_ACTIVE);
    pSctpSession->sctpSessionCallbacks = *pSctpSessionCallbacks;

    CHK_STATUS(initSctpAddrConn(pSctpSession, &localConn));
    CHK_STATUS(initSctpAddrConn(pSctpSession, &remoteConn));

    CHK((pSctpSession->socket = usrsctp_socket(AF_CONN, SOCK_STREAM, IPPROTO_SCTP, (int (*)(struct socket *, union sctp_sockstore, void *, size_t, struct sctp_rcvinfo, int, void *))(void*)onSctpInboundPacket, NULL, 0, pSctpSession)) != NULL,
        STATUS_SCTP_SESSION_SETUP_FAILED);
    usrsctp_register_address(pSctpSession);
    CHK_STATUS(configureSctpSocket(pSctpSession->socket));

    CHK(usrsctp_bind(pSctpSession->socket, (struct sockaddr*) &localConn, SIZEOF(localConn)) == 0, STATUS_SCTP_SESSION_SETUP_FAILED);

    connectStatus = usrsctp_connect(pSctpSession->socket, (struct sockaddr*) &remoteConn, SIZEOF(remoteConn));
    CHK(connectStatus >= 0 || errno == EINPROGRESS, STATUS_SCTP_SESSION_SETUP_FAILED);

    memcpy(&params.spp_address, &remoteConn, SIZEOF(remoteConn));
    params.spp_flags = SPP_PMTUD_DISABLE;
    params.spp_pathmtu = SCTP_MTU;
    CHK(usrsctp_setsockopt(pSctpSession->socket, IPPROTO_SCTP, SCTP_PEER_ADDR_PARAMS, &params, SIZEOF(params)) == 0,
        STATUS_SCTP_SESSION_SETUP_FAILED);

CleanUp:
    if (STATUS_FAILED(retStatus)) {
        freeSctpSession(&pSctpSession);
    }

    *ppSctpSession = pSctpSession;

    LEAVES();
    return retStatus;
}

STATUS freeSctpSession(PSctpSession* ppSctpSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PSctpSession pSctpSession;
    UINT64 shutdownTimeout;

    CHK(ppSctpSession != NULL, STATUS_NULL_ARG);

    pSctpSession = *ppSctpSession;

    CHK(pSctpSession != NULL, retStatus);

    usrsctp_deregister_address(pSctpSession);
    /* handle issue mentioned here: https://github.com/sctplab/usrsctp/issues/147
     * the change in shutdownStatus will trigger onSctpOutboundPacket to return -1 */
    ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_SHUTDOWN_INITIATED);

    if (pSctpSession->socket != NULL) {
        usrsctp_set_ulpinfo(pSctpSession->socket, NULL);
        usrsctp_shutdown(pSctpSession->socket, SHUT_RDWR);
        usrsctp_close(pSctpSession->socket);
    }

    shutdownTimeout = GETTIME() + DEFAULT_SCTP_SHUTDOWN_TIMEOUT;
    while (ATOMIC_LOAD(&pSctpSession->shutdownStatus) != SCTP_SESSION_SHUTDOWN_COMPLETED && GETTIME() < shutdownTimeout) {
        THREAD_SLEEP(DEFAULT_USRSCTP_TEARDOWN_POLLING_INTERVAL);
    }

    SAFE_MEMFREE(*ppSctpSession);

    *ppSctpSession = NULL;

CleanUp:

    LEAVES();
    return retStatus;
}

/* What the association looks like from here, for a caller trying to work
 * out why a send is failing.
 *
 *   rwnd == 0        the peer is not draining -- its receive window is
 *                    shut, so this is the receiver applying backpressure
 *                    and not a broken link.
 *   rwnd > 0 but
 *   unacked high     data is going out and not being acknowledged, which
 *                    is the network or ICE.
 *
 * Without this the two are indistinguishable: both surface as sends that
 * fail after the buffer-wait timeout, with nothing to say which.
 */
STATUS sctpSessionGetStats(PSctpSession pSctpSession, PUINT32 pRwnd, PUINT32 pUnacked)
{
    STATUS retStatus = STATUS_SUCCESS;
    struct sctp_status status;
    socklen_t len = SIZEOF(status);

    CHK(pSctpSession != NULL && pRwnd != NULL && pUnacked != NULL, STATUS_NULL_ARG);
    MEMSET(&status, 0, SIZEOF(status));
    CHK(usrsctp_getsockopt(pSctpSession->socket, IPPROTO_SCTP, SCTP_STATUS, &status, &len) == 0,
        STATUS_INTERNAL_ERROR);

    *pRwnd = status.sstat_rwnd;
    *pUnacked = status.sstat_unackdata;

CleanUp:
    return retStatus;
}

/* Resize the send buffer on a live association.
 *
 * This is the throttle, and its right value depends on message size and on the
 * link -- so being able to sweep it in one run beats a rebuild per value. */
STATUS sctpSessionSetSendBuffer(PSctpSession pSctpSession, INT32 bytes)
{
    STATUS retStatus = STATUS_SUCCESS;
    CHK(pSctpSession != NULL && bytes > 0, STATUS_NULL_ARG);
    CHK(usrsctp_setsockopt(pSctpSession->socket, SOL_SOCKET, SO_SNDBUF, &bytes, SIZEOF(bytes)) == 0,
        STATUS_INTERNAL_ERROR);
CleanUp:
    return retStatus;
}

STATUS sctpSessionWriteMessage(PSctpSession pSctpSession, UINT32 streamId, BOOL isBinary, PBYTE pMessage, UINT32 pMessageLen,
                               PRtcDataChannelInit pRtcDataChannelInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    /* Local, not session state. It is filled here and consumed by
     * usrsctp_sendv a few lines down, so it never outlives the call --
     * while sharing it on the session let two senders overwrite each
     * other's stream id and payload type. Roughly forty bytes. */
    struct sctp_sendv_spa spa;

    CHK(pSctpSession != NULL && pMessage != NULL, STATUS_NULL_ARG);

    MEMSET(&spa, 0x00, SIZEOF(spa));

    spa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = streamId;

    /* Ordering and reliability come from the channel being written, not from
     * pSctpSession->packet. That buffer holds the last DCEP OPEN this session
     * built, so reading it here gave every channel the flags of whichever one
     * was opened most recently -- correct only while there is exactly one. */
    if (pRtcDataChannelInit != NULL) {
        if (!pRtcDataChannelInit->ordered) {
            spa.sendv_sndinfo.snd_flags |= SCTP_UNORDERED;
        }
        if (pRtcDataChannelInit->maxRetransmits.isNull == FALSE) {
            spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
            spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_RTX;
            spa.sendv_prinfo.pr_value = pRtcDataChannelInit->maxRetransmits.value;
        } else if (pRtcDataChannelInit->maxPacketLifeTime.isNull == FALSE) {
            spa.sendv_flags |= SCTP_SEND_PRINFO_VALID;
            spa.sendv_prinfo.pr_policy = SCTP_PR_SCTP_TTL;
            spa.sendv_prinfo.pr_value = pRtcDataChannelInit->maxPacketLifeTime.value;
        }
    }

    putInt32((PINT32) &spa.sendv_sndinfo.snd_ppid, isBinary ? SCTP_PPID_BINARY : SCTP_PPID_STRING);

    /* Report each channel's delivery settings the first time it is written,
     * so two channels opened with different reliability can be seen actually
     * sending differently. One line per data channel, not per application
     * stream multiplexed inside it. Cheap: a bitmask over the low ids. */
    {
        static UINT32 loggedStreams;
        if (streamId < 32 && (loggedStreams & (1u << streamId)) == 0) {
            loggedStreams |= (1u << streamId);
            KVS_INSTR_LOGW("sctp", "channel on sctp stream %u: %s%s", (unsigned) streamId,
                     (spa.sendv_sndinfo.snd_flags & SCTP_UNORDERED) ? "unordered" : "ordered",
                     (spa.sendv_flags & SCTP_SEND_PRINFO_VALID)
                         ? ((spa.sendv_prinfo.pr_policy == SCTP_PR_SCTP_TTL) ? ", lifetime-limited" : ", retransmit-limited")
                         : ", reliable");
        }
    }

    /* What partial reliability actually abandoned, sampled while traffic is
     * flowing. Read once at connection time this is always zero, which says
     * nothing -- the queue has not formed yet. */
#if KVS_INSTR
    {
        static int64_t prWindow;
        int64_t prNow = GETTIME() / HUNDREDS_OF_NANOS_IN_A_MILLISECOND;
        if (prWindow == 0) {
            prWindow = prNow;
        } else if (prNow - prWindow > 5000) {
            struct sctp_prstatus pr;
            socklen_t prlen = SIZEOF(pr);
            MEMSET(&pr, 0, SIZEOF(pr));
            pr.sprstat_sid = (UINT16) streamId;
            pr.sprstat_policy = SCTP_PR_SCTP_TTL;
            if (usrsctp_getsockopt(pSctpSession->socket, IPPROTO_SCTP, SCTP_PR_ASSOC_STATUS, &pr, &prlen) == 0) {
                KVS_INSTR_LOGW("sctp", "stream %u pr-sctp abandoned: %llu unsent, %llu sent",
                         (unsigned) streamId,
                         (unsigned long long) pr.sprstat_abandoned_unsent,
                         (unsigned long long) pr.sprstat_abandoned_sent);
            } else {
                KVS_INSTR_LOGW("sctp", "stream %u pr-sctp status unavailable (errno %d)",
                         (unsigned) streamId, errno);
            }

            /* Whether the association is still making forward progress, which
             * the browser's own counters cannot settle. unackdata climbing
             * while the wire stays busy means we are retransmitting into a
             * peer that stopped acknowledging; a peer rwnd of zero means it
             * is receiving but not draining. */
            {
                struct sctp_status st;
                socklen_t stlen = SIZEOF(st);
                MEMSET(&st, 0, SIZEOF(st));
                if (usrsctp_getsockopt(pSctpSession->socket, IPPROTO_SCTP, SCTP_STATUS, &st, &stlen) == 0) {
                    /* spinfo_mtu is the proof that the MTU option applied.
                     * 1280 means it did not and messages fragment at 1252. */
                    KVS_INSTR_LOGW("sctp", "assoc: state %d, rwnd %u, unacked %u chunks, pending %u, cwnd %u, srtt %u ms, mtu %u",
                             (int) st.sstat_state, (unsigned) st.sstat_rwnd,
                             (unsigned) st.sstat_unackdata, (unsigned) st.sstat_penddata,
                             (unsigned) st.sstat_primary.spinfo_cwnd,
                             (unsigned) st.sstat_primary.spinfo_srtt,
                             (unsigned) st.sstat_primary.spinfo_mtu);
                } else {
                    KVS_INSTR_LOGW("sctp", "assoc status unavailable (errno %d)", errno);
                }
            }
            prWindow = prNow;
        }
    }
#endif

    /* The socket is non-blocking, so a full send buffer comes back as
     * EWOULDBLOCK rather than as a wait. Returning an error there gives the
     * caller no way to tell "try again in a moment" from "this failed", and
     * a caller streaming a large response has no other backpressure signal
     * at all -- so it keeps writing, the buffer never drains, and delivery
     * stops with every send still reporting success.
     *
     * Wait for space instead, bounded so a dead association cannot park the
     * caller forever. This is the equivalent of gating on bufferedAmount,
     * which is what the browser-side APIs expose and this one does not. */
    {
        INT32 sent;
        UINT32 waitedMs = 0;
        for (;;) {
            sent = usrsctp_sendv(pSctpSession->socket, pMessage, pMessageLen, NULL, 0, &spa,
                                 SIZEOF(spa), SCTP_SENDV_SPA, 0);
            if (sent > 0) {
                break;
            }
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                break;
            }
            if (waitedMs >= SCTP_SEND_BUFFER_MAX_WAIT_MS) {
                UINT32 rwnd = 0, unacked = 0;
                sctpSessionGetStats(pSctpSession, &rwnd, &unacked);
                /* ESP_LOGW, not DLOGW: the KVS logger's level is set from
                 * app_webrtc's config and swallows this exactly when it
                 * matters most. */
                ESP_LOGW("sctp", "send buffer still full after %u ms, dropping %u bytes"
                         " (peer rwnd %u, unacked %u chunks)",
                         (unsigned) waitedMs, (unsigned) pMessageLen,
                         (unsigned) rwnd, (unsigned) unacked);
                break;
            }
            THREAD_SLEEP(SCTP_SEND_BUFFER_RETRY_MS * HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
            waitedMs += SCTP_SEND_BUFFER_RETRY_MS;
        }
        if (waitedMs > 0 && sent > 0) {
            DLOGD("sctp send waited %u ms for buffer space", waitedMs);
        }
        CHK(sent > 0, STATUS_INTERNAL_ERROR);
    }

CleanUp:
    LEAVES();
    return retStatus;
}

// https://tools.ietf.org/html/draft-ietf-rtcweb-data-protocol-09#section-5.1
//      0                   1                   2                   3
//      0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |  Message Type |  Channel Type |            Priority           |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |                    Reliability Parameter                      |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     |         Label Length          |       Protocol Length         |
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     \                                                               /
//     |                             Label                             |
//     /                                                               /
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//     \                                                               /
//     |                            Protocol                           |
//     /                                                               /
//     +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
STATUS sctpSessionWriteDcep(PSctpSession pSctpSession, UINT32 streamId, PCHAR pChannelName, UINT32 pChannelNameLen,
                            PRtcDataChannelInit pRtcDataChannelInit)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    struct sctp_sendv_spa spa;

    CHK(pSctpSession != NULL && pChannelName != NULL, STATUS_NULL_ARG);

    MEMSET(&spa, 0x00, SIZEOF(spa));
    MEMSET(pSctpSession->packet, 0x00, SIZEOF(pSctpSession->packet));
    pSctpSession->packetSize = SCTP_DCEP_HEADER_LENGTH + pChannelNameLen;
    /* Setting the fields of DATA_CHANNEL_OPEN message */

    pSctpSession->packet[0] = DCEP_DATA_CHANNEL_OPEN; // message type

    // Set Channel type based on supplied parameters
    pSctpSession->packet[1] = DCEP_DATA_CHANNEL_RELIABLE_ORDERED;

    //   Set channel type and reliability parameters based on input
    //   SCTP allows fine tuning the channel robustness:
    //      1. Ordering: The data packets can be sent out in an ordered/unordered fashion
    //      2. Reliability: This determines how the retransmission of packets is handled.
    //   There are 2 parameters that can be fine tuned to achieve this:
    //      a. Number of retransmits
    //      b. Packet lifetime
    //   Default values for the parameters is 0. This falls back to reliable channel

    if (!pRtcDataChannelInit->ordered) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_RELIABLE_UNORDERED;
    }
    if (pRtcDataChannelInit->maxRetransmits.isNull == FALSE) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_REXMIT;
        putUnalignedInt32BigEndian(pSctpSession->packet + SIZEOF(UINT32), pRtcDataChannelInit->maxRetransmits.value);
    } else if (pRtcDataChannelInit->maxPacketLifeTime.isNull == FALSE) {
        pSctpSession->packet[1] |= DCEP_DATA_CHANNEL_TIMED;
        putUnalignedInt32BigEndian(pSctpSession->packet + SIZEOF(UINT32), pRtcDataChannelInit->maxPacketLifeTime.value);
    }

    putUnalignedInt16BigEndian(pSctpSession->packet + SCTP_DCEP_LABEL_LEN_OFFSET, pChannelNameLen);
    MEMCPY(pSctpSession->packet + SCTP_DCEP_LABEL_OFFSET, pChannelName, pChannelNameLen);
    spa.sendv_flags |= SCTP_SEND_SNDINFO_VALID;
    spa.sendv_sndinfo.snd_sid = streamId;

    putInt32((PINT32) &spa.sendv_sndinfo.snd_ppid, SCTP_PPID_DCEP);
    CHK(usrsctp_sendv(pSctpSession->socket, pSctpSession->packet, pSctpSession->packetSize, NULL, 0, &spa, SIZEOF(spa),
                      SCTP_SENDV_SPA, 0) > 0,
        STATUS_INTERNAL_ERROR);
CleanUp:

    LEAVES();
    return retStatus;
}

INT32 onSctpOutboundPacket(PVOID addr, PVOID data, ULONG length, UINT8 tos, UINT8 set_df)
{
    UNUSED_PARAM(tos);
    UNUSED_PARAM(set_df);

    PSctpSession pSctpSession = (PSctpSession) addr;

    if (pSctpSession == NULL || ATOMIC_LOAD(&pSctpSession->shutdownStatus) == SCTP_SESSION_SHUTDOWN_INITIATED ||
        pSctpSession->sctpSessionCallbacks.outboundPacketFunc == NULL) {
        if (pSctpSession != NULL) {
            ATOMIC_STORE(&pSctpSession->shutdownStatus, SCTP_SESSION_SHUTDOWN_COMPLETED);
        }
        return -1;
    }

    pSctpSession->sctpSessionCallbacks.outboundPacketFunc(pSctpSession->sctpSessionCallbacks.customData, data, length);

    return 0;
}

STATUS putSctpPacket(PSctpSession pSctpSession, PBYTE buf, UINT32 bufLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;

    usrsctp_conninput(pSctpSession, buf, bufLen, 0);

    LEAVES();
    return retStatus;
}

STATUS handleDcepPacket(PSctpSession pSctpSession, UINT32 streamId, PBYTE data, SIZE_T length)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT16 labelLength = 0;
    UINT16 protocolLength = 0;

    // Assert that is DCEP of type DataChannelOpen
    CHK(length > SCTP_DCEP_HEADER_LENGTH && data[0] == DCEP_DATA_CHANNEL_OPEN, STATUS_SUCCESS);

    MEMCPY(&labelLength, data + 8, SIZEOF(UINT16));
    MEMCPY(&protocolLength, data + 10, SIZEOF(UINT16));
    putInt16((PINT16) &labelLength, labelLength);
    putInt16((PINT16) &protocolLength, protocolLength);

    CHK((labelLength + protocolLength + SCTP_DCEP_HEADER_LENGTH) >= length, STATUS_SCTP_INVALID_DCEP_PACKET);

    CHK(SCTP_MAX_ALLOWABLE_PACKET_LENGTH >= length, STATUS_SCTP_INVALID_DCEP_PACKET);

    /* data[1] is the DCEP channel type and data[4..7] the reliability
     * parameter. Hand both up so the channel can be sent on the way the peer
     * opened it. */
    pSctpSession->sctpSessionCallbacks.dataChannelOpenFunc(pSctpSession->sctpSessionCallbacks.customData, streamId, data + SCTP_DCEP_HEADER_LENGTH,
                                                           labelLength, data[1], (UINT32) getUnalignedInt32BigEndian((PINT32) (data + SIZEOF(UINT32))));

CleanUp:
    LEAVES();
    return retStatus;
}

INT32 onSctpInboundPacket(struct socket* sock, union sctp_sockstore addr, PVOID data, ULONG length, struct sctp_rcvinfo rcv, INT32 flags,
                          PVOID ulp_info)
{
    UNUSED_PARAM(sock);
    UNUSED_PARAM(addr);
    UNUSED_PARAM(flags);
    STATUS retStatus = STATUS_SUCCESS;
    PSctpSession pSctpSession = (PSctpSession) ulp_info;
    BOOL isBinary = FALSE;

    rcv.rcv_ppid = ntohl(rcv.rcv_ppid);
    switch (rcv.rcv_ppid) {
        case SCTP_PPID_DCEP:
            CHK_STATUS(handleDcepPacket(pSctpSession, rcv.rcv_sid, data, length));
            break;
        case SCTP_PPID_BINARY:
        case SCTP_PPID_BINARY_EMPTY:
            isBinary = TRUE;
            // fallthrough
        case SCTP_PPID_STRING:
        case SCTP_PPID_STRING_EMPTY:
            pSctpSession->sctpSessionCallbacks.dataChannelMessageFunc(pSctpSession->sctpSessionCallbacks.customData, rcv.rcv_sid, isBinary, data,
                                                                      length);
            break;
        default:
            DLOGI("Unhandled PPID on incoming SCTP message %d", rcv.rcv_ppid);
            break;
    }

CleanUp:

    /*
     * IMPORTANT!!! The allocation is done in the sctp library using default allocator
     * so we need to use the default free API.
     */
    if (data != NULL) {
        free(data);
    }
    if (STATUS_FAILED(retStatus)) {
        return -1;
    }
    return 1;
}
