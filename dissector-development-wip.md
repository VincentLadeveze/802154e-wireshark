Dernier patch wireshark (02/07/2014), méthodes impactées sur packet-ieee802154.c :
==================================================================================
- dissect_ieee802154_nonask_phy
- dissect_ieee802154_common
- dissect_ieee802154_superframe
- dissect_ieee802154_pendaddr
- dissect_ieee802154_assoc_rsp
- dissect_ieee802154_realign
- dissect_ieee802154_gtsreq



Dissector 802.1.54 inside other one :
=====================================
epan/dissectors/packet-6lowpan.c
epan/dissectors/packet-icmpv6.c
epan/dissectors/packet-lwm.c
epan/dissectors/packet-scop.c
epan/dissectors/packet-sll.c
epan/dissectors/packet-zbee.h
epan/dissectors/packet-zbee-nwk.c
epan/dissectors/packet-zbee-nwk.h
epan/dissectors/packet-zbee-nwk-gp.c
epan/dissectors/packet-zbee-security.c
epan/dissectors/packet-zep.c
---> Liste des fichiers à toucher pour le dissector "wpane"


Modif pour pour inclure le dissector"wpane"
===========================================

Ajout d'une directive de compilation : soit on a "wpan" (15.4) soit on a "wpane" (15.4e)
----------------------------------------------------------------------------------------
	Add in $WIRESHARK_CODE_ROOT/config.nmake
	****************************************
# Only for 802.15.4e
WPAN_CODE=^#define HAVE_WPANE 1

	Add in $WIRESHARK_CODE_ROOT/Makefile.nmake
	******************************************
-e "s/@HAVE_WPANE@/$(WPAN_CODE)/" \

	Add in $WIRESHARK_CODE_ROOT/config.h.win32
	******************************************
@HAVE_WPANE@

Changement dans les includes : packet-ieee802154e.h contient un include de packet-ieee802154.h
----------------------------------------------------------------------------------------------
	- Nettoyage du superflux (doublons)
	- Permet à packet-6lowpan.c (et autres) d'avoir 1 seul include (packet-ieee802154e.h) sur les 2.
QUESTION : A quel point puis-je rendre dépendant le dissector "wpane" du dissector "wpan" ?


Implémentation directive de compilation
---------------------------------------
A chaque dépendance du code on ajoute :
#ifdef HAVE_WPANE
    .... code WPANE
#else
    .... code 802.15.4

Fichiers modifiés : 
epan/dissectors/packet-icmpv6.c, 
epan/dissectors/packet-6lowpan.c, 
epan/dissectors/packet-lwm.c, 
epan/dissectors/packet-scop.c
epan/dissectors/packet-zbee-nwk.c
epan/dissectors/packet-zbee-nwk.h
epan/dissectors/packet-zbee-nwk-gp.c
epan/dissectors/packet-zbee-security.c
epan/dissectors/packet-zep.c
