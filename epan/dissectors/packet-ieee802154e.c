/* packet-ieee802154e.c
 *
 * $Id$
 *
 * IEEE 802.15.4e Dissectors for Wireshark
 * By Pieter De Mil <pieter@demil.be>
 * Copyright 2013 iMinds - Ghent University - IBCN
 *
 * Auxiliary Security Header support and
 * option to force TI CC24xx FCS format
 * By Jean-Francois Wauthy <jfw@info.fundp.ac.be>
 * Copyright 2009 The University of Namur, Belgium
 *
 * IEEE 802.15.4 Dissectors for Wireshark
 * By Owen Kirby <osk@exegin.com>
 * Copyright 2007 Exegin Technologies Limited
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
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
 *------------------------------------------------------------
 *
 *  In IEEE 802.15.4e packets, all fields are little endian. And
 *  Each byte is transmitted least significant bit first (reflected
 *  bit ordering).
 *------------------------------------------------------------
 *
 *  IEEE 802.15.4e Packets have the following format:
 *  |  FCF  |Seq No|  Addressing |         Data          |  FCS  |
 *  |2 bytes|1 byte|0 to 20 bytes|Length-(Overhead) bytes|2 Bytes|
 *------------------------------------------------------------
 *
 *  CRC16 is calculated using the x^16 + x^12 + x^5 + 1 polynomial
 *  as specified by ITU-T, and is calculated over the IEEE 802.15.4
 *  packet (excluding the FCS) as transmitted over the air. Note,
 *  that because the least significant bits are transmitted first, this
 *  will require reversing the bit-order in each byte. Also, unlike
 *  most CRC algorithms, IEEE 802.15.4 uses an initial and final value
 *  of 0x0000, instead of 0xffff (which is used by the CCITT).
 *------------------------------------------------------------
 *
 *  This dissector supports both link-layer IEEE 802.15.4e captures
 *  and IEEE 802.15.4e packets encapsulated within other layers.
 *  Additionally, support has been provided for various formats
 *  of the frame check sequence:
 *      - IEEE 802.15.4 compliant FCS.
 *      - ChipCon/Texas Instruments CC24xx style FCS.
 *------------------------------------------------------------
 */

/*  Include files */
#include "config.h"

#include <string.h>

#include <sys/stat.h>

#include <glib.h>

#include <epan/wmem/wmem.h>
#include <epan/decode_as.h>
#include <epan/packet.h>
#include <epan/exceptions.h>
#include <wsutil/pint.h>
#include <epan/crc16-tvb.h>
#include <epan/expert.h>
#include <epan/addr_resolv.h>
#include <epan/prefs.h>
#include <epan/uat.h>
#include <epan/strutil.h>
#include <epan/show_exception.h>

/* Use libgcrypt for cipher libraries. */
#ifdef HAVE_LIBGCRYPT
#include <wsutil/wsgcrypt.h>
#endif /* HAVE_LIBGCRYPT */

#include "packet-ieee802154e.h"
#include "packet-sll.h"

/* Dissection Options for dissect_ieee802154e_common */
#define DISSECT_IEEE802154E_OPTION_CC24xx    0x00000001  /* FCS field contains a TI CC24xx style FCS. */
#define DISSECT_IEEE802154E_OPTION_LINUX     0x00000002  /* Addressing fields are padded DLT_IEEE802_15_4_LINUX, not implemented. */

/* ethertype for 802.15.4 tag - encapsulating an Ethernet packet */
static unsigned int ieee802154_ethertype = 0x809A;

/* boolean value set if the FCS field is using the TI CC24xx format */
static gboolean ieee802154e_cc24xx = FALSE;

/* boolean value set if the FCS must be ok before payload is dissected */
static gboolean ieee802154e_fcs_ok = TRUE;

/* User string with the decryption key. */
static const gchar *ieee802154e_key_str = NULL;
static gboolean     ieee802154e_key_valid;
static guint8       ieee802154e_key[IEEE802154_CIPHER_SIZE];
static const char  *ieee802154e_user    = "User";

/*-------------------------------------
 * Address Hash Tables
 *-------------------------------------
 */
static ieee802154e_map_tab_t ieee802154e_map = { NULL, NULL };

/*-------------------------------------
 * Static Address Mapping UAT
 *-------------------------------------
 */
/* UAT entry structure. */
typedef struct {
    guchar *eui64;
    guint   eui64_len;
    guint   addr16;
    guint   pan;
} static_addr_t;

/* UAT variables */
static uat_t         *static_addr_uat  = NULL;
static static_addr_t *static_addrs     = NULL;
static guint          num_static_addrs = 0;

/* Sanity-checks a UAT record. */
static void
addr_uat_update_cb(void *r, const char **err)
{
    static_addr_t *map = (static_addr_t *)r;
    /* Ensure a valid short address */
    if (map->addr16 >= IEEE802154_NO_ADDR16) {
        *err = g_strdup("Invalid short address");
        return;
    }
    /* Ensure a valid PAN identifier. */
    if (map->pan >= IEEE802154_BCAST_PAN) {
        *err = g_strdup("Invalid PAN identifier");
        return;
    }
    /* Ensure a valid EUI-64 length */
    if (map->eui64_len != sizeof(guint64)) {
        *err = g_strdup("Invalid EUI-64 length");
        return;
    }
} /* ieee802154e_addr_uat_update_cb */

/* Field callbacks. */
UAT_HEX_CB_DEF(addr_uat, addr16, static_addr_t)
UAT_HEX_CB_DEF(addr_uat, pan, static_addr_t)
UAT_BUFFER_CB_DEF(addr_uat, eui64, static_addr_t, eui64, eui64_len)

/*-------------------------------------
 * Dissector Function Prototypes
 *-------------------------------------
 */
/* Register Functions. Loads the dissector into Wireshark. */
void proto_register_ieee802154e(void);
void proto_reg_handoff_ieee802154e(void);

/* Dissection Routines. */
static void dissect_ieee802154e_nonask_phy   (tvbuff_t *, packet_info *, proto_tree *);
static void dissect_ieee802154e              (tvbuff_t *, packet_info *, proto_tree *);
static void dissect_ieee802154e_nofcs        (tvbuff_t *, packet_info *, proto_tree *);
static void dissect_ieee802154e_cc24xx       (tvbuff_t *, packet_info *, proto_tree *);
/*static void dissect_ieee802154e_linux        (tvbuff_t *, packet_info *, proto_tree *);  TODO: Implement Me. */
static void dissect_ieee802154e_common       (tvbuff_t *, packet_info *, proto_tree *, guint);

