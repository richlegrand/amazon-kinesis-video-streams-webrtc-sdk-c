/*******************************************
PeerConnection internal include file
*******************************************/
#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__

#pragma once

/* For the outbound DTLS queue and its task, at the bottom of KvsPeerConnection.
   This fork is ESP-only, and Sctp.c and the teardown watch already depend on
   FreeRTOS directly. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The outbound DTLS queue. Depth is about 140 ms of packets at the rate this
   link runs, which absorbs a burst without letting a stall build a backlog of
   stale video. Slot size covers a full SCTP packet at this port's 1200 byte
   path MTU with room over. Stack is sized for an mbedtls record write and
   lives in PSRAM; its high-water mark is logged when the task stops. */
#define KVS_DTLS_OUT_QUEUE_DEPTH 32
#define KVS_DTLS_OUT_PACKET_MAX  1400
#define KVS_DTLS_OUT_STACK       (8 * 1024)
#define KVS_DTLS_OUT_PRIO        6

#define LOCAL_ICE_UFRAG_LEN 4
#define LOCAL_ICE_PWD_LEN   24
#define LOCAL_CNAME_LEN     16

// https://tools.ietf.org/html/rfc5245#section-15.4
#define MAX_ICE_UFRAG_LEN 256
#define MAX_ICE_PWD_LEN   256

#define PEER_FRAME_BUFFER_SIZE_INCREMENT_FACTOR 1.5

// A non-comprehensive list of valid JSON characters
#define VALID_CHAR_SET_FOR_JSON "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz+/"

#define ICE_CANDIDATE_JSON_TEMPLATE (PCHAR) "{\"candidate\":\"candidate:%s\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}"

#define MAX_ICE_CANDIDATE_JSON_LEN (MAX_SDP_ATTRIBUTE_VALUE_LENGTH + SIZEOF(ICE_CANDIDATE_JSON_TEMPLATE) + 1)

#define CODEC_HASH_TABLE_BUCKET_COUNT  50
#define CODEC_HASH_TABLE_BUCKET_LENGTH 2
#define RTX_HASH_TABLE_BUCKET_COUNT    50
#define RTX_HASH_TABLE_BUCKET_LENGTH   2
#define TWCC_HASH_TABLE_BUCKET_COUNT   100
#define TWCC_HASH_TABLE_BUCKET_LENGTH  2

#define DATA_CHANNEL_HASH_TABLE_BUCKET_COUNT  200
#define DATA_CHANNEL_HASH_TABLE_BUCKET_LENGTH 2

// Environment variable to display SDPs
#define DEBUG_LOG_SDP ((PCHAR) "DEBUG_LOG_SDP")

#define MAX_ACCESS_THREADS_WEBRTC_CLIENT_CONTEXT 50

typedef enum {
    RTC_RTX_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE = 1,
    RTC_RTX_CODEC_VP8 = 2,
    RTC_RTX_CODEC_H265 = 3,
} RTX_CODEC;

typedef struct {
    UINT64 localTimeKvs;
    UINT64 remoteTimeKvs;
    UINT32 packetSize;
} TwccRtpPacketInfo, *PTwccRtpPacketInfo;

typedef struct {
    PHashTable pTwccRtpPktInfosHashTable; // Hash table of [seqNum, PTwccPacket]
    UINT16 firstSeqNumInRollingWindow;    // To monitor the last deleted packet in the rolling window
    UINT16 lastReportedSeqNum;            // To monitor the last packet's seqNum in the TWCC response
    UINT16 prevReportedBaseSeqNum;        // To monitor the base seqNum in the TWCC response
} TwccManager, *PTwccManager;

typedef struct {
    UINT64 peerConnectionCreationTime;
    UINT64 dtlsSessionSetupTime;
    UINT64 iceHolePunchingTime;
    UINT64 closePeerConnectionTime;
    UINT64 freePeerConnectionTime;
} KvsPeerConnectionDiagnostics, *PKvsPeerConnectionDiagnostics;

