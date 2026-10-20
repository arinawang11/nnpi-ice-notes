# Out-of-tree build for the NNP-I card-side sphcs module.
# SPDX-License-Identifier: GPL-2.0-or-later
#
# KDIR must point at a configured kernel build tree for the target kernel:
#   make KDIR=/path/to/linux-5.7 ION_STUB=0
# For kernels without the vendor ION allocator (mainline/Ubuntu), set ION_STUB=1.

KDIR ?= /lib/modules/$(shell uname -r)/build
SPH_CARD := $(src)/card_driver
ION_STUB ?= 0

obj-m := sphcs.o

sphcs-objs := dma_page_pool.o inf_cmd_list.o inf_cmdq.o inf_context.o \
        inf_copy.o inf_cpylst.o inf_devnet.o inf_devres.o inf_exec_req.o \
        inf_req.o ipc_c2h_events.o msg_scheduler.o periodic_timer.o \
        sphcs_chan_net.o sphcs_cmd_chan.o sphcs_crash_dump.o sphcs_cs.o \
        sphcs_ctx_uids.o sphcs_dma_sched.o sphcs_genmsg.o sphcs_hostwin.o \
        sphcs_hwtrace.o sphcs_hw_utils.o sphcs_ibecc.o sphcs_inf.o \
        sphcs_intel_th_stub.o sphcs_main.o sphcs_maintenance.o sphcs_p2p.o \
        sphcs_pcie_sph.o sphcs_power.o sw_counters.o sphcs_trace.o

ifeq ($(ION_STUB),1)
sphcs-objs += sphcs_ion_stub.o
endif

ccflags-y += -I$(SPH_CARD)/card/include -I$(SPH_CARD)/card/driver/include \
        -I$(SPH_CARD)/card/driver/host_interface -I$(SPH_CARD)/common/include \
        -I$(src)/compat -I$(src) -DNO_MCE -DHW_LAYER_SPH -DNNPI_DISABLE_SSH=0 \
        -Wno-unused-function

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
