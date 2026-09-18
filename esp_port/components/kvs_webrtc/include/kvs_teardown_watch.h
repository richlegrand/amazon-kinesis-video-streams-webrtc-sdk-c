/*
 * Breadcrumbs for peer connection teardown.
 *
 * Tearing a peer connection down takes seconds when it goes well and has been
 * seen not to return at all, which strands the session cleanup task and, with
 * it, every later connection attempt. The stages are spread across
 * kvs_pc_destroy_session, closePeerConnection, freePeerConnection and
 * iceAgentShutdown, and a stall leaves no trace: the device simply goes quiet.
 *
 * So each stage checks in here, and a timer reports any stage that overstays.
 * That turns a silent device into a line naming the stage and how long it has
 * been sitting there.
 *
 * Logged at WARN on purpose. The kvs_webrtc tag is pinned below INFO for the
 * sake of frame times, so anything quieter would be invisible in exactly the
 * run we need it for.
 *
 * One teardown is tracked at a time. Session cleanup is serial, so that covers
 * the case this exists for.
 */
#ifndef KVS_TEARDOWN_WATCH_H
#define KVS_TEARDOWN_WATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start tracking a teardown of peer. */
void kvs_teardown_watch_begin(const char *peer);

/* Note that the teardown has reached stage. Must be a string literal or other
 * pointer that outlives the teardown -- it is held, not copied. */
void kvs_teardown_watch_stage(const char *stage);

/* Teardown finished. Quiet again until the next begin. */
void kvs_teardown_watch_end(void);

#ifdef __cplusplus
}
#endif

#endif /* KVS_TEARDOWN_WATCH_H */
