global-incdirs-y += .
global-incdirs-y += registers

srcs-y += main.c imx-common.c

srcs-$(CFG_MX6)$(CFG_MX7) += mmdc.c

srcs-$(CFG_PL310) += imx_pl310.c
ifeq ($(CFG_PSCI_ARM32),y)
srcs-$(CFG_MX6)$(CFG_MX7) += gpcv2.c
srcs-$(CFG_MX7) += pm/pm-imx7.c pm/psci-suspend-imx7.S pm/imx7_suspend.c
$(call force,CFG_PM_ARM32,y)
endif

cflags-pm/psci.c-y += -Wno-suggest-attribute=noreturn

ifneq (,$(filter y, $(CFG_MX6Q) $(CFG_MX6D) $(CFG_MX6DL) $(CFG_MX6S) \
       $(CFG_MX6SX)))
srcs-y += a9_plat_init.S
endif

ifneq (,$(filter y, $(CFG_MX6UL) $(CFG_MX6ULL)))
srcs-y += a7_plat_init.S
endif

srcs-$(CFG_MX7) += a7_plat_init.S
srcs-$(CFG_TZC380) += tzasc.c

srcs-$(CFG_CSU) += imx_csu.c
srcs-$(CFG_SCU) += imx_scu.c

## Place here the objects initialize as service_init
## File order give the order of the initialization
srcs-$(CFG_IMX_OCRAM) += imx_ocram.c
srcs-$(CFG_PSCI_ARM32) += pm/psci.c
