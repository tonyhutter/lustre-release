 /* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2002, 2003 Cluster File Systems, Inc.
 * Author: Phil Schwan <phil@clusterfs.com>
 *         Peter Braam <braam@clusterfs.com>
 *         Mike Shaver <shaver@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_LOV
#ifdef __KERNEL__
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <asm/div64.h>
#include <linux/seq_file.h>
#else
#include <liblustre.h>
#endif

#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_net.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_dlm.h>
#include <linux/lustre_mds.h>
#include <linux/obd_class.h>
#include <linux/obd_lov.h>
#include <linux/obd_ost.h>
#include <linux/lprocfs_status.h>

#include "lov_internal.h"

/* Add log records for each OSC that this object is striped over, and return
 * cookies for each one.  We _would_ have nice abstraction here, except that
 * we need to keep cookies in stripe order, even if some are NULL, so that
 * the right cookies are passed back to the right OSTs at the client side.
 * Unset cookies should be all-zero (which will never occur naturally). */
static int lov_llog_origin_add(struct llog_ctxt *ctxt, struct llog_rec_hdr *rec,
                               void *buf, struct llog_cookie *logcookies, 
                               int numcookies, void *data,
                               struct rw_semaphore **lock, int *lock_count)
{
        struct obd_device *obd = ctxt->loc_obd;
        struct lov_obd *lov = &obd->u.lov;
        struct lov_oinfo *loi;
        struct llog_unlink_rec *lur;
        struct lov_stripe_md *lsm = (struct lov_stripe_md *)buf;
        int i, rc = 0;
        ENTRY;

        OBD_ALLOC(lur, sizeof(*lur));
        if (!lur)
                RETURN(-ENOMEM);
        lur->lur_hdr.lrh_len = lur->lur_tail.lrt_len = sizeof(*lur);
        lur->lur_hdr.lrh_type = MDS_UNLINK_REC;

        LASSERT(logcookies && numcookies >= lsm->lsm_stripe_count);

        /* We need this to serialize llog records between parallel unlinks so
         * we can replay llog records in strict transno and llog order.  If
         * and when we want to make this more scalable we need to lock and
         * write records in strictly ost_idx order not lsm order.  Consider
         * file 1 on ost_idx [1, 2, 3, 4] and file 2 on ost_idx [3, 4, 1, 2] */
        down(&lov->lov_llog_sem);
        for (i = 0,loi = lsm->lsm_oinfo; i < lsm->lsm_stripe_count; i++,loi++) {
                struct obd_device *child =
                        lov->tgts[loi->loi_ost_idx].ltd_exp->exp_obd; 
                struct llog_ctxt *cctxt;
                cctxt = llog_get_context(&child->obd_llogs, ctxt->loc_idx);

                lur->lur_oid = loi->loi_id;
                lur->lur_ogen = loi->loi_gr;
                LASSERT(lsm->lsm_object_gr == loi->loi_gr);
                rc += llog_add(cctxt, &lur->lur_hdr, NULL, logcookies + rc,
                               numcookies - rc, NULL,
                               lock != NULL ? lock + rc : NULL, lock_count);
        }
        up(&lov->lov_llog_sem);
        OBD_FREE(lur, sizeof(*lur));

        RETURN(rc);
}

static int lov_llog_origin_connect(struct llog_ctxt *ctxt, int count,
                                   struct llog_logid *logid,
                                   struct llog_gen *gen, struct obd_uuid *uuid)
{
        struct obd_device *obd = ctxt->loc_obd;
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int i, rc = 0;
        ENTRY;

        LASSERT(lov->desc.ld_tgt_count  == count);
        for (i = 0, tgt = lov->tgts; i < lov->desc.ld_tgt_count; i++, tgt++) {
                struct obd_device *child;
                struct llog_ctxt *cctxt;

                if (!tgt->active)
                        continue;
                child = tgt->ltd_exp->exp_obd;

                cctxt = llog_get_context(&child->obd_llogs, ctxt->loc_idx);
                if (uuid && !obd_uuid_equals(uuid, &lov->tgts[i].uuid))
                        continue;

                rc = llog_connect(cctxt, 1, logid, gen, uuid);
                if (rc) {
                        CERROR("error osc_llog_connect %d\n", i);
                        break;
                }
        }

