/**
 * Kinesis Video Producer ConnectionListener
 */
#define LOG_CLASS "ConnectionListener"
#include "../Include_i.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "kvs_instrumentation.h"

STATUS createConnectionListener(PConnectionListener* ppConnectionListener)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 allocationSize = SIZEOF(ConnectionListener) + MAX_UDP_PACKET_SIZE;
    PConnectionListener pConnectionListener = NULL;

    CHK(ppConnectionListener != NULL, STATUS_NULL_ARG);

    pConnectionListener = (PConnectionListener) MEMCALLOC(1, allocationSize);
    CHK(pConnectionListener != NULL, STATUS_NOT_ENOUGH_MEMORY);

    ATOMIC_STORE_BOOL(&pConnectionListener->terminate, FALSE);
    pConnectionListener->receiveDataRoutine = INVALID_TID_VALUE;
    pConnectionListener->lock = MUTEX_CREATE(FALSE);

    // No sockets are present
    pConnectionListener->socketCount = 0;

    // pConnectionListener->pBuffer starts at the end of ConnectionListener struct
    pConnectionListener->pBuffer = (PBYTE) (pConnectionListener + 1);
    pConnectionListener->bufferLen = MAX_UDP_PACKET_SIZE;

    // Use socketpair only if available
#if defined(HAVE_SOCKETPAIR)
    pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_LISTEN] = -1;
    pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_WRITE] = -1;
    CHK_STATUS(createSocketPair(&(pConnectionListener->kickSocket)));
#endif

CleanUp:

    if (STATUS_FAILED(retStatus) && pConnectionListener != NULL) {
        freeConnectionListener(&pConnectionListener);
        pConnectionListener = NULL;
    }

    if (ppConnectionListener != NULL) {
        *ppConnectionListener = pConnectionListener;
    }

    return retStatus;
}

STATUS freeConnectionListener(PConnectionListener* ppConnectionListener)
{
    STATUS retStatus = STATUS_SUCCESS;
    PConnectionListener pConnectionListener = NULL;
    TID threadId;
#if defined(HAVE_SOCKETPAIR)
    const char* msg = "1";
#endif

    CHK(ppConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(*ppConnectionListener != NULL, retStatus);

    pConnectionListener = *ppConnectionListener;

    ATOMIC_STORE_BOOL(&pConnectionListener->terminate, TRUE);

    if (IS_VALID_MUTEX_VALUE(pConnectionListener->lock)) {
        MUTEX_LOCK(pConnectionListener->lock);
        threadId = pConnectionListener->receiveDataRoutine;
        MUTEX_UNLOCK(pConnectionListener->lock);

        // TODO add support for windows socketpair
        // This writes to the socketpair, kicking the POLL() out early,
        // otherwise wait for the POLL to timeout
#if defined(HAVE_SOCKETPAIR)
        socketWrite(pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_WRITE], msg, STRLEN(msg));
#endif

        // wait for thread to finish.
        if (IS_VALID_TID_VALUE(threadId)) {
            THREAD_JOIN(pConnectionListener->receiveDataRoutine, NULL);
        }

        MUTEX_FREE(pConnectionListener->lock);
    }

    // TODO add support for windows socketpair
#if defined(HAVE_SOCKETPAIR)
    if (pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_LISTEN] != -1) {
        closeSocket(pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_LISTEN]);
    }
    if (pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_WRITE] != -1) {
        closeSocket(pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_WRITE]);
    }
#endif

    MEMFREE(pConnectionListener);

    *ppConnectionListener = NULL;

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}

STATUS connectionListenerAddConnection(PConnectionListener pConnectionListener, PSocketConnection pSocketConnection)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE, iterate = TRUE;
    UINT32 i;

    CHK(pConnectionListener != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    // Check for space
    CHK(pConnectionListener->socketCount < CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION, STATUS_NOT_ENOUGH_MEMORY);

    // Find an empty slot by checking whether connected
    for (i = 0; iterate && i < CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION; i++) {
        if (pConnectionListener->sockets[i] == NULL) {
            pConnectionListener->sockets[i] = pSocketConnection;
            pConnectionListener->socketCount++;
            iterate = FALSE;
        }
    }

    MUTEX_UNLOCK(pConnectionListener->lock);
    locked = FALSE;

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }

    return retStatus;
}

