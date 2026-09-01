/**
 * @copyright Copyright (c) 2026, mp0rta
 *
 * Regression tests for per-path PMTU discovery on a multipath connection.
 */

#ifndef XQC_PMTUD_MP_TEST_H
#define XQC_PMTUD_MP_TEST_H

void xqc_test_pmtud_unprobed_path_does_not_lower_conn(void);
void xqc_test_pmtud_bounded_path_lowers_conn(void);
void xqc_test_pmtud_confirmed_paths_raise_conn(void);
void xqc_test_pmtud_conn_takes_min_of_bounded_paths(void);
void xqc_test_pmtud_closing_path_releases_conn(void);
void xqc_test_pmtud_conn_size_clamped_to_floor_and_limit(void);
void xqc_test_pmtud_probe_ceiling_is_max_over_paths(void);
void xqc_test_pmtud_probing_stays_armed_when_nothing_probed(void);
void xqc_test_pmtud_blackhole_resets_path_to_base(void);

#endif /* XQC_PMTUD_MP_TEST_H */
