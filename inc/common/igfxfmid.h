/*===================== begin_copyright_notice ==================================

Copyright (c) 2017 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


======================= end_copyright_notice ==================================*/
#ifndef __IGFXFMID_H__
#define __IGFXFMID_H__

typedef enum {
    IGFX_UNKNOWN        = 0,
    IGFX_BROADWELL,
    IGFX_CHERRYVIEW,
    IGFX_SKYLAKE,
    IGFX_KABYLAKE,
    IGFX_COFFEELAKE,
    IGFX_WILLOWVIEW,
    IGFX_BROXTON,
    IGFX_GEMINILAKE,
    IGFX_GLENVIEW,
    IGFX_CANNONLAKE,
    IGFX_MAX_PRODUCT,


    IGFX_GENNEXT               = 0x7ffffffe,
    PRODUCT_FAMILY_FORCE_ULONG = 0x7fffffff
} PRODUCT_FAMILY;

typedef enum {
    PCH_UNKNOWN    = 0,
    PCH_IBX,            // Ibexpeak
    PCH_CPT,            // Cougarpoint,
    PCH_CPTR,           // Cougarpoint Refresh,
    PCH_PPT,            // Panther Point
    PCH_LPT,            // Lynx Point
    PCH_LPTR,           // Lynx Point Refresh
    PCH_WPT,            // Wildcat point
    PCH_SPT,            // Sunrise point
    PCH_KBP,            // Kabylake PCH
    PCH_CNP_LP,         // Cannonlake LP PCH
    PCH_CNP_H,          // Cannonlake Halo PCH
    PCH_PRODUCT_FAMILY_FORCE_ULONG = 0x7fffffff
} PCH_PRODUCT_FAMILY;

typedef enum {
    IGFX_UNKNOWN_CORE    = 0,
    IGFX_GEN3_CORE       = 1,   //Gen3 Family
    IGFX_GEN3_5_CORE     = 2,   //Gen3.5 Family
    IGFX_GEN4_CORE       = 3,   //Gen4 Family
    IGFX_GEN4_5_CORE     = 4,   //Gen4.5 Family
    IGFX_GEN5_CORE       = 5,   //Gen5 Family
    IGFX_GEN5_5_CORE     = 6,   //Gen5.5 Family
    IGFX_GEN5_75_CORE    = 7,   //Gen5.75 Family
    IGFX_GEN6_CORE       = 8,   //Gen6 Family
    IGFX_GEN7_CORE       = 9,   //Gen7 Family
    IGFX_GEN7_5_CORE     = 10,  //Gen7.5 Family
    IGFX_GEN8_CORE       = 11,  //Gen8 Family
    IGFX_GEN9_CORE       = 12,  //Gen9 Family
    IGFX_GEN10_CORE      = 13,  //Gen10 Family
    IGFX_MAX_CORE,				//Max Family, for lookup table

    IGFX_GENNEXT_CORE          = 0x7ffffffe,  //GenNext
    GFXCORE_FAMILY_FORCE_ULONG = 0x7fffffff
} GFXCORE_FAMILY;

typedef enum {
    IGFX_SKU_NONE		= 0,	
    IGFX_SKU_ULX        = 1,
    IGFX_SKU_ULT        = 2,
    IGFX_SKU_T          = 3,
    IGFX_SKU_ALL        = 0xff
} PLATFORM_SKU;

typedef enum __GTTYPE
{
    GTTYPE_GT1 = 0x0,
    GTTYPE_GT2,
    GTTYPE_GT2_FUSED_TO_GT1,
    GTTYPE_GT2_FUSED_TO_GT1_6, //IVB
    GTTYPE_GTL, // HSW
    GTTYPE_GTM, // HSW
    GTTYPE_GTH, // HSW
    GTTYPE_GT1_5,//HSW
    GTTYPE_GT1_75,//HSW
    GTTYPE_GT3,//BDW
    GTTYPE_GT4,//BDW
    GTTYPE_GT0,//BDW
    GTTYPE_GTA,// BXT
    GTTYPE_GTC,// BXT
    GTTYPE_GTX, // BXT
    GTTYPE_GT2_5,//CNL
    GTTYPE_GT3_5,//SKL
    GTTYPE_GT0_5,//CNL
    GTTYPE_UNDEFINED,//Always at the end.
}GTTYPE, *PGTTYPE;