/* Sub-dissector helpers. */
static void dissect_ieee802154e_fcf          (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *, guint *);
static void dissect_ieee802154e_superframe   (tvbuff_t *, packet_info *, proto_tree *, guint *);
static void dissect_ieee802154e_gtsinfo      (tvbuff_t *, packet_info *, proto_tree *, guint *);
static void dissect_ieee802154e_pendaddr     (tvbuff_t *, packet_info *, proto_tree *, guint *);
static void dissect_ieee802154e_command      (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_assoc_req    (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_assoc_rsp    (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_disassoc     (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_realign      (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_gtsreq       (tvbuff_t *, packet_info *, proto_tree *, ieee802154e_packet *);
static void dissect_ieee802154e_ielist	     (tvbuff_t *, packet_info *, proto_tree *, /*ieee802154e_packet *,*/ guint *);


/* Decryption helpers. */
typedef enum {
    DECRYPT_PACKET_SUCCEEDED,
    DECRYPT_NOT_ENCRYPTED,
    DECRYPT_VERSION_UNSUPPORTED,
    DECRYPT_PACKET_TOO_SMALL,
    DECRYPT_PACKET_NO_EXT_SRC_ADDR,
    DECRYPT_PACKET_NO_KEY,
    DECRYPT_PACKET_DECRYPT_FAILED,
    DECRYPT_PACKET_MIC_CHECK_FAILED
} ws_decrypt_status;

static tvbuff_t * dissect_ieee802154e_decrypt(tvbuff_t *, guint, packet_info *, ieee802154e_packet *,
        ws_decrypt_status *);
static void ccm_init_block          (gchar *, gboolean, gint, guint64, ieee802154e_packet *, gint);
static gboolean ccm_ctr_encrypt     (const gchar *, const gchar *, gchar *, gchar *, gint);
static gboolean ccm_cbc_mac         (const gchar *, const gchar *, const gchar *, gint, const gchar *, gint, gchar *);

/*  Initialize Protocol and Registered fields */
static int proto_ieee802154e_nonask_phy = -1;
static int hf_ieee802154e_nonask_phy_preamble = -1;
static int hf_ieee802154e_nonask_phy_sfd = -1;
static int hf_ieee802154e_nonask_phy_length = -1;

static int proto_ieee802154e = -1;
static int hf_ieee802154e_frame_length = -1;
static int hf_ieee802154e_frame_type = -1;
static int hf_ieee802154e_security = -1;
static int hf_ieee802154e_pending = -1;
static int hf_ieee802154e_ack_request = -1;
static int hf_ieee802154e_intra_pan = -1;
static int hf_ieee802154e_seqnr_surpression = -1;
static int hf_ieee802154e_ielist_present = -1;
static int hf_ieee802154e_seqno = -1;
static int hf_ieee802154e_src_addr_mode = -1;
static int hf_ieee802154e_dst_addr_mode = -1;
static int hf_ieee802154e_version = -1;
static int hf_ieee802154e_dst_panID = -1;
static int hf_ieee802154e_dst16 = -1;
static int hf_ieee802154e_dst64 = -1;
static int hf_ieee802154e_src_panID = -1;
static int hf_ieee802154e_src16 = -1;
static int hf_ieee802154e_src64 = -1;
static int hf_ieee802154e_src64_origin = -1;
static int hf_ieee802154e_fcs = -1;
static int hf_ieee802154e_rssi = -1;
static int hf_ieee802154e_fcs_ok = -1;
static int hf_ieee802154e_correlation = -1;

/*  Registered fields for Command Packets */
static int hf_ieee802154e_cmd_id = -1;
static int hf_ieee802154e_cinfo_alt_coord = -1;
static int hf_ieee802154e_cinfo_device_type = -1;
static int hf_ieee802154e_cinfo_power_src = -1;
static int hf_ieee802154e_cinfo_idle_rx = -1;
static int hf_ieee802154e_cinfo_sec_capable = -1;
static int hf_ieee802154e_cinfo_alloc_addr = -1;
static int hf_ieee802154e_assoc_addr = -1;
static int hf_ieee802154e_assoc_status = -1;
static int hf_ieee802154e_disassoc_reason = -1;
static int hf_ieee802154e_realign_pan = -1;
static int hf_ieee802154e_realign_caddr = -1;
static int hf_ieee802154e_realign_channel = -1;
static int hf_ieee802154e_realign_addr = -1;
static int hf_ieee802154e_realign_channel_page = -1;
static int hf_ieee802154e_gtsreq_len = -1;
static int hf_ieee802154e_gtsreq_dir = -1;
static int hf_ieee802154e_gtsreq_type = -1;

/*  Registered fields for Beacon Packets */
static int hf_ieee802154e_beacon_order = -1;
static int hf_ieee802154e_superframe_order = -1;
static int hf_ieee802154e_cap = -1;
static int hf_ieee802154e_superframe_battery_ext = -1;
static int hf_ieee802154e_superframe_coord = -1;
static int hf_ieee802154e_assoc_permit = -1;
static int hf_ieee802154e_gts_count = -1;
static int hf_ieee802154e_gts_permit = -1;
static int hf_ieee802154e_gts_direction = -1;
static int hf_ieee802154e_pending16 = -1;
static int hf_ieee802154e_pending64 = -1;
static int hf_ieee802154e_hie_length = -1;
static int hf_ieee802154e_hie_elementID = -1;
static int hf_ieee802154e_pie_length = -1;
static int hf_ieee802154e_pie_groupID = -1;

/*  Registered fields for Auxiliary Security Header */
static int hf_ieee802154e_security_level = -1;
static int hf_ieee802154e_key_id_mode = -1;
static int hf_ieee802154e_aux_sec_reserved = -1;
static int hf_ieee802154e_aux_sec_frame_counter = -1;
static int hf_ieee802154e_aux_sec_key_source = -1;
static int hf_ieee802154e_aux_sec_key_index = -1;

/* 802.15.4-2003 security */
static int hf_ieee802154e_sec_frame_counter = -1;
static int hf_ieee802154e_sec_key_sequence_counter = -1;

/*  Initialize Subtree Pointers */
static gint ett_ieee802154e_nonask_phy = -1;
static gint ett_ieee802154e_nonask_phy_phr = -1;
static gint ett_ieee802154e = -1;
static gint ett_ieee802154e_fcf = -1;
static gint ett_ieee802154e_auxiliary_security = -1;
static gint ett_ieee802154e_aux_sec_control = -1;
static gint ett_ieee802154e_aux_sec_key_id = -1;
static gint ett_ieee802154e_fcs = -1;
static gint ett_ieee802154e_cmd = -1;
static gint ett_ieee802154e_superframe = -1;
static gint ett_ieee802154e_gts = -1;
static gint ett_ieee802154e_gts_direction = -1;
static gint ett_ieee802154e_gts_descriptors = -1;
static gint ett_ieee802154e_pendaddr = -1;
static gint ett_ieee802154e_header_ie = -1;
static gint ett_ieee802154e_payload_ie = -1;

static expert_field ei_ieee802154e_invalid_addressing = EI_INIT;
static expert_field ei_ieee802154e_fcs = EI_INIT;
static expert_field ei_ieee802154e_decrypt_error = EI_INIT;
static expert_field ei_ieee802154e_dst = EI_INIT;
static expert_field ei_ieee802154e_src = EI_INIT;

/*  Dissector handles */
static dissector_handle_t       data_handle;
static dissector_table_t        panid_dissector_table;
static heur_dissector_list_t    ieee802154e_beacon_subdissector_list;
static heur_dissector_list_t    ieee802154e_heur_subdissector_list;

/* Name Strings */
static const value_string ieee802154e_frame_types[] = {
    { IEEE802154_FCF_BEACON,    "Beacon" },
    { IEEE802154_FCF_DATA,      "Data" },
    { IEEE802154_FCF_ACK,       "Ack" },
    { IEEE802154_FCF_CMD,       "Command" },
    { 0, NULL }
};

static const value_string ieee802154e_addr_modes[] = {
    { IEEE802154_FCF_ADDR_NONE, "None" },
    { IEEE802154_FCF_ADDR_SHORT,"Short/16-bit" },
    { IEEE802154_FCF_ADDR_EXT,  "Long/64-bit" },
    { 0, NULL }
};

static const value_string ieee802154e_cmd_names[] = {
    { IEEE802154_CMD_ASRQ,      "Association Request" },
    { IEEE802154_CMD_ASRSP,     "Association Response" },
    { IEEE802154_CMD_DISAS,     "Disassociation Notification" },
    { IEEE802154_CMD_DATA_RQ,   "Data Request" },
    { IEEE802154_CMD_PANID_ERR, "PAN ID Conflict" },
    { IEEE802154_CMD_ORPH_NOTIF,"Orphan Notification" },
    { IEEE802154_CMD_BCN_RQ,    "Beacon Request" },
    { IEEE802154_CMD_COORD_REAL,"Coordinator Realignment" },
    { IEEE802154_CMD_GTS_REQ,   "GTS Request" },
    { 0, NULL }
};

static const value_string ieee802154e_sec_level_names[] = {
    { SECURITY_LEVEL_NONE,        "No Security" },
    { SECURITY_LEVEL_MIC_32,      "32-bit Message Integrity Code" },
    { SECURITY_LEVEL_MIC_64,      "64-bit Message Integrity Code" },
    { SECURITY_LEVEL_MIC_128,     "128-bit Message Integrity Code" },
    { SECURITY_LEVEL_ENC,         "Encryption" },
    { SECURITY_LEVEL_ENC_MIC_32,  "Encryption with 32-bit Message Integrity Code" },
    { SECURITY_LEVEL_ENC_MIC_64,  "Encryption with 64-bit Message Integrity Code" },
    { SECURITY_LEVEL_ENC_MIC_128, "Encryption with 128-bit Message Integrity Code" },
    { 0, NULL }
};

static const value_string ieee802154e_key_id_mode_names[] = {
    { KEY_ID_MODE_IMPLICIT,       "Implicit Key" },
    { KEY_ID_MODE_KEY_INDEX,      "Indexed Key using the Default Key Source" },
    { KEY_ID_MODE_KEY_EXPLICIT_4, "Explicit Key with 4-octet Key Source" },
    { KEY_ID_MODE_KEY_EXPLICIT_8, "Explicit Key with 8-octet Key Source" },
    { 0, NULL }
};

static const true_false_string ieee802154e_gts_direction_tfs = {
    "Receive Only",
    "Transmit Only"
};

/* The 802.15.4-2003 security suites for the security preferences (only AES-CCM suites are supported). */
/* NOTE: The equivalent 2006 security level identifer enumerations are used to simplify 2003 & 2006 integration! */
static const enum_val_t ieee802154e_2003_sec_suite_enums[] = {
    { "AES-CCM-128", "AES-128 Encryption, 128-bit Integrity Protection", SECURITY_LEVEL_ENC_MIC_128 },
    { "AES-CCM-64",  "AES-128 Encryption, 64-bit Integrity Protection",  SECURITY_LEVEL_ENC_MIC_64 },
    { "AES-CCM-32",  "AES-128 Encryption, 32-bit Integrity Protection",  SECURITY_LEVEL_ENC_MIC_32 },
    { NULL, NULL, 0 }
};

/* Preferences for 2003 security */
static gint ieee802154e_sec_suite = SECURITY_LEVEL_ENC_MIC_64;
static gboolean ieee802154e_extend_auth = TRUE;

/* Macro to check addressing, and throw a warning flag if incorrect. */
#define IEEE802154_CMD_ADDR_CHECK(_pinfo_, _item_, _cmdid_, _x_)     \
   if (!(_x_))                                                       \
     expert_add_info_format(_pinfo_, _item_, &ei_ieee802154e_invalid_addressing, \
                            "Invalid Addressing for %s",             \
                            val_to_str_const(_cmdid_, ieee802154e_cmd_names, "Unknown Command"))

/* CRC definitions. IEEE 802.15.4 CRCs vary from CCITT by using an initial value of
 * 0x0000, and no XOR out. IEEE802154_CRC_XOR is defined as 0xFFFF in order to un-XOR
 * the output from the CCITT CRC routines in Wireshark.
 */
#define IEEE802154_CRC_SEED     0x0000
#define IEEE802154_CRC_XOROUT   0xFFFF
#define ieee802154e_crc_tvb(tvb, offset)   (crc16_ccitt_tvb_seed(tvb, offset, IEEE802154_CRC_SEED) ^ IEEE802154_CRC_XOROUT)


/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_fcf
 *  DESCRIPTION
 *      Dissector helper, parses and displays the frame control
 *      field.
 *
 *  PARAMETERS
 *      ieee802154e_packet   *packet - Packet info structure.
 *      tvbuff_t    *tvb    - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree  *tree   - pointer to data tree wireshark uses to display packet.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *      guint       offset  - offset into the tvb to find the FCF.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_fcf(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet, guint *offset)
{
    guint16     fcf;
    proto_tree *field_tree;
    proto_item *ti;

    /* Get the FCF field. */
    fcf = tvb_get_letohs(tvb, *offset);

     /* Parse FCF Flags. */
    packet->frame_type        = fcf & IEEE802154_FCF_TYPE_MASK;
    packet->security_enable   = fcf & IEEE802154_FCF_SEC_EN;
    packet->frame_pending     = fcf & IEEE802154_FCF_FRAME_PND;
    packet->ack_request       = fcf & IEEE802154_FCF_ACK_REQ;
    packet->intra_pan         = fcf & IEEE802154_FCF_INTRA_PAN;
    packet->seqnr_surpression = fcf & IEEE802154_FCF_SEQNR_SURPRESSION;    
    packet->ielist_present    = fcf & IEEE802154_FCF_IELIST_PRESENT;
    packet->version           = (fcf & IEEE802154_FCF_VERSION) >> 12;
    packet->dst_addr_mode     = (fcf & IEEE802154_FCF_DADDR_MASK) >> 10;
    packet->src_addr_mode     = (fcf & IEEE802154_FCF_SADDR_MASK) >> 14;

    /* Display the frame type. */
    proto_item_append_text(tree, " %s", val_to_str_const(packet->frame_type, ieee802154e_frame_types, "Reserved"));
    col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(packet->frame_type, ieee802154e_frame_types, "Reserved"));

    /* Add the FCF to the protocol tree. */
    if (tree) {
        /*  Create the FCF subtree. */
        ti = proto_tree_add_text(tree, tvb, *offset, 2, "Frame Control Field: %s (0x%04x)",
                val_to_str_const(packet->frame_type, ieee802154e_frame_types, "Unknown"), fcf);
        field_tree = proto_item_add_subtree(ti, ett_ieee802154e_fcf);

        /* FCF Fields. */
        proto_tree_add_uint(field_tree, hf_ieee802154e_frame_type, tvb, *offset, 1, fcf & IEEE802154_FCF_TYPE_MASK);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_security, tvb, *offset, 1, fcf & IEEE802154_FCF_SEC_EN);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_pending, tvb, *offset, 1, fcf & IEEE802154_FCF_FRAME_PND);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_ack_request, tvb, *offset, 1, fcf & IEEE802154_FCF_ACK_REQ);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_intra_pan, tvb, *offset, 1, fcf & IEEE802154_FCF_INTRA_PAN);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_seqnr_surpression, tvb, *offset, 1, fcf & IEEE802154_FCF_SEQNR_SURPRESSION);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_ielist_present, tvb, *offset, 1, fcf & IEEE802154_FCF_IELIST_PRESENT);
        proto_tree_add_uint(field_tree, hf_ieee802154e_dst_addr_mode, tvb, (*offset)+1, 1, fcf & IEEE802154_FCF_DADDR_MASK);
        proto_tree_add_uint(field_tree, hf_ieee802154e_version, tvb, (*offset)+1, 1, fcf & IEEE802154_FCF_VERSION);
        proto_tree_add_uint(field_tree, hf_ieee802154e_src_addr_mode, tvb, (*offset)+1, 1, fcf & IEEE802154_FCF_SADDR_MASK);
    }

    *offset += 2;
} /* dissect_ieee802154e_fcf */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_nonask_phy
 *  DESCRIPTION
 *      Dissector for IEEE 802.15.4 non-ASK PHY packet with an FCS containing
 *      a 16-bit CRC value.
 *
 *  PARAMETERS
 *      tvbuff_t *tvb       - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree *tree    - pointer to data tree wireshark uses to display packet.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_nonask_phy(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    proto_tree *ieee802154e_tree = NULL;
    proto_item *proto_root      = NULL;

    guint       offset          = 0;
    guint32     preamble;
    guint8      sfd,phr;
    tvbuff_t*   mac;

    /* Create the protocol tree. */
    if (tree) {
        proto_root = proto_tree_add_protocol_format(tree, proto_ieee802154e_nonask_phy, tvb, 0, tvb_length(tvb), "IEEE 802.15.4e non-ASK PHY");
        ieee802154e_tree = proto_item_add_subtree(proto_root, ett_ieee802154e_nonask_phy);
    }

    /* Add the protocol name. */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "IEEE 802.15.4e non-ASK PHY");
    /* Add the packet length. */
    col_add_fstr(pinfo->cinfo, COL_PACKET_LENGTH, "%i", tvb_length(tvb));

    preamble=tvb_get_letohl(tvb,offset);
    sfd=tvb_get_guint8(tvb,offset+4);
    phr=tvb_get_guint8(tvb,offset+4+1);

    if(tree) {
        proto_tree *phr_tree;
        proto_item *pi;
        guint loffset=offset;

        proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_nonask_phy_preamble, tvb, loffset, 4, preamble);
        loffset+=4;
        proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_nonask_phy_sfd, tvb, loffset, 1, sfd);
        loffset+=1;

        pi = proto_tree_add_text(ieee802154e_tree, tvb, loffset, 1, "PHR: 0x%02x", phr);
        phr_tree = proto_item_add_subtree(pi, ett_ieee802154e_nonask_phy_phr);

        proto_tree_add_uint(phr_tree, hf_ieee802154e_nonask_phy_length, tvb, loffset, 1, phr);
    }

    offset+=4+2*1;
    mac=tvb_new_subset(tvb,offset,-1, phr & IEEE802154_PHY_LENGTH_MASK);

    /* Call the common dissector. */
    dissect_ieee802154e(mac, pinfo, ieee802154e_tree);
} /* dissect_ieee802154e_nonask_phy */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e
 *  DESCRIPTION
 *      Dissector for IEEE 802.15.4 packet with an FCS containing
 *      a 16-bit CRC value.
 *
 *  PARAMETERS
 *      tvbuff_t *tvb       - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree *tree    - pointer to data tree wireshark uses to display packet.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    /* Call the common dissector. */
    dissect_ieee802154e_common(tvb, pinfo, tree, (ieee802154e_cc24xx ? DISSECT_IEEE802154E_OPTION_CC24xx : 0));
} /* dissect_ieee802154e */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_nofcs
 *  DESCRIPTION
 *      Dissector for IEEE 802.15.4 packet with no FCS present.
 *
 *  PARAMETERS
 *      tvbuff_t *tvb       - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree *tree    - pointer to data tree wireshark uses to display packet.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_nofcs(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    tvbuff_t    *new_tvb;
    /* If there is no FCS present in the reported packet, then the length of
     * the true IEEE 802.15.4 packet is actually 2 bytes longer. Re-create
     * the buffer with an extended reported length so that the packet will
     * be handled as though the FCS were truncated.
     *
     * Note, we can't just call tvb_set_reported_length(), because it includes
     * checks to ensure that the new reported length is not longer than the old
     * reported length (why?), and will throw an exception.
     */
    new_tvb = tvb_new_subset(tvb, 0, -1, tvb_reported_length(tvb)+IEEE802154_FCS_LEN);
    /* Call the common dissector. */
    dissect_ieee802154e_common(new_tvb, pinfo, tree, 0);
} /* dissect_ieee802154e_nofcs */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_cc24xx
 *  DESCRIPTION
 *      Dissector for IEEE 802.15.4 packet with a ChipCon/Texas
 *      Instruments compatible FCS. This is typically called by
 *      layers encapsulating an IEEE 802.15.4 packet.
 *
 *  PARAMETERS
 *      tvbuff_t *tvb       - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree *tree    - pointer to data tree wireshark uses to display packet.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_cc24xx(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    /* Call the common dissector. */
    dissect_ieee802154e_common(tvb, pinfo, tree, DISSECT_IEEE802154E_OPTION_CC24xx);
} /* dissect_ieee802154e_cc24xx */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_common
 *  DESCRIPTION
 *      IEEE 802.15.4 packet dissection routine for Wireshark.
 *      This function extracts all the information first before displaying.
 *      If payload exists, that portion will be passed into another dissector
 *      for further processing.
 *
 *      This is called after the individual dissect_ieee802154e* functions
 *      have been called to determine what sort of FCS is present.
 *      The dissect_ieee802154* functions will set the parameters
 *      in the ieee802154_packet structure, and pass it to this one
 *      through the data parameter.
 *
 *  PARAMETERS
 *      tvbuff_t *tvb       - pointer to buffer containing raw packet.
 *      packet_info *pinfo  - pointer to packet information fields
 *      proto_tree *tree    - pointer to data tree wireshark uses to display packet.
 *      guint options       - bitwise or of dissector options (see dissect_ieee802154E_OPTION_xxx).
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_common(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, guint options)
{
    tvbuff_t                *volatile payload_tvb;
    proto_tree              *volatile ieee802154e_tree = NULL;
    proto_item              *volatile proto_root = NULL;
    proto_item              *hidden_item;
    proto_item              *ti;
    void 		    *pd_save;

    guint                   offset = 0;
    volatile gboolean       fcs_ok = TRUE;
    const char              *saved_proto;
    ws_decrypt_status       status;

    ieee802154e_packet      *packet = wmem_new0(wmem_packet_scope(), ieee802154e_packet);
    ieee802154e_short_addr   addr16;
    ieee802154e_hints_t     *ieee_hints;

    heur_dtbl_entry_t      *hdtbl_entry;

    /* Link our packet info structure into the private data field for the
     * Network-Layer heuristic subdissectors. */
    pd_save = pinfo->private_data;
    pinfo->private_data = packet;

    packet->short_table = ieee802154e_map.short_table;

    /* Allocate frame data with hints for upper layers */
    if(!pinfo->fd->flags.visited){
        ieee_hints = wmem_new0(wmem_file_scope(), ieee802154e_hints_t);
        p_add_proto_data(wmem_file_scope(), pinfo, proto_ieee802154e, 0, ieee_hints);
    } else {
        ieee_hints = (ieee802154e_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_ieee802154e, 0);
    }

    /* Create the protocol tree. */
    if (tree) {
        proto_root = proto_tree_add_protocol_format(tree, proto_ieee802154e, tvb, 0, tvb_length(tvb), "IEEE 802.15.4e");
        ieee802154e_tree = proto_item_add_subtree(proto_root, ett_ieee802154e);
    }
    /* Add the protocol name. */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "IEEE 802.15.4e");
    /* Add the packet length. */
    col_add_fstr(pinfo->cinfo, COL_PACKET_LENGTH, "%i", tvb_length(tvb));

    /* Add the packet length to the filter field */
    hidden_item = proto_tree_add_uint(tree, hf_ieee802154e_frame_length, NULL, 0, 0, tvb_reported_length(tvb));
    PROTO_ITEM_SET_HIDDEN(hidden_item);

    /*=====================================================
     * FRAME CONTROL FIELD
     *=====================================================
     */
    dissect_ieee802154e_fcf(tvb, pinfo, ieee802154e_tree, packet, &offset);

    /*=====================================================
     * SEQUENCE NUMBER
     *=====================================================
     */
    if(!packet->seqnr_surpression){
	    packet->seqno = tvb_get_guint8(tvb, offset);
	    if (tree) {
		proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_seqno, tvb, offset, 1, packet->seqno);
		/* For Ack packets display this in the root. */
		if (packet->frame_type == IEEE802154_FCF_ACK) {
		    proto_item_append_text(proto_root, ", Sequence Number: %u", packet->seqno);
		}
	    }
	    offset += 1;
    }else{
    //TODO: seqnr surpression
    }
    /*=====================================================
     * ADDRESSING FIELDS
     *=====================================================
     */
    /* Clear out the addressing strings. */
    SET_ADDRESS(&pinfo->dst, AT_NONE, 0, NULL);
    SET_ADDRESS(&pinfo->src, AT_NONE, 0, NULL);
    SET_ADDRESS(&pinfo->dl_dst, AT_NONE, 0, NULL);
    SET_ADDRESS(&pinfo->dl_src, AT_NONE, 0, NULL);
    SET_ADDRESS(&pinfo->net_dst, AT_NONE, 0, NULL);
    SET_ADDRESS(&pinfo->net_src, AT_NONE, 0, NULL);

    /* Get and display the destination PAN, if present. */
    if ( (packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) ||
         (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT) ) {
        packet->dst_pan = tvb_get_letohs(tvb, offset);
        if (tree) {
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_dst_panID, tvb, offset, 2, packet->dst_pan);
        }
        offset += 2;
    }

    /* Get destination address. */
    if (packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) {
        char dst_addr[32];

        /* Get the address. */
        packet->dst16 = tvb_get_letohs(tvb, offset);

        /* Display the destination address. */
        if ( packet->dst16 == IEEE802154_BCAST_ADDR ) {
            g_snprintf(dst_addr, 32, "Broadcast");
        }
        else {
            g_snprintf(dst_addr, 32, "0x%04x", packet->dst16);
        }
        /* Provide address hints to higher layers that need it. */
        if (ieee_hints) {
            ieee_hints->dst16 = packet->dst16;
        }

        TVB_SET_ADDRESS(&pinfo->dl_dst, AT_IEEE_802_15_4_SHORT, tvb, offset, 2);
        TVB_SET_ADDRESS(&pinfo->dst, AT_IEEE_802_15_4_SHORT, tvb, offset, 2);

        if (tree) {
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_dst16, tvb, offset, 2, packet->dst16);
            proto_item_append_text(proto_root, ", Dst: %s", dst_addr);
        }

         col_append_fstr(pinfo->cinfo, COL_INFO, ", Dst: %s", dst_addr);
        offset += 2;
    }
    else if (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT) {
        static guint64 addr; /* has to be static due to SET_ADDRESS */

        /* Get the address */
        packet->dst64 = tvb_get_letoh64(tvb, offset);

        /* Copy and convert the address to network byte order. */
        addr = pntoh64(&(packet->dst64));

        /* Display the destination address. */
        /* XXX - OUI resolution doesn't happen when displaying resolved
         * EUI64 addresses; that should probably be fixed in
         * epan/addr_resolv.c.
         */
        SET_ADDRESS(&pinfo->dl_dst, AT_EUI64, 8, &addr);
        SET_ADDRESS(&pinfo->dst, AT_EUI64, 8, &addr);
        if (tree) {
            proto_tree_add_item(ieee802154e_tree, hf_ieee802154e_dst64, tvb, offset, 8, ENC_LITTLE_ENDIAN);
            proto_item_append_text(proto_root, ", Dst: %s", ep_eui64_to_display(packet->dst64));
        }
        col_append_fstr(pinfo->cinfo, COL_INFO, ", Dst: %s", ep_eui64_to_display(packet->dst64));
        offset += 8;
    }
    else if (packet->dst_addr_mode != IEEE802154_FCF_ADDR_NONE) {
        /* Invalid Destination Address Mode. Abort Dissection. */
        expert_add_info(pinfo, proto_root, &ei_ieee802154e_dst);
        return;
    }

    /* Get the source PAN if it exists. The source address will be present if:
     *  - The Source addressing exists and
     *  - The Destination addressing doesn't exist, or the Intra-PAN bit is unset.
     */
    if ( ((packet->src_addr_mode == IEEE802154_FCF_ADDR_SHORT) || (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT)) &&
         ((packet->dst_addr_mode == IEEE802154_FCF_ADDR_NONE) || (!packet->intra_pan)) ) {
        /* Source PAN is present, extract it and add it to the tree. */
        packet->src_pan = tvb_get_letohs(tvb, offset);
        if (tree) {
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_src_panID, tvb, offset, 2, packet->src_pan);
        }
        offset += 2;
    }
    /* Set the panID field in case the intra-pan condition was met. */
    else if (packet->dst_addr_mode != IEEE802154_FCF_ADDR_NONE) {
        packet->src_pan = packet->dst_pan;
    }
    /* If all else fails, consider it a broadcast PANID. */
    else {
        packet->src_pan = IEEE802154_BCAST_PAN;
    }

    if (ieee_hints) {
        ieee_hints->src_pan = packet->src_pan;
    }

    /* Get short source address if present. */
    if (packet->src_addr_mode == IEEE802154_FCF_ADDR_SHORT) {
        char src_addr[32];

        /* Get the address. */
        packet->src16 = tvb_get_letohs(tvb, offset);

        /* Update the Address fields. */
        if (packet->src16==IEEE802154_BCAST_ADDR) {
            g_snprintf(src_addr, 32, "Broadcast");
        }
        else {
            g_snprintf(src_addr, 32, "0x%04x", packet->src16);

            if (!pinfo->fd->flags.visited) {
                /* If we know our extended source address from previous packets,
                 * provide a pointer to it in a hint for upper layers */
                addr16.addr = packet->src16;
                addr16.pan = packet->src_pan;

                if (ieee_hints) {
                    ieee_hints->src16 = packet->src16;
                    ieee_hints->map_rec = (ieee802154e_map_rec *)
                        g_hash_table_lookup(ieee802154e_map.short_table, &addr16);
                }
            }
        }

        TVB_SET_ADDRESS(&pinfo->dl_src, AT_IEEE_802_15_4_SHORT, tvb, offset, 2);
        TVB_SET_ADDRESS(&pinfo->src, AT_IEEE_802_15_4_SHORT, tvb, offset, 2);

        /* Add the addressing info to the tree. */
        if (tree) {
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_src16, tvb, offset, 2, packet->src16);
            proto_item_append_text(proto_root, ", Src: %s", src_addr);

            if (ieee_hints && ieee_hints->map_rec) {
                /* Display inferred source address info */
                ti = proto_tree_add_eui64(ieee802154e_tree, hf_ieee802154e_src64, tvb, offset, 0,
                        ieee_hints->map_rec->addr64);
                PROTO_ITEM_SET_GENERATED(ti);

                if ( ieee_hints->map_rec->start_fnum ) {
                    ti = proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_src64_origin, tvb, 0, 0,
                        ieee_hints->map_rec->start_fnum);
                }
                else {
                    ti = proto_tree_add_text(ieee802154e_tree, tvb, 0, 0, "Origin: Pre-configured");
                }
                PROTO_ITEM_SET_GENERATED(ti);
            }
        }

        col_append_fstr(pinfo->cinfo, COL_INFO, ", Src: %s", src_addr);
        offset += 2;
    }
    else if (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) {
        static guint64 addr; /* has to be static due to SET_ADDRESS */

        /* Get the address. */
        packet->src64 = tvb_get_letoh64(tvb, offset);

        /* Copy and convert the address to network byte order. */
        addr = pntoh64(&(packet->src64));

        /* Display the source address. */
        /* XXX - OUI resolution doesn't happen when displaying resolved
         * EUI64 addresses; that should probably be fixed in
         * epan/addr_resolv.c.
         */
        SET_ADDRESS(&pinfo->dl_src, AT_EUI64, 8, &addr);
        SET_ADDRESS(&pinfo->src, AT_EUI64, 8, &addr);
        if (tree) {
            proto_tree_add_item(ieee802154e_tree, hf_ieee802154e_src64, tvb, offset, 8, ENC_LITTLE_ENDIAN);
            proto_item_append_text(proto_root, ", Src: %s", ep_eui64_to_display(packet->src64));
        }

        col_append_fstr(pinfo->cinfo, COL_INFO, ", Src: %s", ep_eui64_to_display(packet->src64));
        offset += 8;
    }
    else if (packet->src_addr_mode != IEEE802154_FCF_ADDR_NONE) {
        /* Invalid Destination Address Mode. Abort Dissection. */
        expert_add_info(pinfo, proto_root, &ei_ieee802154e_src);
        return;
    }

    /*=====================================================
     * VERIFY FRAME CHECK SEQUENCE
     *=====================================================
     */
    /* Check, but don't display the FCS yet, otherwise the payload dissection
     * may be out of place in the tree. But we want to know if the FCS is OK in
     * case the CRC is bad (don't want to continue dissection to the NWK layer).
     */
    if (tvb_bytes_exist(tvb, tvb_reported_length(tvb)-IEEE802154_FCS_LEN, IEEE802154_FCS_LEN)) {
        /* The FCS is in the last two bytes of the packet. */
        guint16     fcs = tvb_get_letohs(tvb, tvb_reported_length(tvb)-IEEE802154_FCS_LEN);
        /* Check if we are expecting a CC2420-style FCS*/
        if (options & DISSECT_IEEE802154E_OPTION_CC24xx) {
            fcs_ok = (fcs & IEEE802154_CC24xx_CRC_OK);
        }
        else {
            guint16 fcs_calc = ieee802154e_crc_tvb(tvb, tvb_reported_length(tvb)-IEEE802154_FCS_LEN);
            fcs_ok = (fcs == fcs_calc);
        }
    }

    /*=====================================================
     * AUXILIARY SECURITY HEADER
     *=====================================================
     */
    /* The Auxiliary Security Header only exists in IEEE 802.15.4-2006 */
    if (packet->security_enable && (packet->version == IEEE802154_VERSION_2006)) {
      proto_tree *header_tree, *field_tree;
      guint8                    security_control;
      guint                     aux_length = 5; /* Minimum length of the auxiliary header. */

      /* Parse the security control field. */
      security_control = tvb_get_guint8(tvb, offset);
      packet->security_level = (ieee802154_security_level)(security_control & IEEE802154_AUX_SEC_LEVEL_MASK);
      packet->key_id_mode = (ieee802154_key_id_mode)((security_control & IEEE802154_AUX_KEY_ID_MODE_MASK) >> IEEE802154_AUX_KEY_ID_MODE_SHIFT);

      /* Compute the length of the auxiliary header and create a subtree.  */
      if (packet->key_id_mode != KEY_ID_MODE_IMPLICIT) aux_length++;
      if (packet->key_id_mode == KEY_ID_MODE_KEY_EXPLICIT_4) aux_length += 4;
      if (packet->key_id_mode == KEY_ID_MODE_KEY_EXPLICIT_8) aux_length += 8;
      ti = proto_tree_add_text(ieee802154e_tree, tvb, offset, aux_length, "Auxiliary Security Header");
      header_tree = proto_item_add_subtree(ti, ett_ieee802154e_auxiliary_security);

      /* Security Control Field */
      ti = proto_tree_add_text(header_tree, tvb, offset, 1, "Security Control Field (0x%02x)", security_control);
      field_tree = proto_item_add_subtree(ti, ett_ieee802154e_aux_sec_control);
      proto_tree_add_uint(field_tree, hf_ieee802154e_security_level, tvb, offset, 1, security_control & IEEE802154_AUX_SEC_LEVEL_MASK);
      proto_tree_add_uint(field_tree, hf_ieee802154e_key_id_mode, tvb, offset, 1, security_control & IEEE802154_AUX_KEY_ID_MODE_MASK);
      proto_tree_add_uint(field_tree, hf_ieee802154e_aux_sec_reserved, tvb, offset, 1, security_control & IEEE802154_AUX_KEY_RESERVED_MASK);
      offset++;

      /* Frame Counter Field */
      packet->frame_counter = tvb_get_letohl (tvb, offset);
      proto_tree_add_uint(header_tree, hf_ieee802154e_aux_sec_frame_counter, tvb, offset,4, packet->frame_counter);
      offset +=4;

      /* Key identifier field(s). */
      if (packet->key_id_mode != KEY_ID_MODE_IMPLICIT) {
        /* Create a subtree. */
        ti = proto_tree_add_text(header_tree, tvb, offset, 1, "Key Identifier Field"); /* Will fix length later. */
        field_tree = proto_item_add_subtree(ti, ett_ieee802154e_aux_sec_key_id);
        /* Add key source, if it exists. */
        if (packet->key_id_mode == KEY_ID_MODE_KEY_EXPLICIT_4) {
          packet->key_source.addr32 = tvb_get_ntohl(tvb, offset);
          proto_tree_add_uint64(field_tree, hf_ieee802154e_aux_sec_key_source, tvb, offset, 4, packet->key_source.addr32);
          proto_item_set_len(ti, 1 + 4);
          offset += (int)sizeof (guint32);
        }
        if (packet->key_id_mode == KEY_ID_MODE_KEY_EXPLICIT_8) {
          packet->key_source.addr64 = tvb_get_ntoh64(tvb, offset);
          proto_tree_add_uint64(field_tree, hf_ieee802154e_aux_sec_key_source, tvb, offset, 8, packet->key_source.addr64);
          proto_item_set_len(ti, 1 + 8);
          offset += 8;
        }
        /* Add key identifier. */
        packet->key_index = tvb_get_guint8(tvb, offset);
        proto_tree_add_uint(field_tree, hf_ieee802154e_aux_sec_key_index, tvb, offset,1, packet->key_index);
        offset++;
      }
    }

    /*=====================================================
     * NONPAYLOAD FIELDS
     *=====================================================
     */
    /* All of the beacon fields, except the beacon payload are considered nonpayload. */
    if (packet->frame_type == IEEE802154_FCF_BEACON) {
	/* Parse the superframe spec. */
	dissect_ieee802154e_superframe(tvb, pinfo, ieee802154e_tree, &offset);
	/* Parse the GTS information fields. */
	dissect_ieee802154e_gtsinfo(tvb, pinfo, ieee802154e_tree, &offset);
	/* Parse the Pending address list. */
	dissect_ieee802154e_pendaddr(tvb, pinfo, ieee802154e_tree, &offset);			
    }/* beacon*/
    /* Only the Command ID is considered nonpayload. */
    if (packet->frame_type == IEEE802154_FCF_CMD) {
        packet->command_id = tvb_get_guint8(tvb, offset);
        if (tree) {
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_cmd_id, tvb, offset, 1, packet->command_id);
        }
        offset++;

        /* Display the command identifier in the info column. */
        col_set_str(pinfo->cinfo, COL_INFO, val_to_str_const(packet->command_id, ieee802154e_cmd_names, "Unknown Command"));
    }
    /* No other frame types have nonpayload fields. */

    /*=====================================================
     * PAYLOAD DISSECTION
     *=====================================================
     */
    /* IEEE 802.15.4-2003 may have security information pre-pended to payload */
    if (packet->security_enable && (packet->version == IEEE802154_VERSION_2003)) {
        /* Store security suite preference in the 2006 security level identifier to simplify 2003 integration! */
        packet->security_level = (ieee802154_security_level)ieee802154e_sec_suite;

        /* Frame Counter and Key Sequence Counter prepended to the payload of an encrypted frame */
        if (IEEE802154_IS_ENCRYPTED(packet->security_level)) {
            packet->frame_counter = tvb_get_letohl (tvb, offset);
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_sec_frame_counter, tvb, offset, (int)sizeof(guint32), packet->frame_counter);
            offset += (int)sizeof(guint32);

            packet->key_sequence_counter = tvb_get_guint8 (tvb, offset);
            proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_sec_key_sequence_counter, tvb, offset, (int)sizeof(guint8), packet->key_sequence_counter);
            offset += (int)sizeof(guint8);
        }
    }

    /* Encrypted Payload. */
    if (packet->security_enable) {
        payload_tvb = dissect_ieee802154e_decrypt(tvb, offset, pinfo, packet, &status);

        /* Get the unencrypted data if decryption failed.  */
        if (!payload_tvb) {
            /* Deal with possible truncation and the FCS field at the end. */
            gint            reported_len = tvb_reported_length(tvb)-offset-IEEE802154_FCS_LEN;
            gint            captured_len = tvb_length(tvb)-offset;
            if (reported_len < captured_len) captured_len = reported_len;
            payload_tvb = tvb_new_subset(tvb, offset, captured_len, reported_len);
        }

        /* Display the reason for failure, and abort if the error was fatal. */
        switch (status) {
        case DECRYPT_PACKET_SUCCEEDED:
        case DECRYPT_NOT_ENCRYPTED:
            /* No problem. */
            break;

        case DECRYPT_VERSION_UNSUPPORTED:
            /* We don't support decryption with that version of the protocol */
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "We don't support decryption with protocol version %u", packet->version);
            call_dissector(data_handle, payload_tvb, pinfo, tree);
            goto dissect_ieee802154e_fcs;

        case DECRYPT_PACKET_TOO_SMALL:
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "Packet was too small to include the CRC and MIC");
            call_dissector(data_handle, payload_tvb, pinfo, tree);
            goto dissect_ieee802154e_fcs;

        case DECRYPT_PACKET_NO_EXT_SRC_ADDR:
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "No extended source address - can't decrypt");
            call_dissector(data_handle, payload_tvb, pinfo, tree);
            goto dissect_ieee802154e_fcs;

        case DECRYPT_PACKET_NO_KEY:
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "No encryption key set - can't decrypt");
            call_dissector(data_handle, payload_tvb, pinfo, tree);
            goto dissect_ieee802154e_fcs;

        case DECRYPT_PACKET_DECRYPT_FAILED:
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "Decrypt failed");
            call_dissector(data_handle, payload_tvb, pinfo, tree);
            goto dissect_ieee802154e_fcs;

        case DECRYPT_PACKET_MIC_CHECK_FAILED:
            expert_add_info_format(pinfo, proto_root, &ei_ieee802154e_decrypt_error, "MIC check failed");
            /*
             * Abort only if the payload was encrypted, in which case we
             * probably didn't decrypt the packet right (eg: wrong key).
             */
            if (IEEE802154_IS_ENCRYPTED(packet->security_level)) {
                call_dissector(data_handle, payload_tvb, pinfo, tree);
                goto dissect_ieee802154e_fcs;
            }
            break;
        }
    }
    /* Plaintext Payload. */
    else {
        /* Deal with possible truncation and the FCS field at the end. */
        gint            reported_len = tvb_reported_length(tvb)-offset-IEEE802154_FCS_LEN;
        gint            captured_len = tvb_length(tvb)-offset;
        if (reported_len < captured_len) captured_len = reported_len;
        payload_tvb = tvb_new_subset(tvb, offset, captured_len, reported_len);
    }

    /*
     * Wrap the sub-dissection in a try/catch block in case the payload is
     * broken. First we store the current protocol so we can fix it if an
     * exception is thrown by the subdissectors.
     */
    saved_proto = pinfo->current_proto;
    /* Try to dissect the payload. */
    TRY {
        switch (packet->frame_type) {
        case IEEE802154_FCF_BEACON:
            if (!dissector_try_heuristic(ieee802154e_beacon_subdissector_list, payload_tvb, pinfo, tree, &hdtbl_entry, packet)) {
                /* Could not subdissect, call the data dissector instead. */
                call_dissector(data_handle, payload_tvb, pinfo, tree);
            }
            break;
        case IEEE802154_FCF_CMD:
            dissect_ieee802154e_command(payload_tvb, pinfo, ieee802154e_tree, packet);
            break;
        case IEEE802154_FCF_DATA:
            /* Sanity-check. */
            if ((!fcs_ok && ieee802154e_fcs_ok) || !tvb_reported_length(payload_tvb)) {
                call_dissector(data_handle, payload_tvb, pinfo, tree);
                break;
            }
            /* Try the PANID dissector table for stateful dissection. */
            if (dissector_try_uint_new(panid_dissector_table, packet->src_pan, payload_tvb, pinfo, tree, TRUE, packet)) {
                break;
            }
            /* Try again with the destination PANID (if different) */
            if (((packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) ||
                 (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT)) &&
                    (packet->dst_pan != packet->src_pan) &&
                    dissector_try_uint_new(panid_dissector_table, packet->src_pan, payload_tvb, pinfo, tree, TRUE, packet)) {
                break;
            }
            /* Try heuristic dissection. */
            if (dissector_try_heuristic(ieee802154e_heur_subdissector_list, payload_tvb, pinfo, tree, &hdtbl_entry, packet)) break;
            /* Fall-through to dump undissectable payloads. */
        default:
            /* Could not subdissect, call the data dissector instead. */
            call_dissector(data_handle, payload_tvb, pinfo, tree);
        } /* switch */
    }
    CATCH_ALL {
        /*
         * Someone encountered an error while dissecting the payload. But
         * we haven't yet finished processing all of our layer. Catch and
         * display the exception, then fall-through to finish displaying
         * the FCS (which we display last so the frame is ordered correctly
         * in the tree).
         */
        show_exception(payload_tvb, pinfo, tree, EXCEPT_CODE, GET_MESSAGE);
        pinfo->current_proto = saved_proto;
    }
    ENDTRY;

    /*=====================================================
     * FRAME CHECK SEQUENCE
     *=====================================================
     */
