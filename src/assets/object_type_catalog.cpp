#include "assets/object_type_catalog.hpp"

#include <array>

namespace sl_open::assets
{
namespace
{
// Exact 256-entry ObjectTypeResource table compiled at
// LANCER.EXE 0x004f7490. Each retail record is 0x14 bytes; these are its
// filename and optional HUD SCEM pointers at +0x00/+0x04.
constexpr std::array<
	ObjectTypeResourceDefinition,
	kObjectTypeResourceCount> kResources{{
	{"uslf_prd.shp", "predscem.spr"}, // 0x00
	{"jlf_nagi.shp", "nagiscem.spr"}, // 0x01
	{"german_grendal.shp", "grenscem.spr"}, // 0x02
	{"british_crusader.shp", "crusscem.spr"}, // 0x03
	{"usa_coyote.shp", "coyoscem.spr"}, // 0x04
	{"french_mirage.shp", "mirascem.spr"}, // 0x05
	{"british_tempest.shp", "tempscem.spr"}, // 0x06
	{"usmf_pat.shp", "patrscem.spr"}, // 0x07
	{"german_wolverine.shp", "wolvscem.spr"}, // 0x08
	{"ushf_reaper.shp", "reapscem.spr"}, // 0x09
	{"jap_shroud.shp", "shroscem.spr"}, // 0x0a
	{"uspf_phx.shp", "phxscem.spr"}, // 0x0b
	{"reliant.shp", "reliscem.spr"}, // 0x0c
	{"yamato.shp", "yamascem.spr"}, // 0x0d
	{nullptr, nullptr}, // 0x0e
	{"kestrel.shp", "kestscem.spr"}, // 0x0f
	{nullptr, nullptr}, // 0x10
	{"victorious.shp", "victscem.spr"}, // 0x11
	{"endeavour.shp", "endescem.spr"}, // 0x12
	{"mitchell.shp", "mitcscem.spr"}, // 0x13
	{"bremen.shp", "bremscem.spr"}, // 0x14
	{nullptr, nullptr}, // 0x15
	{"ulysses.shp", "ulysscem.spr"}, // 0x16
	{"jap_sai.shp", "saiscem.spr"}, // 0x17
	{"nanny.shp", "nannscem.spr"}, // 0x18
	{"btb_glhd.shp", "galascem.spr"}, // 0x19
	{"ustb_hds.shp", "hadescem.spr"}, // 0x1a
	{nullptr, nullptr}, // 0x1b
	{"lonestar.shp", "lonescem.spr"}, // 0x1c
	{"limpet_t_car.shp", "boarscem.spr"}, // 0x1d
	{"us_prowler.shp", "prowscem.spr"}, // 0x1e
	{"ripper_2.shp", "ripscem.spr"}, // 0x1f
	{"lueneburg.shp", "luenscem.spr"}, // 0x20
	{"a_mammoth.shp", "mammscem.spr"}, // 0x21
	{nullptr, nullptr}, // 0x22
	{nullptr, nullptr}, // 0x23
	{nullptr, nullptr}, // 0x24
	{"stork.shp", "storscem.spr"}, // 0x25
	{nullptr, nullptr}, // 0x26
	{"chin_han.shp", "haidscem.spr"}, // 0x27
	{"chin_zhuhai.shp", "karascem.spr"}, // 0x28
	{"luda.shp", "saliscem.spr"}, // 0x29
	{"houjian.shp", "azanscem.spr"}, // 0x2a
	{"rus_sabre.shp", "sabrscem.spr"}, // 0x2b
	{"rus_lagg.shp", "laggscem.spr"}, // 0x2c
	{"rus_kamov.shp", "kamoscem.spr"}, // 0x2d
	{"mid_saracen.shp", "sarascem.spr"}, // 0x2e
	{nullptr, nullptr}, // 0x2f
	{"scimitar.shp", "scimscem.spr"}, // 0x30
	{"rus_basalisk.shp", "basiscem.spr"}, // 0x31
	{"rus_kossac.shp", "kossscem.spr"}, // 0x32
	{nullptr, nullptr}, // 0x33
	{"ramases.shp", "ramascem.spr"}, // 0x34
	{"krasnaya.shp", "krasscem.spr"}, // 0x35
	{"kiev.shp", "morzscem.spr"}, // 0x36
	{"cs_badanov.shp", "badascem.spr"}, // 0x37
	{"pukov.shp", "pukoscem.spr"}, // 0x38
	{nullptr, nullptr}, // 0x39
	{"song.shp", "rizascem.spr"}, // 0x3a
	{"cyclops.shp", "cyclscem.spr"}, // 0x3b
	{"rus_kurgan.shp", "kurgscem.spr"}, // 0x3c
	{"sharov.shp", "shavscem.spr"}, // 0x3d
	{"rmc_gurevich.shp", "gurescem.spr"}, // 0x3e
	{"berijev.shp", "beriscem.spr"}, // 0x3f
	{"kiev.shp", "kievscem.spr"}, // 0x40
	{"crp_loki.shp", "lokiscem.spr"}, // 0x41
	{"cld_scarab.shp", "kalascem.spr"}, // 0x42
	{"saladin.shp", "salascem.spr"}, // 0x43
	{"darkreign.shp", "dreiscem.spr"}, // 0x44
	{"stalag.shp", "stalscem.spr"}, // 0x45
	{"antanov.shp", "antascem.spr"}, // 0x46
	{"kronstadt.shp", "kronscem.spr"}, // 0x47
	{"boridin.shp", "boriscem.spr"}, // 0x48
	{"rus_troopcar.shp", "scarscem.spr"}, // 0x49
	{"torpedo.shp", nullptr}, // 0x4a
	{"ulysses2.shp", nullptr}, // 0x4b
	{"ulysses3.shp", nullptr}, // 0x4c
	{"uly_escape.shp", "epodscem.spr"}, // 0x4d
	{"deb_1.shp", nullptr}, // 0x4e
	{"deb_2.shp", nullptr}, // 0x4f
	{"deb_3.shp", nullptr}, // 0x50
	{"deb_4.shp", nullptr}, // 0x51
	{"deb_5.shp", nullptr}, // 0x52
	{"deb_6.shp", nullptr}, // 0x53
	{"deb_7.shp", nullptr}, // 0x54
	{"deb_8.shp", nullptr}, // 0x55
	{"deb_9.shp", nullptr}, // 0x56
	{"deb_10.shp", nullptr}, // 0x57
	{"rus_man1.shp", nullptr}, // 0x58
	{"rus_man2.shp", nullptr}, // 0x59
	{"rus_man3.shp", nullptr}, // 0x5a
	{"rus_man4.shp", nullptr}, // 0x5b
	{"rus_torp.shp", "russtorp.spr"}, // 0x5c
	{"A_mammoth2.shp", "mammscem.spr"}, // 0x5d
	{"baxter.shp", "baxtscem.spr"}, // 0x5e
	{"neptune_hi_1.shp", nullptr}, // 0x5f
	{"triton_hi_1.shp", nullptr}, // 0x60
	{"uranus_hi_1.shp", nullptr}, // 0x61
	{"saturn_hi_ring_2.shp", nullptr}, // 0x62
	{"titan_hi_1.shp", nullptr}, // 0x63
	{"jupiter_hi_1.shp", nullptr}, // 0x64
	{"europa_hi_1.shp", nullptr}, // 0x65
	{"grany_hi_1.shp", nullptr}, // 0x66
	{"io_hi_1.shp", nullptr}, // 0x67
	{"calist_hi_1.shp", nullptr}, // 0x68
	{"venus1_hi_1.shp", nullptr}, // 0x69
	{"earth.shp", nullptr}, // 0x6a
	{"kronstadt dest.shp", nullptr}, // 0x6b
	{"boridin dest.shp", nullptr}, // 0x6c
	{"coalprotogate.shp", "protscem.spr"}, // 0x6d
	{"coaladvgate.shp", "agatscem.spr"}, // 0x6e
	{"mine_prox.shp", "proxscem.spr"}, // 0x6f
	{"black_box.shp", "bboxscem.spr"}, // 0x70
	{"stork_sat.shp", "ssatscem.spr"}, // 0x71
	{"mamdest.shp", "mammscem.spr"}, // 0x72
	{"mamdest2.shp", "mammscem.spr"}, // 0x73
	{"coalprotogate_plates.shp", "protscem.spr"}, // 0x74
	{"cs_baddead.shp", "badascem.spr"}, // 0x75
	{"cs_baddead2.shp", "badascem.spr"}, // 0x76
	{"rus_kurgan_dest.shp", "kurgscem.spr"}, // 0x77
	{"krasnaya.shp", "krasscem.spr"}, // 0x78
	{"ast_1.shp", nullptr}, // 0x79
	{"ast_2.shp", nullptr}, // 0x7a
	{"ast_3.shp", nullptr}, // 0x7b
	{"ast_4.shp", nullptr}, // 0x7c
	{"ast_5.shp", nullptr}, // 0x7d
	{"ast_6.shp", nullptr}, // 0x7e
	{"ast_7.shp", nullptr}, // 0x7f
	{"coalresstat.shp", "resbscem.spr"}, // 0x80
	{"latov.shp", "latoscem.spr"}, // 0x81
	{"RMC_gurevich_dest.shp", nullptr}, // 0x82
	{"training_hoop.shp", "trinscem.spr"}, // 0x83
	{"czar_docked.shp", "czadscem.spr"}, // 0x84
	{"turast_1.shp", nullptr}, // 0x85
	{"turast_2.shp", nullptr}, // 0x86
	{"turast_3.shp", nullptr}, // 0x87
	{"turast_4.shp", nullptr}, // 0x88
	{"turast_5.shp", nullptr}, // 0x89
	{"turast_6.shp", nullptr}, // 0x8a
	{"turast_7.shp", nullptr}, // 0x8b
	{"DMPowerup.shp", nullptr}, // 0x8c
	{"beacongate.shp", "nukescem.spr"}, // 0x8d
	{"DMBeacon.shp", "beacscem.spr"}, // 0x8e
	{"pukov_dest1.shp", nullptr}, // 0x8f
	{"ber_escape.shp", "escascem.spr"}, // 0x90
	{"us_cargo.shp", "cargscem.spr"}, // 0x91
	{"ussr_cargo.shp", "car2scem.spr"}, // 0x92
	{"grazer.shp", "grazscem.spr"}, // 0x93
	{"archer.shp", "archscem.spr"}, // 0x94
	{"kafelnikof.shp", "kafescem.spr"}, // 0x95
	{"reliant_destback.shp", nullptr}, // 0x96
	{"stalag_doors.shp", nullptr}, // 0x97
	{"stalag_ductcover.shp", nullptr}, // 0x98
	{"russ_shuttle.shp", "yakoscem.spr"}, // 0x99
	{"krasny.shp", "badascem.spr"}, // 0x9a
	{"varyag.shp", "varyscem.spr"}, // 0x9b
	{"ramases.shp", "ramascem.spr"}, // 0x9c
	{"carter.shp", "baxtscem.spr"}, // 0x9d
	{"bremen.shp", "bremscem.spr"}, // 0x9e
	{"washington.shp", "victscem.spr"}, // 0x9f
	{"mitchell.shp", "mitcscem.spr"}, // 0xa0
	{"zakov.shp", "zakoscem.spr"}, // 0xa1
	{"zakov.shp", "zakoscem.spr"}, // 0xa2
	{"Krasnaya LeftArm.SHP", nullptr}, // 0xa3
	{"Krasnaya RightArm.SHP", nullptr}, // 0xa4
	{"rogue_base.shp", "rbasscem.spr"}, // 0xa5
	{"kronstadt arm.shp", nullptr}, // 0xa6
	{"boridin dome.shp", nullptr}, // 0xa7
	{"boridin breakaway.shp", "borbscem.spr"}, // 0xa8
	{"Darkreign_DestHat.shp", nullptr}, // 0xa9
	{"Yamato DestBack.shp", nullptr}, // 0xaa
	{"service_droid.shp", "sdroscem.spr"}, // 0xab
	{"service_astronaut.shp", nullptr}, // 0xac
	{"Rogue_base top dest.SHP", nullptr}, // 0xad
	{"rus_tank.shp", nullptr}, // 0xae
	{"shoot_me.shp", "ttarscem.spr"}, // 0xaf
	{"zakov.shp", "zakoscem.spr"}, // 0xb0
	{"shell.shp", nullptr}, // 0xb1
	{"rockchunk00.SHP", nullptr}, // 0xb2
	{"rockchunk01.SHP", nullptr}, // 0xb3
	{"rockchunk02.SHP", nullptr}, // 0xb4
	{"rockchunk03.SHP", nullptr}, // 0xb5
	{"rockchunk04.SHP", nullptr}, // 0xb6
	{"latov_chunk1.shp", nullptr}, // 0xb7
	{"latov_chunk2.shp", nullptr}, // 0xb8
	{"czar.shp", "czarscem.spr"}, // 0xb9
	{"zakov_FrntDest.SHP", nullptr}, // 0xba
	{"sharov backdest.shp", nullptr}, // 0xbb
	{"limpet_pod.shp", nullptr}, // 0xbc
	{"stalag dest01.shp", nullptr}, // 0xbd
	{"stalag dest02.shp", nullptr}, // 0xbe
	{"saladin dest.shp", nullptr}, // 0xbf
	{nullptr, nullptr}, // 0xc0
	{nullptr, nullptr}, // 0xc1
	{"kiev.shp", "kievscem.spr"}, // 0xc2
	{"kiev backdest.shp", nullptr}, // 0xc3
	{"baxter topdest.shp", nullptr}, // 0xc4
	{"victorious frontdest.shp", nullptr}, // 0xc5
	{"mitchell_dest.shp", nullptr}, // 0xc6
	{"fort bear.shp", "bearscem.spr"}, // 0xc7
	{"fort_sherman.shp", "sherscem.spr"}, // 0xc8
	{"neptune_lo_1.shp", nullptr}, // 0xc9
	{"triton_lo_1.shp", nullptr}, // 0xca
	{"uranus_lo_1.shp", nullptr}, // 0xcb
	{"saturn_lo_ring_1.shp", nullptr}, // 0xcc
	{"titan_lo_1.shp", nullptr}, // 0xcd
	{"jupiter_lo_1.shp", nullptr}, // 0xce
	{"europa_lo_1.shp", nullptr}, // 0xcf
	{"grany_lo_1.shp", nullptr}, // 0xd0
	{"io_lo_1.shp", nullptr}, // 0xd1
	{"calist_lo_1.shp", nullptr}, // 0xd2
	{"venus1_lo_1.shp", nullptr}, // 0xd3
	{"yam_tube.shp", nullptr}, // 0xd4
	{"yamhanger.shp", nullptr}, // 0xd5
	{"reliant_hang.shp", nullptr}, // 0xd6
	{"comms relay.shp", "comsscem.spr"}, // 0xd7
	{"saladin_link.shp", nullptr}, // 0xd8
	{"varyag frontdest.shp", nullptr}, // 0xd9
	{"kestrel.shp", "kestscem.spr"}, // 0xda
	{"krasnaya.shp", "krasscem.spr"}, // 0xdb
	{"krasnaya.shp", "krasscem.spr"}, // 0xdc
	{"kiev.shp", "kievscem.spr"}, // 0xdd
	{"kiev.shp", "kievscem.spr"}, // 0xde
	{"uly_escape.shp", nullptr}, // 0xdf
	{"ber_escape.shp", "escascem.spr"}, // 0xe0
	{"fuel_pod1.shp", "car2scem.spr"}, // 0xe1
	{"zakov.shp", "zakoscem.spr"}, // 0xe2
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe3
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe4
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe5
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe6
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe7
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe8
	{"a_mammoth.shp", "mammscem.spr"}, // 0xe9
	{"a_mammoth.shp", "mammscem.spr"}, // 0xea
	{"a_mammoth.shp", "mammscem.spr"}, // 0xeb
	{"a_mammoth.shp", "mammscem.spr"}, // 0xec
	{"a_mammoth.shp", "mammscem.spr"}, // 0xed
	{"a_mammoth.shp", "mammscem.spr"}, // 0xee
	{"a_mammoth.shp", "mammscem.spr"}, // 0xef
	{"ast_hole1.shp", nullptr}, // 0xf0
	{"ast_hole2.shp", nullptr}, // 0xf1
	{"ast_hole3.shp", nullptr}, // 0xf2
	{"ast_hole4.shp", nullptr}, // 0xf3
	{"t_uslf_prd.shp", "predscem.spr"}, // 0xf4
	{"t_jlf_nagi.shp", "nagiscem.spr"}, // 0xf5
	{"t_german_grendal.shp", "grenscem.spr"}, // 0xf6
	{"t_british_crusader.shp", "crusscem.spr"}, // 0xf7
	{"t_usa_coyote.shp", "coyoscem.spr"}, // 0xf8
	{"t_french_mirage.shp", "mirascem.spr"}, // 0xf9
	{"t_british_tempest.shp", "tempscem.spr"}, // 0xfa
	{"t_usmf_pat.shp", "patrscem.spr"}, // 0xfb
	{"t_german_wolverine.shp", "wolvscem.spr"}, // 0xfc
	{"t_ushf_reaper.shp", "reapscem.spr"}, // 0xfd
	{"t_jap_shroud.shp", "shroscem.spr"}, // 0xfe
	{"t_uspf_phx.shp", "phxscem.spr"}, // 0xff
}};
}

const ObjectTypeResourceDefinition& object_type_resource(
	std::uint16_t type)
{
	static constexpr ObjectTypeResourceDefinition empty{};
	return type < kResources.size() ? kResources[type] : empty;
}

bool object_type_has_model(std::uint16_t type)
{
	return object_type_resource(type).model_path != nullptr;
}

std::uint16_t object_type_runtime_alias(std::uint16_t requested_type)
{
	switch (requested_type)
	{
	case 0x35:
	case 0xdb:
	case 0xdc:
		return 0x78;
	case 0x36:
	case 0x40:
	case 0xdd:
	case 0xde:
		return 0xc2;
	case 0xa0:
		return 0x13;
	case 0xa1:
	case 0xa2:
	case 0xe2:
		return 0xb0;
	case 0xda:
		return 0x0f;
	default:
		return requested_type >= 0xe3 && requested_type <= 0xef
			? 0x21
			: requested_type;
	}
}

std::uint16_t object_type_for_mission_creation(
	std::uint16_t authored_type,
	std::uint16_t mission_number,
	std::uint8_t group_object_class,
	bool mission_25_alternate)
{
	if (mission_number < 14 || group_object_class != 0)
	{
		return authored_type;
	}
	if (mission_number == 25 && !mission_25_alternate)
	{
		return 0x2d;
	}
	return authored_type <= 11
		? static_cast<std::uint16_t>(0xf4 + authored_type)
		: authored_type;
}
}