/////////////////////////////////////////////////////////////////
//
//    Platform types which are used during Sku/Wa initialization.
//
#ifndef _COMMON_PPA
    typedef enum {
        PLATFORM_NONE       = 0x00,
        PLATFORM_DESKTOP    = 0x01,
        PLATFORM_MOBILE     = 0x02,
        PLATFORM_TABLET     = 0X03,
        PLATFORM_ALL        = 0xff, // flag used for applying any feature/WA for All platform types 
    } PLATFORM_TYPE;
#endif
typedef struct PLATFORM_STR {
    PRODUCT_FAMILY      eProductFamily;
    PCH_PRODUCT_FAMILY  ePCHProductFamily;
    GFXCORE_FAMILY      eDisplayCoreFamily;
    GFXCORE_FAMILY      eRenderCoreFamily;
    #ifndef _COMMON_PPA
    PLATFORM_TYPE       ePlatformType;
    #endif

    unsigned short      usDeviceID;
    unsigned short      usRevId;
    unsigned short      usDeviceID_PCH;
    unsigned short      usRevId_PCH;
    // GT Type
    // Note: Is valid only till Gen9. From Gen10 SKUs are not identified by any GT flags. 'GT_SYSTEM_INFO' should be used instead.
    GTTYPE              eGTType;
} PLATFORM;

// add enums at the end 
typedef enum __SKUIDTYPE
{
    SKU_FULL_TYPE = 0x0,
    SKU_VALUE_TYPE,
    SKU_PLUS_FULL_TYPE,
    SKU_PLUS_VALUE_TYPE,
    SKU_T_TYPE,
    SKU_PLUS_T_TYPE,
    SKU_P_TYPE,
    SKU_PLUS_P_TYPE,
    SKU_SMALL_TYPE,
    SKU_LIGHT_TYPE,
    SKU_N_TYPE
}SKUIDTYPE, *PSKUIDTYPE;

typedef enum __CPUTYPE
{
    CPU_UNDEFINED = 0x0,
    CPU_CORE_I3,
    CPU_CORE_I5,
    CPU_CORE_I7,
    CPU_PENTIUM,
    CPU_CELERON,
    CPU_CORE,
    CPU_VPRO,
    CPU_SUPER_SKU,
    CPU_ATOM,
    CPU_CORE1,
    CPU_CORE2,
    CPU_WS,
    CPU_SERVER,
    CPU_CORE_I5_I7,
    CPU_COREX1_4,
    CPU_ULX_PENTIUM,
    CPU_MB_WORKSTATION,
    CPU_DT_WORKSTATION,
    CPU_M3,
    CPU_M5,
    CPU_M7,
	CPU_MEDIA_SERVER //Added for KBL
}CPUTYPE, *PCPUTYPE;

// the code below convert platform real revision number to pre-defined revision number, the revision will be set as follow
// REVISION_A0 - this will include all incarnations for A stepping in all packages types A = {A0}
// REVISION_A1 - this will include all incarnations for A stepping in all packages types A = {A1}
// REVISION_A3 - this will include all incarnations for A stepping in all packages types A = {A3,...,A7}
// REVISION_B - this will include all incarnations for B stepping in all packages types B = {B0,B1,..,B7}
// REVISION_C - this will include all incarnations for C stepping in all packages types C = {C0,C1,..,C7}
// REVISION_D - this will include all incarnations for C stepping in all packages types C = {D0,D1}
// REVISION_K - this will include all incarnations for K stepping in all packages types K = {K0,K1,..,K7}
typedef enum __REVID
{
    REVISION_A0 = 0,
    REVISION_A1, //1
    REVISION_A3,//2
    REVISION_B,//3
    REVISION_C,//4
    REVISION_D,//5
    REVISION_K//6
}REVID, *PREVID;