dissect_ieee802154e_fcs:
    /* The FCS should be the last bytes of the reported packet. */
    offset = tvb_reported_length(tvb)-IEEE802154_FCS_LEN;
    /* Dissect the FCS only if it exists (captures which don't or can't get the
     * FCS will simply truncate the packet to omit it, but should still set the
     * reported length to cover the original packet length), so if the snapshot
     * is too short for an FCS don't make a fuss.
     */
    if (tvb_bytes_exist(tvb, offset, IEEE802154_FCS_LEN) && (tree)) {
        proto_tree  *field_tree;
        guint16     fcs = tvb_get_letohs(tvb, offset);

        /* Display the FCS depending on expected FCS format */
        if ((options & DISSECT_IEEE802154E_OPTION_CC24xx)) {
            /* Create a subtree for the FCS. */
            ti = proto_tree_add_text(ieee802154e_tree, tvb, offset, 2, "Frame Check Sequence (TI CC24xx format): FCS %s", (fcs_ok) ? "OK" : "Bad");
            field_tree = proto_item_add_subtree(ti, ett_ieee802154e_fcs);
            /* Display FCS contents.  */
            ti = proto_tree_add_int(field_tree, hf_ieee802154e_rssi, tvb, offset++, 1, (gint8) (fcs & IEEE802154_CC24xx_RSSI));
            proto_item_append_text(ti, " dBm"); /*  Displaying Units */
            proto_tree_add_boolean(field_tree, hf_ieee802154e_fcs_ok, tvb, offset, 1, (gboolean) (fcs & IEEE802154_CC24xx_CRC_OK));
            proto_tree_add_uint(field_tree, hf_ieee802154e_correlation, tvb, offset, 1, (guint8) ((fcs & IEEE802154_CC24xx_CORRELATION) >> 8));
        }
        else {
            ti = proto_tree_add_uint(ieee802154e_tree, hf_ieee802154e_fcs, tvb, offset, 2, fcs);
            if (fcs_ok) {
                proto_item_append_text(ti, " (Correct)");
            }
            else {
                proto_item_append_text(ti, " (Incorrect, expected FCS=0x%04x", ieee802154e_crc_tvb(tvb, offset));
            }
            /* To Help with filtering, add the fcs_ok field to the tree.  */
            ti = proto_tree_add_boolean(ieee802154e_tree, hf_ieee802154e_fcs_ok, tvb, offset, 2, fcs_ok);
            PROTO_ITEM_SET_HIDDEN(ti);
        }
    }
    else if (tree) {
        /* Even if the FCS isn't present, add the fcs_ok field to the tree to
         * help with filter. Be sure not to make it visible though.
         */
        ti = proto_tree_add_boolean_format_value(ieee802154e_tree, hf_ieee802154e_fcs_ok, tvb, offset, 2, fcs_ok, "Unknown");
        PROTO_ITEM_SET_HIDDEN(ti);
    }

    /* If the CRC is invalid, make a note of it in the info column. */
    if (!fcs_ok) {
        col_append_str(pinfo->cinfo, COL_INFO, ", Bad FCS");
        if (tree) proto_item_append_text(proto_root, ", Bad FCS");

        /* Flag packet as having a bad crc. */
        expert_add_info(pinfo, proto_root, &ei_ieee802154e_fcs);
    }
} /* dissect_ieee802154e_common */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_superframe
 *  DESCRIPTION
 *      Subdissector command for the Superframe specification
 *      sub-field within the beacon frame.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to command subtree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *      guint       *offset         - offset into the tvbuff to begin dissection.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_superframe(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, guint *offset)
{
    proto_tree *field_tree = NULL;
    proto_item *ti;
    guint16     superframe;

    /* Parse the superframe spec. */
    superframe = tvb_get_letohs(tvb, *offset);
    if (tree) {
        /*  Add Subtree for superframe specification */
        ti = proto_tree_add_text(tree, tvb, *offset, 2, "Superframe Specification");
        field_tree = proto_item_add_subtree(ti, ett_ieee802154e_superframe);

        /*  Add Beacon Order to the superframe spec. */
        proto_tree_add_uint(field_tree, hf_ieee802154e_beacon_order, tvb, *offset, 2, superframe & IEEE802154_BEACON_ORDER_MASK);
        proto_tree_add_uint(field_tree, hf_ieee802154e_superframe_order, tvb, *offset, 2, superframe & IEEE802154_SUPERFRAME_ORDER_MASK);
        proto_tree_add_uint(field_tree, hf_ieee802154e_cap, tvb, *offset, 2, superframe & IEEE802154_SUPERFRAME_CAP_MASK);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_superframe_battery_ext, tvb, *offset, 2, superframe & IEEE802154_BATT_EXTENSION_MASK);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_superframe_coord, tvb, *offset, 2, superframe & IEEE802154_SUPERFRAME_COORD_MASK);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_assoc_permit, tvb, *offset, 2, superframe & IEEE802154_ASSOC_PERMIT_MASK);
    }
    (*offset) += 2;
} /* dissect_ieee802154e_superframe */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_gtsinfo
 *  DESCRIPTION
 *      Subdissector command for the GTS information fields within
 *      the beacon frame.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to command subtree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *      guint       *offset         - offset into the tvbuff to begin dissection.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_gtsinfo(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, guint *offset)
{
    proto_tree *field_tree = NULL;
    proto_tree *subtree    = NULL;
    proto_item *ti;
    guint8      gts_spec;
    guint8      gts_count;

    /*  Get and display the GTS specification field */
    gts_spec = tvb_get_guint8(tvb, *offset);
    gts_count = gts_spec & IEEE802154_GTS_COUNT_MASK;
    if (tree) {
        /*  Add Subtree for GTS information. */
        if (gts_count) {
            ti = proto_tree_add_text(tree, tvb, *offset, 2 + (gts_count * 3), "GTS");
        }
        else {
            ti = proto_tree_add_text(tree, tvb, *offset, 1, "GTS");
        }
        field_tree = proto_item_add_subtree(ti, ett_ieee802154e_gts);

        proto_tree_add_uint(field_tree, hf_ieee802154e_gts_count, tvb, *offset, 1, gts_count);
        proto_tree_add_boolean(field_tree, hf_ieee802154e_gts_permit, tvb, *offset, 1, gts_spec & IEEE802154_GTS_PERMIT_MASK);
    }
    (*offset) += 1;

    /* If the GTS descriptor count is nonzero, then the GTS directions mask and descriptor list are present. */
    if (gts_count) {
        guint8  gts_directions = tvb_get_guint8(tvb, *offset);
        guint   gts_rx = 0;
        int     i;

        /* Display the directions mask. */
        if (tree) {
            proto_tree  *dir_tree = NULL;

            /* Create a subtree. */
            ti = proto_tree_add_text(field_tree, tvb, *offset, 1, "GTS Directions");
            dir_tree = proto_item_add_subtree(ti, ett_ieee802154e_gts_direction);

            /* Add the directions to the subtree. */
            for (i=0; i<gts_count; i++) {
                gboolean    dir = gts_directions & IEEE802154_GTS_DIRECTION_SLOT(i);
                proto_tree_add_boolean_format(dir_tree, hf_ieee802154e_gts_direction, tvb, *offset, 1, dir, "GTS Slot %i: %s", i+1, dir?"Receive Only":"Transmit Only");
                if (dir) gts_rx++;
            } /* for */
            proto_item_append_text(ti, ": %i Receive & %i Transmit", gts_rx, gts_count - gts_rx);
        }
        (*offset) += 1;

        /* Create a subtree for the GTS descriptors. */
        if (tree) {
            ti = proto_tree_add_text(field_tree, tvb, *offset, gts_count * 3, "GTS Descriptors");
            subtree = proto_item_add_subtree(ti, ett_ieee802154e_gts_descriptors);
        }

        /* Get and display the GTS descriptors. */
        for (i=0; i<gts_count; i++) {
            guint16 gts_addr        = tvb_get_letohs(tvb, (*offset));
            guint8  gts_slot        = tvb_get_guint8(tvb, (*offset)+2);
            guint8  gts_length      = (gts_slot & IEEE802154_GTS_LENGTH_MASK) >> IEEE802154_GTS_LENGTH_SHIFT;

            if (tree) {
                /* Add address, slot, and time length fields. */
                ti = proto_tree_add_text(subtree, tvb, (*offset), 3, "{Address: 0x%04x", gts_addr);
                proto_item_append_text(ti, ", Slot: %i", gts_slot);
                proto_item_append_text(ti, ", Length: %i}", gts_length);
            }
            (*offset) += 3;
        } /* for */
    }
} /* dissect_ieee802154e_gtsinfo */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_pendaddr
 *  DESCRIPTION
 *      Subdissector command for the pending address list fields
 *      within the beacon frame.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to command subtree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *      guint       *offset         - offset into the tvbuff to begin dissection.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_pendaddr(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree, guint *offset)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint8      pend_spec;
    guint8      pend_num16;
    guint8      pend_num64;
    int         i;

    /*  Get the Pending Addresses specification fields */
    pend_spec = tvb_get_guint8(tvb, *offset);
    pend_num16 = pend_spec & IEEE802154_PENDADDR_SHORT_MASK;
    pend_num64 = (pend_spec & IEEE802154_PENDADDR_LONG_MASK) >> IEEE802154_PENDADDR_LONG_SHIFT;
    if (tree) {
        /*  Add Subtree for the addresses */
        ti = proto_tree_add_text(tree, tvb, *offset, 1 + 2*pend_num16 + 8*pend_num64, "Pending Addresses: %i Short and %i Long", pend_num16, pend_num64);
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_pendaddr);
    }
    (*offset) += 1;

    for (i=0; i<pend_num16; i++) {
        guint16 addr = tvb_get_letohs(tvb, *offset);
        proto_tree_add_uint(subtree, hf_ieee802154e_pending16, tvb, *offset, 2, addr);
        (*offset) += 2;
    } /* for */
    for (i=0; i<pend_num64; i++) {
        proto_tree_add_item(subtree, hf_ieee802154e_pending64, tvb, *offset, 8, ENC_LITTLE_ENDIAN);
        (*offset) += 8;
    } /* for */
} /* dissect_ieee802154e_pendaddr */


