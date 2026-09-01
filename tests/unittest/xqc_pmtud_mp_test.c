/**
 * @copyright Copyright (c) 2026, mp0rta
 *
 * Per-path PMTU discovery on a multipath connection.
 *
 * The defect these pin down: xqc_conn_try_to_update_mss() applied the
 * cross-path minimum only when it was *larger* than the current connection
 * size, so conn->pkt_out_size could only ever rise. A path with a smaller
 * usable MTU could not lower it, and every packet built at the larger size
 * and scheduled onto that path was silently discarded by the first hop that
 * could not forward it -- with the loss fed to congestion control as
 * congestion. Pairing a 1500-MTU link with a 1400-MTU one (an ordinary
 * laptop: WiFi plus a tethered handset) was enough to trigger it, and when
 * the wide link went away the connection kept sizing packets for a path that
 * no longer existed.
 *
 * The tests drive xqc_conn_try_to_update_mss() directly against synthesized
 * paths, since the decision is pure function of the path list.
 */

#include <CUnit/CUnit.h>
#include "xquic/xquic.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/common/xqc_list.h"
#include "xqc_test_helpers.h"
#include "xqc_pmtud_mp_test.h"

#define XQC_TEST_PMTU_LIMIT     1400    /* what the application configured */
#define XQC_TEST_PMTU_CEILING   1420    /* XQC_MAX_PACKET_OUT_SIZE          */

/* Build the fixture connection used by every case below.
 *
 * xqc_timer_unset() logs through manager->log, which the calloc-zeroed
 * fixture leaves NULL, so the timer manager has to borrow the connection's
 * log before try_to_update_mss can be called at all. */
static xqc_connection_t *
pmtud_test_conn(void)
{
    xqc_connection_t *conn = xqc_test_helper_conn_create(NULL);
    if (conn == NULL) {
        return NULL;
    }

    conn->enable_pmtud = 1;
    conn->conn_settings.enable_pmtud = 1;
    conn->conn_settings.probing_pkt_out_size = XQC_TEST_PMTU_CEILING;
    conn->conn_settings.max_pkt_out_size = XQC_TEST_PMTU_LIMIT;
    conn->pkt_out_size_limit = XQC_TEST_PMTU_LIMIT;
    conn->pkt_out_size = XQC_TEST_PMTU_LIMIT;
    conn->max_pkt_out_size = XQC_TEST_PMTU_CEILING;
    conn->probing_pkt_out_size = XQC_TEST_PMTU_CEILING;
    conn->conn_timer_manager.log = conn->log;

    return conn;
}

/* Attach a path in the state the PMTU search would have left it in.
 * bounded == 0 models a path that has not finished probing. */
static xqc_path_ctx_t *
pmtud_test_path(xqc_connection_t *conn, uint64_t path_id, size_t curr,
                size_t max, int bounded)
{
    xqc_path_ctx_t *path =
        xqc_test_helper_path_synthesize(conn, path_id, XQC_PATH_STATE_ACTIVE);
    if (path == NULL) {
        return NULL;
    }

    path->curr_pkt_out_size = curr;
    path->path_max_pkt_out_size = max;
    path->path_probing_pkt_out_size = max;
    path->path_probing_cnt = 0;
    path->path_pmtu_bounded = bounded ? XQC_TRUE : XQC_FALSE;

    return path;
}

/* A path that has not established a limit says nothing about the connection.
 *
 * This is the case that makes the fix safe to deploy: PMTUD is only active
 * when both peers advertise it, so against a peer that does not, no path ever
 * becomes bounded. Were "not measured yet" treated as "cannot carry more than
 * the base", every such connection would be pinned to 1200 forever. */
void
xqc_test_pmtud_unprobed_path_does_not_lower_conn(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    xqc_path_ctx_t *path =
        pmtud_test_path(conn, 0, XQC_PACKET_OUT_SIZE, XQC_TEST_PMTU_CEILING, 0);
    CU_ASSERT_PTR_NOT_NULL_FATAL(path);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == XQC_TEST_PMTU_LIMIT);

    xqc_test_helper_path_destroy(path);
    xqc_test_helper_conn_destroy(conn);
}

