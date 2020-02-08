/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.  A copy is
 * included in the COPYING file that accompanied this code.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * GPL HEADER END
 */

#ifndef __LIBCFS_LINUX_LIST_H__
#define __LIBCFS_LINUX_LIST_H__

#include <linux/list.h>

#ifdef HAVE_HLIST_ADD_AFTER
#define hlist_add_behind(hnode, tail)	hlist_add_after(tail, hnode)
#endif /* HAVE_HLIST_ADD_AFTER */

#endif /* __LIBCFS_LINUX_LIST_H__ */