/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_ielist
 *  DESCRIPTION
 *      Subdissector command for the ielist fields
 *      within the beacon frame.
 *      The IE List Present field shall be set to one if IEs are contained in the frame. This field shall be set to zero
 *      otherwise. If the Frame Version field is b00 or b01, the IE List Present field shall be zero.
 *
 * 	The IE List field is variable length and contains one or more IE. This field is comprised of the Header IE and
 *	Payload IE subfields. This field shall be present only if the IE List Present field in the Frame Control field is
 *	set to one. The format of the IE List field is shown in Figure 5.2.2. Each IE consists of a descriptor and an
 *	optional payload as described in 5.2.4. This standard does not limit the number of IEs within the IE list.
 *	Header IEs, if present, follow the Auxiliary Security Header and are part of the MHR.
 *	Payload IEs, if present, follow the MHR and are considered part of the MAC payload, i.e., they may be
 *	encrypted. A list of payload IEs may require termination. See 5.2.4.22 for details on termination.

 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to command subtree.
 *      //ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *      guint       *offset         - offset into the tvbuff to begin dissection.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_ielist(tvbuff_t *tvb, packet_info *pinfo _U_, proto_tree *tree,/*ieee802154e_packet *packet,*/ guint *offset)
{
    //proto_tree *field_tree = NULL;
    proto_tree *hiesubtree    = NULL;
    proto_tree *piesubtree    = NULL;
    proto_item *ti;
    guint16     ielist_spec;
    gboolean    ie_type; /* type0: header IE, type1: payload IE */ 
    guint16	header_ie_length;
    guint8	header_ie_elementID;
    guint16	payload_ie_length;
    guint8	payload_ie_groupID;

    /*  Get and display the IE list specification field: 2 bytes*/
    ielist_spec = tvb_get_letohs(tvb, *offset);
    ie_type = ielist_spec & IEEE802154_IELIST_TYPE_MASK;


    //The Header IE list is terminated with an IE List Termination IE (ID = 0x7e or 0x7f) that has a content length
    //of zero.       Explicit termination is required after a Header IE if there is one or more Payload IEs (0x7e), or
    //MAC payload (0x7f), following the Header IE list. If an unformatted payload follows the Payload IE list,
    //then the payload IE list is terminated with a list termination IE (ID = 0xf) that has a content length of zero.
    //Otherwise the terminator may be omitted

    //if type 0, list of header IEs
    if(ie_type == 0){
	header_ie_length = (ielist_spec & IEEE802154_IELIST_TYPE0_LENGTH_MASK) >> 9;
	header_ie_elementID = (ielist_spec & IEEE802154_IELIST_TYPE0_ELEMENTID_MASK) >> 1;
	if (tree) {
	/*  Add Subtree for the Header IE */
		ti = proto_tree_add_text(tree, tvb, *offset, 2, "Header IE: 0x%04x", ielist_spec);
		hiesubtree = proto_item_add_subtree(ti, ett_ieee802154e_header_ie);
        }
        proto_tree_add_uint(hiesubtree, hf_ieee802154e_hie_length, tvb, *offset, 2, header_ie_length);
        proto_tree_add_uint(hiesubtree, hf_ieee802154e_hie_elementID, tvb, *offset, 1, header_ie_elementID);
	(*offset) += 2;
	/* parse header IE*/

    }

    //if type 1. list of payload IEs
    if(ie_type == 1){
	payload_ie_length = (ielist_spec & IEEE802154_IELIST_TYPE1_LENGTH_MASK) >> 5;
	payload_ie_groupID = (ielist_spec & IEEE802154_IELIST_TYPE1_GROUPID_MASK) >> 1;
	if (tree) {
	/*  Add Subtree for the Payload IE */
		ti = proto_tree_add_text(tree, tvb, *offset, 2, "Payload IE: 0x%04x", ielist_spec);
		piesubtree = proto_item_add_subtree(ti, ett_ieee802154e_payload_ie);
	}
        proto_tree_add_uint(piesubtree, hf_ieee802154e_pie_length, tvb, *offset, 2, payload_ie_length);
        proto_tree_add_uint(piesubtree, hf_ieee802154e_pie_groupID, tvb, *offset, 1, payload_ie_groupID);
	(*offset) += 2;
	/* parse payload IE*/
	switch(payload_ie_groupID){
		case 0x0:
			break;
		case 0x1:
			/*MLME nested */
			break;
		default:
			break;
	}

    }



} /* dissect_ieee802154e_ieliste_ielist */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_assoc_req
 *  DESCRIPTION
 *      Command subdissector routine for the Association request
 *      command.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields.
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_assoc_req(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint8      capability;

    /* Create a subtree for this command frame. */
    if (tree) {
        ti = proto_tree_add_text(tree, tvb, 0, 1, "%s", val_to_str_const(packet->command_id, ieee802154e_cmd_names, "Unknown Command"));
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_cmd);
    }

    /* Get and display capability info. */
    capability = tvb_get_guint8(tvb, 0);
    if (tree) {
        /* Enter the capability bits. */
        proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_alt_coord, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_ALT_PAN_COORD);
        ti = proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_device_type, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_DEVICE_TYPE);
        if (capability & IEEE802154_CMD_CINFO_DEVICE_TYPE) proto_item_append_text(ti, " (FFD)");
        else proto_item_append_text(ti, " (RFD)");
        ti = proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_power_src, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_POWER_SRC);
        if (capability & IEEE802154_CMD_CINFO_POWER_SRC) proto_item_append_text(ti, " (AC/Mains Power)");
        else proto_item_append_text(ti, " (Battery)");
        proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_idle_rx, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_IDLE_RX);
        proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_sec_capable, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_SEC_CAPABLE);
        proto_tree_add_boolean(subtree, hf_ieee802154e_cinfo_alloc_addr, tvb, 0, 1, capability & IEEE802154_CMD_CINFO_ALLOC_ADDR);
    }

    /* Call the data dissector for any leftover bytes. */
    if (tvb_length(tvb) > 1) {
        call_dissector(data_handle, tvb_new_subset_remaining(tvb, 1), pinfo, tree);
    }
} /* dissect_ieee802154e_assoc_req */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_assoc_rsp
 *  DESCRIPTION
 *      Command subdissector routine for the Association response
 *      command.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields.
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_assoc_rsp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint16     short_addr;
    guint8      status;
    guint       offset  = 0;

    /* Create a subtree for this command frame. */
    if (tree) {
        ti = proto_tree_add_text(tree, tvb, offset, 3, "%s", val_to_str_const(packet->command_id,
                    ieee802154e_cmd_names, "Unknown Command"));
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_cmd);
    }

    /* Get and display the short address. */
    short_addr = tvb_get_letohs(tvb, offset);
    if (tree) {
        proto_tree_add_uint(subtree, hf_ieee802154e_assoc_addr, tvb, offset, 2, short_addr);
    }
    offset += 2;

    /* Get and display the status. */
    status = tvb_get_guint8(tvb, offset);
    if (tree) {
        ti = proto_tree_add_uint(subtree, hf_ieee802154e_assoc_status, tvb, offset, 1, status);
        if (status == IEEE802154_CMD_ASRSP_AS_SUCCESS) proto_item_append_text(ti, " (Association Successful)");
        else if (status == IEEE802154_CMD_ASRSP_PAN_FULL) proto_item_append_text(ti, " (PAN Full)");
        else if (status == IEEE802154_CMD_ASRSP_PAN_DENIED) proto_item_append_text(ti, " (Association Denied)");
        else proto_item_append_text(ti, " (Reserved)");
    }
    offset += 1;

    /* Update the info column. */
    if (status == IEEE802154_CMD_ASRSP_AS_SUCCESS) {
        /* Association was successful. */
        if (packet->src_addr_mode != IEEE802154_FCF_ADDR_SHORT) {
            col_append_fstr(pinfo->cinfo, COL_INFO, ", PAN: 0x%04x", packet->dst_pan);
        }
        if (short_addr != IEEE802154_NO_ADDR16) {
            col_append_fstr(pinfo->cinfo, COL_INFO, " Addr: 0x%04x", short_addr);
        }
    }
    else {
        /* Association was unsuccessful. */
        col_append_str(pinfo->cinfo, COL_INFO, ", Unsuccessful");
    }

    /* Update the address table. */
    if ((status == IEEE802154_CMD_ASRSP_AS_SUCCESS) && (short_addr != IEEE802154_NO_ADDR16)) {
        ieee802154e_addr_update(&ieee802154e_map, short_addr, packet->dst_pan, packet->dst64,
                pinfo->current_proto, pinfo->fd->num);
    }

    /* Call the data dissector for any leftover bytes. */
    if (tvb_length(tvb) > offset) {
        call_dissector(data_handle, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
    }
} /* dissect_ieee802154e_assoc_rsp */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_disassoc
 *  DESCRIPTION
 *      Command subdissector routine for the Disassociate command.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields.
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_disassoc(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint8      reason;

    /* Create a subtree for this command frame. */
    if (tree) {
        ti = proto_tree_add_text(tree, tvb, 0, 1, "%s", val_to_str_const(packet->command_id, ieee802154e_cmd_names, "Unknown Command"));
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_cmd);
    }

    /* Get and display the disassociation reason. */
    reason = tvb_get_guint8(tvb, 0);
    if (tree) {
        ti = proto_tree_add_uint(subtree, hf_ieee802154e_disassoc_reason, tvb, 0, 1, reason);
        switch(reason) {
            case 0x01:
                proto_item_append_text(ti, " (Coordinator requests device to leave)");
                break;

            case 0x02:
                proto_item_append_text(ti, " (Device wishes to leave)");
                break;

            default:
                proto_item_append_text(ti, " (Reserved)");
                break;
        } /* switch */
    }

    if (!pinfo->fd->flags.visited) {
        /* Update the address tables */
        if ( packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT ) {
            ieee802154e_long_addr_invalidate(packet->dst64, pinfo->fd->num);
        } else if ( packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT ) {
            ieee802154e_short_addr_invalidate(packet->dst16, packet->dst_pan, pinfo->fd->num);
        }
    }

    /* Call the data dissector for any leftover bytes. */
    if (tvb_length(tvb) > 1) {
        call_dissector(data_handle, tvb_new_subset_remaining(tvb, 1), pinfo, tree);
    }
} /* dissect_ieee802154e_disassoc */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_realign
 *  DESCRIPTION
 *      Command subdissector routine for the Coordinator Realignment
 *      command.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields.
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_realign(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint16     pan_id;
    guint16     coord_addr;
    guint8      channel;
    guint16     short_addr;
    guint       offset  = 0;

    /* Create a subtree for this command frame. */
    if (tree) {
        ti = proto_tree_add_text(tree, tvb, offset, 0, "%s", val_to_str_const(packet->command_id, ieee802154e_cmd_names, "Unknown Command"));
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_cmd);
    }

    /* Get and display the command PAN ID. */
    pan_id = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(subtree, hf_ieee802154e_realign_pan, tvb, offset, 2, pan_id);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", PAN: 0x%04x", pan_id);
    offset += 2;

    /* Get and display the coordinator address. */
    coord_addr = tvb_get_letohs(tvb, offset);
    proto_tree_add_uint(subtree, hf_ieee802154e_realign_caddr, tvb, offset, 2, coord_addr);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Coordinator: 0x%04x", coord_addr);
    offset += 2;

    /* Get and display the channel. */
    channel = tvb_get_guint8(tvb, offset);
    proto_tree_add_uint(subtree, hf_ieee802154e_realign_channel, tvb, offset, 1, channel);
    col_append_fstr(pinfo->cinfo, COL_INFO, ", Channel: %u", channel);
    offset += 1;

    /* Get and display the short address. */
    short_addr = tvb_get_letohs(tvb, offset);
    if (tree) proto_tree_add_uint(subtree, hf_ieee802154e_realign_addr, tvb, offset, 2, short_addr);
    if ((packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT)
        && (short_addr != IEEE802154_NO_ADDR16)) {
        col_append_fstr(pinfo->cinfo, COL_INFO, ", Addr: 0x%04x", short_addr);
    }
    offset += 2;
    /* Update the address table. */
    if ((short_addr != IEEE802154_NO_ADDR16) && (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT)) {
        ieee802154e_addr_update(&ieee802154e_map, short_addr, packet->dst_pan, packet->dst64,
                pinfo->current_proto, pinfo->fd->num);
    }

    /* Get and display the channel page, if it exists. Added in IEEE802.15.4-2006 */
    if (tvb_bytes_exist(tvb, offset, 1)) {
        guint8  channel_page = tvb_get_guint8(tvb, offset);
        if (tree) proto_tree_add_uint(subtree, hf_ieee802154e_realign_channel_page, tvb, offset, 1, channel_page);
        offset += 1;
    }

    /* Fix the length of the command subtree. */
    if (tree) {
        proto_item_set_len(subtree, offset);
    }

    /* Call the data dissector for any leftover bytes. */
    if (tvb_length(tvb) > offset) {
        call_dissector(data_handle, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
    }
} /* dissect_ieee802154e_realign */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_gtsreq
 *  DESCRIPTION
 *      Command subdissector routine for the GTS request command.
 *
 *      Assumes that COL_INFO will be set to the command name,
 *      command name will already be appended to the command subtree
 *      and protocol root. In addition, assumes that the command ID
 *      has already been parsed.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_gtsreq(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    proto_tree *subtree = NULL;
    proto_item *ti;
    guint8      characteristics;
    guint8      length;
    guint8      direction;
    guint8      type;

    /* Create a subtree for this command frame. */
    if (tree) {
        ti = proto_tree_add_text(tree, tvb, 0, 1, "%s", val_to_str_const(packet->command_id, ieee802154e_cmd_names,
                                                                         "Unknown Command"));
        subtree = proto_item_add_subtree(ti, ett_ieee802154e_cmd);
    }

    /* Get the characteristics field. */
    characteristics = tvb_get_guint8(tvb, 0);
    length = characteristics & IEEE802154_CMD_GTS_REQ_LEN;
    direction = characteristics & IEEE802154_CMD_GTS_REQ_DIR;
    type = characteristics & IEEE802154_CMD_GTS_REQ_TYPE;

    /* Display the characteristics field. */
    if (tree) {
        proto_tree_add_uint(subtree, hf_ieee802154e_gtsreq_len, tvb, 0, 1, length);
        ti = proto_tree_add_boolean(subtree, hf_ieee802154e_gtsreq_dir, tvb, 0, 1, direction);
        if (direction) proto_item_append_text(ti, " (Receive)");
        else proto_item_append_text(ti, " (Transmit)");
        ti = proto_tree_add_boolean(subtree, hf_ieee802154e_gtsreq_type, tvb, 0, 1, type);
        if (type) proto_item_append_text(ti, " (Allocate GTS)");
        else proto_item_append_text(ti, " (Deallocate GTS)");
    }

    /* Call the data dissector for any leftover bytes. */
    if (tvb_length(tvb) > 1) {
        call_dissector(data_handle, tvb_new_subset_remaining(tvb, 1), pinfo, tree);
    }
} /* dissect_ieee802154e_gtsreq */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_command
 *  DESCRIPTION
 *      Subdissector routine all commands.
 *  PARAMETERS
 *      tvbuff_t    *tvb            - pointer to buffer containing raw packet.
 *      packet_info *pinfo          - pointer to packet information fields (unused).
 *      proto_tree  *tree           - pointer to protocol tree.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information (unused).
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
dissect_ieee802154e_command(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, ieee802154e_packet *packet)
{
    switch (packet->command_id) {
    case IEEE802154_CMD_ASRQ:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_addr_mode != IEEE802154_FCF_ADDR_NONE));
        dissect_ieee802154e_assoc_req(tvb, pinfo, tree, packet);
        break;

    case IEEE802154_CMD_ASRSP:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT));
        dissect_ieee802154e_assoc_rsp(tvb, pinfo, tree, packet);
        break;

      case IEEE802154_CMD_DISAS:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT));
        dissect_ieee802154e_disassoc(tvb, pinfo, tree, packet);
        return;

      case IEEE802154_CMD_DATA_RQ:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id, packet->src_addr_mode != IEEE802154_FCF_ADDR_NONE);
        /* No payload expected. */
        break;

      case IEEE802154_CMD_PANID_ERR:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_EXT));
        /* No payload expected. */
        break;

      case IEEE802154_CMD_ORPH_NOTIF:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) &&
            (packet->dst16 == IEEE802154_BCAST_ADDR) &&
            (packet->src_pan == IEEE802154_BCAST_PAN) &&
            (packet->dst_pan == IEEE802154_BCAST_PAN));
        /* No payload expected. */
        break;

      case IEEE802154_CMD_BCN_RQ:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) &&
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_NONE) &&
            (packet->dst16 == IEEE802154_BCAST_ADDR) &&
            (packet->dst_pan == IEEE802154_BCAST_PAN));
        /* No payload expected. */
        break;

      case IEEE802154_CMD_COORD_REAL:
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) &&
            (packet->dst_pan == IEEE802154_BCAST_PAN) &&
            (packet->dst_addr_mode != IEEE802154_FCF_ADDR_NONE));
        if (packet->dst_addr_mode == IEEE802154_FCF_ADDR_SHORT) {
            /* If directed to a 16-bit address, check that it is being broadcast. */
            IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id, packet->dst16 == IEEE802154_BCAST_ADDR);
        }
        dissect_ieee802154e_realign(tvb, pinfo, tree, packet);
        return;

      case IEEE802154_CMD_GTS_REQ:
        /* Check that the addressing is correct for this command type. */
        IEEE802154_CMD_ADDR_CHECK(pinfo, tree, packet->command_id,
            (packet->src_addr_mode == IEEE802154_FCF_ADDR_SHORT) &&
            (packet->dst_addr_mode == IEEE802154_FCF_ADDR_NONE) &&
            (packet->src16 != IEEE802154_BCAST_ADDR) &&
            (packet->src16 != IEEE802154_NO_ADDR16));
        dissect_ieee802154e_gtsreq(tvb, pinfo, tree, packet);
        return;
    } /* switch */

    /* Dump unexpected, or unknown command payloads. */
    call_dissector(data_handle, tvb, pinfo, tree);
} /* dissect_ieee802154e_command */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      dissect_ieee802154e_decrypt
 *  DESCRIPTION
 *      IEEE 802.15.4 decryption algorithm. Tries to find the
 *      appropriate key from the information in the IEEE 802.15.4
 *      packet structure and dissector config.
 *
 *      This function implements the security proceedures for the
 *      2006 version of the spec only. IEEE 802.15.4-2003 is
 *      unsupported.
 *  PARAMETERS
 *      tvbuff_t *tvb               - IEEE 802.15.4 packet.
 *      packet_info * pinfo         - Packet info structure.
 *      guint offset                - Offset where the ciphertext 'c' starts.
 *      ieee802154e_packet *packet   - IEEE 802.15.4 packet information.
 *      ws_decrypt_status *status   - status of decryption returned through here on failure.
 *  RETURNS
 *      tvbuff_t *                  - Decrypted payload.
 *---------------------------------------------------------------
 */