typedef struct {
    RtcPeerConnection peerConnection;
    // UINT32 padding makes transportWideSequenceNumber 64bit aligned
    // we put atomics at the top of structs because customers application could set the packing to 0
    // in which case any atomic operations would result in bus errors if there is a misalignment
    // for more see https://github.com/awslabs/amazon-kinesis-video-streams-webrtc-sdk-c/pull/987#discussion_r534432907
    UINT32 padding;
    volatile SIZE_T transportWideSequenceNumber;

    PIceAgent pIceAgent;
    PDtlsSession pDtlsSession;
    BOOL dtlsIsServer;

    MUTEX pSrtpSessionLock;
    PSrtpSession pSrtpSession;

    PSctpSession pSctpSession;

    PSessionDescription pRemoteSessionDescription;
    PDoubleList pTransceivers;
    PDoubleList pFakeTransceivers;
    PDoubleList pAnswerTransceivers;

    volatile ATOMIC_BOOL sctpIsEnabled;

    CHAR localIceUfrag[LOCAL_ICE_UFRAG_LEN + 1];
    CHAR localIcePwd[LOCAL_ICE_PWD_LEN + 1];

    CHAR remoteIceUfrag[MAX_ICE_UFRAG_LEN + 1];
    CHAR remoteIcePwd[MAX_ICE_PWD_LEN + 1];

    CHAR localCNAME[LOCAL_CNAME_LEN + 1];

    CHAR remoteCertificateFingerprint[CERTIFICATE_FINGERPRINT_LENGTH + 1];

    MUTEX peerConnectionObjLock;

    // If the local session description is an SDP offer.
    // (TRUE = viewer mode, FALSE = master mode)
    BOOL isOffer;

    TIMER_QUEUE_HANDLE timerQueueHandle;

    // Codecs that we support and their payloadTypes
    // When offering we generate values starting from 96
    // When answering this is populated from the remote offer
    PHashTable pCodecTable;

    // Payload types that we use to retransmit data
    // When answering this is populated from the remote offer
    PHashTable pRtxTable;

    // DataChannels keyed by streamId
    PHashTable pDataChannels;

    UINT64 onDataChannelCustomData;
    RtcOnDataChannel onDataChannel;

    UINT64 onIceCandidateCustomData;
    RtcOnIceCandidate onIceCandidate;

    UINT64 onConnectionStateChangeCustomData;
    RtcOnConnectionStateChange onConnectionStateChange;
    RTC_PEER_CONNECTION_STATE connectionState;

    UINT16 MTU;

    NullableBool canTrickleIce;

    // congestion control
    // https://tools.ietf.org/html/draft-holmer-rmcat-transport-wide-cc-extensions-01
    UINT16 twccExtId;
    MUTEX twccLock;
    PTwccManager pTwccManager;
    RtcOnSenderBandwidthEstimation onSenderBandwidthEstimation;
    UINT64 onSenderBandwidthEstimationCustomData;

    UINT64 iceConnectingStartTime;
    KvsPeerConnectionDiagnostics peerConnectionDiagnostics;

    /* Outbound SCTP packets, waiting to be encrypted and sent.
     *
     * usrsctp calls conn_output with the association's TCB lock held, and a
     * whole DTLS encryption plus socket write used to happen inside it.
     * Measured: the lock held for 510 ms of every second at 1.65 ms a time,
     * which lands on the 1.72 ms of mbedtls_ssl_write almost exactly, while
     * the receiver spent 256 ms/s blocked on that same lock. One association
     * carries every data channel, so a sender busy encrypting stalls the
     * acknowledgements that would let it send more.
     *
     * Copying the packet here and returning takes the encryption out of the
     * lock. usrsctp frees the buffer the moment conn_output returns, so a
     * copy was always required.
     *
     * Dropping when the queue is full is deliberate. Blocking would put the
     * wait back inside the lock, which is the whole problem, and a dropped
     * SCTP packet looks like the network loss the protocol already handles. */
    QueueHandle_t dtlsOutQueue;
    TaskHandle_t dtlsOutTask;
    StaticQueue_t dtlsOutQueueBuf;
    StaticTask_t* dtlsOutTcb;
    PVOID dtlsOutStack;
    PVOID dtlsOutStorage;
    volatile SIZE_T dtlsOutRunning;
    /* Set by the routine as its last act, once it is out of the queue loop
       and about to suspend itself. The stopper waits for this rather than for
       eTaskGetState, which reports eDeleted before the kernel has finished
       with the task. See stopDtlsOutTask. */
    volatile SIZE_T dtlsOutParked;
    UINT32 dtlsOutDropped;
} KvsPeerConnection, *PKvsPeerConnection;

typedef struct {
    UINT32 currentDataChannelId;
    PKvsPeerConnection pKvsPeerConnection;
    PHashTable unkeyedDataChannels;
} AllocateSctpSortDataChannelsData, *PAllocateSctpSortDataChannelsData;

typedef struct {
    CHAR hostname[MAX_ICE_CONFIG_URI_LEN + 1];
    DualKvsIpAddresses kvsIpAddresses;
    BOOL isIpInitialized;
    UINT64 startTime;
    UINT64 stunDnsResolutionTime;
    UINT64 expirationDuration;
    STATUS status;
} StunIpAddrContext, *PStunIpAddrContext;

// Declare the structure of the Singleton
// Members of the singleton are responsible for their own sync mechanisms.
typedef struct {
    PStunIpAddrContext pStunIpAddrCtx;
    volatile ATOMIC_BOOL isContextInitialized;
    volatile SIZE_T contextRefCnt;
    MUTEX stunCtxlock;
} WebRtcClientContext, *PWebRtcClientContext;

STATUS onFrameReadyFunc(UINT64, UINT16, UINT16, UINT32);
STATUS onFrameDroppedFunc(UINT64, UINT16, UINT16, UINT32);
VOID onSctpSessionOutboundPacket(UINT64, PBYTE, UINT32);
STATUS startDtlsOutTask(PKvsPeerConnection);
VOID stopDtlsOutTask(PKvsPeerConnection);
VOID onSctpSessionDataChannelMessage(UINT64, UINT32, BOOL, PBYTE, UINT32);
VOID onSctpSessionDataChannelOpen(UINT64, UINT32, PBYTE, UINT32, BYTE, UINT32);

STATUS sendPacketToRtpReceiver(PKvsPeerConnection, PBYTE, UINT32);
STATUS changePeerConnectionState(PKvsPeerConnection, RTC_PEER_CONNECTION_STATE);
STATUS twccManagerOnPacketSent(PKvsPeerConnection, PRtpPacket);
UINT32 parseExtId(PCHAR);

// visible for testing only
VOID onIceConnectionStateChange(UINT64, UINT64);

#ifdef __cplusplus
}
#endif
#endif /* __KINESIS_VIDEO_WEBRTC_CLIENT_PEERCONNECTION_PEERCONNECTION__ */