STATUS connectionListenerRemoveConnection(PConnectionListener pConnectionListener, PSocketConnection pSocketConnection)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE, iterate = TRUE;
    UINT32 i;

    CHK(pConnectionListener != NULL && pSocketConnection != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    // Mark socket as closed
    CHK_STATUS(socketConnectionClosed(pSocketConnection));

    // Remove from the list of sockets
    for (i = 0; iterate && i < CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION; i++) {
        if (pConnectionListener->sockets[i] == pSocketConnection) {
            iterate = FALSE;

            // Mark the slot as empty and decrement the count
            pConnectionListener->sockets[i] = NULL;
            pConnectionListener->socketCount--;
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }

    return retStatus;
}

STATUS connectionListenerRemoveAllConnection(PConnectionListener pConnectionListener)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;
    UINT32 i;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    for (i = 0; i < CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION; i++) {
        if (pConnectionListener->sockets[i] != NULL) {
            CHK_STATUS(socketConnectionClosed(pConnectionListener->sockets[i]));
            pConnectionListener->sockets[i] = NULL;
            pConnectionListener->socketCount--;
        }
    }

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }

    return retStatus;
}

STATUS connectionListenerStart(PConnectionListener pConnectionListener)
{
    STATUS retStatus = STATUS_SUCCESS;
    BOOL locked = FALSE;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);
    CHK(!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate), retStatus);

    MUTEX_LOCK(pConnectionListener->lock);
    locked = TRUE;

    CHK(!IS_VALID_TID_VALUE(pConnectionListener->receiveDataRoutine), retStatus);
#if CONFIG_IDF_CMAKE
#define CONN_LISTENER_THREAD_STACK_SIZE (32 * 1024)
/* Above the pthread default of 5, which is where every other KVS thread lands
 * -- sctpTimer, reconnecter, listener, receiveLwsMsg -- along with whatever
 * the application runs. FreeRTOS round-robins equal priorities, so at 5 this
 * task got a share of the CPU rather than precedence.
 *
 * It is the task that reads every inbound packet, including the peer's
 * connectivity checks. When it is not scheduled promptly the socket mailbox
 * fills and lwIP discards what arrives next, and a lost check eventually reads
 * as a dead connection. Measured under load before this: Chrome sent 20 checks
 * and the device saw 10.
 *
 * Well below lwIP's tcpip thread (18) and the WiFi task (23), so it cannot
 * starve the stack feeding it. It blocks in poll() and only runs when there is
 * something to read, so a high priority costs nothing when idle. */
#define CONN_LISTENER_THREAD_PRIORITY 10

/* Where this thread's 32 KB stack comes from.
 *
 * Internal RAM is the scarce pool on this chip and it fragments. This stack is
 * the largest single allocation any connection asks for, and when it cannot be
 * satisfied the offer never goes out -- observed with 67 KB free and a largest
 * block of 31744, which is 1024 bytes short. A second session existing briefly
 * during a browser refresh is enough to cause it.
 *
 * Moving it was not obviously free -- inbound DTLS decrypt runs on this stack
 * and DTLS costs about 1.8 ms per packet -- so it was measured rather than
 * argued. Same link, same MTU, same scene:
 *
 *   PSRAM     internal free 105351 -> 104995, largest block 32768, ~258 KB/s
 *   internal  internal free 105367 ->  72239, largest block 31744,  265 KB/s
 *
 * 2.7% apart, against a window-to-window spread of 5% within either run, and
 * connectivity checks answered at identical latency in both. The memory
 * difference is not marginal in the same way: on internal RAM one session
 * alone leaves the largest block at 31744, which is precisely what the next
 * 32768 byte request failed on.
 *
 * Set to 0 to measure the internal-RAM side again. Either way the allocation
 * is not fatal: globalCreateThreadPriWithCaps falls back to PSRAM when
 * internal is refused. This flag decides where it starts. */