static tvbuff_t *
dissect_ieee802154e_decrypt(tvbuff_t * tvb, guint offset, packet_info * pinfo, ieee802154e_packet * packet, ws_decrypt_status * status)
{
    tvbuff_t *          ptext_tvb;
    gboolean            have_mic = FALSE;
    guint64             srcAddr;
    unsigned char       key[16];
    unsigned char       tmp[16];
    unsigned char       rx_mic[16];
    guint               M;
    gint                captured_len;
    gint                reported_len;
    ieee802154e_hints_t *ieee_hints;

    /*
     * Check the version; we only support IEEE 802.15.4-2003 and IEEE 802.15.4-2006.
     * We must do this first, as, if this isn't IEEE 802.15.4-2003 or IEEE 802.15.4-2006,
     * we don't have the Auxiliary Security Header, and haven't
     * filled in the information for it, and none of the stuff
     * we do afterwards, which uses that information, is doable.
     */
    if ((packet->version != IEEE802154_VERSION_2012) && (packet->version != IEEE802154_VERSION_2006) && (packet->version != IEEE802154_VERSION_2003)) {
        *status = DECRYPT_VERSION_UNSUPPORTED;
        return NULL;
    }

    ieee_hints = (ieee802154e_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo, proto_ieee802154e, 0);

    /* Get the captured and on-the-wire length of the payload. */
    M = IEEE802154_MIC_LENGTH(packet->security_level);
    reported_len = tvb_reported_length_remaining(tvb, offset) - IEEE802154_FCS_LEN - M;
    if (reported_len < 0) {
        *status = DECRYPT_PACKET_TOO_SMALL;
        return NULL;
    }
    /* Check of the payload is truncated.  */
    if (tvb_bytes_exist(tvb, offset, reported_len)) {
        captured_len = reported_len;
    }
    else {
        captured_len = tvb_length_remaining(tvb, offset);
    }

    /* Check if the MIC is present in the captured data. */
    have_mic = tvb_bytes_exist(tvb, offset + reported_len, M);
    if (have_mic) {
        tvb_memcpy(tvb, rx_mic, offset + reported_len, M);
    }

    /*=====================================================
     * Key Lookup - Need to find the appropriate key.
     *=====================================================
     */
    /*
     * Oh God! The specification is so bad. This is the worst
     * case of design-by-committee I've ever seen in my life.
     * The IEEE has created an unintelligable mess in order
     * to decipher which key is used for which message.
     *
     * Let's hope it's simpler to implement for dissecting only.
     *
     * Also need to find the extended address of the sender.
     */
    if (packet->src_addr_mode == IEEE802154_FCF_ADDR_EXT) {
        /* The source EUI-64 is included in the headers. */
        srcAddr = packet->src64;
    }
    else if (ieee_hints && ieee_hints->map_rec && ieee_hints->map_rec->addr64) {
        /* Use the hint */
        srcAddr = ieee_hints->map_rec->addr64;
    }
    else {
        /* Lookup failed.  */
        *status = DECRYPT_PACKET_NO_EXT_SRC_ADDR;
        return NULL;
    }

    /* Lookup the key. */
    /*
     * TODO: What this dissector really needs is a UAT to store multiple keys
     * and a variety of key configuration data. However, a single shared key
     * should be sufficient to get packet encryption off to a start.
     */
    if (!ieee802154e_key_valid) {
        *status = DECRYPT_PACKET_NO_KEY;
        return NULL;
    }
    memcpy(key, ieee802154e_key, IEEE802154_CIPHER_SIZE);

    /*=====================================================
     * CCM* - CTR mode payload encryption
     *=====================================================
     */
    /* Create the CCM* initial block for decryption (Adata=0, M=0, counter=0). */
    ccm_init_block(tmp, FALSE, 0, srcAddr, packet, 0);

    /* Decrypt the ciphertext, and place the plaintext in a new tvb. */
    if (IEEE802154_IS_ENCRYPTED(packet->security_level) && captured_len) {
        guint8 *text;
        /*
         * Make a copy of the ciphertext in heap memory.
         *
         * We will decrypt the message in-place and then use the buffer as the
         * real data for the new tvb.
         */
        text = (guint8 *)tvb_memdup(NULL, tvb, offset, captured_len);

        /* Perform CTR-mode transformation. */
        if (!ccm_ctr_encrypt(key, tmp, rx_mic, text, captured_len)) {
            g_free(text);
            *status = DECRYPT_PACKET_DECRYPT_FAILED;
            return NULL;
        }

        /* Create a tvbuff for the plaintext. */
        ptext_tvb = tvb_new_child_real_data(tvb, text, captured_len, reported_len);
        tvb_set_free_cb(ptext_tvb, g_free);
        add_new_data_source(pinfo, ptext_tvb, "Decrypted IEEE 802.15.4 payload");
        *status = DECRYPT_PACKET_SUCCEEDED;
    }
    /* There is no ciphertext. Wrap the plaintext in a new tvb. */
    else {
        /* Decrypt the MIC (if present). */
        if ((have_mic) && (!ccm_ctr_encrypt(key, tmp, rx_mic, NULL, 0))) {
            *status = DECRYPT_PACKET_DECRYPT_FAILED;
            return NULL;
        }

        /* Create a tvbuff for the plaintext. This might result in a zero-length tvbuff. */
        ptext_tvb = tvb_new_subset(tvb, offset, captured_len, reported_len);
        *status = DECRYPT_PACKET_SUCCEEDED;
    }

    /*=====================================================
     * CCM* - CBC-mode message authentication
     *=====================================================
     */
    /* We can only verify the message if the MIC wasn't truncated. */
    if (have_mic) {
        unsigned char           dec_mic[16];
        guint                   l_m = captured_len;
        guint                   l_a = offset;

        /* Adjust the lengths of the plantext and additional data if unencrypted. */
        if (!IEEE802154_IS_ENCRYPTED(packet->security_level)) {
            l_a += l_m;
            l_m = 0;
        }
        else if ((packet->version == IEEE802154_VERSION_2003) && !ieee802154e_extend_auth)
            l_a -= 5;   /* Exclude Frame Counter (4 bytes) and Key Sequence Counter (1 byte) from authentication data */


        /* Create the CCM* initial block for authentication (Adata!=0, M!=0, counter=l(m)). */
        ccm_init_block(tmp, TRUE, M, srcAddr, packet, l_m);

        /* Compute CBC-MAC authentication tag. */
        /*
         * And yes, despite the warning in tvbuff.h, I think tvb_get_ptr is the
         * right function here since either A) the payload wasn't encrypted, in
         * which case l_m is zero, or B) the payload was encrypted, and the tvb
         * already points to contiguous memory, since we just allocated it in
         * decryption phase.
         */
        if (!ccm_cbc_mac(key, tmp, (const gchar *)tvb_memdup(wmem_packet_scope(), tvb, 0, l_a), l_a, tvb_get_ptr(ptext_tvb, 0, l_m), l_m, dec_mic)) {
            *status = DECRYPT_PACKET_MIC_CHECK_FAILED;
        }
        /* Compare the received MIC with the one we generated. */
        else if (memcmp(rx_mic, dec_mic, M) != 0) {
            *status = DECRYPT_PACKET_MIC_CHECK_FAILED;
        }
    }

    /* Done! */
    return ptext_tvb;
} /* dissect_ieee802154e_decrypt */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ccm_init_block
 *  DESCRIPTION
 *      Creates the CCM* initial block value for IEEE 802.15.4.
 *  PARAMETERS
 *      gchar *block        - Output pointer for the initial block.
 *      gboolean adata      - TRUE if additional auth data is present
 *      gint M              - CCM* parameter M.
 *      guint64 addr        - Source extended address.
 *      ieee802154e_packet *packet - IEEE 802.15.4 packet information.
 *      guint16 ctr_val     - Value in the last L bytes of the block.
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
ccm_init_block(gchar *block, gboolean adata, gint M, guint64 addr, ieee802154e_packet *packet, gint ctr_val)
{
    gint                i = 0;

    /* Flags: Reserved(0) || Adata || (M-2)/2 || (L-1) */
    block[i] = (0x2 - 1); /* (L-1) */
    if (M > 0) block[i] |= (((M-2)/2) << 3); /* (M-2)/2 */
    if (adata) block[i] |= (1 << 6); /* Adata */
    i++;
    /* 2003 CCM Nonce:  Source Address || Frame Counter || Key Sequence Counter */
    /* 2006 CCM* Nonce: Source Address || Frame Counter || Security Level */
    block[i++] = (guint8)((addr >> 56) & 0xff);
    block[i++] = (guint8)((addr >> 48) & 0xff);
    block[i++] = (guint8)((addr >> 40) & 0xff);
    block[i++] = (guint8)((addr >> 32) & 0xff);
    block[i++] = (guint8)((addr >> 24) & 0xff);
    block[i++] = (guint8)((addr >> 16) & 0xff);
    block[i++] = (guint8)((addr >> 8) & 0xff);
    block[i++] = (guint8)((addr >> 0) & 0xff);
    block[i++] = (guint8)((packet->frame_counter >> 24) & 0xff);
    block[i++] = (guint8)((packet->frame_counter >> 16) & 0xff);
    block[i++] = (guint8)((packet->frame_counter >> 8) & 0xff);
    block[i++] = (guint8)((packet->frame_counter >> 0) & 0xff);
    if (packet->version == IEEE802154_VERSION_2003)
        block[i++] = packet->key_sequence_counter;
    else
        block[i++] = packet->security_level;
    /* Plaintext length. */
    block[i++] = (guint8)((ctr_val >> 8) & 0xff);
    block[i] = (guint8)((ctr_val >> 0) & 0xff);
} /* ccm_init_block */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ccm_ctr_encrypt
 *  DESCRIPTION
 *      Performs an in-place CTR-mode encryption/decryption.
 *  PARAMETERS
 *      const gchar *key    - Encryption Key.
 *      const gchar *iv     - Counter initial value.
 *      gchar *mic          - MIC to encrypt/decrypt.
 *      gchar *data         - Buffer to encrypt/decrypt.
 *      gint length         - Length of the buffer.
 *  RETURNS
 *      gboolean            - TRUE on SUCCESS, FALSE on error.
 *---------------------------------------------------------------
 */