typedef enum __NATIVEGTTYPE
{
    NATIVEGTTYPE_HSW_UNDEFINED  = 0x00,
    NATIVEGTTYPE_HSW_GT1        = 0x01,
    NATIVEGTTYPE_HSW_GT2        = 0x02,
    NATIVEGTTYPE_HSW_GT3        = 0x03,
}NATIVEGTTYPE;

// Following macros return true/false depending on the current PCH family
#define PCH_IS_PRODUCT(p, r)            ( (p).ePCHProductFamily == r )
#define PCH_GET_CURRENT_PRODUCT(p)      ( (p).ePCHProductFamily )

// These macros return true/false depending on current product/core family.
#define GFX_IS_PRODUCT(p, r)           ( (p).eProductFamily == r )
#define GFX_IS_DISPLAYCORE(p, d)       ( (p).eDisplayCoreFamily == d )
#define GFX_IS_RENDERCORE(p, r)        ( (p).eRenderCoreFamily == r )
// These macros return the current product/core family enum.
// Relational compares (</>) should not be done when using GFX_GET_CURRENT_PRODUCT
// macro.  There are no relationships between the PRODUCT_FAMILY enum values.
#define GFX_GET_CURRENT_PRODUCT(p)     ( (p).eProductFamily )
#define GFX_GET_CURRENT_DISPLAYCORE(p) ( (p).eDisplayCoreFamily )
#define GFX_GET_CURRENT_RENDERCORE(p)  ( (p).eRenderCoreFamily )
// These macros return true/false depending on the current render family.
#define GFX_IS_NAPA_RENDER_FAMILY(p)   ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN3_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN3_5_CORE ) )

#define GFX_IS_GEN_RENDER_FAMILY(p)    ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN4_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN4_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_75_CORE ) ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN6_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_5_OR_LATER(p)       ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_75_CORE ) ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN6_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_5_75_OR_LATER(p)    ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN5_75_CORE ) ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN6_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_6_OR_LATER(p)       ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN6_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_7_OR_LATER(p)       ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||   \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_7_5_OR_LATER(p)     ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN7_5_CORE )  ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_8_OR_LATER(p)       ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN8_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_8_CHV_OR_LATER(p)   ( ( GFX_GET_CURRENT_PRODUCT(p) == IGFX_CHERRYVIEW )      ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_9_OR_LATER(p)       ( ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN9_CORE )    ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )   ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_GEN_10_OR_LATER(p)       (( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GEN10_CORE )  ||  \
                                         ( GFX_GET_CURRENT_RENDERCORE(p) == IGFX_GENNEXT_CORE ) )

#define GFX_IS_ATOM_PRODUCT_FAMILY(p)  ( GFX_IS_PRODUCT(p, IGFX_VALLEYVIEW)   ||  \
                                         GFX_IS_PRODUCT(p, IGFX_CHERRYVIEW)   ||  \
                                         GFX_IS_PRODUCT(p, IGFX_BROXTON) )

///////////////////////////////////////////////////////////////////
//
// macros for comparing Graphics family and products
//
///////////////////////////////////////////////////////////////////
#define GFX_IS_FAMILY_EQUAL_OR_ABOVE(family1, family2) ((family1)>=(family2) ? TRUE : FALSE)
#define GFX_IS_FAMILY_EQUAL_OR_BELOW(family1, family2) ((family1)<=(family2) ? TRUE : FALSE)
#define GFX_IS_FAMILY_BELOW(family1, family2) ((family1)<(family2) ? TRUE : FALSE)
#define GFX_IS_PRODUCT_EQUAL_OR_ABOVE(product1, product2) ((product1)>=(product2) ? TRUE : FALSE)
#define GFX_IS_PRODUCT_EQUAL_OR_BELOW(product1, product2) ((product1)<=(product2) ? TRUE : FALSE)
#define GFX_IS_PRODUCT_BELOW(product1, product2)  ((product1) <(product2) ? TRUE : FALSE)

//Feature ID: Graphics PRD PC11.0 - Brookdale-G Support
//Description: Move device and vendor ID's to igfxfmid.h.
//  Add #include "igfxfmid.h".
//Other Files Modified: dispconf.c, kcconfig.c, kchmisc.c, kchsys.c,
//  driver.h, igfxfmid.h, imdefs.h, kchialm.h, kchname.h, softbios.h,
//  swbios.h, vddcomm.h, vidmini.h

