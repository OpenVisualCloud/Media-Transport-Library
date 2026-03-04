/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: Fabrics target setup implementation
 */

#include "poc_fabrics_target.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int poc_fabrics_target_init(poc_fabrics_target_t *ft,
                            poc_mxl_sink_t *sink,
                            const poc_config_t *cfg)
{
    memset(ft, 0, sizeof(*ft));
    mxlStatus st;

    /* ── Create fabrics instance ── */
    st = mxlFabricsCreateInstance(sink->instance, &ft->fab_instance);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[TARGET] mxlFabricsCreateInstance failed: %d\n", st);
        return -1;
    }

    /* ── Create target ── */
    st = mxlFabricsCreateTarget(ft->fab_instance, &ft->target);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[TARGET] mxlFabricsCreateTarget failed: %d\n", st);
        goto err;
    }

    /* ── Get memory regions from the FlowWriter ── */
    st = mxlFabricsRegionsForFlowWriter(sink->writer, &ft->regions);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[TARGET] mxlFabricsRegionsForFlowWriter failed: %d\n", st);
        goto err;
    }

    /* ── Resolve provider ── */
    mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
    if (cfg->fabrics_provider[0]) {
        st = mxlFabricsProviderFromString(cfg->fabrics_provider, &provider);
        if (st != MXL_STATUS_OK) {
            fprintf(stderr, "[TARGET] Unknown provider '%s', defaulting to TCP\n",
                    cfg->fabrics_provider);
            provider = MXL_FABRICS_PROVIDER_TCP;
        }
    }

    /* ── Setup target ── */
    mxlFabricsTargetConfig target_cfg = {
        .endpointAddress = {
            .node    = cfg->fabrics_local_ip,
            .service = cfg->fabrics_local_port,
        },
        .provider      = provider,
        .regions       = ft->regions,
        .deviceSupport = false,
    };

    st = mxlFabricsTargetSetup(ft->target, &target_cfg, &ft->target_info);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[TARGET] mxlFabricsTargetSetup failed: %d\n", st);
        goto err;
    }

    /* ── Serialize targetInfo to string ── */
    ft->target_info_len = 0;
    st = mxlFabricsTargetInfoToString(ft->target_info, NULL, &ft->target_info_len);
    if (st != MXL_STATUS_OK && ft->target_info_len == 0) {
        fprintf(stderr, "[TARGET] mxlFabricsTargetInfoToString (size query) failed: %d\n", st);
        goto err;
    }

    ft->target_info_str = (char *)malloc(ft->target_info_len + 1);
    if (!ft->target_info_str) {
        fprintf(stderr, "[TARGET] malloc for targetInfo string failed\n");
        goto err;
    }

    st = mxlFabricsTargetInfoToString(ft->target_info, ft->target_info_str,
                                       &ft->target_info_len);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[TARGET] mxlFabricsTargetInfoToString failed: %d\n", st);
        goto err;
    }
    ft->target_info_str[ft->target_info_len] = '\0';

    /* ── Print for the sender to consume ── */
    printf("[TARGET] ═══════════════ TARGET INFO (copy to sender) ═══════════════\n");
    printf("%s\n", ft->target_info_str);
    printf("[TARGET] ═══════════════════════════════════════════════════════════\n");
    fflush(stdout);

    return 0;

err:
    poc_fabrics_target_destroy(ft);
    return -1;
}

void poc_fabrics_target_destroy(poc_fabrics_target_t *ft)
{
    if (ft->target_info) {
        mxlFabricsFreeTargetInfo(ft->target_info);
        ft->target_info = NULL;
    }
    if (ft->target && ft->fab_instance) {
        mxlFabricsDestroyTarget(ft->fab_instance, ft->target);
        ft->target = NULL;
    }
    if (ft->regions) {
        mxlFabricsRegionsFree(ft->regions);
        ft->regions = NULL;
    }
    if (ft->fab_instance) {
        mxlFabricsDestroyInstance(ft->fab_instance);
        ft->fab_instance = NULL;
    }
    if (ft->target_info_str) {
        free(ft->target_info_str);
        ft->target_info_str = NULL;
    }
}