#ifdef HAVE_LIBGCRYPT
static gboolean
ccm_ctr_encrypt(const gchar *key, const gchar *iv, gchar *mic, gchar *data, gint length)
{
    gcry_cipher_hd_t    cipher_hd;

    /* Open the cipher. */
    if (gcry_cipher_open(&cipher_hd, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_CTR, 0)) {
        return FALSE;
    }

    /* Set the key and initial value. */
    if (gcry_cipher_setkey(cipher_hd, key, 16)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }
    if (gcry_cipher_setctr(cipher_hd, iv, 16)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }

    /* Decrypt the MIC. */
    if (gcry_cipher_encrypt(cipher_hd, mic, 16, NULL, 0)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }
    /* Decrypt the payload. */
    if (gcry_cipher_encrypt(cipher_hd, data, length, NULL, 0)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }

    /* Done with the cipher. */
    gcry_cipher_close(cipher_hd);
    return TRUE;
} /* ccm_ctr_encrypt */
#else
static gboolean
ccm_ctr_encrypt(const gchar *key _U_, const gchar *iv _U_, gchar *mic _U_, gchar *data _U_, gint length _U_)
{
    return FALSE;
}
#endif /* HAVE_LIBGCRYPT */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ccm_cbc_mac
 *  DESCRIPTION
 *      Generates a CBC-MAC of the decrypted payload and additional
 *      authentication headers.
 *  PARAMETERS
 *      const gchar key     - Encryption Key.
 *      const gchar iv      - Counter initial value.
 *      const gchar a       - Additional auth headers.
 *      gint a_len                  - Length of the additional headers.
 *      const gchar m       - Plaintext message.
 *      gint m_len                  - Length of plaintext message.
 *      gchar *mic          - Output for CBC-MAC.
 *  RETURNS
 *      gboolean            - TRUE on SUCCESS, FALSE on error.
 *---------------------------------------------------------------
 */
#ifdef HAVE_LIBGCRYPT
static gboolean
ccm_cbc_mac(const gchar *key, const gchar *iv, const gchar *a, gint a_len, const gchar *m, gint m_len, gchar *mic)
{
    gcry_cipher_hd_t cipher_hd;
    guint            i = 0;
    unsigned char    block[16];

    /* Open the cipher. */
    if (gcry_cipher_open(&cipher_hd, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_CBC, GCRY_CIPHER_CBC_MAC)) return FALSE;

    /* Set the key. */
    if (gcry_cipher_setkey(cipher_hd, key, 16)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }

    /* Process the initial value. */
    if (gcry_cipher_encrypt(cipher_hd, mic, 16, iv, 16)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }

    /* Encode L(a) */
    i = 0;

/* XXX: GINT_MAX is not defined so #if ... will always be false */
#if (GINT_MAX >= (1LL << 32))
    if (a_len >= (1LL << 32)) {
        block[i++] = 0xff;
        block[i++] = 0xff;
        block[i++] = (a_len >> 56) & 0xff;
        block[i++] = (a_len >> 48) & 0xff;
        block[i++] = (a_len >> 40) & 0xff;
        block[i++] = (a_len >> 32) & 0xff;
        block[i++] = (a_len >> 24) & 0xff;
        block[i++] = (a_len >> 16) & 0xff;
        block[i++] = (a_len >> 8) & 0xff;
        block[i++] = (a_len >> 0) & 0xff;
    }
    else
#endif
    if (a_len >= ((1 << 16) - (1 << 8))) {
        block[i++] = 0xff;
        block[i++] = 0xfe;
        block[i++] = (a_len >> 24) & 0xff;
        block[i++] = (a_len >> 16) & 0xff;
        block[i++] = (a_len >> 8) & 0xff;
        block[i++] = (a_len >> 0) & 0xff;
    }
    else {
        block[i++] = (a_len >> 8) & 0xff;
        block[i++] = (a_len >> 0) & 0xff;
    }
    /* Append a to get the first block of input (pad if we encounter the end of a). */
    while ((i < sizeof(block)) && (a_len-- > 0)) block[i++] = *a++;
    while (i < sizeof(block)) block[i++] = 0;

    /* Process the first block of AuthData. */
    if (gcry_cipher_encrypt(cipher_hd, mic, 16, block, 16)) {
        gcry_cipher_close(cipher_hd);
        return FALSE;
    }

    /* Transform and process the remainder of a. */
    while (a_len > 0) {
        /* Copy and pad. */
        if ((guint)a_len >= sizeof(block)) memcpy(block, a, sizeof(block));
        else {memcpy(block, a, a_len); memset(block+a_len, 0, sizeof(block)-a_len);}
        /* Adjust pointers. */
        a += sizeof(block);
        a_len -= (int)sizeof(block);
        /* Execute the CBC-MAC algorithm. */
        if (gcry_cipher_encrypt(cipher_hd, mic, 16, block, sizeof(block))) {
            gcry_cipher_close(cipher_hd);
            return FALSE;
        }
    } /* while */

    /* Process the message, m. */
    while (m_len > 0) {
        /* Copy and pad. */
        if ((guint)m_len >= sizeof(block)) memcpy(block, m, sizeof(block));
        else {memcpy(block, m, m_len); memset(block+m_len, 0, sizeof(block)-m_len);}
        /* Adjust pointers. */
        m += sizeof(block);
        m_len -= (int)sizeof(block);
        /* Execute the CBC-MAC algorithm. */
        if (gcry_cipher_encrypt(cipher_hd, mic, 16, block, sizeof(block))) {
            gcry_cipher_close(cipher_hd);
            return FALSE;
        }
    }

    /* Done with the cipher. */
    gcry_cipher_close(cipher_hd);
    return TRUE;
} /* ccm_cbc_mac */
#else
static gboolean
ccm_cbc_mac(const gchar *key _U_, const gchar *iv _U_,
        const gchar *a _U_, gint a_len _U_, const gchar *m _U_, gint m_len _U_, gchar *mic _U_)
{
    return FALSE;
}
#endif /* HAVE_LIBGCRYPT */

/* Key hash function. */
guint ieee802154e_short_addr_hash(gconstpointer key)
{
    return (((ieee802154e_short_addr *)key)->addr) | (((ieee802154e_short_addr *)key)->pan << 16);
}

/* Key equal function. */
gboolean ieee802154e_short_addr_equal(gconstpointer a, gconstpointer b)
{
    return (((ieee802154e_short_addr *)a)->pan == ((ieee802154e_short_addr *)b)->pan) &&
           (((ieee802154e_short_addr *)a)->addr == ((ieee802154e_short_addr *)b)->addr);
}

/* Key hash function. */
guint ieee802154e_long_addr_hash(gconstpointer key)
{
    return (guint)(((ieee802154e_long_addr *)key)->addr) & 0xFFFFFFFF;
}

/* Key equal function. */
gboolean ieee802154e_long_addr_equal(gconstpointer a, gconstpointer b)
{
    return (((ieee802154e_long_addr *)a)->addr == ((ieee802154e_long_addr *)b)->addr);
}

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ieee802154e_addr_update
 *  DESCRIPTION
 *      Creates a record that maps the given short address and pan
 *      to a long (extended) address.
 *  PARAMETERS
 *      guint16 short_addr  - 16-bit short address
 *      guint16 pan         - 16-bit PAN id
 *      guint64 long_addr   - 64-bit long (extended) address
 *      const char *        - Pointer to name of current protocol
 *      guint               - Frame number this mapping became valid
 *  RETURNS
 *      TRUE                - Record was updated
 *      FALSE               - Couldn't find it
 *---------------------------------------------------------------
 */