#define INTEL_VENDOR_ID              0x8086   // Intel Corporation

//Device IDs
#define UNKNOWN_DEVICE_ID            0xFFFF   // Unknown device

//CHV device ids
#define ICHV_MOBL_DEVICE_F0_ID           0x22B0   // CHV TABLET i.e CHT
#define ICHV_PLUS_MOBL_DEVICE_F0_ID      0x22B1   // Essential i.e Braswell
#define ICHV_DESK_DEVICE_F0_ID           0x22B2   // Reserved
#define ICHV_PLUS_DESK_DEVICE_F0_ID      0x22B3   // Reserved

//BDW device ids
#define IBDW_GT0_DESK_DEVICE_F0_ID              0x0BD0
#define IBDW_GT1_DESK_DEVICE_F0_ID              0x0BD1
#define IBDW_GT2_DESK_DEVICE_F0_ID              0x0BD2
#define IBDW_GT3_DESK_DEVICE_F0_ID              0x0BD3
#define IBDW_GT4_DESK_DEVICE_F0_ID              0x0BD4

#define IBDW_GT1_HALO_MOBL_DEVICE_F0_ID         0x1602
#define IBDW_GT1_ULT_MOBL_DEVICE_F0_ID          0x1606
#define IBDW_GT1_RSVD_DEVICE_F0_ID              0x160B
#define IBDW_GT1_SERV_DEVICE_F0_ID              0x160A
#define IBDW_GT1_WRK_DEVICE_F0_ID               0x160D
#define IBDW_GT1_ULX_DEVICE_F0_ID               0x160E
#define IBDW_GT2_HALO_MOBL_DEVICE_F0_ID         0x1612
#define IBDW_GT2_ULT_MOBL_DEVICE_F0_ID          0x1616
#define IBDW_GT2_RSVD_DEVICE_F0_ID              0x161B
#define IBDW_GT2_SERV_DEVICE_F0_ID              0x161A
#define IBDW_GT2_WRK_DEVICE_F0_ID               0x161D 
#define IBDW_GT2_ULX_DEVICE_F0_ID               0x161E
#define IBDW_GT3_HALO_MOBL_DEVICE_F0_ID         0x1622
#define IBDW_GT3_ULT_MOBL_DEVICE_F0_ID          0x1626
#define IBDW_GT3_ULT25W_MOBL_DEVICE_F0_ID       0x162B 
#define IBDW_GT3_SERV_DEVICE_F0_ID              0x162A
#define IBDW_GT3_WRK_DEVICE_F0_ID               0x162D
#define IBDW_GT3_ULX_DEVICE_F0_ID               0x162E
#define IBDW_RSVD_MRKT_DEVICE_F0_ID             0x1632
#define IBDW_RSVD_ULT_MOBL_DEVICE_F0_ID         0x1636
#define IBDW_RSVD_HALO_MOBL_DEVICE_F0_ID        0x163B
#define IBDW_RSVD_SERV_DEVICE_F0_ID             0x163A
#define IBDW_RSVD_WRK_DEVICE_F0_ID              0x163D
#define IBDW_RSVD_ULX_DEVICE_F0_ID              0x163E

//skl placeholder

#define ISKL_GT4_DT_DEVICE_F0_ID                0x1932 
#define ISKL_GT2_DT_DEVICE_F0_ID                0x1912 // Used on actual Silicon

#define ISKL_GT1_DT_DEVICE_F0_ID                0x1902  


#define ISKL_GT2_ULT_DEVICE_F0_ID               0x1916 
#define ISKL_GT2F_ULT_DEVICE_F0_ID              0x1921
#define ISKL_GT3e_ULT_DEVICE_F0_ID_540          0x1926 
#define ISKL_GT3e_ULT_DEVICE_F0_ID_550          0x1927

#define ISKL_GT2_ULX_DEVICE_F0_ID               0x191E  
#define ISKL_GT1_ULT_DEVICE_F0_ID               0x1906 
#define ISKL_GT3_MEDIA_SERV_DEVICE_F0_ID        0x192D
#define ISKL_GT1_5_ULT_DEVICE_F0_ID             0x1913  

