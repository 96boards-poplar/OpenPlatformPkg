/** @file
*
*  Copyright (c) 2014-2017, Linaro Limited. All rights reserved.
*  Copyright (c) 2014-2017, Hisilicon Limited. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#ifndef __HI3798CV200_H__
#define __HI3798CV200_H__

#define HI3798CV200_MDDRC_AXI_BASE              0xF8A30000
#define HI3798CV200_AXI_REGION_MAP              0x100
#define HI3798CV200_REGION_SIZE_MASK            (7 << 8)
// (0 << 8) means 16MB, (7 << 8) means 2GB
#define HI3798CV200_REGION_SIZE(x)              (1U << ((((x) & HI3798CV200_REGION_SIZE_MASK) >> 8) + 24))

#define HI3798CV200_GIC_PERIPH_BASE             0xF1000000
#define HI3798CV200_GIC_PERIPH_SZ               SIZE_64KB
#define HI3798CV200_PERIPH_BASE                 0xF8000000
#define HI3798CV200_PERIPH_SZ                   0x02000000

#define HI3798CV200_PERI_BASE                   0xF8A20000
#define HI3798CV200_PERI_USB0                   (HI3798CV200_PERI_BASE + 0x0120)
#define USB2_PHY01_TEST_CLK                     (1 << 22)
#define HI3798CV200_PERI_USB3                   (HI3798CV200_PERI_BASE + 0x012C)
#define USB2_2P_CHIPID                          (1 << 28)

#define HI3798CV200_CRG_BASE                    0xF8A22000
#define HI3798CV200_CRG46                       (HI3798CV200_CRG_BASE + 0x00B8)
#define USB2_OTG_PHY_SRST_REQ                   (1 << 17)
#define USB2_HST_PHY_SRST_REQ                   (1 << 16)
#define USB2_UTMI_SRST_REQ                      (1 << 13)
#define USB2_BUS_SRST_REQ                       (1 << 12)
#define USB2_UTMI_CKEN                          (1 << 5)
#define USB2_HST_PHY_CKEN                       (1 << 4)
#define USB2_OTG_UTMI_CKEN                      (1 << 3)
#define USB2_OHCI12M_CKEN                       (1 << 2)
#define USB2_OHCI48M_CKEN                       (1 << 1)
#define USB2_BUS_CKEN                           (1 << 0)
#define HI3798CV200_CRG47                       (HI3798CV200_CRG_BASE + 0x00BC)
#define USB2_PHY01_SRST_TREQ1                   (1 << 9)
#define USB2_PHY01_SRST_TREQ0                   (1 << 8)
#define USB2_PHY01_SRST_REQ                     (1 << 4)
#define USB2_PHY01_REF_CKEN                     (1 << 0)

#define HI3798CV200_SDIO2_BASE                  0xF9830000

#endif  /* __HI3798CV200_H__ */