        RETURN(rc);
}

/* the replicators commit callback */
static int lov_llog_repl_cancel(struct llog_ctxt *ctxt, int count, 
                                struct llog_cookie *cookies, int flags,
                                void *data)
{
        struct lov_stripe_md *lsm = (struct lov_stripe_md *)data;
        struct lov_obd *lov;
        struct obd_device *obd = ctxt->loc_obd;
        struct lov_oinfo *loi;
        int rc = 0, i;
        ENTRY;

        LASSERT(lsm != NULL);
        LASSERT(count == lsm->lsm_stripe_count);

        loi = lsm->lsm_oinfo;
        lov = &obd->u.lov;
        for (i = 0; i < count; i++, cookies++, loi++) {
                struct obd_device *child =
                        lov->tgts[loi->loi_ost_idx].ltd_exp->exp_obd; 
                struct llog_ctxt *cctxt;
                int err;

                cctxt = llog_get_context(&child->obd_llogs, ctxt->loc_idx);
                err = llog_cancel(cctxt, 1, cookies, flags, NULL);
                if (err && lov->tgts[loi->loi_ost_idx].active) {
                        CERROR("error: objid "LPX64" subobj "LPX64
                               " on OST idx %d: rc = %d\n", lsm->lsm_object_id,
                               loi->loi_id, loi->loi_ost_idx, err);
                        if (!rc)
                                rc = err;
                }
        }
        RETURN(rc);
}

static struct llog_operations lov_unlink_orig_logops = {
        lop_add: lov_llog_origin_add,
        lop_connect: lov_llog_origin_connect
};

static struct llog_operations lov_size_repl_logops = {
        lop_cancel: lov_llog_repl_cancel
};

int lov_llog_init(struct obd_device *obd, struct obd_llogs *llogs,
                  struct obd_device *tgt, int count, struct llog_catid *logid)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *ctgt;
        int i, rc = 0;
        ENTRY;
        
        rc = obd_llog_setup(obd, llogs, LLOG_UNLINK_ORIG_CTXT, tgt, 0, NULL,
                            &lov_unlink_orig_logops);
        if (rc)
                RETURN(rc);

        rc = obd_llog_setup(obd, llogs, LLOG_SIZE_REPL_CTXT, tgt, 0, NULL, 
                            &lov_size_repl_logops);
        if (rc)
                RETURN(rc);

        LASSERT(lov->desc.ld_tgt_count  == count);
        for (i = 0, ctgt = lov->tgts; i < lov->desc.ld_tgt_count; i++, ctgt++) {
                struct obd_device *child;

                if (!ctgt->active)
                        continue;
                child = ctgt->ltd_exp->exp_obd;
                rc = obd_llog_init(child, &child->obd_llogs, tgt, 1, logid + i);
                if (rc) {
                        CERROR("error osc_llog_init %d\n", i);
                        break;
                }
        }
        RETURN(rc);
}

int lov_llog_finish(struct obd_device *obd, struct obd_llogs *llogs, int count)
{
        struct lov_obd *lov = &obd->u.lov;
        struct lov_tgt_desc *tgt;
        int i, rc = 0;
        ENTRY;

        rc = obd_llog_cleanup(llog_get_context(llogs, LLOG_UNLINK_ORIG_CTXT));
        if (rc)
                RETURN(rc);

        rc = obd_llog_cleanup(llog_get_context(llogs, LLOG_SIZE_REPL_CTXT));
        if (rc)
                RETURN(rc);

        if (lov->desc.ld_tgt_count != count) {
                CERROR("LOV tgt count != passed tgt count (%d != %d)\n",
                       lov->desc.ld_tgt_count, count);
                count = MIN(lov->desc.ld_tgt_count, count);
        }
        for (i = 0, tgt = lov->tgts; i < count; i++, tgt++) {
                struct obd_device *child;

                if (!tgt->active)
                        continue;
                child = tgt->ltd_exp->exp_obd;
                rc = obd_llog_finish(child, &child->obd_llogs, 1);
                if (rc) {
                        CERROR("osc_llog_finish error; index=%d; rc=%d\n",
                               i, rc);
                        break;
                }
        }
        RETURN(rc);
}