ieee802154e_map_rec *ieee802154e_addr_update(ieee802154e_map_tab_t *au_ieee802154e_map,
        guint16 short_addr, guint16 pan, guint64 long_addr, const char *proto, guint fnum)
{
    ieee802154e_short_addr  addr16;
    ieee802154e_map_rec    *p_map_rec;
    gpointer               old_key;

    /* Look up short address hash */
    addr16.pan = pan;
    addr16.addr = short_addr;
    p_map_rec = (ieee802154e_map_rec *)g_hash_table_lookup(au_ieee802154e_map->short_table, &addr16);

    /* Update mapping record */
    if (p_map_rec) {
        /* record already exists */
        if ( p_map_rec->addr64 == long_addr ) {
            /* no change */
            return p_map_rec;
        }
        else {
            /* mark current mapping record invalid */
            p_map_rec->end_fnum = fnum;
        }
    }

    /* create a new mapping record */
    p_map_rec = wmem_new(wmem_file_scope(), ieee802154e_map_rec);
    p_map_rec->proto = proto;
    p_map_rec->start_fnum = fnum;
    p_map_rec->end_fnum = 0;
    p_map_rec->addr64 = long_addr;

    /* link new mapping record to addr hash tables */
    if ( g_hash_table_lookup_extended(au_ieee802154e_map->short_table, &addr16, &old_key, NULL) ) {
        /* update short addr hash table, reusing pointer to old key */
        g_hash_table_insert(au_ieee802154e_map->short_table, &old_key, p_map_rec);
    } else {
        /* create new hash entry */
        g_hash_table_insert(au_ieee802154e_map->short_table, wmem_memdup(wmem_file_scope(), &addr16, sizeof(addr16)), p_map_rec);
    }

    if ( g_hash_table_lookup_extended(au_ieee802154e_map->long_table, &long_addr, &old_key, NULL) ) {
        /* update long addr hash table, reusing pointer to old key */
        g_hash_table_insert(au_ieee802154e_map->long_table, &old_key, p_map_rec);
    } else {
        /* create new hash entry */
        g_hash_table_insert(au_ieee802154e_map->long_table, wmem_memdup(wmem_file_scope(), &long_addr, sizeof(long_addr)), p_map_rec);
    }

    return p_map_rec;
} /* ieee802154e_addr_update */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ieee802154e_short_addr_invalidate
 *  DESCRIPTION
 *      Marks a mapping record associated with device with short_addr
 *      as invalid at a certain frame number, typically when a
 *      dissassociation occurs.
 *  PARAMETERS
 *      guint16 short_addr  - 16-bit short address
 *      guint16 pan         - 16-bit PAN id
 *      guint               - Frame number when mapping became invalid
 *  RETURNS
 *      TRUE                - Record was updated
 *      FALSE               - Couldn't find it
 *---------------------------------------------------------------
 */
gboolean ieee802154e_short_addr_invalidate(guint16 short_addr, guint16 pan, guint fnum)
{
    ieee802154e_short_addr  addr16;
    ieee802154e_map_rec    *map_rec;

    addr16.pan = pan;
    addr16.addr = short_addr;

    map_rec = (ieee802154e_map_rec *)g_hash_table_lookup(ieee802154e_map.short_table, &addr16);
    if ( map_rec ) {
        /* indicates this mapping is invalid at frame fnum */
        map_rec->end_fnum = fnum;
        return TRUE;
    }

    return FALSE;
} /* ieee802154e_short_addr_invalidate */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      ieee802154e_long_addr_invalidate
 *  DESCRIPTION
 *      Marks a mapping record associated with device with long_addr
 *      as invalid at a certain frame number, typically when a
 *      dissassociation occurs.
 *  PARAMETERS
 *      guint64 long_addr   - 16-bit short address
 *      guint               - Frame number when mapping became invalid
 *  RETURNS
 *      TRUE                - If record was updated
 *      FALSE               - If record wasn't updated
 *---------------------------------------------------------------
 */
gboolean ieee802154e_long_addr_invalidate(guint64 long_addr, guint fnum)
{
    ieee802154e_map_rec   *map_rec;

    map_rec = (ieee802154e_map_rec *)g_hash_table_lookup(ieee802154e_map.long_table, &long_addr);
    if ( map_rec ) {
        /* indicates this mapping is invalid at frame fnum */
        map_rec->end_fnum = fnum;
        return TRUE;
    }

    return FALSE;
} /* ieee802154e_long_addr_invalidate */


/*FUNCTION:------------------------------------------------------
 *  NAME
 *      proto_init_ieee802154e
 *  DESCRIPTION
 *      Init routine for the IEEE 802.15.4 dissector. Creates hash
 *      tables for mapping between 16-bit to 64-bit addresses and
 *      populates them with static address pairs from a UAT
 *      preference table.
 *  PARAMETERS
 *      none
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
static void
proto_init_ieee802154e(void)
{
    guint       i;

    /* Destroy hash tables, if they exist. */
    if (ieee802154e_map.short_table)
        g_hash_table_destroy(ieee802154e_map.short_table);
    if (ieee802154e_map.long_table)
        g_hash_table_destroy(ieee802154e_map.long_table);

    /* Create the hash tables. */
    ieee802154e_map.short_table = g_hash_table_new(ieee802154e_short_addr_hash, ieee802154e_short_addr_equal);
    ieee802154e_map.long_table = g_hash_table_new(ieee802154e_long_addr_hash, ieee802154e_long_addr_equal);
    /* Re-load the hash table from the static address UAT. */
    for (i=0; (i<num_static_addrs) && (static_addrs); i++) {
        ieee802154e_addr_update(&ieee802154e_map,(guint16)static_addrs[i].addr16, (guint16)static_addrs[i].pan,
               pntoh64(static_addrs[i].eui64), ieee802154e_user, IEEE802154_USER_MAPPING);
    } /* for */
} /* proto_init_ieee802154e */

/* Returns the prompt string for the Decode-As dialog. */
static void ieee802154e_da_prompt(packet_info *pinfo _U_, gchar* result)
{
    ieee802154e_hints_t *hints;
    hints = (ieee802154e_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo,
                proto_get_id_by_filter_name(IEEE802154E_PROTOABBREV_WPAN), 0);
    g_snprintf(result, MAX_DECODE_AS_PROMPT_LEN, "IEEE 802.15.4e PAN 0x%04x as", hints->src_pan);
} /* iee802154_da_prompt */

/* Returns the value to index the panid decode table with (source PAN)*/
static gpointer ieee802154e_da_value(packet_info *pinfo _U_)
{
    ieee802154e_hints_t *hints;
    hints = (ieee802154e_hints_t *)p_get_proto_data(wmem_file_scope(), pinfo,
                proto_get_id_by_filter_name(IEEE802154E_PROTOABBREV_WPAN), 0);
    return GUINT_TO_POINTER((guint)(hints->src_pan));
} /* iee802154_da_value */