/* The regression itself: a path proven to carry less than the connection is
 * sending must bring the connection down. Against the old code
 * conn->pkt_out_size stays at 1400 and every full-size packet on this path is
 * lost. */
void
xqc_test_pmtud_bounded_path_lowers_conn(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    /* 1372 is what a 1400-byte link leaves once IPv4 and UDP headers are on
     * the datagram. */
    xqc_path_ctx_t *path = pmtud_test_path(conn, 0, 1372, 1380, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(path);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == 1372);

    xqc_test_helper_path_destroy(path);
    xqc_test_helper_conn_destroy(conn);
}

/* With two bounded paths the connection has to fit the smaller: one buffer
 * size is used to build a packet that may be scheduled onto either. */
void
xqc_test_pmtud_conn_takes_min_of_bounded_paths(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    xqc_path_ctx_t *wide = pmtud_test_path(conn, 0, XQC_TEST_PMTU_LIMIT,
                                           XQC_TEST_PMTU_CEILING, 1);
    xqc_path_ctx_t *narrow = pmtud_test_path(conn, 1, 1272, 1280, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(wide);
    CU_ASSERT_PTR_NOT_NULL_FATAL(narrow);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == 1272);

    xqc_test_helper_path_destroy(wide);
    xqc_test_helper_path_destroy(narrow);
    xqc_test_helper_conn_destroy(conn);
}

/* Losing the narrow path must give the size back, which is the failover case:
 * a connection that stayed clamped to a link it is no longer using would keep
 * paying for it. Paths at or past CLOSING are excluded from the minimum. */
void
xqc_test_pmtud_closing_path_releases_conn(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    xqc_path_ctx_t *wide = pmtud_test_path(conn, 0, XQC_TEST_PMTU_LIMIT,
                                           XQC_TEST_PMTU_CEILING, 1);
    xqc_path_ctx_t *narrow = pmtud_test_path(conn, 1, 1272, 1280, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(wide);
    CU_ASSERT_PTR_NOT_NULL_FATAL(narrow);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == 1272);

    narrow->path_state = XQC_PATH_STATE_CLOSING;
    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == XQC_TEST_PMTU_LIMIT);

    xqc_test_helper_path_destroy(wide);
    xqc_test_helper_path_destroy(narrow);
    xqc_test_helper_conn_destroy(conn);
}

/* The recomputed size stays inside the band QUIC and the configuration allow:
 * never below the size every path must carry, never above what the
 * application asked for. */
void
xqc_test_pmtud_conn_size_clamped_to_floor_and_limit(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    /* Absurdly small bound -- must floor at the QUIC minimum rather than
     * follow the path down. */
    xqc_path_ctx_t *tiny = pmtud_test_path(conn, 0, 600, 620, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(tiny);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == XQC_PACKET_OUT_SIZE);

    /* A path bounded above the configured limit must not raise the
     * connection past it. */
    tiny->curr_pkt_out_size = XQC_TEST_PMTU_CEILING;
    tiny->path_max_pkt_out_size = XQC_TEST_PMTU_CEILING;
    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == XQC_TEST_PMTU_LIMIT);

    xqc_test_helper_path_destroy(tiny);
    xqc_test_helper_conn_destroy(conn);
}

/* The probe ceiling is the largest size any live path might still reach, so
 * it is a maximum over paths. It used to be captured inside the branch that
 * tracked the minimum, which took it from whichever path held the smallest
 * packet size and stalled the search everywhere else. */
