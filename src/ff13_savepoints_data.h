// AUTO-GENERATED from savepoint_coords.csv + location_key_region.csv.
// zone id -> save points {name, uid, region} + zone area byte.
#ifndef FF13_SAVEPOINTS_DATA_H
#define FF13_SAVEPOINTS_DATA_H

typedef struct { const char* name; short uid; short region; } ff13_sp_t;
typedef struct { int zone; unsigned char area; const ff13_sp_t* sp; int n; } ff13_zone_t;

static const ff13_sp_t SP_z2[] = { {"s_point_hgcr_01",101,2}, {"s_point_hgcr_00",101,2}, {"s_point_hgcr_02",102,2}, {"s_point_hgcr_03",103,2}, {"s_point_hgcr_04",104,2}, {"s_point_hgcr_05",105,21}, {"s_point_hgcr_06",106,21}, {"s_point_hgcr_07",107,21}, {"s_point_hgcr_08",108,2}, {"s_point_hgcr_09",109,2} };
static const ff13_sp_t SP_z3[] = { {"s_point_hiku_00",100,8}, {"s_point_hiku_01",101,22}, {"s_point_hiku_02",102,8}, {"s_point_hiku_03",103,8}, {"s_point_hiku_04",104,8}, {"s_point_hiku_05",105,8}, {"s_point_hiku_06",106,8}, {"s_point_hiku_07",107,8}, {"s_point_hiku_08",108,8}, {"s_point_hiku_09",109,8}, {"s_point_hiku_10",110,8}, {"s_point_hiku_11",111,8}, {"s_point_hiku_12",112,8} };
static const ff13_sp_t SP_z4[] = { {"s_point_vpek_00",500,3}, {"s_point_vpek_01",501,3}, {"s_point_vpek_02",502,3}, {"s_point_vpek_03",503,3}, {"s_point_vpek_04",504,3}, {"s_point_vpek_05",505,3}, {"s_point_vpek_06",506,3}, {"s_point_vpek_07",507,3}, {"s_point_vpek_08",508,3}, {"s_point_vpek_09",509,3}, {"s_point_vpek_10",510,3}, {"s_point_vpek_11",511,3}, {"s_point_vpek_12",512,3}, {"s_point_vpek_13",513,3}, {"s_point_vpek_14",514,3}, {"s_point_vpek_15",515,3}, {"sp_chpt_vpek_01",600,3}, {"sp_chpt_vpek_02",610,3} };
static const ff13_sp_t SP_z6[] = { {"s_point_nati_00",100,7}, {"s_point_nati_01",101,7}, {"s_point_nati_02",102,7}, {"s_point_nati_03",103,7}, {"s_point_nati_04",104,7}, {"s_point_nati_05",105,7}, {"s_point_nati_06",106,7}, {"s_point_nati_07",107,7}, {"s_point_nati_08",108,7}, {"s_point_nati_09",109,7} };
static const ff13_sp_t SP_z8[] = { {"sp_chpt_pmpm_01",7,6}, {"s_point_pmpm_00",100,6}, {"s_point_pmpm_01",101,6}, {"s_point_pmpm_02",102,6}, {"s_point_pmpm_03",103,6}, {"s_point_pmpm_04",104,6}, {"s_point_pmpm_05",105,6}, {"s_point_pmpm_06",106,6}, {"s_point_pmpm_07",107,6}, {"s_point_pmpm_08",108,6}, {"s_point_pmpm_09",109,6}, {"s_point_pmpm_10",110,6}, {"s_point_pmpm_11",111,6}, {"s_point_pmpm_12",112,6}, {"s_point_pmpm_13",113,6}, {"s_point_pmpm_14",114,6} };
static const ff13_sp_t SP_z10[] = { {"s_point_snls_00",100,5}, {"s_point_snls_01",101,5}, {"s_point_snls_02",102,5}, {"s_point_snls_03",103,5}, {"s_point_snls_04",104,5}, {"s_point_snls_05",105,5}, {"s_point_snls_06",106,5}, {"s_point_snls_07",107,5}, {"s_point_snls_08",108,5}, {"s_point_snls_09",109,5} };
static const ff13_sp_t SP_z15[] = { {"s_point_gapr_00",100,4}, {"s_point_gapr_01",101,4}, {"s_point_gapr_02",102,4}, {"s_point_gapr_03",103,4}, {"s_point_gapr_04",104,4}, {"s_point_gapr_05",105,4}, {"s_point_gapr_06",106,4}, {"s_point_gapr_07",107,4}, {"s_point_gapr_08",108,4}, {"s_point_gapr_09",109,4} };
static const ff13_sp_t SP_z16[] = { {"s_point_hang_00",100,0}, {"s_point_hang_01",101,0}, {"s_point_hang_02",102,0}, {"s_point_hang_03",103,0}, {"s_point_hang_04",104,0}, {"s_point_hang_05",105,0}, {"s_point_hang_06",106,0}, {"s_point_hang_07",107,0}, {"s_point_hang_08",108,0}, {"s_point_hang_09",109,0} };
static const ff13_sp_t SP_z17[] = { {"s_point_hgin_00",100,1}, {"s_point_hgin_01",101,1}, {"s_point_hgin_02",102,1}, {"s_point_hgin_03",103,1}, {"s_point_hgin_04",104,1}, {"s_point_hgin_05",105,1}, {"s_point_hgin_06",106,1}, {"s_point_hgin_07",107,1}, {"s_point_hgin_08",108,1}, {"s_point_hgin_09",109,1} };
static const ff13_sp_t SP_z18[] = { {"s_point_fark_01",101,9}, {"s_point_fark_02",102,9}, {"s_point_fark_03",103,9}, {"s_point_fark_04",104,9}, {"s_point_fark_05",105,9}, {"s_point_fark_06",106,9}, {"s_point_fark_07",107,9}, {"s_point_fark_08",108,9}, {"s_point_fark_09",109,9}, {"s_point_fark_10",110,9}, {"s_point_fark_11",111,9}, {"s_point_fark_00",112,9} };
static const ff13_sp_t SP_z19[] = { {"s_point_eden_00",100,19}, {"s_point_eden_01",101,24}, {"s_point_eden_02",102,25}, {"s_point_eden_03",103,25}, {"s_point_eden_04",104,26}, {"s_point_eden_05",105,26}, {"s_point_eden_06",106,26}, {"s_point_eden_07",107,26}, {"s_point_eden_08",108,26}, {"s_point_eden_09",109,27}, {"s_point_eden_10",300,27} };
static const ff13_sp_t SP_z20[] = { {"s_point_gpst_00",100,11}, {"s_point_gpst_01",101,11}, {"s_point_gpst_02",102,11}, {"s_point_gpst_03",103,11}, {"s_point_gpst_04",104,11}, {"s_point_gpst_05",105,11}, {"s_point_gpst_06",106,11}, {"s_point_gpst_07",107,11}, {"s_point_gpst_08",108,11}, {"s_point_gpst_09",109,11} };
static const ff13_sp_t SP_z21[] = { {"s_point_gpcg_00",100,12}, {"s_point_gpcg_01",101,12}, {"s_point_gpcg_02",102,12}, {"s_point_gpcg_03",103,12}, {"s_point_gpcg_04",104,12}, {"s_point_gpcg_05",105,12}, {"s_point_gpcg_06",106,12}, {"s_point_gpcg_07",107,12}, {"s_point_gpcg_08",108,12}, {"s_point_gpcg_09",109,12} };
static const ff13_sp_t SP_z22[] = { {"s_point_gpyu_00",100,13}, {"s_point_gpyu_01",101,13}, {"s_point_gpyu_02",102,13}, {"s_point_gpyu_03",103,13}, {"s_point_gpyu_04",104,13}, {"s_point_gpyu_05",105,13}, {"s_point_gpyu_06",106,13}, {"s_point_gpyu_07",107,13}, {"s_point_gpyu_08",108,13}, {"s_point_gpyu_09",109,13} };
static const ff13_sp_t SP_z23[] = { {"s_point_gpda_00",100,14}, {"s_point_gpda_01",101,14}, {"s_point_gpda_02",102,14}, {"s_point_gpda_03",103,14}, {"s_point_gpda_04",104,14}, {"s_point_gpda_05",105,14}, {"s_point_gpda_06",106,14}, {"s_point_gpda_07",107,14}, {"s_point_gpda_08",108,14}, {"s_point_gpda_09",109,14} };
static const ff13_sp_t SP_z24[] = { {"s_point_gptk_00",100,15}, {"s_point_gptk_01",101,15}, {"s_point_gptk_02",102,15}, {"s_point_gptk_03",103,15}, {"s_point_gptk_04",104,15}, {"s_point_gptk_05",105,15}, {"s_point_gptk_06",106,15}, {"s_point_gptk_07",107,15}, {"s_point_gptk_08",108,15}, {"s_point_gptk_09",109,15} };
static const ff13_sp_t SP_z26[] = { {"s_point_gptt_00",100,16}, {"s_point_gptt_01",101,16}, {"s_point_gptt_02",102,16}, {"s_point_gptt_03",103,16}, {"s_point_gptt_04",104,16}, {"s_point_gptt_05",105,16}, {"s_point_gptt_06",106,16}, {"s_point_gptt_07",107,16}, {"s_point_gptt_08",108,16}, {"s_point_gptt_09",109,16} };
static const ff13_sp_t SP_z27[] = { {"s_point_gpwo_00",100,17}, {"s_point_gpwo_01",101,17}, {"s_point_gpwo_02",102,17}, {"s_point_gpwo_03",103,17}, {"s_point_gpwo_04",104,17}, {"s_point_gpwo_05",105,17}, {"s_point_gpwo_06",106,17}, {"s_point_gpwo_07",107,17}, {"s_point_gpwo_08",108,17}, {"s_point_gpwo_09",109,17} };
static const ff13_sp_t SP_z29[] = { {"s_point_lasd_00",100,20}, {"s_point_lasd_01",101,20}, {"s_point_lasd_02",102,20}, {"s_point_lasd_03",103,20}, {"s_point_lasd_04",104,20}, {"s_point_lasd_05",105,20}, {"s_point_lasd_06",106,20}, {"s_point_lasd_07",107,20}, {"s_point_lasd_08",108,28}, {"sp_chpt_lasd_00",108,90}, {"s_point_lasd_09",109,20} };
static const ff13_sp_t SP_z30[] = { {"s_point_gpoc_00",100,18}, {"s_point_gpoc_01",101,18}, {"s_point_gpoc_02",102,18}, {"s_point_gpoc_03",103,18}, {"s_point_gpoc_04",104,18}, {"s_point_gpoc_05",105,18}, {"s_point_gpoc_06",106,18}, {"s_point_gpoc_07",107,18}, {"s_point_gpoc_08",108,18}, {"s_point_gpoc_09",109,18} };
static const ff13_sp_t SP_z105[] = { {"s_point_eark_00",100,10}, {"s_point_eark_01",101,10}, {"s_point_eark_02",102,10}, {"s_point_eark_03",103,10}, {"s_point_eark_04",104,10}, {"s_point_eark_05",105,10}, {"s_point_eark_06",106,10}, {"s_point_eark_07",107,10}, {"s_point_eark_08",108,10}, {"s_point_eark_09",109,10} };

static const ff13_zone_t FF13_ZONES[] = {
  { 2, 0x02, SP_z2, 10 },
  { 3, 0x03, SP_z3, 13 },
  { 4, 0x04, SP_z4, 18 },
  { 6, 0x06, SP_z6, 10 },
  { 8, 0x08, SP_z8, 16 },
  { 10, 0x0A, SP_z10, 10 },
  { 15, 0x0F, SP_z15, 10 },
  { 16, 0x10, SP_z16, 10 },
  { 17, 0x11, SP_z17, 10 },
  { 18, 0x12, SP_z18, 12 },
  { 19, 0x13, SP_z19, 11 },
  { 20, 0x14, SP_z20, 10 },
  { 21, 0x15, SP_z21, 10 },
  { 22, 0x16, SP_z22, 10 },
  { 23, 0x17, SP_z23, 10 },
  { 24, 0x18, SP_z24, 10 },
  { 26, 0x1A, SP_z26, 10 },
  { 27, 0x1B, SP_z27, 10 },
  { 29, 0x1D, SP_z29, 11 },
  { 30, 0x1E, SP_z30, 10 },
  { 105, 0x69, SP_z105, 10 }
};
static const int FF13_ZONES_N = 21;

#endif