/*FUNCTION:------------------------------------------------------
 *  NAME
 *      proto_register_ieee802154e
 *  DESCRIPTION
 *      IEEE 802.15.4e protocol registration routine.
 *  PARAMETERS
 *      none
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
void proto_register_ieee802154e(void)
{
    /* Protocol fields  */
    static hf_register_info hf_phy[] = {
        /* PHY level */

        { &hf_ieee802154e_nonask_phy_preamble,
        { "Preamble",                       "wpane-nonask-phy.preamble", FT_UINT32, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_nonask_phy_sfd,
        { "Start of Frame Delimiter",       "wpane-nonask-phy.sfd", FT_UINT8, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_nonask_phy_length,
        { "Frame Length",                   "wpane-nonask-phy.frame_length", FT_UINT8, BASE_HEX, NULL,
            IEEE802154_PHY_LENGTH_MASK, NULL, HFILL }},
    };

    static hf_register_info hf[] = {

        { &hf_ieee802154e_frame_length,
        { "Frame Length",                   "wpane.frame_length", FT_UINT8, BASE_DEC, NULL, 0x0,
            "Frame Length as reported from lower layer", HFILL }},

        { &hf_ieee802154e_frame_type,
        { "Frame Type",                     "wpane.frame_type", FT_UINT16, BASE_HEX, VALS(ieee802154e_frame_types),
            IEEE802154_FCF_TYPE_MASK, NULL, HFILL }},

        { &hf_ieee802154e_security,
        { "Security Enabled",               "wpane.security", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_SEC_EN,
            "Whether security operations are performed at the MAC layer or not.", HFILL }},

        { &hf_ieee802154e_pending,
        { "Frame Pending",                  "wpane.pending", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_FRAME_PND,
            "Indication of additional packets waiting to be transferred from the source device.", HFILL }},

        { &hf_ieee802154e_ack_request,
        { "Acknowledge Request",            "wpane.ack_request", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_ACK_REQ,
            "Whether the sender of this packet requests acknowledgement or not.", HFILL }},

        { &hf_ieee802154e_intra_pan,
        { "Intra-PAN",                      "wpane.intra_pan", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_INTRA_PAN,
            "Whether this packet originated and terminated within the same PAN or not.", HFILL }},

        { &hf_ieee802154e_seqno,
        { "Sequence Number",                "wpane.seq_no", FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_dst_addr_mode,
        { "Destination Addressing Mode",    "wpane.dst_addr_mode", FT_UINT16, BASE_HEX, VALS(ieee802154e_addr_modes),
            IEEE802154_FCF_DADDR_MASK, NULL, HFILL }},

        { &hf_ieee802154e_seqnr_surpression,
        { "Sequence Number Surpression",    "wpane.seqnr_surpression", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_SEQNR_SURPRESSION,
            "Whether this packet uses sequence number surpression or not", HFILL }},

        { &hf_ieee802154e_ielist_present,
        { "IE list present",    "wpane.ielist_present", FT_BOOLEAN, 16, NULL, IEEE802154_FCF_IELIST_PRESENT,
            "Whether this packet uses IE list or not", HFILL }},

        { &hf_ieee802154e_src_addr_mode,
        { "Source Addressing Mode",         "wpane.src_addr_mode", FT_UINT16, BASE_HEX, VALS(ieee802154e_addr_modes),
            IEEE802154_FCF_SADDR_MASK, NULL, HFILL }},

        { &hf_ieee802154e_version,
        { "Frame Version",                  "wpane.version", FT_UINT16, BASE_DEC, NULL, IEEE802154_FCF_VERSION,
            NULL, HFILL }},

        { &hf_ieee802154e_dst_panID,
        { "Destination PAN",                "wpane.dst_pan", FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_dst16,
        { "Destination",                    "wpane.dst16", FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_dst64,
        { "Destination",                    "wpane.dst64", FT_EUI64, BASE_NONE, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_src_panID,
        { "Source PAN",                     "wpane.src_pan", FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_src16,
        { "Source",                         "wpane.src16", FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_src64,
        { "Extended Source",                "wpane.src64", FT_EUI64, BASE_NONE, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_src64_origin,
        { "Origin",                           "wpane.src64.origin", FT_FRAMENUM, BASE_NONE, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_fcs,
        { "FCS",                            "wpane.fcs", FT_UINT16, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_rssi,
        { "RSSI",                           "wpane.rssi", FT_INT8, BASE_DEC, NULL, 0x0,
            "Received Signal Strength", HFILL }},

        { &hf_ieee802154e_fcs_ok,
        { "FCS Valid",                      "wpane.fcs_ok", FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_correlation,
        { "LQI Correlation Value",          "wpane.correlation", FT_UINT8, BASE_DEC, NULL, 0x0,
            NULL, HFILL }},

            /*  Command Frame Specific Fields */
            /*--------------------------------*/

        { &hf_ieee802154e_cmd_id,
        { "Command Identifier",         "wpane.cmd", FT_UINT8, BASE_HEX, VALS(ieee802154e_cmd_names), 0x0,
            NULL, HFILL }},

            /*  Capability Information Fields */
        { &hf_ieee802154e_cinfo_alt_coord,
        { "Alternate PAN Coordinator",  "wpane.cinfo.alt_coord", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_ALT_PAN_COORD,
            "Whether this device can act as a PAN coordinator or not.", HFILL }},

        { &hf_ieee802154e_cinfo_device_type,
        { "Device Type",                "wpane.cinfo.device_type", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_DEVICE_TYPE,
            "Whether this device is RFD (reduced-function device) or FFD (full-function device).", HFILL }},

        { &hf_ieee802154e_cinfo_power_src,
        { "Power Source",               "wpane.cinfo.power_src", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_POWER_SRC,
            "Whether this device is operating on AC/mains or battery power.", HFILL }},

        { &hf_ieee802154e_cinfo_idle_rx,
        { "Receive On When Idle",       "wpane.cinfo.idle_rx", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_IDLE_RX,
            "Whether this device can receive packets while idle or not.", HFILL }},

        { &hf_ieee802154e_cinfo_sec_capable,
        { "Security Capability",        "wpane.cinfo.sec_capable", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_SEC_CAPABLE,
            "Whether this device is capable of receiving encrypted packets.", HFILL }},

        { &hf_ieee802154e_cinfo_alloc_addr,
        { "Allocate Address",           "wpane.cinfo.alloc_addr", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_CINFO_ALLOC_ADDR,
            "Whether this device wishes to use a 16-bit short address instead of its IEEE 802.15.4 64-bit long address.", HFILL }},

            /*  Association response fields */
        { &hf_ieee802154e_assoc_addr,
        { "Short Address",              "wpane.asoc.addr", FT_UINT16, BASE_HEX, NULL, 0x0,
            "The short address that the device should assume. An address of 0xfffe indicates that the device should use its IEEE 64-bit long address.", HFILL }},

        { &hf_ieee802154e_assoc_status,
        { "Association Status",         "wpane.assoc.status", FT_UINT8, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

        { &hf_ieee802154e_disassoc_reason,
        { "Disassociation Reason",      "wpane.disassoc.reason", FT_UINT8, BASE_HEX, NULL, 0x0,
            NULL, HFILL }},

            /*  Coordinator Realignment fields */
        { &hf_ieee802154e_realign_pan,
        { "PAN ID",                     "wpane.realign.pan", FT_UINT16, BASE_HEX, NULL, 0x0,
            "The PAN identifier the coordinator wishes to use for future communication.", HFILL }},

        { &hf_ieee802154e_realign_caddr,
        { "Coordinator Short Address",  "wpane.realign.addr", FT_UINT16, BASE_HEX, NULL, 0x0,
            "The 16-bit address the coordinator wishes to use for future communication.", HFILL }},

        { &hf_ieee802154e_realign_channel,
        { "Logical Channel",            "wpane.realign.channel", FT_UINT8, BASE_DEC, NULL, 0x0,
            "The logical channel the coordinator wishes to use for future communication.", HFILL }},

        { &hf_ieee802154e_realign_addr,
        { "Short Address",              "wpane.realign.addr", FT_UINT16, BASE_HEX, NULL, 0x0,
            "A short-address that the orphaned device shall assume if applicable.", HFILL }},

        { &hf_ieee802154e_realign_channel_page,
        { "Channel Page",               "wpane.realign.channel_page", FT_UINT8, BASE_DEC, NULL, 0x0,
            "The logical channel page the coordinator wishes to use for future communication.", HFILL }},

        { &hf_ieee802154e_gtsreq_len,
        { "GTS Length",                 "wpane.gtsreq.length", FT_UINT8, BASE_DEC, NULL, IEEE802154_CMD_GTS_REQ_LEN,
            "Number of superframe slots the device is requesting.", HFILL }},

        { &hf_ieee802154e_gtsreq_dir,
        { "GTS Direction",              "wpane.gtsreq.direction", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_GTS_REQ_DIR,
            "The direction of traffic in the guaranteed timeslot.", HFILL }},

        { &hf_ieee802154e_gtsreq_type,
        { "Characteristic Type",        "wpane.gtsreq.type", FT_BOOLEAN, 8, NULL, IEEE802154_CMD_GTS_REQ_TYPE,
            "Whether this request is to allocate or deallocate a timeslot.", HFILL }},

            /*  Beacon Frame Specific Fields */
            /*-------------------------------*/
        { &hf_ieee802154e_beacon_order,
        { "Beacon Interval",            "wpane.beacon_order", FT_UINT16, BASE_DEC, NULL, IEEE802154_BEACON_ORDER_MASK,
            "Specifies the transmission interval of the beacons.", HFILL }},

        { &hf_ieee802154e_superframe_order,
        { "Superframe Interval",        "wpane.superframe_order", FT_UINT16, BASE_DEC, NULL,
            IEEE802154_SUPERFRAME_ORDER_MASK,
            "Specifies the length of time the coordinator will interact with the PAN.", HFILL }},

        { &hf_ieee802154e_cap,
        { "Final CAP Slot",             "wpane.cap", FT_UINT16, BASE_DEC, NULL, IEEE802154_SUPERFRAME_CAP_MASK,
            "Specifies the final superframe slot used by the CAP.", HFILL }},

        { &hf_ieee802154e_superframe_battery_ext,
        { "Battery Extension",          "wpane.battery_ext", FT_BOOLEAN, 16, NULL, IEEE802154_BATT_EXTENSION_MASK,
            "Whether transmissions may not extend past the length of the beacon frame.", HFILL }},

        { &hf_ieee802154e_superframe_coord,
        { "PAN Coordinator",            "wpane.bcn_coord", FT_BOOLEAN, 16, NULL, IEEE802154_SUPERFRAME_COORD_MASK,
            "Whether this beacon frame is being transmitted by the PAN coordinator or not.", HFILL }},

        { &hf_ieee802154e_assoc_permit,
        { "Association Permit",         "wpane.assoc_permit", FT_BOOLEAN, 16, NULL, IEEE802154_ASSOC_PERMIT_MASK,
            "Whether this PAN is accepting association requests or not.", HFILL }},

        { &hf_ieee802154e_gts_count,
        { "GTS Descriptor Count",       "wpane.gts.count", FT_UINT8, BASE_DEC, NULL, 0x0,
            "The number of GTS descriptors present in this beacon frame.", HFILL }},

        { &hf_ieee802154e_gts_permit,
        { "GTS Permit",                 "wpane.gts.permit", FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "Whether the PAN coordinator is accepting GTS requests or not.", HFILL }},

        { &hf_ieee802154e_gts_direction,
        { "Direction",                  "wpane.gts.direction", FT_BOOLEAN, BASE_NONE, TFS(&ieee802154e_gts_direction_tfs), 0x0,
            "A flag defining the direction of the GTS Slot.", HFILL }},

        { &hf_ieee802154e_pending16,
        { "Address",                    "wpane.pending16", FT_UINT16, BASE_HEX, NULL, 0x0,
            "Device with pending data to receive.", HFILL }},

        { &hf_ieee802154e_pending64,
        { "Address",                    "wpane.pending64", FT_EUI64, BASE_NONE, NULL, 0x0,
            "Device with pending data to receive.", HFILL }},

            /* Auxiliary Security Header Fields */
            /*----------------------------------*/
        { &hf_ieee802154e_security_level,
        { "Security Level", "wpane.aux_sec.sec_level", FT_UINT8, BASE_HEX, VALS(ieee802154e_sec_level_names),
            IEEE802154_AUX_SEC_LEVEL_MASK, "The Security Level of the frame", HFILL }},

        { &hf_ieee802154e_key_id_mode,
        { "Key Identifier Mode", "wpane.aux_sec.key_id_mode", FT_UINT8, BASE_HEX, VALS(ieee802154e_key_id_mode_names),
            IEEE802154_AUX_KEY_ID_MODE_MASK,
            "The scheme to use by the recipient to lookup the key in its key table", HFILL }},

        { &hf_ieee802154e_aux_sec_reserved,
        { "Reserved", "wpane.aux_sec.reserved", FT_UINT8, BASE_HEX, NULL, IEEE802154_AUX_KEY_RESERVED_MASK,
            NULL, HFILL }},

        { &hf_ieee802154e_aux_sec_frame_counter,
        { "Frame Counter", "wpane.aux_sec.frame_counter", FT_UINT32, BASE_DEC, NULL, 0x0,
            "Frame counter of the originator of the protected frame", HFILL }},

        { &hf_ieee802154e_aux_sec_key_source,
        { "Key Source", "wpane.aux_sec.key_source", FT_UINT64, BASE_HEX, NULL, 0x0,
            "Key Source for processing of the protected frame", HFILL }},

        { &hf_ieee802154e_aux_sec_key_index,
        { "Key Index", "wpane.aux_sec.key_index", FT_UINT8, BASE_HEX, NULL, 0x0,
            "Key Index for processing of the protected frame", HFILL }},

            /* IEEE 802.15.4-2003 Security Header Fields */
        { &hf_ieee802154e_sec_frame_counter,
        { "Frame Counter", "wpane.sec_frame_counter", FT_UINT32, BASE_HEX, NULL, 0x0,
            "Frame counter of the originator of the protected frame (802.15.4-2003)", HFILL }},

        { &hf_ieee802154e_sec_key_sequence_counter,
        { "Key Sequence Counter", "wpane.sec_key_sequence_counter", FT_UINT8, BASE_HEX, NULL, 0x0,
            "Key Sequence counter of the originator of the protected frame (802.15.4-2003)", HFILL }},

        { &hf_ieee802154e_hie_length,
        { "Header IE Length",                    "wpane.hie_length", FT_UINT16, BASE_DEC, NULL, 0x0,
            "Length of Header IE.", HFILL }},

        { &hf_ieee802154e_hie_elementID,
        { "Header IE Element ID",                    "wpane.hie_elementID", FT_UINT8, BASE_DEC, NULL, 0x0,
            "Element ID of Header IE.", HFILL }},

        { &hf_ieee802154e_pie_length,
        { "Payload IE Length",                    "wpane.pie_length", FT_UINT16, BASE_DEC, NULL, 0x0,
            "Length of Payload IE.", HFILL }},

        { &hf_ieee802154e_pie_groupID,
        { "Payload IE Group ID",                    "wpane.pie_groupID", FT_UINT8, BASE_DEC, NULL, 0x0,
            "Group ID of Payload IE.", HFILL }}
    };

    /* Subtrees */
    static gint *ett[] = {
        &ett_ieee802154e_nonask_phy,
        &ett_ieee802154e_nonask_phy_phr,
        &ett_ieee802154e,
        &ett_ieee802154e_fcf,
        &ett_ieee802154e_auxiliary_security,
        &ett_ieee802154e_aux_sec_control,
        &ett_ieee802154e_aux_sec_key_id,
        &ett_ieee802154e_fcs,
        &ett_ieee802154e_cmd,
        &ett_ieee802154e_superframe,
        &ett_ieee802154e_gts,
        &ett_ieee802154e_gts_direction,
        &ett_ieee802154e_gts_descriptors,
        &ett_ieee802154e_pendaddr
    };

    static ei_register_info ei[] = {
        { &ei_ieee802154e_invalid_addressing, { "wpane.invalid_addressing", PI_MALFORMED, PI_WARN, "Invalid Addressing", EXPFILL }},
        { &ei_ieee802154e_dst, { "wpane.dst_invalid", PI_MALFORMED, PI_ERROR, "Invalid Destination Address Mode", EXPFILL }},
        { &ei_ieee802154e_src, { "wpane.src_invalid", PI_MALFORMED, PI_ERROR, "Invalid Source Address Mode", EXPFILL }},
        { &ei_ieee802154e_decrypt_error, { "wpane.decrypt_error", PI_UNDECODED, PI_WARN, "Decryption error", EXPFILL }},
        { &ei_ieee802154e_fcs, { "wpane.fcs.bad", PI_CHECKSUM, PI_WARN, "Bad FCS", EXPFILL }},
    };

    /* Preferences. */
    module_t *ieee802154e_module;
    expert_module_t* expert_ieee802154e;

    static uat_field_t addr_uat_flds[] = {
        UAT_FLD_HEX(addr_uat,addr16,"Short Address",
                "16-bit short address in hexadecimal."),
        UAT_FLD_HEX(addr_uat,pan,"PAN Identifier",
                "16-bit PAN identifier in hexadecimal."),
        UAT_FLD_BUFFER(addr_uat,eui64,"EUI-64",
                "64-bit extended unique identifier."),
        UAT_END_FIELDS
    };

    static build_valid_func     ieee802154e_da_build_value[1] = {ieee802154e_da_value};
    static decode_as_value_t    ieee802154e_da_values = {ieee802154e_da_prompt, 1, ieee802154e_da_build_value};
    static decode_as_t          ieee802154e_da = {
        IEEE802154E_PROTOABBREV_WPAN, "PAN", IEEE802154E_PROTOABBREV_WPAN_PANID,
        1, 0, &ieee802154e_da_values, NULL, NULL,
        decode_as_default_populate_list, decode_as_default_reset, decode_as_default_change, NULL
    };

    /* Register the init routine. */
    register_init_routine(proto_init_ieee802154e);

    /*  Register Protocol name and description. */
    proto_ieee802154e = proto_register_protocol("IEEE 802.15.4e Low-Rate Wireless PAN", "IEEE 802.15.4e",
           IEEE802154E_PROTOABBREV_WPAN);
    proto_ieee802154e_nonask_phy = proto_register_protocol("IEEE 802.15.4e Low-Rate Wireless PAN non-ASK PHY",
            "IEEE 802.15.4e non-ASK PHY", "wpane-nonask-phy");

    /*  Register header fields and subtrees. */
    proto_register_field_array(proto_ieee802154e, hf, array_length(hf));
    proto_register_field_array(proto_ieee802154e, hf_phy, array_length(hf_phy));

    proto_register_subtree_array(ett, array_length(ett));

    expert_ieee802154e = expert_register_protocol(proto_ieee802154e);
    expert_register_field_array(expert_ieee802154e, ei, array_length(ei));

    /* add a user preference to set the 802.15.4 ethertype */
    ieee802154e_module = prefs_register_protocol(proto_ieee802154e,
                                   proto_reg_handoff_ieee802154e);
    prefs_register_uint_preference(ieee802154e_module, "802154_ethertype",
                                   "802.15.4 Ethertype (in hex)",
                                   "(Hexadecimal) Ethertype used to indicate IEEE 802.15.4 frame.",
                                   16, &ieee802154_ethertype);
    prefs_register_bool_preference(ieee802154e_module, "802154_cc24xx",
                                   "TI CC24xx FCS format",
                                   "Set if the FCS field is in TI CC24xx format.",
                                   &ieee802154e_cc24xx);
    prefs_register_bool_preference(ieee802154e_module, "802154_fcs_ok",
                                   "Dissect only good FCS",
                                   "Dissect payload only if FCS is valid.",
                                   &ieee802154e_fcs_ok);

    /* Create a UAT for static address mappings. */
    static_addr_uat = uat_new("Static Addresses",
            sizeof(static_addr_t),      /* record size */
            "802154_addresses",         /* filename */
            TRUE,                       /* from_profile */
            &static_addrs,              /* data_ptr */
            &num_static_addrs,          /* numitems_ptr */
            UAT_AFFECTS_DISSECTION,     /* affects dissection of packets, but not set of named fields */
            NULL,                       /* help */
            NULL,                       /* copy callback */
            addr_uat_update_cb,         /* update callback */
            NULL,                       /* free callback */
            NULL,                       /* post update callback */
            addr_uat_flds);             /* UAT field definitions */
    prefs_register_uat_preference(ieee802154e_module, "static_addr",
                "Static Addresses",
                "A table of static address mappings between 16-bit short addressing and EUI-64 addresses",
                static_addr_uat);

    /* Register preferences for a decryption key */
    /* TODO: Implement a UAT for multiple keys, and with more advanced key management. */
    prefs_register_string_preference(ieee802154e_module, "802154_key", "Decryption key",
            "128-bit decryption key in hexadecimal format", (const char **)&ieee802154e_key_str);

    prefs_register_enum_preference(ieee802154e_module, "802154_sec_suite",
                                   "Security Suite (802.15.4-2003)",
                                   "Specifies the security suite to use for 802.15.4-2003 secured frames"
                                   " (only supported suites are listed). Option ignored for 802.15.4-2006"
                                   " and unsecured frames.",
                                   &ieee802154e_sec_suite, ieee802154e_2003_sec_suite_enums, FALSE);

    prefs_register_bool_preference(ieee802154e_module, "802154_extend_auth",
                                   "Extend authentication data (802.15.4-2003)",
                                   "Set if the manufacturer extends the authentication data with the"
                                   " security header. Option ignored for 802.15.4-2006 and unsecured frames.",
                                   &ieee802154e_extend_auth);

    /* Register the subdissector list */
    panid_dissector_table = register_dissector_table(IEEE802154E_PROTOABBREV_WPAN_PANID, "IEEE 802.15.4e PANID", FT_UINT16, BASE_HEX);
    register_heur_dissector_list(IEEE802154E_PROTOABBREV_WPAN, &ieee802154e_heur_subdissector_list);
    register_heur_dissector_list(IEEE802154E_PROTOABBREV_WPAN_BEACON, &ieee802154e_beacon_subdissector_list);

    /*  Register dissectors with Wireshark. */
    register_dissector(IEEE802154E_PROTOABBREV_WPAN, dissect_ieee802154e, proto_ieee802154e);
    register_dissector("wpane_nofcs", dissect_ieee802154e_nofcs, proto_ieee802154e);
    register_dissector("wpane_cc24xx", dissect_ieee802154e_cc24xx, proto_ieee802154e);
    register_dissector("wpane-nonask-phy", dissect_ieee802154e_nonask_phy, proto_ieee802154e_nonask_phy);

    /* Register a Decode-As handler. */
    register_decode_as(&ieee802154e_da);
} /* proto_register_ieee802154e */


/*FUNCTION:------------------------------------------------------
 *  NAME
 *      proto_reg_handoff_ieee802154e
 *  DESCRIPTION
 *      Registers the IEEE 802.15.4 dissector with Wireshark.
 *      Will be called every time 'apply' is pressed in the preferences menu.
 *       as well as during Wireshark initialization
 *  PARAMETERS
 *      none
 *  RETURNS
 *      void
 *---------------------------------------------------------------
 */
void proto_reg_handoff_ieee802154e(void)
{
    static gboolean            prefs_initialized = FALSE;
    static dissector_handle_t  ieee802154e_handle;
    static dissector_handle_t  ieee802154e_nonask_phy_handle;
    static dissector_handle_t  ieee802154e_nofcs_handle;
    static unsigned int        old_ieee802154_ethertype;
    GByteArray                *bytes;
    gboolean                   res;

    if (!prefs_initialized){
        /* Get the dissector handles. */
        ieee802154e_handle   = find_dissector(IEEE802154E_PROTOABBREV_WPAN);
        ieee802154e_nonask_phy_handle = find_dissector("wpane-nonask-phy");
        ieee802154e_nofcs_handle = find_dissector("wpane_nofcs");
        data_handle         = find_dissector("data");

        dissector_add_uint("wtap_encap", WTAP_ENCAP_IEEE802_15_4, ieee802154e_handle);
        dissector_add_uint("wtap_encap", WTAP_ENCAP_IEEE802_15_4_NONASK_PHY, ieee802154e_nonask_phy_handle);
        dissector_add_uint("wtap_encap", WTAP_ENCAP_IEEE802_15_4_NOFCS, ieee802154e_nofcs_handle);
        dissector_add_uint("sll.ltype", LINUX_SLL_P_IEEE802154, ieee802154e_handle);

        prefs_initialized = TRUE;
    } else {
        dissector_delete_uint("ethertype", old_ieee802154_ethertype, ieee802154e_handle);
    }

    old_ieee802154_ethertype = ieee802154_ethertype;

    /* Get the IEEE 802.15.4 decryption key. */
    bytes = g_byte_array_new();
    res = hex_str_to_bytes(ieee802154e_key_str, bytes, FALSE);
    ieee802154e_key_valid =  (res && bytes->len >= IEEE802154_CIPHER_SIZE);
    if (ieee802154e_key_valid) {
        memcpy(ieee802154e_key, bytes->data, IEEE802154_CIPHER_SIZE);
    }
    g_byte_array_free(bytes, TRUE);

    /* Register dissector handles. */
    dissector_add_uint("ethertype", ieee802154_ethertype, ieee802154e_handle);
} /* proto_reg_handoff_ieee802154e */
