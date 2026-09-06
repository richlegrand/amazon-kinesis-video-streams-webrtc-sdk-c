//
// Sctp
//

#ifndef __KINESIS_VIDEO_WEBRTC_CLIENT_SCTP_SCTP__
#define __KINESIS_VIDEO_WEBRTC_CLIENT_SCTP_SCTP__

/* How long sctpSessionWriteMessage will wait for send-buffer space before
 * giving up on a message, and how often it re-checks. The socket is
 * non-blocking; without this the caller has no backpressure signal. */
/* How often usrsctp_handle_timers is driven. usrsctp's own timer thread
 * uses 10ms; anything much coarser delays retransmission. */
#define SCTP_TIMER_INTERVAL_MS       10

/* Send buffer, in bytes.
 *
 * 32 KB, down from 64. At 64 the throttle landed in different places
 * depending on message size: the benchmark's 16 KB messages self-limited at
 * about 284 KB/s while the camera's 8 KB ones ran to 354, because a large
 * message needs a large contiguous gap in the buffer before it is accepted and
 * so blocks its producer sooner. Message size was acting as a rate control
 * parameter, which is not its job.
 *
 * Two floors constrain how far this can go, and 32 KB is close to both. A
 * message larger than the buffer is refused outright with EMSGSIZE rather than
 * queued, and the largest here is 8200 bytes through the HTTP bridge. And it
 * must exceed the bandwidth-delay product or it caps throughput rather than
 * latency -- roughly 33 KB on a 200 ms path at these rates, so this is at the
 * edge for a remote peer and should be watched there. */
#define SCTP_SESSION_SNDBUF_BYTES    (32 * 1024)
#define SCTP_TIMER_THREAD_STACK_SIZE (8 * 1024)

#define SCTP_SEND_BUFFER_RETRY_MS    2
#define SCTP_SEND_BUFFER_MAX_WAIT_MS 5000

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 1200 - 12 (SCTP header Size)
/* Must stay under what one DTLS record can carry, because an SCTP packet
 * has to travel as a single datagram.
 *
 * With the DTLS MTU at 1440, a GCM record spends 13 bytes on the header, 8 on
 * the explicit nonce and 16 on the tag, leaving about 1403. 1360 keeps margin
 * for that arithmetic being slightly off, which matters because getting it
 * wrong fails silently -- see below.
 *
 * Dtls_mbedtls.c sets the DTLS MTU to DEFAULT_MTU_SIZE_BYTES (1200) and
 * then, in dtlsSessionPutApplicationData, splits anything larger than
 * mbedtls_ssl_get_max_out_record_payload() across several records. For
 * AES-GCM that limit is about 1163 -- 1200 less a 13-byte record header,
 * an 8-byte explicit nonce and a 16-byte tag. At 1188 a full-size SCTP
 * packet exceeded it, was split, and both halves were discarded by the
 * peer as malformed, with no error anywhere.
 *
 * Small messages fit in one packet and survived, so this only appeared
 * once a message was large enough for usrsctp to fragment. 1100 leaves
 * room for ciphersuites with more overhead than GCM. */
#define SCTP_MTU                         1360
#define SCTP_ASSOCIATION_DEFAULT_PORT    5000
#define SCTP_DCEP_HEADER_LENGTH          12
#define SCTP_DCEP_LABEL_LEN_OFFSET       8
#define SCTP_DCEP_LABEL_OFFSET           12
#define SCTP_MAX_ALLOWABLE_PACKET_LENGTH (SCTP_DCEP_HEADER_LENGTH + MAX_DATA_CHANNEL_NAME_LEN + MAX_DATA_CHANNEL_PROTOCOL_LEN + 2)

#define SCTP_SESSION_ACTIVE             0
#define SCTP_SESSION_SHUTDOWN_INITIATED 1
#define SCTP_SESSION_SHUTDOWN_COMPLETED 2

#define DEFAULT_SCTP_SHUTDOWN_TIMEOUT 2 * HUNDREDS_OF_NANOS_IN_A_SECOND

#define DEFAULT_USRSCTP_TEARDOWN_POLLING_INTERVAL (10 * HUNDREDS_OF_NANOS_IN_A_MILLISECOND)

enum { SCTP_PPID_DCEP = 50, SCTP_PPID_STRING = 51, SCTP_PPID_BINARY = 53, SCTP_PPID_STRING_EMPTY = 56, SCTP_PPID_BINARY_EMPTY = 57 };

enum {
    DCEP_DATA_CHANNEL_OPEN = 0x03,
};

typedef enum {
    DCEP_DATA_CHANNEL_RELIABLE_ORDERED = (BYTE) 0x00,
    DCEP_DATA_CHANNEL_RELIABLE_UNORDERED = (BYTE) 0x80,
    DCEP_DATA_CHANNEL_REXMIT = (BYTE) 0x01,
    DCEP_DATA_CHANNEL_TIMED = (BYTE) 0x02
} DATA_CHANNEL_TYPE;

// Callback that is fired when SCTP Association wishes to send packet
typedef VOID (*SctpSessionOutboundPacketFunc)(UINT64, PBYTE, UINT32);

// Callback that is fired when SCTP has a new DataChannel
// Argument is ChannelID and ChannelName + Len
/* customData, streamId, label, labelLen, DCEP channel type, reliability param.
 * The last two carry what the peer asked for so the channel can be sent on
 * the way it was opened, rather than however the session last happened to be
 * configured. */
typedef VOID (*SctpSessionDataChannelOpenFunc)(UINT64, UINT32, PBYTE, UINT32, BYTE, UINT32);

// Callback that is fired when SCTP has a DataChannel Message.
// Argument is ChannelID and Message + Len
typedef VOID (*SctpSessionDataChannelMessageFunc)(UINT64, UINT32, BOOL, PBYTE, UINT32);

typedef struct {
    UINT64 customData;
    SctpSessionOutboundPacketFunc outboundPacketFunc;
    SctpSessionDataChannelOpenFunc dataChannelOpenFunc;
    SctpSessionDataChannelMessageFunc dataChannelMessageFunc;
} SctpSessionCallbacks, *PSctpSessionCallbacks;

typedef struct {
    volatile SIZE_T shutdownStatus;
    struct socket* socket;
    BYTE packet[SCTP_MAX_ALLOWABLE_PACKET_LENGTH];
    UINT32 packetSize;
    SctpSessionCallbacks sctpSessionCallbacks;
} SctpSession, *PSctpSession;

STATUS initSctpSession();
VOID deinitSctpSession();
STATUS createSctpSession(PSctpSessionCallbacks, PSctpSession*);
STATUS freeSctpSession(PSctpSession*);
STATUS putSctpPacket(PSctpSession, PBYTE, UINT32);
STATUS sctpSessionGetStats(PSctpSession, PUINT32, PUINT32);
STATUS sctpSessionWriteMessage(PSctpSession, UINT32, BOOL, PBYTE, UINT32, PRtcDataChannelInit);
STATUS sctpSessionWriteDcep(PSctpSession, UINT32, PCHAR, UINT32, PRtcDataChannelInit);

// Callbacks used by usrsctp
INT32 onSctpOutboundPacket(PVOID, PVOID, ULONG, UINT8, UINT8);
INT32 onSctpInboundPacket(struct socket*, union sctp_sockstore, PVOID, ULONG, struct sctp_rcvinfo, INT32, PVOID);

#ifdef __cplusplus
}
#endif
#endif //__KINESIS_VIDEO_WEBRTC_CLIENT_SCTP_SCTP__