void
xqc_test_pmtud_probe_ceiling_is_max_over_paths(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    /* The path with the smallest confirmed size also has the lowest ceiling;
     * the other path can still reach the full 1420. */
    xqc_path_ctx_t *narrow = pmtud_test_path(conn, 0, 1272, 1280, 1);
    xqc_path_ctx_t *wide = pmtud_test_path(conn, 1, 1300, XQC_TEST_PMTU_CEILING, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(narrow);
    CU_ASSERT_PTR_NOT_NULL_FATAL(wide);

    xqc_conn_try_to_update_mss(conn);

    CU_ASSERT(conn->pkt_out_size == 1272);
    CU_ASSERT(conn->max_pkt_out_size == XQC_TEST_PMTU_CEILING);

    xqc_test_helper_path_destroy(narrow);
    xqc_test_helper_path_destroy(wide);
    xqc_test_helper_conn_destroy(conn);
}

/* A round in which no probe went out is not a converged search.
 *
 * The timer must stay armed when every active path is still validating -- the
 * state a connection is in immediately after a failover -- or when the probe
 * writes fail transiently. Reading "no probe sent" as "search finished" leaves
 * the connection with no armed timer and PMTUD off for the rest of its life,
 * which would disable the discovery exactly when it is most needed. */
void
xqc_test_pmtud_probing_stays_armed_when_nothing_probed(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    /* Reach the probe loop: without this the function returns before it. */
    conn->conn_flag |= XQC_CONN_FLAG_CAN_SEND_1RTT;

    /* Validating, not active: no probe is written, so the fixture needs no
     * send queue. */
    xqc_path_ctx_t *path = xqc_test_helper_path_synthesize(
        conn, 0, XQC_PATH_STATE_VALIDATING);
    CU_ASSERT_PTR_NOT_NULL_FATAL(path);
    path->curr_pkt_out_size = XQC_PACKET_OUT_SIZE;
    path->path_max_pkt_out_size = XQC_TEST_PMTU_CEILING;
    path->path_probing_pkt_out_size = XQC_TEST_PMTU_CEILING;

    conn->conn_timer_manager.timer[XQC_TIMER_PMTUD_PROBING].timer_is_set = 0;

    xqc_conn_ptmud_probing(conn);

    CU_ASSERT(conn->conn_timer_manager.timer[XQC_TIMER_PMTUD_PROBING].timer_is_set != 0);

    xqc_test_helper_path_destroy(path);
    xqc_test_helper_conn_destroy(conn);
}

/* RFC 8899 5.2: persistent loss of everything in flight is how a PMTU black
 * hole announces itself. The path drops back to the guaranteed size and the
 * search reopens above it, so an MTU that shrinks mid-connection recovers
 * instead of discarding every oversized packet indefinitely. */
void
xqc_test_pmtud_blackhole_resets_path_to_base(void)
{
    xqc_connection_t *conn = pmtud_test_conn();
    CU_ASSERT_PTR_NOT_NULL_FATAL(conn);

    xqc_path_ctx_t *path = pmtud_test_path(conn, 0, XQC_TEST_PMTU_LIMIT,
                                           XQC_TEST_PMTU_CEILING, 1);
    CU_ASSERT_PTR_NOT_NULL_FATAL(path);

    xqc_conn_try_to_update_mss(conn);
    CU_ASSERT(conn->pkt_out_size == XQC_TEST_PMTU_LIMIT);

    /* xqc_send_ctl_on_pmtu_blackhole only reads ctl_conn and ctl_path, so a
     * stack send_ctl is enough and keeps the engine-less fixture usable. */
    xqc_send_ctl_t ctl;
    memset(&ctl, 0, sizeof(ctl));
    ctl.ctl_conn = conn;
    ctl.ctl_path = path;

    xqc_send_ctl_on_pmtu_blackhole(&ctl);

    CU_ASSERT(path->curr_pkt_out_size == XQC_PACKET_OUT_SIZE);
    CU_ASSERT(conn->pkt_out_size == XQC_PACKET_OUT_SIZE);
    /* The search must be reopened, otherwise the connection would be stuck at
     * the base for the rest of its life. */
    CU_ASSERT(path->path_max_pkt_out_size == XQC_TEST_PMTU_CEILING);
    CU_ASSERT(path->path_probing_cnt == 0);
    CU_ASSERT((conn->conn_flag & XQC_CONN_FLAG_PMTUD_PROBING) != 0);

    /* Already at the base: a second black hole is not about packet size and
     * must not be reported as a further reduction. */
    xqc_send_ctl_on_pmtu_blackhole(&ctl);
    CU_ASSERT(path->curr_pkt_out_size == XQC_PACKET_OUT_SIZE);

    xqc_test_helper_path_destroy(path);
    xqc_test_helper_conn_destroy(conn);
}
