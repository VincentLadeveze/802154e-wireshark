/* packet-ieee802154e.h
 *
 * $Id:$
 *
 * Routines for IEEE802.15.4e dissection
 * Copyright 2013, Pieter De Mil <pieter.demil@intec.ugent.be>
 *
 * IEEE 802.15.4e Dissectors for Wireshark
 * By Owen Kirby <osk@exegin.com>
 * Copyright 2007 Exegin Technologies Limited
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Copied from packet-ieee802154.h 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*   include baddly misplaced but necessary */
#include "packet-ieee802154.h"


#ifndef __PACKET_IEEE802154E_H__
#define __PACKET_IEEE802154E_H__

/* Protocol Abbreviation */
#define IEEE802154E_PROTOABBREV_WPAN     "wpane"
#define IEEE802154E_PROTOABBREV_WPAN_BEACON  "wpane.beacon"
/* PANID dissector list is for Decode-As and stateful dissection only. */
#define IEEE802154E_PROTOABBREV_WPAN_PANID   "wpane.panid"


/* Bit-masks for the IE list*/
#define IEEE802154_IELIST_TYPE_MASK			0x0001
#define IEEE802154_IELIST_TYPE0_ELEMENTID_MASK		0x01FE
#define IEEE802154_IELIST_TYPE0_LENGTH_MASK		0xFE00

#define IEEE802154_IELIST_TYPE1_GROUPID_MASK		0x001E
#define IEEE802154_IELIST_TYPE1_LENGTH_MASK		0xFFE0



#define IEEE802154_FCF_SEQNR_SURPRESSION    0x0100
#define IEEE802154_FCF_IELIST_PRESENT       0x0200

/* Frame version definitions. */
#define IEEE802154_VERSION_2012             0x2

/*  Structure containing information regarding all necessary packet fields. */
typedef struct {
    /* Frame control field. */
    gint32      version;
    gint32      frame_type;
    gint32      dst_addr_mode;
    gint32      src_addr_mode;
    gboolean    security_enable;
    gboolean    frame_pending;
    gboolean    ack_request;
    gboolean    intra_pan;
    gboolean    seqnr_surpression;
    gboolean    ielist_present;

    guint8      seqno;

    /* Addressing Info. */
    guint16     dst_pan;
    guint16     src_pan;
    guint16     dst16;
    guint64     dst64;
    guint16     src16;
    guint64     src64;

    /* Security Info. */
    ieee802154_security_level   security_level;
    ieee802154_key_id_mode      key_id_mode;
    guint32     frame_counter;
    guint8      key_sequence_counter;    /* Only for 802.15.4-2003 security suite with encryption */

    union {
        guint32 addr32;
        guint64 addr64;
    } key_source;
    guint8      key_index;

    /* Command ID (only if frame_type == 0x3) */
    guint8      command_id;
    GHashTable *short_table;
} ieee802154e_packet;

/* Structure for two-way mapping table */
typedef struct {
    GHashTable *long_table;
    GHashTable *short_table;
} ieee802154e_map_tab_t;

/* Key used by the short address hash table. */
typedef struct {
    guint16     pan;
    guint16     addr;
} ieee802154e_short_addr;

/* Key used by the long address hash table. */
typedef struct {
    guint64     addr;
} ieee802154e_long_addr;

/* A mapping record for a frame, pointed to by hash table */
typedef struct {
    const char *proto; /* name of protocol that created this record */
    guint       start_fnum;
    guint       end_fnum;
    guint64     addr64;
    /*guint32   frame_counter;   TODO for frame counter sequence checks. */
} ieee802154e_map_rec;

typedef struct {
    guint16             src_pan;
    guint16             src16;
    guint16             dst16;
    ieee802154e_map_rec *map_rec;
} ieee802154e_hints_t;

/* Short to Extended Address Prototypes */
extern ieee802154e_map_rec *ieee802154e_addr_update(ieee802154e_map_tab_t *, guint16, guint16, guint64,
        const char *, guint);
extern guint    ieee802154e_short_addr_hash(gconstpointer);
extern guint    ieee802154e_long_addr_hash(gconstpointer key);
extern gboolean ieee802154e_short_addr_equal(gconstpointer, gconstpointer);
extern gboolean ieee802154e_long_addr_equal(gconstpointer a, gconstpointer b);

extern gboolean ieee802154e_short_addr_invalidate(guint16, guint16, guint);
extern gboolean ieee802154e_long_addr_invalidate(guint64, guint);

#endif /* __PACKET_IEEE802154E_H__ */