#define ISKL_GT3_ULT_DEVICE_F0_ID               0x1923 

#define ISKL_GT2_HALO_MOBL_DEVICE_F0_ID         0x191B 

#define ISKL_GT4_HALO_MOBL_DEVICE_F0_ID         0x193B 
#define ISKL_GT4_SERV_DEVICE_F0_ID				0x193A 
#define ISKL_GT2_WRK_DEVICE_F0_ID				0x191D 
#define ISKL_GT4_WRK_DEVICE_F0_ID				0x193D 


#define ISKL_GT0_DESK_DEVICE_F0_ID              0x0900
#define ISKL_GT1_DESK_DEVICE_F0_ID              0x0901
#define ISKL_GT2_DESK_DEVICE_F0_ID              0x0902
#define ISKL_GT3_DESK_DEVICE_F0_ID              0x0903
#define ISKL_GT4_DESK_DEVICE_F0_ID              0x0904
#define ISKL_GT1_ULX_DEVICE_F0_ID               0x190E
//SKL strings to be be deleted in future

#define ISKL_GT1_HALO_MOBL_DEVICE_F0_ID         0x190B
#define ISKL_GT1_SERV_DEVICE_F0_ID				0x190A
#define ISKL_GT1_5_ULX_DEVICE_F0_ID             0x1915
#define ISKL_GT1_5_DT_DEVICE_F0_ID              0x1917
#define ISKL_GT2_SERV_DEVICE_F0_ID				0x191A
#define ISKL_LP_DEVICE_F0_ID                    0x9905
#define ISKL_GT3_HALO_MOBL_DEVICE_F0_ID         0x192B
#define ISKL_GT3_SERV_DEVICE_F0_ID				0x192A
#define ISKL_GT0_MOBL_DEVICE_F0_ID              0xFFFF

// KabyLake Device ids
#define IKBL_GT1_ULT_DEVICE_F0_ID               0x5906
#define IKBL_GT1_5_ULT_DEVICE_F0_ID             0x5913
#define IKBL_GT2_ULT_DEVICE_F0_ID               0x5916 
#define IKBL_GT2F_ULT_DEVICE_F0_ID              0x5921
#define IKBL_GT3_15W_ULT_DEVICE_F0_ID           0x5926 
//#define IKBL_GT3E_ULT_DEVICE_F0_ID              0x5926
#define IKBL_GT1_ULX_DEVICE_F0_ID               0x590E
#define IKBL_GT1_5_ULX_DEVICE_F0_ID             0x5915
#define IKBL_GT2_ULX_DEVICE_F0_ID               0x591E
#define IKBL_GT1_DT_DEVICE_F0_ID                0x5902
#define IKBL_GT2_R_ULT_DEVICE_F0_ID             0x5917 
#define IKBL_GT2_DT_DEVICE_F0_ID                0x5912
#define IKBL_GT1_HALO_DEVICE_F0_ID              0x590B
#define IKBL_GT1F_HALO_DEVICE_F0_ID             0x5908
#define IKBL_GT2_HALO_DEVICE_F0_ID              0x591B
#define IKBL_GT4_HALO_DEVICE_F0_ID              0x593B
#define IKBL_GT1_SERV_DEVICE_F0_ID              0x590A
#define IKBL_GT2_SERV_DEVICE_F0_ID              0x591A
#define IKBL_GT2_WRK_DEVICE_F0_ID               0x591D
#define IKBL_GT3_ULT_DEVICE_F0_ID               0x5923
#define IKBL_GT3_28W_ULT_DEVICE_F0_ID           0x5927
//keeping the below ids as its been used in linux . need to be removed once removed from linux files.
#define IKBL_GT4_DT_DEVICE_F0_ID                0x5932
#define IKBL_GT3_HALO_DEVICE_F0_ID              0x592B
#define IKBL_GT3_SERV_DEVICE_F0_ID              0x592A
#define IKBL_GT4_SERV_DEVICE_F0_ID              0x593A
#define IKBL_GT4_WRK_DEVICE_F0_ID               0x593D