#define CONN_LISTENER_STACK_IN_PSRAM 1

#if CONN_LISTENER_STACK_IN_PSRAM
#define CONN_LISTENER_THREAD_CREATE THREAD_CREATE_EX_PRI_PSRAM
#else
#define CONN_LISTENER_THREAD_CREATE THREAD_CREATE_EX_PRI
#endif
    {
        /* Say where the stack came from, so a run can be attributed without
         * trusting which binary was flashed. Internal drops by the stack size
         * when it comes from there and barely moves when it does not. */
#if KVS_INSTR
        SIZE_T beforeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
#endif
        CHK_STATUS(CONN_LISTENER_THREAD_CREATE(&pConnectionListener->receiveDataRoutine, "connListener",
                   CONN_LISTENER_THREAD_STACK_SIZE, TRUE, connectionListenerReceiveDataRoutine,
                   CONN_LISTENER_THREAD_PRIORITY, (PVOID) pConnectionListener));
        KVS_INSTR_LOGW("ice", "connListener stack: %s, %u KB; internal free %u -> %u, largest block %u",
                 CONN_LISTENER_STACK_IN_PSRAM ? "PSRAM" : "internal",
                 (unsigned) (CONN_LISTENER_THREAD_STACK_SIZE / 1024),
                 (unsigned) beforeInternal,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
#else
    CHK_STATUS(THREAD_CREATE(&pConnectionListener->receiveDataRoutine, connectionListenerReceiveDataRoutine, (PVOID) pConnectionListener));
#endif

CleanUp:

    if (locked) {
        MUTEX_UNLOCK(pConnectionListener->lock);
    }

    return retStatus;
}

BOOL canReadFd(INT32 fd, struct pollfd* fds, INT32 nfds)
{
    INT32 i;
    for (i = 0; i < nfds; i++) {
        if (fds[i].fd == fd && (fds[i].revents & POLLIN) != 0) {
            return TRUE;
        }
    }
    return FALSE;
}

PVOID connectionListenerReceiveDataRoutine(PVOID arg)
{
    STATUS retStatus = STATUS_SUCCESS;
    PConnectionListener pConnectionListener = (PConnectionListener) arg;
    PSocketConnection pSocketConnection;
    BOOL iterate = TRUE;
    PSocketConnection sockets[CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION];
    UINT32 i, socketCount;

    INT32 nfds = 0;
    //+1 added for the pipe() to kickout poll()
    struct pollfd rfds[CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION + 1];
    INT32 retval, localSocket;
    INT64 readLen;
    // the source address is put here. sockaddr_storage can hold either sockaddr_in or sockaddr_in6
    struct sockaddr_storage srcAddrBuff;
    socklen_t srcAddrBuffLen = SIZEOF(srcAddrBuff);
    struct sockaddr_in* pIpv4Addr;
    struct sockaddr_in6* pIpv6Addr;
    KvsIpAddress srcAddr;
    PKvsIpAddress pSrcAddr = NULL;

    CHK(pConnectionListener != NULL, STATUS_NULL_ARG);

    /* Ensure that memory sanitizers consider
     * rfds initialized even if FD_ZERO is
     * implemented in assembly. */
    MEMSET(&rfds, 0x00, SIZEOF(rfds));

    srcAddr.isPointToPoint = FALSE;

    while (!ATOMIC_LOAD_BOOL(&pConnectionListener->terminate)) {
        nfds = 0;

        // Perform the socket connection gathering under the lock
        // NOTE: There is no cleanup jump from the lock/unlock block
        // so we don't need to use a boolean indicator whether locked
        MUTEX_LOCK(pConnectionListener->lock);
        for (i = 0, socketCount = 0; i < CONNECTION_LISTENER_DEFAULT_MAX_LISTENING_CONNECTION; i++) {
            pSocketConnection = pConnectionListener->sockets[i];
            if (pSocketConnection != NULL) {
                if (!socketConnectionIsClosed(pSocketConnection)) {
                    MUTEX_LOCK(pSocketConnection->lock);
                    localSocket = pSocketConnection->localSocket;
                    MUTEX_UNLOCK(pSocketConnection->lock);
                    rfds[nfds].fd = localSocket;
                    rfds[nfds].events = POLLIN | POLLPRI;
#if !defined(HAVE_SOCKETPAIR)
                    rfds[nfds].events &= ~POLLPRI;
#endif
                    rfds[nfds].revents = 0;
                    nfds++;

                    // Store the sockets locally while in use and mark it as in use
                    sockets[socketCount++] = pSocketConnection;
                    ATOMIC_STORE_BOOL(&pSocketConnection->inUse, TRUE);
                } else {
                    // Remove the connection
                    pConnectionListener->sockets[i] = NULL;
                    pConnectionListener->socketCount--;
                }
            }
        }

        // Need to unlock the mutex to ensure other racing threads unblock
        MUTEX_UNLOCK(pConnectionListener->lock);
        retval = 0;
        if (nfds != 0) {
            // TODO add support for socketpair() in windows
            // This end of the socketpair has been added to the list of sockets polled
            // in order to have a way to end the poll early from the destructor
#if defined(HAVE_SOCKETPAIR)
            rfds[nfds].fd = pConnectionListener->kickSocket[CONNECTION_LISTENER_KICK_SOCKET_LISTEN];
            rfds[nfds].events = POLLIN;
            rfds[nfds].revents = 0;
            nfds++;
#endif
            // blocking call until resolves as a timeout, an error, a signal or data received
            retval = POLL(rfds, nfds, CONNECTION_LISTENER_SOCKET_WAIT_FOR_DATA_TIMEOUT / HUNDREDS_OF_NANOS_IN_A_MILLISECOND);
        } else {
            // No sockets to poll (no active sessions). Yield to other tasks.
            THREAD_SLEEP(CONNECTION_LISTENER_SOCKET_WAIT_FOR_DATA_TIMEOUT);
        }

        // In case of 0 we have a timeout and should re-lock to allow for other
        // interlocking operations to proceed. A positive return means we received data
        if (retval == -1) {
            DLOGW("poll() failed with errno %s", getErrorString(getErrorCode()));
        } else if (retval > 0) {
            for (i = 0; i < socketCount; i++) {
                pSocketConnection = sockets[i];
                if (!socketConnectionIsClosed(pSocketConnection)) {
                    MUTEX_LOCK(pSocketConnection->lock);
                    localSocket = pSocketConnection->localSocket;
                    MUTEX_UNLOCK(pSocketConnection->lock);

                    if (canReadFd(localSocket, rfds, nfds)) {
                        iterate = TRUE;
                        while (iterate) {
#if KVS_INSTR
                            int64_t t_recv0 = esp_timer_get_time();
#endif
                            readLen = recvfrom(localSocket, pConnectionListener->pBuffer, pConnectionListener->bufferLen, 0,
                                               (struct sockaddr*) &srcAddrBuff, &srcAddrBuffLen);
#if KVS_INSTR
                            int64_t t_recv1 = esp_timer_get_time();
#endif
                            if (readLen < 0) {
                                switch (getErrorCode()) {
                                    case EWOULDBLOCK:
                                        break;
                                    default:
                                        /* on any other error, close connection */
                                        CHK_STATUS(socketConnectionClosed(pSocketConnection));
                                        DLOGD("recvfrom() failed with errno %s for socket %d", getErrorString(getErrorCode()), localSocket);
                                        break;
                                }

                                iterate = FALSE;
                            } else if (readLen == 0) {
                                CHK_STATUS(socketConnectionClosed(pSocketConnection));
                                iterate = FALSE;
                            } else if (/* readLen > 0 */
                                       ATOMIC_LOAD_BOOL(&pSocketConnection->receiveData) && pSocketConnection->dataAvailableCallbackFn != NULL &&
                                       /* data could be encrypted so they need to be decrypted through socketConnectionReadData
                                        * and get the decrypted data length. */
                                       STATUS_SUCCEEDED(socketConnectionReadData(pSocketConnection, pConnectionListener->pBuffer,
                                                                                 pConnectionListener->bufferLen, (PUINT32) &readLen))) {
#if KVS_INSTR
                                /* The decrypt happened inside the condition above. */
                                int64_t t_decrypted = esp_timer_get_time();
#endif
                                if (pSocketConnection->protocol == KVS_SOCKET_PROTOCOL_UDP) {
                                    if (srcAddrBuff.ss_family == AF_INET) {
                                        srcAddr.family = KVS_IP_FAMILY_TYPE_IPV4;
                                        pIpv4Addr = (struct sockaddr_in*) &srcAddrBuff;
                                        MEMCPY(srcAddr.address, (PBYTE) &pIpv4Addr->sin_addr, IPV4_ADDRESS_LENGTH);
                                        srcAddr.port = pIpv4Addr->sin_port;
                                    } else if (srcAddrBuff.ss_family == AF_INET6) {
                                        srcAddr.family = KVS_IP_FAMILY_TYPE_IPV6;
                                        pIpv6Addr = (struct sockaddr_in6*) &srcAddrBuff;
                                        MEMCPY(srcAddr.address, (PBYTE) &pIpv6Addr->sin6_addr, IPV6_ADDRESS_LENGTH);
                                        srcAddr.port = pIpv6Addr->sin6_port;
                                    }
                                    pSrcAddr = &srcAddr;
                                } else {
                                    // srcAddr is ignored in TCP callback handlers
                                    pSrcAddr = NULL;
                                }

                                // readLen may be 0 if SSL does not emit any application data.
                                // in that case, no need to call dataAvailable callback
                                if (readLen > 0) {
                                    pSocketConnection->dataAvailableCallbackFn(pSocketConnection->dataAvailableCallbackCustomData, pSocketConnection,
                                                                               pConnectionListener->pBuffer, (UINT32) readLen, pSrcAddr,
                                                                               NULL); // no dest information available right now.
                                }
#if KVS_INSTR
                                /* Where the inbound millisecond and a half goes.
                                 *
                                 * connListener is 29% of all CPU while doing nothing but taking
                                 * SACKs, at ~1.8 us per packet, and that cost does not move when
                                 * cwnd changes by a factor of ten -- so it is not the SACK walking
                                 * the retransmission queue. Three brackets split what is left: the
                                 * socket read, the DTLS decrypt, and everything usrsctp does with
                                 * the result. */
                                {
                                    static int64_t sumRecv, sumDecrypt, sumSctp, windowStart;
                                    static UINT32 packets;
                                    int64_t now = esp_timer_get_time();
                                    sumRecv += t_recv1 - t_recv0;
                                    sumDecrypt += t_decrypted - t_recv1;
                                    sumSctp += now - t_decrypted;
                                    packets++;
                                    if (windowStart == 0) {
                                        windowStart = t_recv0;
                                    } else if (now - windowStart > 5000000) {
                                        ESP_LOGW("ice", "inbound %u packets: %.2f ms each = %.2f recvfrom + %.2f decrypt + %.2f sctp",
                                                 (unsigned) packets,
                                                 (sumRecv + sumDecrypt + sumSctp) / 1000.0 / packets,
                                                 sumRecv / 1000.0 / packets, sumDecrypt / 1000.0 / packets,
                                                 sumSctp / 1000.0 / packets);
                                        sumRecv = sumDecrypt = sumSctp = 0;
                                        packets = 0;
                                        windowStart = now;
                                    }
                                }
#endif
                            }

                            // reset srcAddrBuffLen to actual size
                            srcAddrBuffLen = SIZEOF(srcAddrBuff);
                        }
                    }
                }
            }
        }

        // Mark as unused
        for (i = 0; i < socketCount; i++) {
            ATOMIC_STORE_BOOL(&sockets[i]->inUse, FALSE);
        }
    }

CleanUp:

    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}