//GLK Device ids
#define IGLK_GT2_ULT_18EU_DEVICE_F0_ID          0x3184
#define IGLK_GT2_ULT_12EU_DEVICE_F0_ID          0x3185

//BXT BIOS programmed Silicon ids.
#define IBXT_GT_3x6_DEVICE_ID                0x0A84
#define IBXT_PRO_3x6_DEVICE_ID               0x1A84 //18EU
#define IBXT_PRO_12EU_3x6_DEVICE_ID          0x1A85 //12 EU
#define IBXT_P_3x6_DEVICE_ID                 0x5A84 //18EU APL
#define IBXT_P_12EU_3x6_DEVICE_ID            0x5A85 //12EU APL

// CNL Placeholder
// These device ID defs to be removed later on after UMD switches to GT_SYSTEM_INFO interface.
#define ICNL_GT0_DESK_DEVICE_F0_ID              0XDEAD      // Not Valid - To be cleaned up.
#define ICNL_GT1_DESK_DEVICE_F0_ID              0x0A01
#define ICNL_GT2_DESK_DEVICE_F0_ID              0x0A02
#define ICNL_GT2_5_DESK_DEVICE_F0_ID            0x0A00      // Not POR - To be cleaned up.
#define ICNL_GT3_DESK_DEVICE_F0_ID              0x0A05
#define ICNL_GT4_DESK_DEVICE_F0_ID              0x0A07

// CNL Si device ids
#define ICNL_5x8_ULX_DEVICE_F0_ID               0x5A51      //GT2
#define ICNL_5x8_ULT_DEVICE_F0_ID               0x5A52      //GT2
#define ICNL_4x8_ULT_DEVICE_F0_ID               0x5A5A      //GT1.5
#define ICNL_3x8_ULT_DEVICE_F0_ID               0x5A42      //GT1
#define ICNL_2x8_ULT_DEVICE_F0_ID               0x5A4A      //GT0.5
#define ICNL_9x8_ULT_DEVICE_F0_ID               0x5A62      
#define ICNL_9x8_SUPERSKU_DEVICE_F0_ID          0x5A60      
#define ICNL_5x8_SUPERSKU_DEVICE_F0_ID          0x5A50      //GT2
#define ICNL_1x6_5x8_SUPERSKU_DEVICE_F0_ID      0x5A40      //GTx
#define ICNL_5x8_HALO_DEVICE_F0_ID              0x5A54      //GT2
#define ICNL_3x8_HALO_DEVICE_F0_ID              0x5A44      //GT1
#define ICNL_5x8_DESKTOP_DEVICE_F0_ID           0x5A55      
#define ICNL_3x8_DESKTOP_DEVICE_F0_ID           0x5A45      
#define ICNL_4x8_ULX_DEVICE_F0_ID               0x5A59      //GT1.5
#define ICNL_3x8_ULX_DEVICE_F0_ID               0x5A41      //GT1
#define ICNL_2x8_ULX_DEVICE_F0_ID               0x5A49      //GT0.5
#define ICNL_4x8_HALO_DEVICE_F0_ID              0x5A5C      //GT1.5

#define ICFL_GT1_S61_DT_DEVICE_F0_ID            0x3E90
#define ICFL_GT1_S41_DT_DEVICE_F0_ID            0x3E93
#define ICFL_GT2_S62_DT_DEVICE_F0_ID            0x3E92
#define ICFL_GT2_HALO_DEVICE_F0_ID              0x3E9B
#define ICFL_GT2_SERV_DEVICE_F0_ID              0x3E96 
#define ICFL_GT2_HALO_WS_DEVICE_F0_ID           0x3E94 
#define ICFL_GT2_S42_DT_DEVICE_F0_ID            0x3E91
#define ICFL_GT3_ULT_15W_DEVICE_F0_ID           0x3EA6   
#define ICFL_GT3_ULT_15W_42EU_DEVICE_F0_ID      0x3EA7
#define ICFL_GT3_ULT_28W_DEVICE_F0_ID           0x3EA8
#define ICFL_GT3_ULT_DEVICE_F0_ID               0x3EA5
#define ICFL_HALO_DEVICE_F0_ID                  0x3E95

#endif
